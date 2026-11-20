// The host's connection to the zboard daemon for one board: the message channel that zbp_host.send and
// recv give the plugin (include/zboard_plugin.h). Emulator-independent, POSIX; every host adapter uses it.
//
// Wire protocol on the daemon's Unix socket (z2bridged.py, "zboard clients"):
//   hello    host -> daemon   'H', protocol version (1), name length, the peer's name (zbp_desc.peer)
//            daemon -> host   'H' when it has the peer, else 'e' and it closes the socket
//   message  both ways        4-byte big-endian length, then that many bytes (1 to ZBP_MSG_MAX)
// The daemon serves other clients on the same socket with the debug-bridge protocol, which never starts
// with 'H'.
//
//   - send() only copies into an outgoing buffer; a thread writes it to the socket. The buffer is bounded
//     (MAX_OUT bytes): once it is full, send() waits, so a peer that cannot keep up (a host tool holding the
//     board, a slow UART) slows the emulated board down instead of growing the buffer without limit.
//   - Messages from the peer wait in a queue until recv() takes them, on the emulation thread.
//   - A daemon that goes away is logged once; up() turns false and messages are dropped until open() runs
//     again (the adapter does at the next Autoconfig).
//   - close() gives the outgoing buffer CLOSE_MS to reach the socket, then drops the rest.
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef ZBOARD_CHANNEL_H
#define ZBOARD_CHANNEL_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class ZbChannel {
public:
    using LogFn = std::function<void(int level, const std::string &msg)>;   // level: ZBP_LOG_*

    ~ZbChannel();
    // where the daemon listens: "unix:PATH" or a path; "" or "default": default_path()
    static std::string resolve(const std::string &spec);
    static std::string default_path();     // $XDG_RUNTIME_DIR/z2bridge.sock, else /tmp/z2bridge-UID.sock

    // connect, say hello for the peer and start the I/O thread; false (logged) when that fails
    bool open(const std::string &spec, const char *peer, LogFn log);
    bool up() const { return fd >= 0 && !lost; }
    bool send(const void *msg, uint32_t len);
    uint32_t recv(void *buf, uint32_t cap);
    void close();

    static constexpr size_t MAX_OUT = size_t(8) << 20;   // bytes waiting for the socket
    static constexpr int CLOSE_MS = 2000;

    // statistics (approximate when read from another thread)
    uint64_t msgs_out = 0, bytes_out = 0, msgs_in = 0, full_waits = 0;

private:
    int fd = -1;
    int wake = -1;                          // eventfd: new data to send, or close()
    std::atomic<bool> lost {false};
    std::string where, peer_name;
    LogFn log;
    std::thread th;
    std::mutex mx;
    std::condition_variable room;
    std::vector<uint8_t> out;               // framed bytes not yet sent; out_head of them are done
    size_t out_head = 0;
    std::deque<std::vector<uint8_t>> inq;
    bool stop = false;
    std::atomic<int64_t> quit_at {0};       // close(): steady ms after which the rest is dropped

    void run();
    void lose(const std::string &why);
    void say(int level, const std::string &msg) const { if (log) log(level, msg); }
};

#endif
