// Board-in-the-loop link to a real Nano 20K (z2rtg_link.h).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "z2rtg_link.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

#include "zorro2rtg_regs.h"

static constexpr unsigned BURST = 256;       // words per bridge burst
static constexpr int WINDOW = 4;             // bursts in flight (docs/debug_bridge.md)
static constexpr uint32_t VRAM_START = 0x010000;   // below: registers, palette, cursor image (kept in order)

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
    close();                                     // a port lost earlier: its thread and descriptor
    log = lg;
    dev = device;
    lost = false;
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
    const bool was_open = th.joinable() || fd >= 0;
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
    if (log && was_open)
        fprintf(log, "z2rtg link: %llu words in %llu bursts, %llu blitter polls, %llu errors, %llu bursts resent, "
                "queue up to %zu, %llu input events\n", (unsigned long long)words, (unsigned long long)bursts,
                (unsigned long long)polls, (unsigned long long)errors, (unsigned long long)resent, queue_max,
                (unsigned long long)events);
}

void Z2rtgLink::port_lost(const char *what)
{
    if (lost.exchange(true))
        return;
    if (log) {
        fprintf(log, "z2rtg link: lost %s (%s): no more writes reach the real board, which keeps its last "
                "picture. A reset of the emulated Amiga opens the port again\n", dev.c_str(), what);
        fflush(log);
    }
}

void Z2rtgLink::write(uint32_t offset, uint16_t value)
{
    if (!is_open())
        return;
    offset &= 0x3FFFFEu;
    if (input && offset >= Z2R_REG_IN_STATUS && offset <= Z2R_REG_IN_POP)
        return;                                  // the real queue belongs to the link (enable_input)
    {
        std::lock_guard<std::mutex> g(mx);
        q.push_back({offset, value, false});
        if (q.size() > queue_max) queue_max = q.size();
    }
    cv.notify_one();
}

void Z2rtgLink::wait_blitter()
{
    if (!is_open())
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
        if (lost)
            return false;
        ssize_t k = ::write(fd, p, n);
        if (k < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            port_lost(strerror(errno));
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
        if (lost) break;
        pollfd pf {fd, POLLIN, 0};
        if (poll(&pf, 1, left) <= 0) continue;
        if (pf.revents & (POLLHUP | POLLERR | POLLNVAL)) {
            port_lost("hang-up");
            break;
        }
        ssize_t k = ::read(fd, p + got, n - got);
        if (k > 0) got += static_cast<size_t>(k);
        else if (k == 0 || (errno != EINTR && errno != EAGAIN)) {
            port_lost(k == 0 ? "end of file" : strerror(errno));
            break;
        }
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
    if (lost) {
        inflight.clear();
        return false;
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
    for (int attempt = 0; attempt < 3 && !lost; attempt++) {
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
    if (lost) {
        inflight.clear();
        return false;
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
            if (lost)
                return false;
            errors++;
            if (log) fprintf(log, "z2rtg link: no answer to a STATUS read, retrying\n");
            usleep(30000);                       // a refused read: resynchronise and ask again
            tcflush(fd, TCIFLUSH);
            continue;
        }
        if (!((r[0] << 8 | r[1]) & Z2R_STATUS_BLT_BUSY))
            return true;
    }
    if (log) fprintf(log, "z2rtg link: the real BLT_BUSY did not clear\n");
    return false;
}

bool Z2rtgLink::take_event(std::array<uint8_t, 7> &ev)
{
    std::lock_guard<std::mutex> g(mx);
    if (evq.empty())
        return false;
    ev = evq.front();
    evq.pop_front();
    return true;
}

// IN_STATUS .. IN_EV3 in one burst read; each event found is popped on the real board (IN_POP)
void Z2rtgLink::poll_input()
{
    if (!drain())
        return;
    for (int n = 0; n < 16 && !lost; n++) {
        const uint8_t cmd[5] = {'S', 0, 0, static_cast<uint8_t>(Z2R_REG_IN_STATUS), 5};
        uint8_t r[12];
        if (!send(cmd, 5) || recv(r, 12, 500) != 12) {
            if (!lost) {
                errors++;
                if (log) fprintf(log, "z2rtg link: no answer to an IN_STATUS read\n");
                usleep(30000);
                tcflush(fd, TCIFLUSH);
            }
            return;
        }
        const uint16_t st = static_cast<uint16_t>(r[0] << 8 | r[1]);
        if (st & (Z2R_IN_STATUS_OVERFLOW | Z2R_IN_STATUS_FRAME_ERR)) {
            if (log) fprintf(log, "z2rtg link: real board input%s%s\n", st & Z2R_IN_STATUS_OVERFLOW ? " overflow" : "",
                             st & Z2R_IN_STATUS_FRAME_ERR ? " frame error" : "");
            const uint16_t w1c = st & (Z2R_IN_STATUS_OVERFLOW | Z2R_IN_STATUS_FRAME_ERR);
            const uint8_t clr[6] = {'W', 0, 0, static_cast<uint8_t>(Z2R_REG_IN_STATUS),
                                    static_cast<uint8_t>(w1c >> 8), static_cast<uint8_t>(w1c)};
            uint8_t k = 0;
            if (!send(clr, 6) || recv(&k, 1, 500) != 1 || k != 'k')
                return;
        }
        if (!(st & Z2R_IN_STATUS_AVAIL))
            return;
        // EV0 = type:p0, EV1 = p1:p2, EV2 = p3:p4, EV3 = p5:-
        std::array<uint8_t, 7> ev {r[4], r[5], r[6], r[7], r[8], r[9], r[10]};
        const uint8_t pop[6] = {'W', 0, 0, static_cast<uint8_t>(Z2R_REG_IN_POP), 0, Z2R_IN_POP_POP};
        uint8_t k = 0;
        if (!send(pop, 6) || recv(&k, 1, 500) != 1 || k != 'k')
            return;
        {
            std::lock_guard<std::mutex> g(mx);
            evq.push_back(ev);
        }
        events++;
    }
}

void Z2rtgLink::run()
{
    std::vector<Item> work;
    std::vector<uint16_t> run_w;
    auto next_report = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    auto next_input = std::chrono::steady_clock::now();
    for (;;) {
        if (input && !lost && std::chrono::steady_clock::now() >= next_input) {
            poll_input();
            next_input = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
        }
        {
            std::unique_lock<std::mutex> g(mx);
            cv.wait_for(g, std::chrono::milliseconds(input ? 5 : 20), [this] { return stop || !q.empty(); });
            if (stop && q.empty())
                break;
            work.assign(q.begin(), q.end());
            q.clear();
        }
        if (lost) {
            work.clear();
            inflight.clear();
            continue;
        }
        std::map<uint32_t, uint16_t> seg;
        for (size_t i = 0; i < work.size();) {
            // progress every 10 s: a link far behind the emulated board shows as a growing backlog
            if (log && std::chrono::steady_clock::now() >= next_report) {
                next_report += std::chrono::seconds(10);
                fprintf(log, "z2rtg link: %llu words in %llu bursts, %llu polls, %llu errors, %zu writes waiting\n",
                        (unsigned long long)words, (unsigned long long)bursts, (unsigned long long)polls,
                        (unsigned long long)errors, work.size() - i);
                fflush(log);
            }
            if (work[i].wait) {
                blitter_idle();
                i++;
                continue;
            }
            if (work[i].off < VRAM_START) {
                // registers in order; consecutive offsets become one burst
                uint32_t start = work[i].off;
                run_w.clear();
                while (i < work.size() && !work[i].wait && work[i].off < VRAM_START && run_w.size() < BURST &&
                       work[i].off == start + 2 * run_w.size())
                    run_w.push_back(work[i++].v);
                burst(start, run_w.data(), static_cast<unsigned>(run_w.size()));
                continue;
            }
            // VRAM up to the next register write or blitter wait: the order inside does not matter, only
            // each word's last value, so the words go sorted, as long bursts (a CPU clearing memory
            // backwards would otherwise give one burst per longword)
            seg.clear();
            while (i < work.size() && !work[i].wait && work[i].off >= VRAM_START) {
                seg[work[i].off] = work[i].v;
                i++;
            }
            for (auto it = seg.begin(); it != seg.end();) {
                uint32_t start = it->first;
                run_w.clear();
                while (it != seg.end() && run_w.size() < BURST && it->first == start + 2 * run_w.size())
                    run_w.push_back((it++)->second);
                burst(start, run_w.data(), static_cast<unsigned>(run_w.size()));
            }
        }
        drain();
    }
    drain();
}
