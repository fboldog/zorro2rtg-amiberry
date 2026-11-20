// The host's connection to the zboard daemon for one board (zboard_channel.h).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "zboard_channel.h"
#include "zboard_plugin.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int64_t steady_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

ZbChannel::~ZbChannel() { close(); }

std::string ZbChannel::default_path()
{
    if (const char *run = getenv("XDG_RUNTIME_DIR"); run && *run)
        return std::string(run) + "/z2bridge.sock";
    return "/tmp/z2bridge-" + std::to_string(getuid()) + ".sock";
}

std::string ZbChannel::resolve(const std::string &spec)
{
    if (spec.empty() || spec == "default")
        return default_path();
    if (!spec.compare(0, 5, "unix:"))
        return spec.substr(5);
    return spec;
}

// sun_path holds 107 characters: a longer path is reached through its directory's /proc/self/fd entry
// (no chdir, which would race with the emulator's other threads)
static int connect_unix(const std::string &path)
{
    const int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0)
        return -1;
    sockaddr_un a {};
    a.sun_family = AF_UNIX;
    int dfd = -1;
    std::string target = path;
    if (target.size() >= sizeof a.sun_path) {
        const size_t slash = path.rfind('/');
        const std::string dir = slash == 0 ? "/" : path.substr(0, slash);
        dfd = ::open(dir.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
        target = "/proc/self/fd/" + std::to_string(dfd) + "/" + path.substr(slash + 1);
    }
    int r = -1;
    if (target.size() < sizeof a.sun_path) {
        memcpy(a.sun_path, target.c_str(), target.size() + 1);
        r = connect(s, reinterpret_cast<sockaddr *>(&a), sizeof a);
    } else {
        errno = ENAMETOOLONG;
    }
    const int e = errno;
    if (dfd >= 0)
        ::close(dfd);
    if (r < 0) {
        ::close(s);
        errno = e;
        return -1;
    }
    return s;
}

bool ZbChannel::open(const std::string &spec, const char *peer, LogFn lg)
{
    close();
    log = std::move(lg);
    where = resolve(spec);
    peer_name = peer ? peer : "";
    lost = false;
    quit_at = 0;
    if (peer_name.empty() || peer_name.size() > 255) {
        say(ZBP_LOG_WARN, "zboard: the plugin names no peer for the daemon");
        return false;
    }
    fd = connect_unix(where);
    if (fd < 0) {
        say(ZBP_LOG_WARN, "zboard: " + where + ": " + strerror(errno) + " (is z2bridged.py running?)");
        return false;
    }
    std::string hello = "H";
    hello += static_cast<char>(1);
    hello += static_cast<char>(peer_name.size());
    hello += peer_name;
    uint8_t r = 0;
    pollfd pf {fd, POLLIN, 0};
    if (::send(fd, hello.data(), hello.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(hello.size()) ||
        poll(&pf, 1, 3000) != 1 || ::read(fd, &r, 1) != 1 || r != 'H') {
        say(ZBP_LOG_WARN, "zboard: the daemon at " + where + " has no peer '" + peer_name + "'" +
                              (r == 'e' ? "" : " (no answer to the hello)"));
        ::close(fd);
        fd = -1;
        return false;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    stop = false;
    out.clear();
    out_head = 0;
    inq.clear();
    msgs_out = bytes_out = msgs_in = full_waits = 0;
    say(ZBP_LOG_INFO, "zboard: connected to peer '" + peer_name + "' through " + where);
    th = std::thread(&ZbChannel::run, this);
    return true;
}

void ZbChannel::lose(const std::string &why)
{
    if (lost.exchange(true))
        return;
    say(ZBP_LOG_WARN, "zboard: lost the daemon at " + where + " (" + why + "); messages to peer '" + peer_name +
                          "' are dropped until the next Autoconfig");
    std::lock_guard<std::mutex> g(mx);
    out.clear();
    out_head = 0;
    room.notify_all();
}

bool ZbChannel::send(const void *msg, uint32_t len)
{
    if (!up() || len == 0 || len > ZBP_MSG_MAX)
        return false;
    {
        std::unique_lock<std::mutex> g(mx);
        if (out.size() - out_head + 4 + len > MAX_OUT) {
            full_waits++;
            room.wait(g, [&] { return out.size() - out_head + 4 + len <= MAX_OUT || stop || lost; });
            if (stop || lost)
                return false;
        }
        const uint8_t h[4] = {static_cast<uint8_t>(len >> 24), static_cast<uint8_t>(len >> 16),
                              static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len)};
        out.insert(out.end(), h, h + 4);
        out.insert(out.end(), static_cast<const uint8_t *>(msg), static_cast<const uint8_t *>(msg) + len);
        msgs_out++;
        bytes_out += len;
    }
    const uint64_t one = 1;
    if (::write(wake, &one, sizeof one) < 0) {
        // the counter is saturated: the thread is awake anyway
    }
    return true;
}

uint32_t ZbChannel::recv(void *buf, uint32_t cap)
{
    std::lock_guard<std::mutex> g(mx);
    if (inq.empty())
        return 0;
    const std::vector<uint8_t> &m = inq.front();
    const uint32_t n = static_cast<uint32_t>(std::min<size_t>(m.size(), cap));
    memcpy(buf, m.data(), n);
    inq.pop_front();
    return n;
}

void ZbChannel::run()
{
    std::vector<uint8_t> rbuf;
    uint8_t tmp[65536];
    for (;;) {
        bool want_out;
        {
            std::lock_guard<std::mutex> g(mx);
            want_out = out.size() > out_head;
            if (stop && (!want_out || lost))
                break;
        }
        const int64_t q = quit_at.load();
        if (q && steady_ms() >= q)
            break;                                   // close() gave up on the rest
        pollfd pf[2] = {{fd, static_cast<short>(lost ? 0 : POLLIN | (want_out ? POLLOUT : 0)), 0}, {wake, POLLIN, 0}};
        if (poll(pf, 2, 50) <= 0)
            continue;
        if (pf[1].revents & POLLIN) {
            uint64_t v;
            if (::read(wake, &v, sizeof v) < 0) {
                // nothing to clear
            }
        }
        if (lost)
            continue;
        if (pf[0].revents & POLLIN) {
            const ssize_t k = ::read(fd, tmp, sizeof tmp);
            if (k == 0 || (k < 0 && errno != EAGAIN && errno != EINTR)) {
                lose(k == 0 ? "it closed the connection" : strerror(errno));
                continue;
            }
            if (k > 0) {
                rbuf.insert(rbuf.end(), tmp, tmp + k);
                size_t at = 0;
                while (rbuf.size() - at >= 4) {
                    const uint32_t n = (uint32_t(rbuf[at]) << 24) | (uint32_t(rbuf[at + 1]) << 16) |
                                       (uint32_t(rbuf[at + 2]) << 8) | rbuf[at + 3];
                    if (n == 0 || n > ZBP_MSG_MAX) {
                        lose("a message of " + std::to_string(n) + " bytes from the daemon");
                        break;
                    }
                    if (rbuf.size() - at - 4 < n)
                        break;
                    std::lock_guard<std::mutex> g(mx);
                    inq.emplace_back(rbuf.begin() + at + 4, rbuf.begin() + at + 4 + n);
                    msgs_in++;
                    at += 4 + n;
                }
                rbuf.erase(rbuf.begin(), rbuf.begin() + at);
            }
        } else if (pf[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            lose("hang-up");
            continue;
        }
        if (pf[0].revents & POLLOUT) {
            int err = 0;
            {
                std::lock_guard<std::mutex> g(mx);
                const ssize_t k = ::send(fd, out.data() + out_head, out.size() - out_head, MSG_NOSIGNAL | MSG_DONTWAIT);
                if (k > 0) {
                    out_head += static_cast<size_t>(k);
                    if (out_head == out.size()) {
                        out.clear();
                        out_head = 0;
                    } else if (out_head >= (size_t(1) << 20)) {
                        out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(out_head));
                        out_head = 0;
                    }
                    room.notify_all();
                } else if (k < 0 && errno != EAGAIN && errno != EINTR) {
                    err = errno;
                }
            }
            if (err)
                lose(strerror(err));
        }
    }
}

void ZbChannel::close()
{
    const bool was_open = th.joinable();
    if (th.joinable()) {
        {
            std::lock_guard<std::mutex> g(mx);
            stop = true;
        }
        quit_at = steady_ms() + CLOSE_MS;
        room.notify_all();
        const uint64_t one = 1;
        if (::write(wake, &one, sizeof one) < 0) {
            // awake anyway
        }
        th.join();
    }
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
    if (wake >= 0) {
        ::close(wake);
        wake = -1;
    }
    if (was_open) {
        size_t left;
        {
            std::lock_guard<std::mutex> g(mx);
            left = out.size() - out_head;
            out.clear();
            out_head = 0;
            inq.clear();
        }
        say(ZBP_LOG_INFO, "zboard: closed the channel to peer '" + peer_name + "': " + std::to_string(msgs_out) +
                              " messages (" + std::to_string(bytes_out) + " bytes) sent, " + std::to_string(msgs_in) +
                              " received, " + std::to_string(full_waits) + " waits for room" +
                              (left ? ", " + std::to_string(left) + " bytes dropped at close" : ""));
    }
}
