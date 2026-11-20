# SPDX-License-Identifier: GPL-3.0-or-later
"""zbexample's peer in the zboard daemon (docs/writing-a-plugin.md): the half of the example board that owns
its "hardware", here a UDP socket, as a network card's peer would own a USB device, an SWD probe or a raw
socket. It needs no Nano 20K, so the daemon serves it with --port none too.

- Each 'T' message from the plugin (a packet the emulated Amiga sent) goes out as one datagram to
  ZBEXAMPLE_DEST (host:port; by default this socket itself, a loopback).
- Each datagram that arrives goes to the plugin as 'R' + its bytes; the plugin keeps it until the emulated
  Amiga reads it, so the Amiga's reads never wait for the socket.
- 'L' 1 tells the plugin the device is up, once, at the start; 'L' 0 when it closes.
- The socket is in the daemon's select loop through api.watch, so arrivals are handled at once.

Environment (of the daemon): ZBEXAMPLE_BIND (host:port, default 127.0.0.1:0), ZBEXAMPLE_DEST (default: the
bound address).
"""
import collections
import os
import socket
import time

NEEDS_BRIDGE = False   # this peer brings its own device; it never uses the Nano's debug bridge
MTU = 1514
REPORT_EVERY = 10.0


def addr(spec, default):
    if not spec:
        return default
    host, _, port = spec.rpartition(":")
    return (host or "127.0.0.1", int(port))


class Peer:
    def __init__(self, api):
        self.api = api
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(addr(os.environ.get("ZBEXAMPLE_BIND"), ("127.0.0.1", 0)))
        self.sock.setblocking(False)
        self.dest = addr(os.environ.get("ZBEXAMPLE_DEST"), self.sock.getsockname())
        self.stats = collections.Counter()
        self.next_report = time.monotonic() + REPORT_EVERY
        self.reported = None
        api.watch(self.sock.fileno(), self.readable)
        api.send(b"L\x01")
        api.log(f"UDP {self.sock.getsockname()[0]}:{self.sock.getsockname()[1]} -> {self.dest[0]}:{self.dest[1]}")

    # ---- the daemon's calls
    def on_message(self, msg):
        if msg[:1] != b"T" or not 1 < len(msg) <= 1 + MTU:
            raise ValueError(f"not a packet: {bytes(msg[:8])!r}... ({len(msg)} bytes)")
        try:
            self.sock.sendto(bytes(msg[1:]), self.dest)
            self.stats["sent"] += 1
        except OSError:
            self.stats["send_errors"] += 1

    def readable(self):
        while True:
            try:
                data = self.sock.recv(65536)
            except (BlockingIOError, InterruptedError):
                return
            if not 0 < len(data) <= MTU:
                self.stats["too_long"] += 1
                continue
            self.api.send(b"R" + data)
            self.stats["received"] += 1

    def poll(self, now):
        if now >= self.next_report:
            self.next_report = now + REPORT_EVERY
            if self.stats and self.stats != self.reported:     # only when something happened
                self.reported = self.stats.copy()
                self.api.log(self.line())

    def backlog(self):
        return 0                  # nothing waits here: each message is sent at once

    def busy(self):
        return False

    def close(self):
        self.api.unwatch(self.sock.fileno())
        self.sock.close()
        self.api.log("closed: " + self.line())

    def line(self):
        s = self.stats
        return (f"{s['sent']} packets sent, {s['received']} received, {s['send_errors']} send errors, "
                f"{s['too_long']} too long")
