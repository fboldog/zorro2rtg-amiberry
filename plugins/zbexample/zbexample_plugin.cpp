// zbexample: the smallest complete zboard plugin with a peer (docs/writing-a-plugin.md). A 64 KB Zorro II I/O
// board with no display, a packet mailbox in the shape of a network card:
//   $00 ID       R   $5A45
//   $02 STATUS   R   bit 0 RX_AVAIL (a received packet waits), bit 1 LINK (the peer's device is up)
//   $04 CTRL     RW  bit 0 IRQ_EN: /INT2 while RX_AVAIL
//   $06 TX_DATA  W   the next byte of the packet being written (low byte)
//   $08 TX_SEND  W   any value: send the packet written so far (1 to MTU bytes), start a new one
//   $0A RX_LEN   R   the oldest received packet's length, 0 when none waits
//   $0C RX_DATA  R   its next byte (low byte); reading moves on
//   $0E RX_DONE  W   any value: drop the oldest packet
// Everything the Amiga reads is answered here, from what the peer pushed earlier: received packets wait in
// the plugin, so a read never waits for the daemon or the device ("pushed state", the pattern for any board
// whose reads depend on outside hardware). Writes that must leave (a packet to send) become messages.
//
// Messages, plugin -> peer: 'T' + the packet's bytes.
//           peer -> plugin: 'R' + a received packet's bytes; 'L' + 1 byte: the device is up (1) or down (0).
// The peer (peer.py next to this file) owns its device itself, a UDP socket; it needs no Nano 20K.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "zboard_plugin.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <new>
#include <vector>

namespace {

constexpr uint32_t BOARD_SIZE = 0x10000;
constexpr uint32_t MTU = 1514;           // the longest packet either way
constexpr size_t RX_DEPTH = 32;          // received packets that wait; more are dropped (counted)
constexpr uint64_t POLL_NS = 1000000;    // tick at least every 1 ms of emulated time
constexpr uint32_t BUS_NS = 560;         // a Zorro II cycle: what each access costs the CPU (VERIFY on a board)

enum : uint32_t {
    R_ID = 0x00, R_STATUS = 0x02, R_CTRL = 0x04, R_TX_DATA = 0x06, R_TX_SEND = 0x08,
    R_RX_LEN = 0x0A, R_RX_DATA = 0x0C, R_RX_DONE = 0x0E,
};
constexpr uint16_t ID = 0x5A45, ST_RX_AVAIL = 1, ST_LINK = 2, CTRL_IRQ_EN = 1;

const zbp_region regions[] = {
    {0x0000, BOARD_SIZE, ZBP_REGION_IO, "zbexample"},
};

const zbp_desc desc = {
    sizeof(zbp_desc), "zbexample", "Zorro2-Tang25k",
    0x5A32, 0x10,       // placeholder manufacturer ID (shared with Zorro2RTG's) and product 16 (VERIFY: a real one)
    0xC1, 0x00,         // Zorro II, 64 KB, not memory, no DiagArea
    1, 0,               // serial; no boot ROM
    BOARD_SIZE, 2, ZBP_DESC_NOT_RAM,
    sizeof(regions) / sizeof(regions[0]), regions,
    "zbexample",        // the peer in the daemon: plugins/zbexample/peer.py
};

constexpr size_t HOST_V1 = offsetof(zbp_host, log) + sizeof(void *);
constexpr size_t HOST_CHANNEL = offsetof(zbp_host, recv) + sizeof(void *);

}  // namespace

struct zbp_board {
    const zbp_host *host;
    void *ctx;
    bool configured = false;
    uint16_t ctrl = 0;
    bool link = false;                      // the peer's last 'L'
    std::vector<uint8_t> tx;                // the packet being written
    std::deque<std::vector<uint8_t>> rx;    // received packets, oldest first
    size_t rx_pos = 0;                      // the next byte of rx.front()
    std::vector<uint8_t> msg;               // a message from the peer
    unsigned rx_dropped = 0, tx_dropped = 0;

    bool channel() const
    {
        return host->struct_size >= HOST_CHANNEL && host->channel_up && host->send && host->recv &&
               host->channel_up(ctx);
    }
    void log(int level, const char *m) const
    {
        if (host->log)
            host->log(ctx, level, m);
    }
    void charge() const
    {
        if (host->charge_ns)
            host->charge_ns(ctx, BUS_NS);
    }
    uint16_t status() const { return static_cast<uint16_t>((rx.empty() ? 0 : ST_RX_AVAIL) | (link ? ST_LINK : 0)); }

    // messages from the peer: received packets and the device's state
    void receive()
    {
        if (!channel())
            return;
        msg.resize(ZBP_MSG_MAX);
        while (const uint32_t n = host->recv(ctx, msg.data(), ZBP_MSG_MAX)) {
            if (msg[0] == 'R' && n > 1 && n - 1 <= MTU) {
                if (rx.size() < RX_DEPTH)
                    rx.emplace_back(msg.begin() + 1, msg.begin() + n);
                else
                    rx_dropped++;
            } else if (msg[0] == 'L' && n == 2) {
                link = msg[1] != 0;
            }
        }
    }

    void send_packet()
    {
        if (tx.empty())
            return;
        std::vector<uint8_t> m(tx.size() + 1);
        m[0] = 'T';
        memcpy(m.data() + 1, tx.data(), tx.size());
        tx.clear();
        if (!channel() || !host->send(ctx, m.data(), static_cast<uint32_t>(m.size())))
            tx_dropped++;                   // no daemon: the packet is lost, as on a cable that is unplugged
    }
};

static zbp_board *p_create(const zbp_host *host, void *ctx, const char *)
{
    if (!host || host->struct_size < HOST_V1)
        return nullptr;
    auto *b = new (std::nothrow) zbp_board{};
    if (!b)
        return nullptr;
    b->host = host;
    b->ctx = ctx;
    return b;
}

static void p_destroy(zbp_board *b)
{
    if (b && (b->rx_dropped || b->tx_dropped)) {
        char m[96];
        snprintf(m, sizeof m, "zbexample: %u received packets dropped, %u sent packets lost", b->rx_dropped,
                 b->tx_dropped);
        b->log(ZBP_LOG_INFO, m);
    }
    delete b;
}

static void p_configured(zbp_board *b, uint32_t base, uint64_t)
{
    char m[64];
    snprintf(m, sizeof m, "zbexample configured at %08x%s", base, b->channel() ? ", peer connected" : "");
    b->log(ZBP_LOG_INFO, m);
    b->configured = true;
}

static void p_reset_board(zbp_board *b)
{
    b->configured = false;
    b->ctrl = 0;
    b->tx.clear();
    b->rx.clear();
    b->rx_pos = 0;
}

static uint32_t p_read(zbp_board *b, uint32_t off, int bytes, uint64_t)
{
    b->charge();
    uint16_t v = 0;
    switch (off & ~1u) {
    case R_ID: v = ID; break;
    case R_STATUS: v = b->status(); break;
    case R_CTRL: v = b->ctrl; break;
    case R_RX_LEN: v = b->rx.empty() ? 0 : static_cast<uint16_t>(b->rx.front().size()); break;
    case R_RX_DATA:
        if (!b->rx.empty() && b->rx_pos < b->rx.front().size())
            v = b->rx.front()[b->rx_pos++];
        break;
    default: break;
    }
    if (bytes == 1)
        return (off & 1) ? (v & 0xFF) : (v >> 8);
    return v;
}

static void p_write(zbp_board *b, uint32_t off, uint32_t value, int bytes, uint64_t)
{
    b->charge();
    // a byte write to the low byte carries the value; to the high byte it is ignored (a 68000 move.b to an
    // even address lands in bits 15:8)
    if (bytes == 1 && !(off & 1))
        return;
    const uint16_t v = static_cast<uint16_t>(value);
    switch (off & ~1u) {
    case R_CTRL: b->ctrl = v & CTRL_IRQ_EN; break;
    case R_TX_DATA:
        if (b->tx.size() < MTU)
            b->tx.push_back(static_cast<uint8_t>(v));
        break;
    case R_TX_SEND: b->send_packet(); break;
    case R_RX_DONE:
        if (!b->rx.empty())
            b->rx.pop_front();
        b->rx_pos = 0;
        break;
    default: break;
    }
}

static const uint8_t *p_ptr(zbp_board *, uint32_t, uint32_t) { return nullptr; }   // no RAM region

static uint32_t p_tick(zbp_board *b, uint64_t)
{
    if (!b->configured)
        return 0;
    b->receive();
    return (b->ctrl & CTRL_IRQ_EN) && !b->rx.empty() ? (1u << 2) : 0;   // /INT2, level-triggered
}

static int p_frame(zbp_board *, zbp_frame *) { return 0; }   // no display (ZBP_DESC_DISPLAY not set)

static const zbp_api api = {
    sizeof(zbp_api), ZBP_VERSION, &desc,
    p_create, p_destroy, p_configured, p_reset_board,
    p_read, p_write, p_ptr,
    static_cast<uint32_t>(POLL_NS), p_tick,
    p_frame,
};

extern "C" const zbp_api *zbp_get_api(uint32_t version)
{
    return version == ZBP_VERSION ? &api : nullptr;
}
