# Zorro2RTG plugin

The first board behind `include/zboard_plugin.h` (see the repo's `README.md`). Zorro2RTG is a Zorro II RTG
card built on a Sipeed Tang Nano 20K (the Zorro2-Tang25k project). The emulated board implements the card's register map:
- registers, palette and cursor image;
- 8 MB VRAM with BANK;
- scanout in every gateware mode, RGB565 and CLUT8;
- the hardware cursor and the blitter;
- the USB input queue and the boot ROM window.

Its own Picasso96 and PrismRTG drivers run on it unchanged. It can also mirror everything the emulated Amiga
writes to a real Nano 20K, so the real card's HDMI output shows the emulated Workbench ("board in the loop").

## Files

| File | What it is |
|---|---|
| `z2rtg_plugin.cpp` | The board as a plugin (`zbp_get_api`): the model, its timing and bus time, the write stream, USB input, boot ROM, traces and finished frames; its options |
| `z2rtg_core.{h,cpp}` | The board model, independent of any emulator |
| `z2rtg_capi.cpp` | C interface to the model, for host tests |
| `z2rtg_link.{h,cpp}` | Board in the loop, the plugin's half: the write stream as messages for the peer, through the host's channel |
| `peer.py` | Board in the loop, the daemon's half: the messages as debug-bridge commands to a real Nano 20K |
| `zorro2rtg_regs.h` | Register map header, a copy of the file generated in the main project (`sw/gen_regs.py`); the peer reads it too |

## Environment options

| Variable | Effect |
|---|---|
| `Z2RTG_INPUT` | `FIFO`: single-wire link frames from a named pipe. `bridge`: events from the real board's USB input queue (board in the loop) |
| `Z2RTG_BOOTROM` | The boot ROM image for the BOOTROM window |
| `Z2RTG_BUS=0` | No Zorro II bus time charged per access |
| | Each variable `Z2RTG_<KEY>` is also the plugin option `key=value` (create's option string, which the Amiberry adapter leaves empty for now) |
| `Z2RTG_TRACE`, `Z2RTG_TRACE_BLT`, `Z2RTG_TRACE_TPL`, `Z2RTG_TRACE_FILE`, `Z2RTG_NOCURSOR` | Debugging |

Board in the loop needs no option here: it runs when the host opens the channel to the peer (in Amiberry,
`ZBOARD_DAEMON`). `Z2RTG_BRIDGE` and `Z2RTG_BRIDGE_BAUD` are gone (2026-11-20): the plugin no longer opens
the serial port or the daemon's socket itself.

## Board in the loop

The plugin sends the emulated board's write stream to its peer in the daemon, which sends it to a real
Nano 20K, so the real card's HDMI output shows the emulated Workbench. Reads are always answered by the
model and never forwarded, since a USB round trip takes about 1 ms.

Messages, plugin to peer: a sequence of records, up to 32 KB a message.

| Record | Bytes | Meaning |
|---|---|---|
| New stream | `Z e` | the start of a stream (epoch e): the peer counts acknowledgements from 0 |
| Write run | `W a2 a1 a0 nH nL` + n big-endian words | n consecutive words from board offset a (n up to 4096) |
| Barrier | `B` | the real `BLT_BUSY` must clear before what follows |
| Input | `I` | poll the real board's USB input queue from now on |

Peer to plugin, one record per message:

| Record | Bytes | Meaning |
|---|---|---|
| Event | `E t p0 p1 p2 p3 p4 p5` | one input event (`IN_EV0-3`: type, then six bytes) |
| Acknowledgement | `A e n3 n2 n1 n0` | n messages of stream e passed to the daemon so far |

The plugin (`z2rtg_link.cpp`) sends register writes in order. A VRAM write only marks its word as changed,
and a flush sends the model's current value of each changed word, in address order, after the registers.
Changed VRAM goes out only while at most two messages are unacknowledged, so when the real board falls
behind (a window outline drawn by the CPU, one word per row) the changes merge in the plugin: the emulated
Amiga never waits, and the real board skips the intermediate pictures but ends exact. Before a blitter start
or a `BANK` write reaches the model, every changed word goes out regardless, with its value from before,
since the model's blitter finishes at once. The plugin puts a barrier after a blitter job of 16 KB or 64 rows
or more, and keeps `IN_*` writes local while input comes from the real board.

The peer (`peer.py`) turns runs into bridge bursts of 256 words, acknowledges each message once its last
record has gone to the daemon, waits at a barrier for the real `BLT_BUSY`, polls the input queue every 5 ms
(no bursts while a poll runs), and resends refused bursts once the daemon is back in step. The daemon queues
its commands with every other client's (the main project's `docs/debug_bridge.md`), so a host tool that holds
the board makes them wait; while more than 524,288 words wait in the peer, the daemon stops reading the
emulator, whose channel then fills and makes the emulated CPU wait (with the acknowledgements, only blitter
jobs on a held board get there).

The bridge protocol the peer speaks (the main project's `docs/debug_bridge.md`):

| Command | Bytes sent | Reply |
|---|---|---|
| Ping | `?` | `Z` |
| Write word | `W a2 a1 a0 dH dL` | `k` |
| Read word | `R a2 a1 a0` | 2 bytes |
| Burst write | `B a2 a1 a0 n` + n+1 words | `k` |
| Burst read | `S a2 a1 a0 n` | n+1 words |

Values are big-endian; the offset is the 24-bit offset in the 4 MB board window, and `e` means refused.

## Tests

The host tests live in the main project (`make -C sim host`):
- `sw/tests/test_z2rtg_core.py`: the model;
- `sw/tests/test_zboard_plugin.py`: the board through the plugin interface, as a host uses it, and its
  messages through a fake channel;
- `sw/tests/test_z2rtg_link.py`: input through the peer during a long redraw;
- `sw/tests/test_z2bridged.py`: the plugin, the host channel, the daemon and the peer against a fake bridge
  (with a dragged outline: the emulator never waits and the last outline arrives exact),
  together with a second client: exact areas, a resent burst, a hold, backpressure.
