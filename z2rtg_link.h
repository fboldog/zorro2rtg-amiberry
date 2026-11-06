// Board-in-the-loop link: mirrors the emulated board's writes to a real Nano 20K through the UART debug
// bridge (docs/debug_bridge.md, docs/driver.md). The real gateware then shows what the emulated Amiga
// draws on its own HDMI output. Emulator-independent, POSIX.
//   - One queue, so the real board sees the writes in the model's order. Consecutive word offsets are
//     sent as bursts of up to 256 words, with up to four bursts in flight.
//   - wait_blitter() puts a marker in the queue: the link polls the real STATUS until BLT_BUSY clears
//     before it sends anything after it. The emulated CPU waits on the model's BLT_BUSY, which the real
//     board never sees, so a long job would otherwise still be running when the next writes arrive.
//   - Reads are never forwarded: the model answers them.
//   - A burst the bridge refuses ('e', for example after a pause on the USB side) is sent again, with
//     every burst after it, once the bridge has resynchronised. Word writes can be repeated safely; a
//     repeated BLT_OP would run its job twice, which is logged.
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef Z2RTG_LINK_H
#define Z2RTG_LINK_H

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

class Z2rtgLink {
public:
    ~Z2rtgLink();
    // opens the serial port (raw 8N1) and pings the bridge; log may be null
    bool open(const char *device, int baud, FILE *log);
    bool is_open() const { return fd >= 0; }
    void write(uint32_t offset, uint16_t value);       // a word at a board offset (even)
    void wait_blitter();                               // the real BLT_BUSY must clear before what follows
    void close();

    // statistics (read without the lock: approximate)
    uint64_t words = 0, bursts = 0, polls = 0, errors = 0, resent = 0;
    size_t queue_max = 0;

private:
    struct Item { uint32_t off; uint16_t v; bool wait; };
    int fd = -1;
    FILE *log = nullptr;
    std::thread th;
    std::mutex mx;
    std::condition_variable cv;
    std::deque<Item> q;
    bool stop = false;
    std::deque<std::vector<uint8_t>> inflight;   // bursts sent and not yet acknowledged, oldest first

    void run();
    bool send(const uint8_t *p, size_t n);
    int recv(uint8_t *p, size_t n, int timeout_ms);
    bool ack();
    bool drain();
    bool recover();
    bool burst(uint32_t off, const uint16_t *w, unsigned n);
    bool blitter_idle();
};

#endif
