# SPDX-License-Identifier: GPL-3.0-or-later
"""picozorro's peer in the zboard daemon: the real PicoZorro card on a UART, through an SWD debug probe
(a Raspberry Pi Debug Probe gives both: its CDC UART carries the bus cycles, its SWD port flashes the card and,
optionally, streams the firmware's defmt log over RTT). No Nano 20K: run the daemon with --port none.

The card runs the PicoZorro firmware built with `uart-backplane` (flash.sh next to this file): its register
window served on UART0 (GPIO0 TX, GPIO1 RX, 1 Mbaud 8N1), each datagram + CRC-16/CCITT-FALSE, COBS-encoded,
0x00 before and after (PicoZorro firmware/pz-core/src/frame.rs, backplane.rs).

- 'D' + request datagram from the plugin: queued; one request is on the line at a time (stop and wait). With no
  reply within 50 ms plus 20 us a byte it goes again with the same seq, up to RETRIES times: the firmware answers a repeated seq
  from its reply cache without running it again, so a write never lands twice. Then the peer gives up: 'X' +
  seq to the plugin, and the next request goes.
- Replies go to the plugin as 'D' + datagram; so do the card's /INT notifications (flag 0x40), at once.
- The card's log frames ('LO' + text) become daemon log lines ("card: ...").
- 'L' 1 to the plugin once the UART is open, and again after a give-up once the card answers again.

Environment (of the daemon):
  PICOZORRO_UART      the probe's UART, e.g. /dev/serial/by-id/usb-Raspberry_Pi_Debugprobe_...-if01 (required)
  PICOZORRO_BAUD      default 1000000 (the firmware's pz_hal::backplane::BAUD)
  PICOZORRO_RTT_ELF   the flashed picozorro ELF: the peer runs `probe-rs attach` on it and logs the card's defmt
                      lines ("rtt: ..."); unset: no SWD use while running. On a Raspberry Pi Debugprobe whose
                      UART carries the link, RTT polling makes the probe drop UART bytes (measured: 4 % of
                      frames, reads 20x slower); use it for debugging only
  PICOZORRO_PROBE     probe-rs --probe selector (VID:PID[:SERIAL]) when more than one probe is attached
  PICOZORRO_PROBE_RS  the probe-rs binary (default: probe-rs)
"""
import collections
import fcntl
import os
import subprocess
import termios
import time

NEEDS_BRIDGE = False
MAGIC, VERSION = 0x5042, 1
F_IS_REPLY, F_INT = 0x80, 0x40
DGRAM_MAX = 2048
# No reply within RETRY_BASE_S + RETRY_BYTE_S per byte of the request: it goes again. A reply normally comes in
# about 0.7 ms (the longest seen: 23 ms); a full 2 KB datagram is 20 ms each way at 1 Mbaud. Too early a retry
# costs nothing: the firmware answers it from its reply cache, and the late first reply is dropped as stray.
RETRY_BASE_S = 0.05
RETRY_BYTE_S = 20e-6
RETRIES = 3
REPORT_EVERY = 10.0


def crc16(data):
    """CRC-16/CCITT-FALSE: poly 0x1021, init 0xffff (frame.rs)."""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encode(dgram):
    """0x00, COBS(datagram + CRC big-endian), 0x00."""
    data = bytes(dgram) + crc16(dgram).to_bytes(2, "big")
    out = bytearray([0])
    code_at = len(out)
    out.append(0)
    code = 1
    for b in data:
        if b == 0:
            out[code_at] = code
            code_at = len(out)
            out.append(0)
            code = 1
        else:
            out.append(b)
            code += 1
            if code == 0xFF:
                out[code_at] = code
                code_at = len(out)
                out.append(0)
                code = 1
    out[code_at] = code
    out.append(0)
    return bytes(out)


def cobs_decode(buf):
    out = bytearray()
    r, n = 0, len(buf)
    while r < n:
        code = buf[r]
        if code == 0 or r + code > n:
            return None
        out += buf[r + 1:r + code]
        r += code
        if code != 0xFF and r < n:
            out.append(0)
    return bytes(out)


class Deframer:
    """Bytes from the UART into checked datagrams (frame.rs Deframer)."""

    def __init__(self):
        self.buf = bytearray()
        self.errors = 0

    def feed(self, data):
        out = []
        start = 0
        while True:
            z = data.find(b"\x00", start)
            if z < 0:
                self.buf += data[start:]
                if len(self.buf) > 2 * DGRAM_MAX + 16:
                    self.buf.clear()
                    self.errors += 1
                return out
            self.buf += data[start:z]
            start = z + 1
            if not self.buf:
                continue
            d = cobs_decode(self.buf)
            self.buf = bytearray()
            if d is None or len(d) < 2 or crc16(d[:-2]) != int.from_bytes(d[-2:], "big"):
                self.errors += 1
                continue
            out.append(d[:-2])


def open_uart(path, baud):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    speed = getattr(termios, f"B{baud}", None)
    if speed is None:
        os.close(fd)
        raise ValueError(f"no termios speed for {baud} baud")
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0                                                   # iflag: raw
    attrs[1] = 0                                                   # oflag
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL       # cflag: 8N1, no flow control
    attrs[3] = 0                                                   # lflag
    attrs[4] = attrs[5] = speed
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


class Peer:
    def __init__(self, api):
        self.api = api
        path = os.environ.get("PICOZORRO_UART")
        if not path:
            raise ValueError("PICOZORRO_UART is not set (the probe's UART to the card's GPIO0/1)")
        baud = int(os.environ.get("PICOZORRO_BAUD", "1000000"))
        self.fd = open_uart(path, baud)
        self.deframer = Deframer()
        self.queue = collections.deque()       # request datagrams from the plugin, not sent yet
        self.inflight = None                   # [seq, datagram, sent at, tries]
        self.out = bytearray()                 # framed bytes the UART has not taken yet
        self.logline = bytearray()
        self.gave_up = False
        self.stats = collections.Counter()
        self.reported = None
        self.next_report = time.monotonic() + REPORT_EVERY
        self.rtt = None
        self.rtt_buf = bytearray()
        api.watch(self.fd, self.readable)
        api.log(f"card on {path}, {baud} baud")
        api.send(b"L\x01")
        elf = os.environ.get("PICOZORRO_RTT_ELF")
        if elf:
            self.start_rtt(elf)

    # ---- SWD: the firmware's defmt log over RTT
    def start_rtt(self, elf):
        cmd = [os.environ.get("PICOZORRO_PROBE_RS", "probe-rs"), "attach", "--chip", "RP235x"]
        if os.environ.get("PICOZORRO_PROBE"):
            cmd += ["--probe", os.environ["PICOZORRO_PROBE"]]
        cmd.append(elf)
        try:
            self.rtt = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT)
        except OSError as e:
            self.api.log(f"RTT: cannot start {cmd[0]}: {e}")
            return
        fl = fcntl.fcntl(self.rtt.stdout, fcntl.F_GETFL)
        fcntl.fcntl(self.rtt.stdout, fcntl.F_SETFL, fl | os.O_NONBLOCK)
        self.api.watch(self.rtt.stdout.fileno(), self.rtt_readable)
        self.api.log("RTT: " + " ".join(cmd))

    def rtt_readable(self):
        fd = self.rtt.stdout.fileno()
        while True:
            try:
                data = os.read(fd, 4096)
            except (BlockingIOError, InterruptedError):
                break
            if not data:                       # probe-rs ended
                self.api.unwatch(fd)
                self.api.log(f"RTT: probe-rs exited ({self.rtt.wait()})")
                self.rtt = None
                return
            self.rtt_buf += data
        *lines, rest = self.rtt_buf.split(b"\n")
        self.rtt_buf = bytearray(rest)
        for line in lines:
            if line.strip():
                self.api.log("rtt: " + line.decode(errors="replace").rstrip())

    # ---- the plugin's messages
    def on_message(self, msg):
        if msg[:1] != b"D" or len(msg) < 1 + 6 or len(msg) > 1 + DGRAM_MAX:
            raise ValueError(f"not a backplane request: {bytes(msg[:8])!r} ({len(msg)} bytes)")
        d = bytes(msg[1:])
        if int.from_bytes(d[0:2], "big") != MAGIC or d[2] != VERSION:
            raise ValueError(f"not a backplane request: {d[:8]!r}")
        self.queue.append(d)
        self.stats["requests"] += 1
        self.next_request()

    # ---- the UART
    def write(self, data):
        self.out += data
        self.drain()

    def drain(self):
        while self.out:
            try:
                k = os.write(self.fd, self.out)
            except (BlockingIOError, InterruptedError):
                return
            del self.out[:k]

    def next_request(self):
        if self.inflight is None and self.queue:
            d = self.queue.popleft()
            self.inflight = [int.from_bytes(d[4:6], "big"), d, time.monotonic(), 0]
            self.write(encode(d))

    def readable(self):
        while True:
            try:
                data = os.read(self.fd, 65536)
            except (BlockingIOError, InterruptedError):
                return
            if not data:
                return
            for d in self.deframer.feed(data):
                self.datagram(d)

    def datagram(self, d):
        if d[:2] == b"LO":
            self.card_log(d[2:])
            return
        if len(d) < 7 or int.from_bytes(d[0:2], "big") != MAGIC or d[2] != VERSION:
            self.stats["unknown"] += 1
            return
        if self.gave_up:
            self.gave_up = False
            self.api.send(b"L\x01")
        if d[3] & F_INT:
            self.stats["int_notes"] += 1
            self.api.send(b"D" + d)
            return
        if not d[3] & F_IS_REPLY:
            return
        seq = int.from_bytes(d[4:6], "big")
        if self.inflight is None or seq != self.inflight[0]:
            self.stats["stray_replies"] += 1   # the late reply of a retried request
            return
        self.stats["replies"] += 1
        if len(d) > 6 and d[6] != 0:
            self.stats["error_replies"] += 1
        self.inflight = None
        self.api.send(b"D" + d)
        self.next_request()

    def card_log(self, text):
        self.logline += text
        *lines, rest = self.logline.split(b"\n")
        self.logline = bytearray(rest[-512:])
        for line in lines:
            if line.strip():
                self.api.log("card: " + line.decode(errors="replace").rstrip())

    # ---- the daemon's loop
    def poll(self, now):
        self.drain()
        f = self.inflight
        if f is not None and now - f[2] > RETRY_BASE_S + RETRY_BYTE_S * len(f[1]):
            if f[3] < RETRIES:
                f[2], f[3] = now, f[3] + 1
                self.stats["retries"] += 1
                self.write(encode(f[1]))
            else:
                self.stats["given_up"] += 1
                if not self.gave_up:
                    self.api.log(f"seq {f[0]}: no reply after {RETRIES + 1} tries; is the card running the "
                                 "uart-backplane firmware, TX/RX crossed, the baud rate right?")
                self.gave_up = True
                self.inflight = None
                self.api.send(b"X" + f[0].to_bytes(2, "big"))
                self.next_request()
        if now >= self.next_report:
            self.next_report = now + REPORT_EVERY
            if self.stats and self.stats != self.reported:
                self.reported = self.stats.copy()
                self.api.log(self.line())

    def backlog(self):
        return len(self.queue) * 64

    def busy(self):
        return bool(self.queue or self.inflight or self.out)

    def close(self):
        self.api.unwatch(self.fd)
        os.close(self.fd)
        if self.rtt is not None:
            self.api.unwatch(self.rtt.stdout.fileno())
            self.rtt.terminate()
            try:
                self.rtt.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.rtt.kill()
            self.rtt = None
        self.api.log("closed: " + self.line())

    def line(self):
        s = self.stats
        return (f"{s['requests']} requests, {s['replies']} replies ({s['error_replies']} errors), "
                f"{s['retries']} retries, {s['given_up']} given up, {s['int_notes']} /INT notes, "
                f"{s['stray_replies']} stray, {self.deframer.errors} bad frames")
