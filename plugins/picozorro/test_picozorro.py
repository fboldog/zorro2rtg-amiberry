#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""picozorro end to end: the plugin through the zboard interface (a ctypes host whose channel is a real
connection to z2bridged), the daemon with --port none, this directory's peer, and a card on a UART.

The card is either
  --card CMD    a stand-in speaking the backplane on stdin/stdout (PicoZorro's pz-core Window and
                backplane::execute built for the host); the test puts it behind a pty, which the peer opens
  --uart PATH   the real card: an RP2350 with the uart-backplane firmware (flash.sh), on the probe's UART

Checks: the description; configured() reads the card's Autoconfig ROM (2011/$5A) and configures it; MAGIC at
A16 = 0 ("PZ") and A16 = 1 ("PU"); SCRATCH by word, by even and odd byte; a run of word writes to one register
(WRITE_N) ends with the last value; reset_board and a second Autoconfig; received frames from the LAN, read
as picozorro.device reads them, each in one request (the RX prefetch; skipped when none arrives, so a cable
and some network traffic are needed); sent frames, UDP broadcasts through the alias received on this PC by a
socket (skipped when none comes back); /INT: a lost release note dropped after 250 ms, a held level kept by the
card's repeats; without a channel reads give $FFFF; the read latency. With --card and PZSIM_DROP=1 in the environment the stand-in drops some first tries, and the
peer's retries must cover them.

Run: python3 test_picozorro.py --daemon .../Zorro2-Tang25k/zorro2-tang25k/sw/tools/host/z2bridged.py --card CMD
"""
import argparse
import ctypes as C
import os
import pathlib
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
errors = checks = 0


def check(what, got, want):
    global errors, checks
    checks += 1
    ok = got == want
    print(("ok   " if ok else "FAIL ") + what + ("" if ok else f": got {got!r}, want {want!r}"))
    if not ok:
        errors += 1


# ---- zboard_plugin.h for ctypes
class Region(C.Structure):
    _fields_ = [("offset", C.c_uint32), ("size", C.c_uint32), ("kind", C.c_uint32), ("name", C.c_char_p)]


class Desc(C.Structure):
    _fields_ = [("struct_size", C.c_uint32), ("name", C.c_char_p), ("vendor", C.c_char_p),
                ("manufacturer", C.c_uint16), ("product", C.c_uint8), ("er_type", C.c_uint8), ("er_flags", C.c_uint8),
                ("serial", C.c_uint32), ("diag_vec", C.c_uint16), ("size", C.c_uint32), ("zorro", C.c_uint32),
                ("flags", C.c_uint32), ("n_regions", C.c_uint32), ("regions", C.POINTER(Region)),
                ("peer", C.c_char_p)]


CHARGE = C.CFUNCTYPE(None, C.c_void_p, C.c_uint32)
DISPLAY = C.CFUNCTYPE(None, C.c_void_p, C.c_int)
LOG = C.CFUNCTYPE(None, C.c_void_p, C.c_int, C.c_char_p)
UP = C.CFUNCTYPE(C.c_int, C.c_void_p)
SEND = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_void_p, C.c_uint32)
RECV = C.CFUNCTYPE(C.c_uint32, C.c_void_p, C.c_void_p, C.c_uint32)


class Host(C.Structure):
    _fields_ = [("struct_size", C.c_uint32), ("charge_ns", CHARGE), ("display", DISPLAY), ("log", LOG),
                ("channel_up", UP), ("send", SEND), ("recv", RECV)]


class Api(C.Structure):
    _fields_ = [("struct_size", C.c_uint32), ("version", C.c_uint32), ("desc", C.POINTER(Desc)),
                ("create", C.CFUNCTYPE(C.c_void_p, C.POINTER(Host), C.c_void_p, C.c_char_p)),
                ("destroy", C.CFUNCTYPE(None, C.c_void_p)),
                ("configured", C.CFUNCTYPE(None, C.c_void_p, C.c_uint32, C.c_uint64)),
                ("reset_board", C.CFUNCTYPE(None, C.c_void_p)),
                ("read", C.CFUNCTYPE(C.c_uint32, C.c_void_p, C.c_uint32, C.c_int, C.c_uint64)),
                ("write", C.CFUNCTYPE(None, C.c_void_p, C.c_uint32, C.c_uint32, C.c_int, C.c_uint64)),
                ("ptr", C.CFUNCTYPE(C.c_void_p, C.c_void_p, C.c_uint32, C.c_uint32)),
                ("tick_ns", C.c_uint32),
                ("tick", C.CFUNCTYPE(C.c_uint32, C.c_void_p, C.c_uint64)),
                ("frame", C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_void_p))]


class Board:
    """The host: a channel that is a zboard client of the daemon (hello, then 4-byte BE length messages)."""

    def __init__(self, api, sock_path=None):
        self.api, self.logs, self.t = api, [], 0
        self.sock, self.inbuf = None, bytearray()
        if sock_path:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(sock_path)
            s.settimeout(5)
            s.sendall(b"H\x01" + bytes([9]) + b"picozorro")
            assert s.recv(1) == b"H", "the daemon refused the picozorro peer (see its log)"
            s.setblocking(False)
            self.sock = s
        self.cbs = [CHARGE(lambda c, ns: None), DISPLAY(lambda c, on: None),
                    LOG(lambda c, lv, m: self.logs.append(m.decode())),
                    UP(lambda c: 1 if self.sock else 0), SEND(self._send), RECV(self._recv)]
        self.host = Host(C.sizeof(Host), *self.cbs)
        self.b = api.create(C.byref(self.host), None, b"")

    def _send(self, c, msg, n):
        data = C.string_at(msg, n)
        self.sock.setblocking(True)
        self.sock.sendall(n.to_bytes(4, "big") + data)
        self.sock.setblocking(False)
        return 1

    def _recv(self, c, buf, cap):
        try:
            while True:
                chunk = self.sock.recv(65536)
                if not chunk:
                    break
                self.inbuf += chunk
        except (BlockingIOError, InterruptedError):
            pass
        if len(self.inbuf) < 4:
            return 0
        n = int.from_bytes(self.inbuf[:4], "big")
        if len(self.inbuf) < 4 + n:
            return 0
        C.memmove(buf, bytes(self.inbuf[4:4 + n]), n)
        del self.inbuf[:4 + n]
        return n

    def rd(self, off, n=2):
        return self.api.read(self.b, off, n, self.t)

    def wr(self, off, v, n=2):
        self.api.write(self.b, off, v, n, self.t)

    def tick(self):
        self.t += 64000
        return self.api.tick(self.b, self.t)


def rx_frame(b):
    """One frame the way picozorro.device takes it (backend_pz.c, pz_poll_rx): RX_LEN, the first word from
    RX_DATA, the rest through the $C0-$FE alias as movem.l bursts do, RX_DONE. Also one read past the end, which
    the card answers $0000 only if exactly the frame's words came off it before."""
    n = b.rd(0x30)
    if not n or n > 1514:
        if n:
            b.wr(0x34, 0)
        return None
    words = [b.rd(0x32)] + [b.rd(0xC0 + 2 * (i % 32)) for i in range(1, (n + 1) // 2)]
    past = b.rd(0x32)
    b.wr(0x34, 0)
    return n, b"".join(w.to_bytes(2, "big") for w in words)[:n], past


def csum16(data):
    if len(data) & 1:
        data += b"\0"
    s = sum(int.from_bytes(data[i:i + 2], "big") for i in range(0, len(data), 2))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return s


def udp_ok(f):
    """True/False for an IPv4 UDP frame with a checksum (it covers every payload byte), None for others."""
    if len(f) < 42 or f[12:14] != b"\x08\x00" or f[14] >> 4 != 4 or f[23] != 17:
        return None
    ihl = (f[14] & 15) * 4
    ip = f[14:14 + ihl]
    total = int.from_bytes(ip[2:4], "big")
    udp = f[14 + ihl:14 + total]
    if len(udp) < 8 or udp[6:8] == b"\0\0":
        return None
    pseudo = ip[12:20] + b"\0\x11" + len(udp).to_bytes(2, "big")
    return csum16(ip) == 0xFFFF and csum16(pseudo + udp) == 0xFFFF


def udp_broadcast(src_mac, payload, port):
    """An Ethernet frame: IPv4 UDP from 192.0.2.1 (TEST-NET-1) to 255.255.255.255:port, checksums filled in."""
    udp_len = 8 + len(payload)
    ip = bytearray(b"\x45\x00" + (20 + udp_len).to_bytes(2, "big") + b"\x12\x34\x00\x00\x40\x11\x00\x00" +
                   bytes([192, 0, 2, 1]) + bytes([255] * 4))
    ip[10:12] = (0xFFFF - csum16(bytes(ip))).to_bytes(2, "big")
    udp = bytearray(port.to_bytes(2, "big") + port.to_bytes(2, "big") + udp_len.to_bytes(2, "big") + b"\0\0" +
                    payload)
    c = 0xFFFF - csum16(bytes(ip[12:20]) + b"\0\x11" + udp_len.to_bytes(2, "big") + bytes(udp))
    udp[6:8] = (c or 0xFFFF).to_bytes(2, "big")
    return b"\xff" * 6 + src_mac + b"\x08\x00" + bytes(ip) + bytes(udp)


def tx_frame(b, frame):
    """One frame the way picozorro.device sends it (backend_pz.c, pz_send): STATUS until TX_FREE, TX_LEN, the
    words through the $C0-$FE alias as movem.l bursts do, TX_COMMIT. Returns the STATUS reads it took."""
    reads = 1
    while not (b.rd(0x14) >> 8) & 0xF:
        reads += 1
    b.wr(0x20, len(frame))
    padded = frame + b"\0" * (len(frame) & 1)
    for i in range(len(padded) // 2):
        b.wr(0xC0 + 2 * (i % 32), int.from_bytes(padded[2 * i:2 * i + 2], "big"))
    b.wr(0x24, 0)
    return reads


def build(tmp):
    out = pathlib.Path(tmp) / "libpicozorro.so"
    subprocess.run(["g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                    f"-I{REPO}/include", str(HERE / "picozorro_plugin.cpp"), "-o", str(out)], check=True)
    lib = C.CDLL(str(out))
    lib.zbp_get_api.restype = C.POINTER(Api)
    lib.zbp_get_api.argtypes = [C.c_uint32]
    return lib


def card_behind_pty(cmd):
    """The stand-in on a pty's master; the peer opens the slave as its UART."""
    master, slave = os.openpty()
    proc = subprocess.Popen(cmd, shell=True, stdin=master, stdout=master)
    return proc, os.ttyname(slave), slave


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--daemon", required=True, help="z2bridged.py (Zorro2-Tang25k sw/tools/host)")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--card", help="a backplane stand-in on stdin/stdout")
    g.add_argument("--uart", help="the real card's UART")
    ap.add_argument("--reads", type=int, default=200, help="reads for the latency figure")
    ap.add_argument("--rx-frames", type=int, default=5, help="frames to receive from the LAN")
    ap.add_argument("--rx-secs", type=float, default=15, help="how long to wait for them")
    ap.add_argument("--tx-frames", type=int, default=20, help="UDP broadcasts to send through the card")
    ap.add_argument("--tx-port", type=int, default=47809, help="their UDP port, received on this PC")
    a = ap.parse_args()

    sys.path.insert(0, str(pathlib.Path(a.daemon).resolve().parent))
    import z2bridged

    proc = slave = None
    if a.card:
        proc, uart, slave = card_behind_pty(a.card)
    else:
        uart = a.uart
    os.environ["PICOZORRO_UART"] = uart

    with tempfile.TemporaryDirectory() as tmp:
        lib = build(tmp)
        check("zbp_get_api: version 2 refused", bool(lib.zbp_get_api(2)), False)
        api = lib.zbp_get_api(1).contents
        d = api.desc.contents
        check("description: name, peer, 2011/$5A, 128 KB Zorro II", (d.name, d.peer, d.manufacturer, d.product,
              d.size, d.zorro, d.er_type & 0xEF, d.diag_vec, d.n_regions), (b"PicoZorro", b"picozorro", 2011,
              0x5A, 0x20000, 2, 0xC2, 0x100, 2))

        b = Board(api)
        api.configured(b.b, 0xE90000, 0)
        check("no channel: reads $FFFF", (b.rd(0), b.rd(1, 1)), (0xFFFF, 0xFF))
        api.destroy(b.b)

        path = os.path.join(tmp, "z.sock")
        dlogs = []
        dm = z2bridged.Daemon(None, z2bridged.listen(path), peers=HERE.parent,
                              log=lambda m: (dlogs.append(m), print("  daemon:", m)))
        threading.Thread(target=dm.run, daemon=True).start()

        b = Board(api, path)
        api.configured(b.b, 0xE90000, 0)
        for m in b.logs:
            print("  plugin:", m)
        check("configured: the card's Autoconfig ROM is the PicoZorro's",
              any("the card says 2011/$5A, er_Type $" in m for m in b.logs), True)
        if any("boot ROM" in m and "no boot ROM" not in m for m in b.logs if "the card says" in m):
            # ROM mode: reads give the DiagArea until the first write (docs/REGISTERS.md); the drivers write
            # SCRATCH before they read MAGIC
            check("boot ROM: ROM mode after configuration, the DiagArea's first word", b.rd(0x00) >> 12, 0x9)
            b.wr(0x04, 0)
        check("MAGIC at A16 = 0: PZ", b.rd(0x00), 0x505A)
        check("MAGIC at A16 = 1: PU", b.rd(0x10000), 0x5055)
        check("the register window aliases every 256 bytes", b.rd(0x0100 * 7), 0x505A)
        b.wr(0x04, 0x1234)
        check("SCRATCH word", b.rd(0x04), 0x1234)
        b.wr(0x05, 0xAB, 1)
        check("SCRATCH odd byte (/LDS)", (b.rd(0x04), b.rd(0x05, 1)), (0x12AB, 0xAB))
        for v in range(1000):
            b.wr(0x04, v)
        check("1000 word writes to one register (WRITE_N runs): the last one stays", b.rd(0x04), 999)
        b.wr(0x04, 0x5555)
        b.tick()                                   # posted at the tick, not awaited
        time.sleep(0.1)
        b.tick()
        check("a write posted at tick lands", b.rd(0x04), 0x5555)

        api.reset_board(b.b)
        check("after reset_board, before Autoconfig: $FFFF", b.rd(0x00), 0xFFFF)
        api.configured(b.b, 0xEA0000, 0)
        b.wr(0x04, 0)                              # out of ROM mode, if the card has a boot ROM
        check("configured again at another base: MAGIC", b.rd(0x00), 0x505A)

        # received frames: online, then whatever the LAN sends (broadcasts, multicasts), read as the driver does
        mac = b"".join(b.rd(0x16 + 2 * k).to_bytes(2, "big") for k in range(3))
        b.wr(0x12, 0x0001)                         # CTRL_ONLINE
        frames, end = [], time.monotonic() + a.rx_secs
        while len(frames) < a.rx_frames and time.monotonic() < end:
            f = rx_frame(b)
            if f:
                frames.append(f)
            else:
                time.sleep(0.02)
        b.wr(0x12, 0x0000)
        while rx_frame(b):                         # drain what came after
            pass
        rx_words = 0
        if not frames:
            print(f"  RX: no frame in {a.rx_secs} s (no cable, or a silent network): RX checks skipped")
        else:
            for n, data, past in frames:
                print(f"  RX: {n} bytes to {data[:6].hex(':')} from {data[6:12].hex(':')}, type ${data[12:14].hex()}")
            rx_words = sum((n + 1) // 2 for n, _, _ in frames)
            check(f"RX: {len(frames)} frames, each to us or to a group (broadcast, multicast)",
                  all(d[:6] == mac or d[0] & 1 for _, d, _ in frames), True)
            check("RX: a read past each frame's end gives $0000 (exactly the frame came off the card)",
                  [p for _, _, p in frames], [0] * len(frames))
            udp = [udp_ok(d) for _, d, _ in frames]
            if any(u is not None for u in udp):
                check(f"RX: IP and UDP checksums of the {sum(u is not None for u in udp)} UDP frames (every byte "
                      "in place)", [u for u in udp if u is not None], [True] * sum(u is not None for u in udp))

        # sent frames: UDP broadcasts from the card, received here by an ordinary socket. The kernel checks the IP
        # and UDP checksums and drops what fails, so each payload that comes back proves its whole frame.
        rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        rx.bind(("", a.tx_port))
        rx.settimeout(0.5)
        b.wr(0x12, 0x0001)
        sent = [bytes((k * 7 + i) & 0xFF for i in range(1400 + k)) for k in range(a.tx_frames)]   # odd and even
        t0 = time.monotonic()
        polls = sum(tx_frame(b, udp_broadcast(mac, p, a.tx_port)) for p in sent)
        b.rd(0x00)                                 # the last frame is off the UART when this is answered
        el = time.monotonic() - t0
        got = []
        try:
            while len(got) < len(sent):
                got.append(rx.recv(2048))
        except socket.timeout:
            pass
        rx.close()
        b.wr(0x12, 0x0000)
        nbytes = sum(len(p) + 42 for p in sent)
        print(f"  TX: {len(sent)} frames, {nbytes} bytes in {el:.2f} s ({el / len(sent) * 1e3:.1f} ms a frame, "
              f"{nbytes / el / 1024:.1f} KiB/s), {polls} STATUS reads; {len(got)} came back")
        if not got:
            print("  TX: none came back (not on this PC's LAN, or broadcasts filtered): TX checks skipped")
        else:
            check(f"TX: the {len(sent)} UDP broadcasts sent through the alias arrive whole, in order",
                  got, sent)

        # /INT. A release note lost on the UART: an asserted note the card never sent, the card's own level low
        # (offline, nothing enabled). /INT must drop once no repeat came for 250 ms.
        b.wr(0x10, 0x0000)                         # INT_ENABLE: nothing
        for _ in range(10):
            b.tick()
            time.sleep(0.01)
        note = b"D" + bytes([0x50, 0x42, 1, 0x40, 0, 0, 1])
        b.inbuf += len(note).to_bytes(4, "big") + note
        held = b.tick()
        time.sleep(0.35)
        check("/INT: a lost release note: held, then released after 250 ms without the card's repeats",
              (held, b.tick()), (4, 0))
        # A level the card holds: RX_AVAIL enabled and a frame left unread. The card repeats the note every
        # 100 ms, so /INT stays up for a whole second of ticks, then drops when RX_AVAIL is disabled again.
        if frames:
            b.wr(0x12, 0x0001)
            b.wr(0x10, 0x0001)                     # INT_ENABLE: RX_AVAIL
            end = time.monotonic() + a.rx_secs
            while not b.tick() and time.monotonic() < end:
                time.sleep(0.01)
            ticks = []
            end = time.monotonic() + 1.0
            while time.monotonic() < end:
                ticks.append(b.tick())
                time.sleep(0.01)
            b.wr(0x10, 0x0000)
            b.wr(0x12, 0x0000)
            while rx_frame(b):
                pass
            for _ in range(20):
                last = b.tick()
                time.sleep(0.01)
            check(f"/INT: held by the card for 1 s ({len(ticks)} ticks, up at each), released when disabled",
                  (bool(ticks) and all(t == 4 for t in ticks), last), (True, 0))

        t0 = time.monotonic()
        for _ in range(a.reads):
            b.rd(0x00)
        per = (time.monotonic() - t0) / a.reads * 1e3
        print(f"  {a.reads} reads: {per:.3f} ms a read")
        api.destroy(b.b)
        for m in b.logs[-1:]:
            print("  plugin:", m)
        check("no timeouts, no error replies", any(" 0 timeouts, 0 given up by the peer, 0 error replies" in m
                                                   for m in b.logs), True)
        stale = [int(g[1]) for m in b.logs for g in [re.search(r"(\d+) /INT releases presumed lost", m)] if g]
        check("/INT: the injected release counted as lost", bool(stale) and stale[-1] >= 1, True)
        if frames:
            # each frame (the checked ones and the drained ones) costs one READ_N; only its first word waits
            st = next(m for m in b.logs if "RX frames prefetched" in m)
            got = re.search(r"(\d+) RX frames prefetched \((\d+) reads answered", st)
            pref, local = int(got[1]), int(got[2])
            check("RX: each frame fetched in one request, its other words answered locally",
                  (pref >= len(frames), local >= rx_words - len(frames)), (True, True))
        b.sock.close()
        end = time.monotonic() + 5
        while not any("closed:" in m for m in dlogs) and time.monotonic() < end:
            time.sleep(0.05)
        check("the peer closes", any("peer picozorro: closed:" in m for m in dlogs), True)
        if os.environ.get("PZSIM_DROP") and a.card:
            check("dropped first tries were retried", any(" 0 retries" in m for m in dlogs if "closed:" in m), False)
        dm.stop = True
    if proc:
        proc.terminate()
        os.close(slave)
    if errors:
        sys.exit(f"FAIL test_picozorro: {errors} of {checks} checks")
    print(f"PASS test_picozorro: {checks} checks")


if __name__ == "__main__":
    main()
