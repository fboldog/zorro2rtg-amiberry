# Writing a zboard plugin

A zboard plugin is one Zorro expansion board for an emulator, and, when the board talks to outside hardware,
its peer in the zboard daemon. This guide is for writing a new one. Two plugins come with this repo:

| Plugin | What it shows |
|---|---|
| `plugins/zbexample` | The smallest complete plugin: a 64 KB Zorro II I/O board (a packet mailbox, shaped like a network card) whose peer owns its own device (a UDP socket), with the Amiga tool `zbping`. Start from this one |
| `plugins/zorro2rtg` | A large RTG card whose peer drives a real board over the Nano 20K's debug bridge: write mirroring, flow control, input from the real board |

The interface itself is `include/zboard_plugin.h`; read it alongside this guide.

## 1. The two halves

```
emulator (Amiberry)                          zboard daemon (z2bridged.py)               hardware
  hosts/amiberry/zboard_*.cpp  (adapter)       one client per board
  hosts/common/zboard_channel  ---------->     plugins/<name>/peer.py   ------------>  your device
  plugins/<name>/*.cpp         <----------     (Peer class)             <------------  (USB, SWD, network, ...)
  (the board: registers, memory, timing)
```

- **The plugin** is the whole board as the emulated Amiga sees it: Autoconfig data, registers, memory, its
  timing, interrupts and, for an RTG card, frames. It is plain C++ (C interface), names no emulator type and
  never opens a port, socket or file of its own for outside hardware.
- **The host** (an emulator's adapter) maps the board, charges bus time, raises interrupts and gives the plugin a
  **channel**: messages to and from its peer. It opens the channel when the user asks for the daemon
  (`ZBOARD_DAEMON`).
- **The peer** is a Python class the daemon loads for the board (`plugins/<name>/peer.py`). It gets the plugin's
  messages and talks to the hardware. It can use the Nano 20K's debug bridge through the daemon, or a device of
  its own.

A board with no outside hardware needs no peer (`zbp_desc.peer = NULL`).

## 2. Files

```
plugins/<name>/
  plugin.json          what integrate.py needs (section 7)
  <name>_plugin.cpp    the board: zbp_get_api and everything behind it
  peer.py              the daemon half (optional)
  README.md            the board: registers, options, messages, tests
  amiga/               Amiga-side tools or drivers for testing (optional; zbexample has zbping.c)
```

The plugin may have any number of `.cpp` and `.h` files; `plugin.json` lists them.

## 3. The plugin interface in practice

Everything is in `include/zboard_plugin.h`. The parts every plugin needs:

- **`zbp_get_api(version)`**: the one exported symbol, `extern "C"`. Return NULL for a version you do not
  speak. One plugin is compiled into an Amiberry build today, so the symbol is not prefixed.
- **`zbp_desc`**: the board's Autoconfig data and memory layout.
  - `manufacturer`, `product`, `serial`: a real manufacturer ID before release. The examples use the placeholder
    `$5A32`.
  - `er_type`: the whole `er_Type` byte. Bits 7-6: `11` for Zorro II. Bit 5: memory list. Bit 4:
    `ERTF_DIAGVALID`. Bits 2-0: the size code (`001` = 64 KB, `010` = 128 KB, `011` = 256 KB, `100` = 512 KB,
    `101` = 1 MB, `110` = 2 MB, `111` = 4 MB, `000` = 8 MB).
  - `diag_vec`: the DiagArea offset when `ERTF_DIAGVALID` is set (a boot ROM in one of your regions).
  - `size` must match the size code. `zorro` is 2 (version 1 is Zorro II only).
  - `regions`: cover 0 to `size - 1`, sorted, 64 KB aligned. `ZBP_REGION_IO` for registers (every access
    goes through `read`/`write`), `ZBP_REGION_RAM` for memory the CPU may execute from or the host may copy
    (`ptr` gives a direct pointer; writes still go through `write`).
  - `flags`: `ZBP_DESC_DISPLAY` only for an RTG card (it decides which Amiberry adapter serves the board).
    `ZBP_DESC_NOT_RAM` for anything that must never become free memory.
  - `peer`: the peer's name, `[a-z0-9_]`, the same as the plugin directory by convention.
- **`create(host, ctx, options)`**: check `host->struct_size` before using any field past `log`; the channel
  fields (`channel_up`, `send`, `recv`) exist only in a new enough host (`zbexample_plugin.cpp`,
  `HOST_CHANNEL`). Options: a `key=value` string from the host's config (empty in Amiberry today); a plugin
  may read its own environment variables as well.
- **`configured(base, now)`**: Autoconfig placed the board. The channel is open from here on, if the user asked
  for the daemon. It is called again after every reset that reconfigures the board.
- **`reset_board`**: the Amiga's /RESET. Back to power-on state; Autoconfig and `configured` follow.
- **`read` / `write`**: 1 or 2 bytes at a board offset, as the Amiga sees them (a word at an even offset has
  the even byte in bits 15:8). Longwords arrive as two word accesses, high word first.
  - Reads must answer at once from the plugin's own state. A read never waits for the channel (section 4).
  - Call `host->charge_ns` for the bus time an access costs (a Zorro II cycle is about 560 ns; wait states on
    top), if the board's timing matters to software.
- **`tick(now)`**: called at least every `tick_ns` of emulated time (Amiberry: every 64 us line). Advance the
  board's own timing, take the peer's messages (`host->recv` until it returns 0), and return the interrupt
  levels the board asserts now: bit 2 for /INT2, bit 6 for /INT6. Level-triggered: return the bit for as long
  as the board holds the line.
- **`frame`**: RTG cards only.
- **Time**: every call carries the host's emulated time in ns. Treat a backward step or a jump of seconds as a
  resync, not as time passing.
- **Threads**: every call comes on the emulation thread. No exceptions across the C boundary.

## 4. Reads that depend on outside hardware

The model stays in the emulator: a read is answered by the plugin, never forwarded to the hardware. A Unix
socket round trip costs about 2.5 us here, a USB round trip several ms, and a Zorro II cycle 560 ns, so a
forwarded read would stall the emulated CPU for thousands of cycles. Version 1 of the interface has no blocking
request and reply on the channel. A board whose registers depend on outside hardware uses one of two designs:

1. **Mirror the writes** (`zorro2rtg`): the board is mostly written to, and its state is fully known from what
   the Amiga wrote. The plugin keeps a complete model, answers reads from it, and sends the writes so the real
   board ends in the same state. Fits framebuffers, display controllers, write-only hardware.
2. **Pushed state** (`zbexample`): the hardware produces data the Amiga reads (received packets, disk blocks,
   input events, status). The peer pushes it to the plugin as it arrives, and the plugin keeps it until the Amiga
   reads it. Writes that must reach the hardware (a packet to send, a block request) become messages. Status
   bits the Amiga polls (link up, data waiting) are kept in the plugin and updated by the peer's messages.
   - A request whose answer the Amiga waits for (read block N): the plugin sends the request, shows "busy" in a
     status register, and the Amiga's driver polls it or waits for an interrupt; the answer arrives as a
     message, the plugin raises the interrupt in `tick`. The real card works the same way, just faster.
   - Bound every queue in the plugin and count what you drop (`zbexample`: 32 packets).
   - The Amiga sees the hardware's events a few ms late. Usually this does not matter. Drivers with tight
     timeouts may need them raised in emulation.

## 5. The channel

- Messages arrive whole and in order, 1 to `ZBP_MSG_MAX` (65,536) bytes, both ways. Only the plugin and its
  peer read them; choose your own records. A one-letter record type at the start is the convention (`zbexample`:
  `T` packet out, `R` packet in, `L` link state).
- `host->send` queues a message and returns 0 when there is no channel (the message is dropped). When the host's
  outgoing buffer (8 MB in `hosts/common/zboard_channel`) is full, `send` waits, so the emulation slows to the
  peer's pace. Prefer not to get there: if the hardware is slower than the emulated Amiga can write, add flow
  control. `zorro2rtg` has the peer acknowledge each message (`A`), and the plugin merges its writes while more
  than two messages are unacknowledged (`plugins/zorro2rtg/z2rtg_link.h`).
- `host->recv` returns the next message from the peer, 0 when there is none. Call it from `tick`.
- `host->channel_up` is nonzero while the channel is open. The host opens it before `configured()`, and
  reconnects at the next Autoconfig after it loses the daemon.

## 6. The peer

The daemon (`sw/tools/host/z2bridged.py` in the Zorro2-Tang25k project) loads `PEERS/<name>/peer.py` when an
emulator's channel says hello for `<name>`, and makes one `Peer` per channel.

```python
NEEDS_BRIDGE = False        # True (the default): the peer uses the Nano 20K's debug bridge (api.submit)

class Peer:
    def __init__(self, api): ...        # open your device; api.watch its fd; api.send messages if useful
    def on_message(self, msg): ...      # a message from the plugin (bytes); raise on garbage: the client is dropped
    def poll(self, now): ...            # called every daemon loop turn (at least every 5 ms); timers, retries
    def backlog(self): return 0         # work waiting here (any unit); above PEER_BACKLOG words the daemon stops
                                        #   reading this emulator, which then waits (backpressure)
    def busy(self): return False        # True while work is unfinished: after the emulator leaves, the peer is
                                        #   kept until it returns False, at most PEER_LINGER (30 s)
    def close(self): ...                # release the device; the client and its watches are gone after this
```

What the daemon gives the peer (`PeerApi` in `z2bridged.py`):

| Call | Use |
|---|---|
| `api.send(msg)` | a message to the plugin (bytes, up to 65,536) |
| `api.log(text)` | a line in the daemon's output, prefixed with the client and peer |
| `api.watch(fd, on_readable)` / `api.unwatch(fd)` | your device's file descriptor in the daemon's `select` loop; `on_readable()` is called when it is readable. Make the fd nonblocking and read until it would block. An exception drops the client |
| `api.bridge()` | whether the daemon has a Nano 20K bridge |
| `api.submit(cmd, reply_len, cb)` | a debug-bridge command to the Nano (`docs/debug_bridge.md`), `cb(reply)` with its reply or `b"e"`; only with `NEEDS_BRIDGE` |
| `api.outstanding()`, `api.outstanding_bytes()` | this peer's bridge commands not answered yet |

Rules:
- One thread, one loop: never block in a call. A device whose library blocks (a probe library, a USB stack)
  runs in a thread of its own inside the peer, which hands results to the loop through an `os.pipe()` whose read
  end is watched, and takes requests from a `queue.Queue`.
- Run the daemon without a Nano for peers that do not need it: `z2bridged.py --port none`. A peer with
  `NEEDS_BRIDGE` left at True is refused then, with a log line.
- `--peers DIR` loads peers from another directory (the Amiberry bundle has its own copy).

## 7. plugin.json and building

```json
{
    "name": "zbexample",
    "description": "one line",
    "sources": ["zbexample_plugin.cpp"],
    "headers": [],
    "amiberry": {
        "adapter": "expansion",
        "expansion": {"config_name": "zbexample", "name": "zbexample (zboard example)",
                      "manufacturer_name": "Zorro2-Tang25k"}
    }
}
```

- `"adapter": "expansion"`: any Zorro II board that is not an RTG card (`hosts/amiberry/zboard_expansion.cpp`).
  The adapter builds the Autoconfig bytes from `zbp_desc`. Switch the board on in an Amiberry config with
  `<config_name>_rom_file=:ENABLED`. It uses `ROMTYPE_ZBOARD` (`0x00100099`, free in v8.5.0, **VERIFY** before
  proposing it upstream), one board per configuration.
- `"adapter": "rtg"`: an RTG card (`hosts/amiberry/zboard_amiberry.cpp`), with `"rtg"`: the fields of
  Amiberry's `gfxboard` row (see `plugins/zorro2rtg/plugin.json`), which must match `zbp_desc`; select it with
  `gfxcard_type=<config_name>`.
- Build: `hosts/amiberry/build.sh [AMIBERRY_SRC [PLUGIN]]`. `integrate.py` starts from the v8.5.0 checkout each
  time, so switching plugins in one tree works. One plugin per Amiberry build.
- Run with the daemon: start `z2bridged.py` (`--port none` without a Nano), then Amiberry with
  `ZBOARD_DAEMON=default` (or `unix:PATH`).

## 8. Testing

Copy `sw/tests/test_zboard_example.py` (Zorro2-Tang25k project):
- Build the plugin as a shared library and play the host with ctypes (`test_zboard_plugin.py` has the ctypes
  layout of `zboard_plugin.h` and a `Board` class with a fake channel: `sent`, `inbox`).
- Check the description, every register, the messages the plugin sends, what it does with the peer's messages,
  and the interrupt levels from `tick`.
- Run the daemon in the test process without a Nano (`z2bridged.Daemon(None, ...)`), connect as an emulator's
  channel would (hello `H`, version 1, name length, name; then 4-byte big-endian lengths), and check the peer
  end to end with a stand-in for its device.
- In Amiberry: an Amiga-side tool that finds the board with `FindConfigDev` and exercises it (`zbping`), run
  headless with `sw/emu/amiberry/run_test.py --config ...` and `z2amiga.py sh "DH0:tool"`.

## 9. Checklist

- [ ] `plugins/<name>/`: plugin, `plugin.json`, `peer.py` if any, `README.md`
- [ ] `zbp_desc`: Autoconfig fields, size code and `size` agree, regions cover the board, `peer` named
- [ ] reads answered from the plugin's state; bounded queues; drops counted and logged at `destroy`
- [ ] `tick`: messages taken, interrupt levels returned while asserted
- [ ] `reset_board` returns to power-on state
- [ ] works with no channel (no daemon): the board still configures, outside data simply never arrives
- [ ] peer: `NEEDS_BRIDGE`, nonblocking, `watch` for its devices, `busy`/`backlog` honest, `close` releases
- [ ] tests: plugin through ctypes, peer through the daemon, Amiga tool in Amiberry
- [ ] a real manufacturer ID before release

## 10. Notes for a microcontroller board over SWD (for example an RP2350 Zorro card)

- SWD gives a debugger the microcontroller's memory and core, not the Zorro register window: on such a card
  the registers are served by firmware (PIO state machines answering bus cycles). So the peer cannot simply
  forward register accesses over SWD. Two ways:
  - **A mailbox in the firmware's RAM**: the firmware has a buffer the peer reads and writes over SWD (with a
    probe library such as pyOCD, or OpenOCD's TCL port) as a second register-window client. This needs firmware
    support; check whether the firmware has such a debug backend.
  - **One level up**: the plugin models the card's register window itself, and the peer moves the card's data
    (packets, disk blocks, USB reports) to and from the real card with whatever protocol its firmware offers.
- Either way, design by section 4: pushed state for packets, blocks and reports, requests as messages, status
  kept in the plugin.
- A probe library blocks: run it in a thread inside the peer and hand results to the daemon's loop through a
  watched pipe (section 6).
- The card's own boot ROM: `ERTF_DIAGVALID` in `er_type`, `diag_vec`, and the ROM image in an IO region the
  plugin serves.
- /INT2 or /INT6, as the card's jumper selects: bit 2 or bit 6 from `tick`.
