// picozorro: the real PicoZorro card in the loop. The emulated Amiga's bus cycles to the board go, through the
// zboard daemon and this plugin's peer (peer.py), over a UART to an RP2350 running the PicoZorro firmware built
// with `uart-backplane` (PicoZorro: firmware/pz-hal/src/backplane.rs, pz-core/src/backplane.rs). The card's
// whole register window (Autoconfig slave, network, USB, MPEG, update and boot ROM) is the firmware's own, not a
// model in here: the Amiga drivers, the boot ROM and pzflash run unchanged against the real card.
//
// This plugin is a bus proxy, the one exception to "reads never wait" (docs/writing-a-plugin.md, section 4):
//   - Writes are posted. They collect into a batch of backplane ops (WRITE, WRITE_N for runs to one register,
//     WINDOW for A16) and leave at the next read, at the next tick, or when the batch is full. Word writes to
//     the $C0-$FE data alias go as TX_DATA writes (the same thing by docs/REGISTERS.md), so a frame the driver
//     sends with movem.l bursts through the alias is one WRITE_N run, not a WRITE per word.
//   - A read joins the batch, the batch leaves and the plugin waits for the card's reply. To the emulated CPU
//     this is a long XRDY wait state: emulated time does not move, wall-clock time does (a round trip over a
//     USB-serial probe is about 1 ms). Code that polls the card runs slowly but correctly.
//   - Received frames are the exception to one read, one round trip: the first RX_DATA (or $C0-$FE alias) read
//     after RX_LEN fetches the whole head frame with one READ_N, and the frame's other reads are answered from
//     it. That is exact (docs/REGISTERS.md, "Receiving a frame"): RX_LEN holds until RX_DONE, RX_DATA reads
//     only advance through the frame, and RX_DONE drops what was not read. Without it a full frame costs about
//     760 round trips, and a LAN's broadcast traffic alone keeps the emulated Amiga busy.
//   - /INT arrives as notification datagrams; tick() returns /INT2 (or /INT6, PICOZORRO_INT=6) while the card
//     asserts it. The card sends a note at each change and repeats the asserted level every 100 ms (INT_REPEAT,
//     pz-hal/src/backplane.rs), never the released one. A note lost on the UART is not retried, so a lost
//     release would hold /INT (the Amiga then runs its interrupt servers on every line, each reading the card):
//     with /INT held and no note for INT_STALE_MS, the level must have dropped, and the plugin releases it. A
//     level still asserted comes back with the next repeat.
//   - Autoconfig is the host's, from the description below (the card's own ROM: 2011/$5A, 128 KB). At
//     configured() the plugin puts the card through a reset, reads its Autoconfig ROM (logged and compared)
//     and writes the base, as expansion.library would: the card is then configured at the same address.
//   - Without a channel (no daemon) or after the card stopped answering, reads return $FFFF at once.
//
// Board layout: 128 KB, A16 = 0 the register window (docs/REGISTERS.md), A16 = 1 the USB / MPEG / update
// window. The card decodes A7-A1 and A16 only, so offset & $FF is the register, the rest of each half aliases.
//
// Messages, plugin -> peer: 'D' + a backplane request datagram (header with seq, every one asks for a reply).
//           peer -> plugin: 'D' + a datagram from the card (a reply, or an /INT notification);
//                           'X' + seq (2 bytes BE): the peer gave up on that request (no reply after retries);
//                           'L' + 1 byte: the UART is open and answering (1) or lost (0).
//
// Environment (of Amiberry): PICOZORRO_BOOTROM=0 (no DiagArea: for a card without a boot image),
// PICOZORRO_INT=2|6, PICOZORRO_TIMEOUT_MS (a read's backstop wait, default 5000), PICOZORRO_RX_PREFETCH=0 (read
// received frames word by word, one round trip each, instead of the whole frame at the first RX_DATA read).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "zboard_plugin.h"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>
#include <vector>

namespace {

constexpr uint32_t BOARD_SIZE = 0x20000;
constexpr uint32_t BUS_NS = 560;          // a Zorro II cycle, charged per access; the real wait is wall-clock

// pz-core/src/backplane.rs
constexpr uint16_t BP_MAGIC = 0x5042;
constexpr uint8_t BP_VERSION = 1, F_REPLY = 0x01, F_IS_REPLY = 0x80, F_INT = 0x40;
constexpr uint8_t OP_WRITE = 1, OP_READ = 2, OP_WRITE_N = 3, OP_READ_N = 4, OP_RESET = 5, OP_WINDOW = 6;
constexpr uint8_t ST_OK = 0;
constexpr uint8_t S_UDS = 2, S_LDS = 1, S_WORD = 3;
constexpr size_t HDR = 6, REPLY_HDR = 8;
constexpr size_t BATCH_MAX = 1800;        // ops bytes per datagram (DGRAM_MAX 2048, room for the header)
constexpr uint16_t WRITE_N_MAX = 512;     // words in one WRITE_N

// docs/REGISTERS.md, the network window (A16 = 0): receiving a frame
constexpr uint8_t R_TX_DATA = 0x22;
constexpr uint8_t R_RX_LEN = 0x30, R_RX_DATA = 0x32, R_RX_DONE = 0x34, R_ALIAS = 0xc0;   // alias $C0-$FE
constexpr uint16_t RX_FRAME_MAX = 1514;

// the card repeats an asserted /INT every 100 ms: no note for this long while held means the release was lost
constexpr uint64_t INT_STALE_NS = 250000000ull;

// pz-core/src/autoconfig.rs
constexpr uint16_t MANUFACTURER = 2011;
constexpr uint8_t PRODUCT = 0x5A, ER_TYPE = 0xC2, ERTF_DIAGVALID = 0x10;
constexpr uint8_t AC_BASE_HI = 0x48, AC_BASE_LO = 0x4A;

const zbp_region regions[] = {
    {0x00000, 0x10000, ZBP_REGION_IO, "PicoZorro"},
    {0x10000, 0x10000, ZBP_REGION_IO, "PicoZorro A16"},
};

// not const: zbp_get_api sets DIAGVALID from PICOZORRO_BOOTROM, configured() follows the card's ROM
zbp_desc desc = {
    sizeof(zbp_desc), "PicoZorro", "PicoZorro",
    MANUFACTURER, PRODUCT,
    ER_TYPE | ERTF_DIAGVALID, 0x00,     // Zorro II, 128 KB, I/O; the boot ROM's DiagArea at $0100
    31337, 0x0100,
    BOARD_SIZE, 2, ZBP_DESC_NOT_RAM,
    sizeof(regions) / sizeof(regions[0]), regions,
    "picozorro",                        // plugins/picozorro/peer.py
};

constexpr size_t HOST_V1 = offsetof(zbp_host, log) + sizeof(void *);
constexpr size_t HOST_CHANNEL = offsetof(zbp_host, recv) + sizeof(void *);

uint64_t mono_ns()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

void pause_us(long us)
{
    timespec ts {0, us * 1000};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

long env_long(const char *name, long dflt)
{
    const char *s = getenv(name);
    if (!s || !*s)
        return dflt;
    char *end = nullptr;
    const long v = strtol(s, &end, 0);
    return end && !*end ? v : dflt;
}

}  // namespace

struct zbp_board {
    const zbp_host *host;
    void *ctx;
    bool configured = false;
    bool lost = false;                  // the card stopped answering: no waits until the peer says 'L' 1
    bool irq = false;                   // the card's last /INT notification
    uint32_t int_bit = 1u << 2;
    uint64_t timeout_ns = 5000000000ull;
    uint16_t seq = 0;

    // the batch being built: ops only, the header is added when it leaves
    std::vector<uint8_t> ops;
    bool a16 = false;                   // A16 the batch's ops are at (each request starts at 0)
    size_t last = SIZE_MAX;             // offset in ops of the last write op, for WRITE_N runs

    std::vector<uint8_t> msg;           // a message from the peer
    std::vector<uint8_t> reply;         // the awaited reply

    // RX prefetch: the head frame's words, read from the card in one READ_N and handed out one RX_DATA read at
    // a time (docs/REGISTERS.md, "Receiving a frame": RX_LEN holds until RX_DONE, RX_DATA reads only advance
    // through the frame, reads past its end give $0000). Words in rx_words are already off the card.
    uint16_t rx_len = 0;                // RX_LEN as last read
    bool rx_len_valid = false;          // read since the last RX_DONE
    bool rx_fetched = false;            // this frame was prefetched
    bool rx_prefetch_on = true;         // PICOZORRO_RX_PREFETCH
    std::vector<uint16_t> rx_words;
    size_t rx_pos = 0;

    // statistics, logged at destroy
    uint64_t reads = 0, writes = 0, datagrams = 0, waits_ns = 0, wait_max_ns = 0;
    uint64_t rx_frames = 0, rx_local = 0;
    unsigned timeouts = 0, gave_up = 0, bad_status = 0, notes = 0, int_stale = 0;
    uint64_t last_note_ns = 0;          // when the last /INT note came

    void rx_forget()
    {
        rx_len_valid = false;
        rx_fetched = false;
        rx_words.clear();
        rx_pos = 0;
    }

    static bool is_rx_data(uint8_t reg) { return reg == R_RX_DATA || reg >= R_ALIAS; }

    // the head frame from the card in one request, the first word returned; false: not fetched (read normally)
    bool rx_prefetch(uint16_t &first)
    {
        if (!rx_prefetch_on || !rx_len_valid || rx_fetched || rx_len == 0 || rx_len > RX_FRAME_MAX)
            return false;
        rx_fetched = true;
        const uint16_t n = static_cast<uint16_t>((rx_len + 1) / 2);
        window(false);
        const uint8_t op[] = {OP_READ_N, R_RX_DATA, uint8_t(n >> 8), uint8_t(n)};
        ops.insert(ops.end(), op, op + sizeof op);
        last = SIZE_MAX;
        first = 0xffff;
        if (!transact() || reply.size() < REPLY_HDR + 2u * n)
            return true;                // lost or refused: $FFFF, as a plain read would give
        rx_words.resize(n);
        for (uint16_t i = 0; i < n; i++)
            rx_words[i] = static_cast<uint16_t>(reply[REPLY_HDR + 2 * i] << 8 | reply[REPLY_HDR + 2 * i + 1]);
        rx_pos = 1;
        rx_frames++;
        first = rx_words[0];
        return true;
    }

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

    void window(bool hi)
    {
        if (hi != a16) {
            ops.push_back(OP_WINDOW);
            ops.push_back(hi ? 1 : 0);
            a16 = hi;
            last = SIZE_MAX;
        }
    }

    void put_write(bool hi, uint8_t reg, uint16_t data, uint8_t strobes)
    {
        window(hi);
        // a word write to the register of the write before: one WRITE_N for the run (FIFOs: TX_DATA, BOOT,
        // the USB request records)
        if (strobes == S_WORD && last != SIZE_MAX && ops[last + 1] == reg) {
            if (ops[last] == OP_WRITE && ops[last + 2] == S_WORD) {
                const uint8_t d0 = ops[last + 3], d1 = ops[last + 4];
                ops.resize(last);
                const uint8_t run[] = {OP_WRITE_N, reg, 0, 2, d0, d1, uint8_t(data >> 8), uint8_t(data)};
                ops.insert(ops.end(), run, run + sizeof run);
                return;
            }
            if (ops[last] == OP_WRITE_N) {
                const uint16_t n = static_cast<uint16_t>(ops[last + 2] << 8 | ops[last + 3]);
                if (n < WRITE_N_MAX) {
                    ops[last + 2] = uint8_t((n + 1) >> 8);
                    ops[last + 3] = uint8_t(n + 1);
                    ops.push_back(uint8_t(data >> 8));
                    ops.push_back(uint8_t(data));
                    return;
                }
            }
        }
        last = ops.size();
        const uint8_t op[] = {OP_WRITE, reg, strobes, uint8_t(data >> 8), uint8_t(data)};
        ops.insert(ops.end(), op, op + sizeof op);
    }

    // the batch as one request datagram to the peer; returns its seq, or -1 when nothing left
    int flush()
    {
        if (ops.empty())
            return -1;
        const uint16_t s = ++seq;
        std::vector<uint8_t> m;
        m.reserve(1 + HDR + ops.size());
        const uint8_t hdr[] = {'D', uint8_t(BP_MAGIC >> 8), uint8_t(BP_MAGIC), BP_VERSION, F_REPLY,
                               uint8_t(s >> 8), uint8_t(s)};
        m.insert(m.end(), hdr, hdr + sizeof hdr);
        m.insert(m.end(), ops.begin(), ops.end());
        ops.clear();
        a16 = false;
        last = SIZE_MAX;
        if (!channel() || !host->send(ctx, m.data(), static_cast<uint32_t>(m.size())))
            return -1;
        datagrams++;
        return s;
    }

    // one message from the peer; true when it is the reply to `want` (copied into reply) or its give-up
    bool handle(const uint8_t *p, uint32_t n, int want, bool &ok)
    {
        if (p[0] == 'L' && n == 2) {
            if (p[1] && lost) {
                lost = false;
                log(ZBP_LOG_INFO, "picozorro: the card answers again (reset the Amiga to bring it back in step)");
            }
            return false;
        }
        if (p[0] == 'X' && n == 3) {
            gave_up++;
            const int s = p[1] << 8 | p[2];
            if (s == want) {
                ok = false;
                return true;
            }
            return false;
        }
        if (p[0] != 'D' || n < 1 + 7)
            return false;
        const uint8_t *d = p + 1;
        const uint32_t len = n - 1;
        if ((d[0] << 8 | d[1]) != BP_MAGIC || d[2] != BP_VERSION)
            return false;
        if (d[3] & F_INT) {
            irq = d[6] != 0;
            last_note_ns = mono_ns();
            notes++;
            return false;
        }
        if (!(d[3] & F_IS_REPLY) || len < REPLY_HDR)
            return false;
        if (d[6] != ST_OK)
            bad_status++;
        if ((d[4] << 8 | d[5]) != want)
            return false;                  // a posted batch's reply
        reply.assign(d, d + len);
        ok = d[6] == ST_OK;
        return true;
    }

    // take the peer's messages; with want >= 0 wait for that request's reply
    bool pump(int want)
    {
        if (!channel())
            return false;
        if (msg.size() < ZBP_MSG_MAX)
            msg.resize(ZBP_MSG_MAX);
        const uint64_t start = want >= 0 ? mono_ns() : 0;
        unsigned idle = 0;
        for (;;) {
            const uint32_t n = host->recv(ctx, msg.data(), ZBP_MSG_MAX);
            if (n) {
                idle = 0;
                bool ok = false;
                if (handle(msg.data(), n, want, ok) && want >= 0) {
                    const uint64_t w = mono_ns() - start;
                    waits_ns += w;
                    if (w > wait_max_ns)
                        wait_max_ns = w;
                    return ok;
                }
                continue;
            }
            if (want < 0)
                return false;
            if (mono_ns() - start > timeout_ns || !channel()) {
                timeouts++;
                lost = true;
                log(ZBP_LOG_ERROR, "picozorro: no reply from the card; reads give $FFFF until it answers");
                return false;
            }
            if (++idle > 50)               // spin briefly (a reply can be 100 us away), then sleep
                pause_us(20);
        }
    }

    // an awaited request: the batch plus the reads in it; the reply's data, or none
    bool transact()
    {
        const int s = flush();
        if (s < 0)
            return false;
        return pump(s) && reply.size() >= REPLY_HDR;
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
    b->int_bit = env_long("PICOZORRO_INT", 2) == 6 ? 1u << 6 : 1u << 2;
    b->timeout_ns = static_cast<uint64_t>(env_long("PICOZORRO_TIMEOUT_MS", 5000)) * 1000000ull;
    b->rx_prefetch_on = env_long("PICOZORRO_RX_PREFETCH", 1) != 0;
    if (host->struct_size < HOST_CHANNEL)
        b->log(ZBP_LOG_WARN, "picozorro: this host has no daemon channel; the card cannot be reached");
    return b;
}

static void p_destroy(zbp_board *b)
{
    if (!b)
        return;
    b->flush();
    char m[512];
    snprintf(m, sizeof m,
             "picozorro: %llu reads, %llu writes in %llu datagrams; waits %.1f ms total, %.2f ms longest, "
             "%.3f ms a read; %llu RX frames prefetched (%llu reads answered from them); %u timeouts, %u given up "
             "by the peer, %u error replies, %u /INT notes, %u /INT releases presumed lost",
             (unsigned long long)b->reads, (unsigned long long)b->writes, (unsigned long long)b->datagrams,
             b->waits_ns / 1e6, b->wait_max_ns / 1e6, b->reads ? b->waits_ns / 1e6 / b->reads : 0.0,
             (unsigned long long)b->rx_frames, (unsigned long long)b->rx_local, b->timeouts, b->gave_up,
             b->bad_status, b->notes, b->int_stale);
    b->log(ZBP_LOG_INFO, m);
    delete b;
}

static void p_configured(zbp_board *b, uint32_t base, uint64_t)
{
    b->configured = true;
    b->irq = false;
    b->rx_forget();
    char m[200];
    if (!b->channel()) {
        snprintf(m, sizeof m, "picozorro configured at %08x without the daemon: the card reads $FFFF", base);
        b->log(ZBP_LOG_WARN, m);
        return;
    }
    b->lost = false;
    b->pump(-1);                           // what came before (notes of a previous run)
    b->flush();
    // the card from power-on: reset, its Autoconfig ROM (bytes 0..11, two nibbles each, on D15-D12), the base
    // as expansion.library writes it: (byte << 4) to $4A, then the byte to $48, /UDS only
    b->ops.push_back(OP_RESET);
    for (uint8_t reg = 0; reg < 0x30; reg += 2) {
        const uint8_t op[] = {OP_READ, reg, S_UDS};
        b->ops.insert(b->ops.end(), op, op + sizeof op);
    }
    const uint8_t page = static_cast<uint8_t>(base >> 16);
    b->put_write(false, AC_BASE_LO, static_cast<uint16_t>(uint8_t(page << 4) << 8), S_UDS);
    b->last = SIZE_MAX;
    b->put_write(false, AC_BASE_HI, static_cast<uint16_t>(page << 8), S_UDS);
    if (!b->transact() || b->reply.size() < REPLY_HDR + 48) {
        snprintf(m, sizeof m, "picozorro configured at %08x: the card did not answer (peer log?)", base);
        b->log(ZBP_LOG_ERROR, m);
        return;
    }
    uint8_t rom[12];
    for (int i = 0; i < 12; i++) {
        const uint8_t hi = b->reply[REPLY_HDR + 4 * i] >> 4, lo = b->reply[REPLY_HDR + 4 * i + 2] >> 4;
        rom[i] = static_cast<uint8_t>(i == 0 ? (hi << 4 | lo) : (~(hi << 4 | lo) & 0xff));   // inverted but er_Type
    }
    const uint16_t man = static_cast<uint16_t>(rom[4] << 8 | rom[5]);
    const uint32_t serial = uint32_t(rom[6]) << 24 | uint32_t(rom[7]) << 16 | uint32_t(rom[8]) << 8 | rom[9];
    const bool card_rom = rom[0] & ERTF_DIAGVALID;
    snprintf(m, sizeof m, "picozorro configured at %08x: the card says %u/$%02X, er_Type $%02X, serial %u, %s",
             base, man, rom[1], rom[0], serial, card_rom ? "boot ROM" : "no boot ROM");
    b->log(ZBP_LOG_INFO, m);
    if (man != MANUFACTURER || rom[1] != PRODUCT)
        b->log(ZBP_LOG_WARN, "picozorro: that is not the PicoZorro's Autoconfig ID: is the firmware uart-backplane?");
    if (card_rom != bool(desc.er_type & ERTF_DIAGVALID)) {
        desc.er_type = static_cast<uint8_t>(card_rom ? (desc.er_type | ERTF_DIAGVALID) : (desc.er_type & ~ERTF_DIAGVALID));
        b->log(ZBP_LOG_WARN, card_rom ? "picozorro: the card has a boot ROM; offered from the next reset"
                                      : "picozorro: the card has no boot ROM; set PICOZORRO_BOOTROM=0 (DiagArea "
                                        "dropped from the next reset)");
    }
}

static void p_reset_board(zbp_board *b)
{
    b->configured = false;
    b->irq = false;
    b->rx_forget();
    if (b->channel() && !b->lost) {
        b->ops.push_back(OP_RESET);       // posted, after the writes before it
        b->flush();
    } else {
        b->ops.clear();
        b->a16 = false;
        b->last = SIZE_MAX;
    }
}

static uint32_t p_read(zbp_board *b, uint32_t off, int bytes, uint64_t)
{
    b->charge();
    b->reads++;
    if (!b->configured || b->lost || !b->channel())
        return bytes == 1 ? 0xff : 0xffff;
    const bool hi = (off >> 16) & 1;
    const uint8_t reg = static_cast<uint8_t>(off & 0xfe);
    const uint8_t strobes = bytes == 2 ? S_WORD : (off & 1) ? S_LDS : S_UDS;
    // a word read of the RX data port: the next word of a prefetched frame, or the whole frame now
    if (bytes == 2 && !hi && zbp_board::is_rx_data(reg)) {
        if (b->rx_pos < b->rx_words.size()) {
            b->rx_local++;
            return b->rx_words[b->rx_pos++];
        }
        uint16_t first;
        if (b->rx_prefetch(first))
            return first;
    }
    b->window(hi);
    const uint8_t op[] = {OP_READ, reg, strobes};
    b->ops.insert(b->ops.end(), op, op + sizeof op);
    b->last = SIZE_MAX;
    uint16_t v = 0xffff;
    if (b->transact() && b->reply.size() >= REPLY_HDR + 2) {
        v = static_cast<uint16_t>(b->reply[REPLY_HDR] << 8 | b->reply[REPLY_HDR + 1]);
        if (bytes == 2 && !hi && reg == R_RX_LEN) {
            b->rx_len = v;
            b->rx_len_valid = true;
        }
    }
    if (bytes == 1)
        return (off & 1) ? (v & 0xff) : (v >> 8);
    return v;
}

static void p_write(zbp_board *b, uint32_t off, uint32_t value, int bytes, uint64_t)
{
    b->charge();
    b->writes++;
    if (!b->configured || b->lost || !b->channel())
        return;
    const bool hi = (off >> 16) & 1;
    uint8_t reg = static_cast<uint8_t>(off & 0xfe);
    // a word write anywhere in the $C0-$FE alias is a TX_DATA write (docs/REGISTERS.md, "Data port alias"):
    // sent as one, a movem.l frame through the alias becomes one WRITE_N run, 2 bytes a word on the UART
    // instead of a 5-byte WRITE per word to a different offset each time
    if (!hi && bytes == 2 && reg >= R_ALIAS)
        reg = R_TX_DATA;
    uint16_t data;
    uint8_t strobes;
    if (bytes == 2) {
        data = static_cast<uint16_t>(value);
        strobes = S_WORD;
    } else if (off & 1) {
        data = static_cast<uint16_t>(value & 0xff);              // the odd byte: D7-D0, /LDS
        strobes = S_LDS;
    } else {
        data = static_cast<uint16_t>((value & 0xff) << 8);       // the even byte: D15-D8, /UDS
        strobes = S_UDS;
    }
    if (!hi && reg == R_RX_DONE)
        b->rx_forget();                    // the card drops the head frame, read or not
    b->put_write(hi, reg, data, strobes);
    if (b->ops.size() >= BATCH_MAX)
        b->flush();
}

static const uint8_t *p_ptr(zbp_board *, uint32_t, uint32_t) { return nullptr; }   // I/O only

static uint32_t p_tick(zbp_board *b, uint64_t)
{
    if (!b->configured)
        return 0;
    b->flush();                            // posted writes leave at least once a line
    b->pump(-1);
    if (b->irq && mono_ns() - b->last_note_ns > INT_STALE_NS) {
        b->irq = false;                    // the card's release note was lost (see the top of this file)
        b->int_stale++;
    }
    return b->irq ? b->int_bit : 0;
}

static int p_frame(zbp_board *, zbp_frame *) { return 0; }

static const zbp_api api = {
    sizeof(zbp_api), ZBP_VERSION, &desc,
    p_create, p_destroy, p_configured, p_reset_board,
    p_read, p_write, p_ptr,
    64000, p_tick,
    p_frame,
};

extern "C" const zbp_api *zbp_get_api(uint32_t version)
{
    if (version != ZBP_VERSION)
        return nullptr;
    if (env_long("PICOZORRO_BOOTROM", 1) == 0)
        desc.er_type = static_cast<uint8_t>(desc.er_type & ~ERTF_DIAGVALID);
    return &api;
}
