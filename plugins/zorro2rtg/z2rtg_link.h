// Board in the loop, the plugin's half: the emulated board's writes as messages for its peer in the daemon
// (peer.py next to this file), which sends them to a real Nano 20K over the UART debug bridge
// (docs/debug_bridge.md, docs/driver.md in the Zorro2-Tang25k project). The real gateware then shows what
// the emulated Amiga draws on its own HDMI output. The plugin never opens a port: the messages go through
// the host's channel (zbp_host.send, include/zboard_plugin.h). No thread, no I/O; emulator-independent.
//   - Register writes (below VRAM) keep their order in a queue; consecutive offsets become one run.
//   - VRAM writes only mark their word as changed. A flush sends the model's current value of each changed
//     word, in runs, so a word written many times costs one write, and a CPU loop that rewrites a place
//     costs nothing extra.
//   - The peer acknowledges each message once it has passed it to the bridge ('A'). Changed VRAM goes out
//     only while at most MAX_UNACKED messages are unacknowledged: when the real board falls behind (a
//     window outline drawn by the CPU, one word per row), the changes merge here, the emulated Amiga never
//     waits, and the real board skips the intermediate pictures but ends exact.
//   - Before a blitter start (BLT_OP) or a BANK write reaches the model, flush(true) sends every change
//     regardless: the model's blitter finishes at once, so the values after it would already hold the
//     job's result, and BANK changes what an offset means. Registers go before the changed VRAM in a
//     flush, so a job started earlier runs before the writes that follow it.
//   - The pointer's position (CURSOR_X, CURSOR_Y) is not queued: only its latest value matters and it only
//     changes what the monitor shows, so each message carries it as one 'P' record, which the peer sends
//     ahead of the drawing it still has queued. The pointer on the real monitor then keeps up while the
//     real board is busy drawing (a directory listing scrolling).
//   - wait_blitter() puts a barrier in the stream: the peer polls the real STATUS until BLT_BUSY clears
//     before it sends anything after it. The emulated CPU waits on the model's BLT_BUSY, which the real
//     board never sees. The real bridge holds its writes while a job runs (so they land after it), but it
//     buffers only 64 bytes meanwhile, so a long job gets a barrier.
//   - Reads are never forwarded: the model answers them.
//   - With input on, the peer polls the real board's USB input queue (IN_*, docs/usb_input.md) and sends
//     each event back. Writes to IN_* are then not forwarded, so the emulated driver's IN_POP never pops a
//     real event.
//
// Messages, plugin -> peer: a sequence of records, up to MSG_MAX bytes in all
//   'Z' e                                           a new stream (epoch e): count acknowledgements from 0
//   'W' a2 a1 a0 nH nL  then n big-endian words     n consecutive words from board offset a (n <= RUN_MAX)
//   'B'                                             blitter barrier
//   'I'                                             poll the real board's input queue from now on
//   'P' xH xL yH yL                                 the pointer's position now (CURSOR_X, CURSOR_Y)
// peer -> plugin: one record per message
//   'E' t p0 p1 p2 p3 p4 p5                         an input event (IN_EV0-3: type, then six bytes)
//   'A' e n3 n2 n1 n0                               n messages of epoch e passed to the bridge so far
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef Z2RTG_LINK_H
#define Z2RTG_LINK_H

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

class Z2rtgLink {
public:
    using SendFn = std::function<bool(const uint8_t *msg, uint32_t len)>;
    using ReadFn = std::function<uint16_t(uint32_t offset)>;   // the model's VRAM word at a board offset

    // the channel is up: from now on the stream goes to send; read gives the changed words' values;
    // input: poll the real board's input queue
    void start(SendFn send, ReadFn read, bool input);
    void stop();                                       // the channel is gone: queued writes are dropped
    bool active() const { return static_cast<bool>(out); }
    void write(uint32_t offset, uint16_t value);       // a word at a board offset (even); VRAM: value unused
    void wait_blitter();                               // the real BLT_BUSY must clear before what follows
    // queued registers into messages, and the changed VRAM when force or the peer has room
    void flush(bool force = false);
    // a message from the peer: an acknowledgement is taken here; true and ev filled for an input event
    bool receive(const uint8_t *msg, uint32_t len, std::array<uint8_t, 7> &ev);
    bool room() const { return sent - acked <= MAX_UNACKED; }

    static constexpr uint32_t MSG_MAX = 32768;         // bytes per message (ZBP_MSG_MAX is the limit)
    static constexpr uint32_t RUN_MAX = 4096;          // words per 'W' record
    static constexpr size_t FLUSH_AT = 65536;          // queued register writes that flush without waiting
    static constexpr uint32_t MAX_UNACKED = 2;         // messages the peer may hold before VRAM waits

    uint64_t words = 0, messages = 0, barriers = 0, held = 0;   // statistics; held: flushes VRAM waited

private:
    struct Item { uint32_t off; uint16_t v; bool wait; };
    SendFn out;
    ReadFn rd;
    bool input = false;
    uint8_t epoch = 0;
    uint16_t ptr_x = 0, ptr_y = 0;                     // CURSOR_X and CURSOR_Y, latest
    bool ptr_new = false;                              // not sent yet
    uint32_t sent = 0, acked = 0;                      // messages of this epoch
    std::vector<Item> q;                               // registers and barriers, in order
    std::vector<uint64_t> dirty;                       // one bit per VRAM word
    std::vector<uint32_t> groups;                      // indices into dirty that have a bit set
    std::vector<uint8_t> msg;
    std::vector<uint16_t> run;

    void put_run(uint32_t off, const uint16_t *w, uint32_t n);
    void put(uint8_t rec);
    void send_msg();
    void put_vram();
};

#endif
