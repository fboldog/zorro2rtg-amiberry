# SPDX-License-Identifier: GPL-3.0-or-later
"""Zorro2RTG's peer in the zboard daemon (z2bridged.py in the Zorro2-Tang25k project): the half of board in
the loop that talks to the real Nano 20K over its UART debug bridge (docs/debug_bridge.md). The plugin's half
(z2rtg_link.h) sends the emulated board's write stream as messages; this peer turns them into bridge commands.

- Writes: each 'W' run becomes bursts ('B') of up to BURST words, in the stream's order. At most OUT_BYTES
  and OUT_CMDS of this peer's commands wait in the daemon at a time, so an input poll never queues behind a
  whole redraw. Both limits sit a little above the daemon's UART window (WINDOW_BYTES, WINDOW_CMDS), which
  keeps many short commands in flight, so 1-word bursts (a vertical line drawn by the CPU, as in a window
  outline) are not bound by the USB round trip.
- Barrier ('B'): nothing after it goes out until every earlier burst is acknowledged and the real STATUS
  reads BLT_BUSY clear (up to BLIT_POLLS reads).
- Input ('I'): every INPUT_EVERY the peer reads IN_STATUS..IN_EV3 in one burst read; an event found is sent
  to the plugin ('E' and its seven bytes) and popped on the real board (IN_POP) together with the next read,
  so each event costs one USB round trip. No writes go out while a poll runs, so the real queue (16 events)
  empties between bursts even during a long redraw. A pop is only sent for an event already read. Overflow and frame
  errors are counted and cleared (W1C).
- Acknowledgements: after the last record of each message has gone to the daemon, the peer counts it and
  sends 'A' with the count of this stream ('Z' starts one). The plugin sends changed VRAM only while few
  messages are unacknowledged, so the backlog stays short and the emulated Amiga never waits on a slow
  link: the changes merge in the plugin instead.
- Errors: the bridge drops everything in flight after a broken command, and the daemon answers 'e' for each
  of this peer's commands. The refused bursts are sent again in order once every reply is in (the daemon
  holds them until the bridge is back in step), up to RESEND_TRIES times. A resent BLT_OP runs its job twice,
  which is logged.

Messages: see z2rtg_link.h.
"""
import collections
import pathlib
import re
import time

HERE = pathlib.Path(__file__).resolve().parent
REGS = {k: int(v, 16) for k, v in re.findall(r"#define Z2R_(\w+) 0x([0-9A-Fa-f]+)U?",
                                            (HERE / "zorro2rtg_regs.h").read_text())}
STATUS, BLT_BUSY, BLT_OP = REGS["REG_STATUS"], REGS["STATUS_BLT_BUSY"], REGS["REG_BLT_OP"]
IN_STATUS, IN_POP, IN_POP_POP = REGS["REG_IN_STATUS"], REGS["REG_IN_POP"], REGS["IN_POP_POP"]
IN_AVAIL, IN_OVF, IN_FERR = REGS["IN_STATUS_AVAIL"], REGS["IN_STATUS_OVERFLOW"], REGS["IN_STATUS_FRAME_ERR"]

BURST = 256            # words per bridge burst
OUT_BYTES = 5 * 517    # bytes of this peer's commands waiting in the daemon (its UART window is 4 * 517)
OUT_CMDS = 40          # and commands (its UART window is 32)
INPUT_EVERY = 0.005    # input poll period
INPUT_LOOP = 32        # events taken in one poll at most
RESEND_TRIES = 3
BLIT_POLLS = 2000      # a 1280x720 copy takes about 13 ms
REPORT_EVERY = 10.0


def a3(off):
    return (off & 0xFFFFFF).to_bytes(3, "big")


class Peer:
    def __init__(self, api):
        self.api = api
        self.q = collections.deque()      # [b'W', offset, data] (data: big-endian words still to send), [b'B'],
                                          # or [b'M', epoch]: the end of a message
        self.epoch = None                 # the plugin's stream ('Z')
        self.done = self.done_sent = 0    # its messages passed to the daemon, and the count last sent ('A')
        self.backlog_words = 0
        self.sent = collections.deque()   # bursts submitted and not answered yet: [command, tries]
        self.failed = []                  # refused bursts, in order, to send again once all replies are in
        self.recovering = False
        self.barrier = None               # None, 'drain' (earlier bursts first) or 'poll' (a STATUS read out)
        self.polls_left = 0
        self.input = False
        self.in_busy = False              # an input read (and its pop) is out
        self.in_loop = 0
        self.in_since = 0.0               # when the input poll now out was sent
        self.next_input = 0.0
        self.stats = collections.Counter()
        self.reported = None
        self.next_report = time.monotonic() + REPORT_EVERY

    # ---- the daemon's calls
    def on_message(self, msg):
        i, n = 0, len(msg)
        while i < n:
            k = msg[i:i + 1]
            if k == b"W":
                if n - i < 6:
                    raise ValueError("a W record cut short")
                off, cnt = int.from_bytes(msg[i + 1:i + 4], "big"), int.from_bytes(msg[i + 4:i + 6], "big")
                data = msg[i + 6:i + 6 + 2 * cnt]
                if cnt == 0 or len(data) != 2 * cnt:
                    raise ValueError(f"a W record of {cnt} words with {len(data)} bytes")
                self.q.append([b"W", off & ~1, data])
                self.backlog_words += cnt
                i += 6 + 2 * cnt
            elif k == b"B":
                self.q.append([b"B"])
                i += 1
            elif k == b"Z":
                if n - i < 2:
                    raise ValueError("a Z record cut short")
                self.epoch, self.done, self.done_sent = msg[i + 1], 0, -1
                i += 2
            elif k == b"I":
                if not self.input:
                    self.api.log("USB input from the real board's queue")
                self.input = True
                self.next_input = 0.0
                i += 1
            else:
                raise ValueError(f"unknown record {k!r}")
        self.q.append([b"M", self.epoch])

    def backlog(self):
        return self.backlog_words

    def busy(self):
        return bool(self.q or self.sent or self.failed or self.barrier or self.in_busy)

    def poll(self, now):
        self.report(now)
        self.run(now)
        self.acknowledge()

    def run(self, now):
        if self.recovering:
            if self.sent:
                return                    # every refused reply first
            self.recovering = False
            retry, self.failed = self.failed, []
            for cmd, tries in retry:
                if tries >= RESEND_TRIES:
                    self.api.log(f"{len(retry)} bursts dropped after {RESEND_TRIES} attempts; the real board is "
                                 "out of step")
                    break
                off = int.from_bytes(cmd[1:4], "big")
                if off <= BLT_OP < off + 2 * (cmd[4] + 1):
                    self.api.log("resending a burst with BLT_OP: the job may run twice")
                self.burst(cmd, tries + 1)
                self.stats["resent"] += 1
            return
        if self.input and not self.in_busy and now >= self.next_input:
            self.in_busy, self.in_loop, self.in_since = True, 0, now
            self.stats["in_polls"] += 1
            self.api.submit(b"S" + a3(IN_STATUS) + bytes([5]), 12, self.input_reply)
        if self.barrier == "drain" and not self.sent:
            self.barrier, self.polls_left = "poll", BLIT_POLLS
            self.stats["polls"] += 1
            self.api.submit(b"R" + a3(STATUS), 2, self.status_reply)
        if self.barrier or self.in_busy:
            return                        # an input poll takes the whole queue before more writes go out
        while self.q and self.api.outstanding_bytes() < OUT_BYTES and self.api.outstanding() < OUT_CMDS:
            item = self.q[0]
            if item[0] == b"M":
                self.q.popleft()
                if item[1] is not None and item[1] == self.epoch:
                    self.done += 1
                continue
            if item[0] == b"B":
                self.q.popleft()
                self.barrier = "drain"
                return self.run(now)
            _, off, data = item
            n = min(BURST, len(data) // 2)
            self.burst(b"B" + a3(off) + bytes([n - 1]) + data[:2 * n], 0)
            self.backlog_words -= n
            self.stats["words"] += n
            if len(data) > 2 * n:
                item[1], item[2] = off + 2 * n, data[2 * n:]
            else:
                self.q.popleft()

    def acknowledge(self):
        if self.epoch is not None and self.done != self.done_sent:
            self.done_sent = self.done
            self.api.send(b"A" + bytes([self.epoch]) + self.done.to_bytes(4, "big"))

    def close(self):
        self.report(None)

    # ---- bridge commands and their replies
    def burst(self, cmd, tries):
        entry = [cmd, tries]
        self.sent.append(entry)
        self.api.submit(cmd, 1, lambda r, e=entry: self.burst_reply(e, r))

    def burst_reply(self, entry, r):
        if self.sent and self.sent[0] is entry:
            self.sent.popleft()
        elif entry in self.sent:
            self.sent.remove(entry)
        if r == b"k":
            self.stats["bursts"] += 1
            return
        if not self.recovering:
            self.stats["errors"] += 1
            self.recovering = True
        self.failed.append(entry)

    def status_reply(self, r):
        if len(r) != 2:                   # refused: read again once the daemon is back in step
            self.stats["errors"] += 1
            self.barrier = "drain"
            return
        if not int.from_bytes(r, "big") & BLT_BUSY:
            self.barrier = None
            return
        self.polls_left -= 1
        if self.polls_left <= 0:
            self.api.log("the real BLT_BUSY did not clear")
            self.barrier = None
            return
        self.api.submit(b"R" + a3(STATUS), 2, self.status_reply)

    def input_reply(self, r):
        now = time.monotonic()
        if len(r) != 12:                  # refused ('e'): try again after the bridge's gap
            self.stats["input_errors"] += 1
            self.in_busy, self.next_input = False, now + 0.03
            return
        st = int.from_bytes(r[0:2], "big")
        w1c = st & (IN_OVF | IN_FERR)
        if w1c:
            if st & IN_OVF:
                self.stats["overflows"] += 1
            if st & IN_FERR:
                self.stats["frame_errors"] += 1
            if self.stats["overflows"] + self.stats["frame_errors"] == 1:
                self.api.log("real board input " + ("overflow: events were lost" if st & IN_OVF else "frame error")
                             + " (counted in the progress line from now on)")
            self.api.submit(b"W" + a3(IN_STATUS) + w1c.to_bytes(2, "big"), 1, lambda _r: None)
        if not st & IN_AVAIL:
            self.in_busy, self.next_input = False, now + INPUT_EVERY
            return
        # EV0 = type:p0, EV1 = p1:p2, EV2 = p3:p4, EV3 = p5:-
        self.api.send(b"E" + bytes(r[4:11]))
        self.stats["events"] += 1
        self.in_loop += 1
        self.api.submit(b"W" + a3(IN_POP) + IN_POP_POP.to_bytes(2, "big"), 1, lambda _r: None)
        if self.in_loop < INPUT_LOOP:
            self.api.submit(b"S" + a3(IN_STATUS) + bytes([5]), 12, self.input_reply)
        else:
            self.in_busy, self.next_input = False, now

    # ---- progress
    def report(self, now):
        if now is not None and now < self.next_report:
            return
        if now is not None:
            self.next_report = now + REPORT_EVERY
        snap = (dict(self.stats), self.backlog_words)
        if snap == self.reported and now is not None:
            return
        self.reported = snap
        s = self.stats
        line = (f"{s['words']} words in {s['bursts']} bursts, {s['polls']} blitter waits, {s['errors']} errors, "
                f"{s['resent']} bursts resent, {self.backlog_words} words waiting")
        if self.input:
            line += (f"; input {s['events']} events, {s['overflows']} overflows, {s['frame_errors']} frame errors, "
                     f"{s['input_errors']} refused reads, {s['in_polls']} polls")
            if self.in_busy and now is not None and now - self.in_since > 1.0:
                line += f" (a poll out for {now - self.in_since:.1f} s, {self.api.outstanding()} commands out)"
        self.api.log(line if now is not None else "closed: " + line)
