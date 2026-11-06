// Board-in-the-loop link to a real Nano 20K (z2rtg_link.h).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "z2rtg_link.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

#include "zorro2rtg_regs.h"

static constexpr unsigned BURST = 256;       // words per bridge burst
static constexpr int WINDOW = 4;             // bursts in flight (docs/debug_bridge.md)

Z2rtgLink::~Z2rtgLink() { close(); }

static speed_t baud_const(int baud)
{
    switch (baud) {
    case 115200: return B115200;
    case 1000000: return B1000000;
    case 2000000: return B2000000;
    default: return B3000000;
    }
}

bool Z2rtgLink::open(const char *device, int baud, FILE *lg)
{
    log = lg;
    fd = ::open(device, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        if (log) fprintf(log, "z2rtg link: %s: %s\n", device, strerror(errno));
        return false;
    }
    termios t {};
    tcgetattr(fd, &t);
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    cfsetispeed(&t, baud_const(baud));
    cfsetospeed(&t, baud_const(baud));
    tcsetattr(fd, TCSANOW, &t);
    tcflush(fd, TCIOFLUSH);
    uint8_t c = '?', r = 0;
    if (!send(&c, 1) || recv(&r, 1, 500) != 1 || r != 'Z') {
        if (log) fprintf(log, "z2rtg link: no bridge ping on %s at %d baud\n", device, baud);
        ::close(fd);
        fd = -1;
        return false;
    }
    if (log) fprintf(log, "z2rtg link: bridge on %s at %d baud\n", device, baud);
    stop = false;
    th = std::thread(&Z2rtgLink::run, this);
    return true;
}

void Z2rtgLink::close()
{
    if (th.joinable()) {
        {
            std::lock_guard<std::mutex> g(mx);
            stop = true;
        }
        cv.notify_all();
        th.join();
    }
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
    if (log)
        fprintf(log, "z2rtg link: %llu words in %llu bursts, %llu blitter polls, %llu errors, %llu bursts resent, "
                "queue up to %zu\n", (unsigned long long)words, (unsigned long long)bursts,
                (unsigned long long)polls, (unsigned long long)errors, (unsigned long long)resent, queue_max);
}

void Z2rtgLink::write(uint32_t offset, uint16_t value)
{
    if (fd < 0)
        return;
    {
        std::lock_guard<std::mutex> g(mx);
        q.push_back({offset & 0x3FFFFEu, value, false});
        if (q.size() > queue_max) queue_max = q.size();
    }
    cv.notify_one();
}

void Z2rtgLink::wait_blitter()
{
    if (fd < 0)
        return;
    {
        std::lock_guard<std::mutex> g(mx);
        q.push_back({0, 0, true});
    }
    cv.notify_one();
}

bool Z2rtgLink::send(const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t k = ::write(fd, p, n);
        if (k < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return false;
        }
        p += k;
        n -= static_cast<size_t>(k);
    }
    return true;
}

int Z2rtgLink::recv(uint8_t *p, size_t n, int timeout_ms)
{
    size_t got = 0;
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (got < n) {
        int left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        end - std::chrono::steady_clock::now()).count());
        if (left <= 0) break;
        pollfd pf {fd, POLLIN, 0};
        if (poll(&pf, 1, left) <= 0) continue;
        ssize_t k = ::read(fd, p + got, n - got);
        if (k > 0) got += static_cast<size_t>(k);
    }
    return static_cast<int>(got);
}

bool Z2rtgLink::ack()
{
    uint8_t r = 0;
    if (recv(&r, 1, 2000) == 1 && r == 'k') {
        inflight.pop_front();
        return true;
    }
    errors++;
    if (log)
        fprintf(log, "z2rtg link: expected 'k' for the burst at %02x%02x%02x of %u words, got %s; %zu in flight\n",
                inflight.front()[1], inflight.front()[2], inflight.front()[3], inflight.front()[4] + 1u,
                r == 'e' ? "'e'" : r ? "another byte" : "nothing", inflight.size());
    return recover();
}

bool Z2rtgLink::drain()
{
    while (!inflight.empty())
        if (!ack())
            return false;
    return true;
}

// The bridge ignores input after an error until it has seen a gap of 20 ms (UART_GAP), then answers 'e'.
// Wait that out, then send the refused burst and every one after it again, one at a time.
bool Z2rtgLink::recover()
{
    for (int attempt = 0; attempt < 3; attempt++) {
        usleep(30000);
        tcflush(fd, TCIFLUSH);
        bool ok = true;
        while (ok && !inflight.empty()) {
            const std::vector<uint8_t> &p = inflight.front();
            uint32_t off = (p[1] << 16) | (p[2] << 8) | p[3];
            if (log && off <= Z2R_REG_BLT_OP && off + 2u * (p[4] + 1u) > Z2R_REG_BLT_OP)
                fprintf(log, "z2rtg link: resending a burst with BLT_OP: the job may run twice\n");
            uint8_t r = 0;
            ok = send(p.data(), p.size()) && recv(&r, 1, 2000) == 1 && r == 'k';
            if (ok) {
                inflight.pop_front();
                resent++;
            }
        }
        if (ok)
            return true;
        errors++;
    }
    if (log)
        fprintf(log, "z2rtg link: %zu bursts dropped after three attempts; the real board is out of step\n",
                inflight.size());
    inflight.clear();
    return false;
}

bool Z2rtgLink::burst(uint32_t off, const uint16_t *w, unsigned n)
{
    std::vector<uint8_t> b;
    b.reserve(5 + 2 * n);
    b.push_back('B');
    b.push_back(static_cast<uint8_t>(off >> 16));
    b.push_back(static_cast<uint8_t>(off >> 8));
    b.push_back(static_cast<uint8_t>(off));
    b.push_back(static_cast<uint8_t>(n - 1));
    for (unsigned i = 0; i < n; i++) {
        b.push_back(static_cast<uint8_t>(w[i] >> 8));
        b.push_back(static_cast<uint8_t>(w[i]));
    }
    if (!send(b.data(), b.size()))
        return false;
    inflight.push_back(std::move(b));
    words += n;
    bursts++;
    return static_cast<int>(inflight.size()) < WINDOW || ack();
}

bool Z2rtgLink::blitter_idle()
{
    if (!drain())
        return false;
    polls++;
    const uint8_t cmd[4] = {'R', 0, 0, static_cast<uint8_t>(Z2R_REG_STATUS)};
    for (int n = 0; n < 2000; n++) {             // a 1280x720 copy takes about 13 ms
        uint8_t r[2];
        if (!send(cmd, 4) || recv(r, 2, 500) != 2) {
            errors++;
            usleep(30000);                       // a refused read: resynchronise and ask again
            tcflush(fd, TCIFLUSH);
            continue;
        }
        if (!((r[0] << 8 | r[1]) & Z2R_STATUS_BLT_BUSY))
            return true;
    }
    return false;
}

void Z2rtgLink::run()
{
    std::vector<Item> work;
    std::vector<uint16_t> run_w;
    for (;;) {
        {
            std::unique_lock<std::mutex> g(mx);
            cv.wait_for(g, std::chrono::milliseconds(20), [this] { return stop || !q.empty(); });
            if (stop && q.empty())
                break;
            work.assign(q.begin(), q.end());
            q.clear();
        }
        // a failure has been logged and recovered as far as possible: carry on with the rest
        for (size_t i = 0; i < work.size();) {
            if (work[i].wait) {
                blitter_idle();
                i++;
                continue;
            }
            // consecutive offsets become one burst
            uint32_t start = work[i].off;
            run_w.clear();
            while (i < work.size() && !work[i].wait && run_w.size() < BURST &&
                   work[i].off == start + 2 * run_w.size())
                run_w.push_back(work[i++].v);
            burst(start, run_w.data(), static_cast<unsigned>(run_w.size()));
        }
        drain();
    }
    drain();
}
