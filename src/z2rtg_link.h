// Board-in-the-loop link: mirrors the emulated board's writes to a real Nano 20K through the UART debug
// bridge (docs/debug_bridge.md, docs/driver.md in the Zorro2-Tang25k project). The real gateware then shows what the emulated Amiga
// draws on its own HDMI output. Emulator-independent, POSIX.
//   - One queue, so the real board sees the writes in the model's order. Consecutive word offsets are
//     sent as bursts of up to 256 words, with up to four bursts in flight.
//   - wait_blitter() puts a marker in the queue: the link polls the real STATUS until BLT_BUSY clears
//     before it sends anything after it. The emulated CPU waits on the model's BLT_BUSY, which the real
//     board never sees, so a long job would otherwise still be running when the next writes arrive.
//   - Reads are never forwarded: the model answers them.
//   - enable_input(): the real board's USB input queue (IN_*, docs/usb_input.md) becomes the source of the
//     model's events. About every 5 ms, also between the bursts of a long redraw, the link reads IN_STATUS and IN_EV0-3 and pops each
//     event on the real board; take_event() hands them to the emulator. Writes to IN_* are then not
//     forwarded, so the emulated driver's IN_POP never pops a real event.
//   - A burst the bridge refuses ('e', for example after a pause on the USB side) is sent again, with
//     every burst after it, once the bridge has resynchronised. Word writes can be repeated safely; a
//     repeated BLT_OP would run its job twice, which is logged.
//   - A port that goes away (the module dropped off USB and came back under another name) is logged once
//     and the link stops: is_open() turns false and nothing more is sent. The real board then keeps its last
//     picture. The next Autoconfig (a reset of the emulated Amiga) opens the port again, and the reboot
//     redraws everything; start.sh names the port by its /dev/serial/by-id link, which follows the module.
//   - Transport: the serial port itself, or ("unix:PATH" or a socket path) the Unix socket of z2bridged.py,
//     which carries the same protocol and shares the board with other programs (README.md). A socket the
//     daemon closes counts as a lost port.
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef Z2RTG_LINK_H
#define Z2RTG_LINK_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <array>
#include <thread>
#include <string>
#include <vector>

class Z2rtgLink {
public:
    ~Z2rtgLink();
    // opens the serial port (raw 8N1), or with "unix:PATH" or a socket path z2bridged's Unix socket, which
    // carries the same protocol; then pings the bridge. log may be null
    bool open(const char *device, int baud, FILE *log);
    bool is_open() const { return fd >= 0 && !lost; }
    void write(uint32_t offset, uint16_t value);       // a word at a board offset (even)
    void wait_blitter();                               // the real BLT_BUSY must clear before what follows
    void enable_input() { input = true; }              // poll the real board's USB input queue
    bool take_event(std::array<uint8_t, 7> &ev);       // the next event read from it, if any
    void close();

    // statistics (read without the lock: approximate)
    uint64_t words = 0, bursts = 0, polls = 0, errors = 0, resent = 0, events = 0, in_overflows = 0, in_frame_errs = 0;
    size_t queue_max = 0;

private:
    struct Item { uint32_t off; uint16_t v; bool wait; };
    int fd = -1;
    bool sock = false;                           // fd is a Unix socket to z2bridged, not the serial port
    std::atomic<bool> lost {false};              // the port failed: stop sending
    std::string dev;
    FILE *log = nullptr;
    std::thread th;
    std::mutex mx;
    std::condition_variable cv;
    std::deque<Item> q;
    bool stop = false;
    std::atomic<bool> input {false};
    std::deque<std::array<uint8_t, 7>> evq;      // events from the real board, under mx
    std::deque<std::vector<uint8_t>> inflight;   // bursts sent and not yet acknowledged, oldest first

    void run();
    void port_lost(const char *what);
    void flush_input();
    bool send(const uint8_t *p, size_t n);
    int recv(uint8_t *p, size_t n, int timeout_ms);
    bool ack();
    bool drain();
    bool recover();
    bool burst(uint32_t off, const uint16_t *w, unsigned n);
    bool blitter_idle();
    void poll_input();
    void maybe_poll_input();                     // poll_input() if 5 ms have passed since the last
    std::chrono::steady_clock::time_point next_input {};
};

#endif
