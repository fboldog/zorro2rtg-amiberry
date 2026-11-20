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
| `z2rtg_plugin.cpp` | The board as a plugin (`zbp_get_api`): the model, its timing and bus time, the link, USB input, boot ROM, traces and finished frames; its options |
| `z2rtg_core.{h,cpp}` | The board model, independent of any emulator |
| `z2rtg_capi.cpp` | C interface to the model, for host tests |
| `z2rtg_link.{h,cpp}` | Board-in-the-loop link: the model's write stream to a real board, over its serial port or a Unix socket |
| `zorro2rtg_regs.h` | Register map header, a copy of the file generated in the main project (`sw/gen_regs.py`) |

## Environment options

| Variable | Effect |
|---|---|
| `Z2RTG_BRIDGE` | Mirror every write to a real board. `/dev/ttyUSBn` uses the Nano's serial port directly. `unix:PATH`, or a path that is a socket, goes through `z2bridged.py`, so other programs can use the board meanwhile |
| `Z2RTG_BRIDGE_BAUD` | Serial port only, default 3000000 |
| `Z2RTG_INPUT` | `FIFO`: single-wire link frames from a named pipe. `bridge`: events from the real board's USB input queue |
| `Z2RTG_BOOTROM` | The boot ROM image for the BOOTROM window |
| `Z2RTG_BUS=0` | No Zorro II bus time charged per access |
| | Each variable `Z2RTG_<KEY>` is also the plugin option `key=value` (create's option string, which the Amiberry adapter leaves empty for now) |
| `Z2RTG_TRACE`, `Z2RTG_TRACE_BLT`, `Z2RTG_TRACE_TPL`, `Z2RTG_TRACE_FILE`, `Z2RTG_NOCURSOR` | Debugging |

## The link's socket

The socket carries the Nano 20K debug bridge's own byte protocol (the main project's `docs/debug_bridge.md`):

| Command | Bytes sent | Reply |
|---|---|---|
| Ping | `?` | `Z` |
| Write word | `W a2 a1 a0 dH dL` | `k` |
| Read word | `R a2 a1 a0` | 2 bytes |
| Burst write | `B a2 a1 a0 n` + n+1 words | `k` |
| Burst read | `S a2 a1 a0 n` | n+1 words |

Values are big-endian; the offset is the 24-bit offset in the 4 MB board window, and `e` means refused. The
same link code therefore drives a serial port or the daemon unchanged. The daemon adds `L` and `U`, with which a
host tool holds the board for a while; the link never sends them, but its commands then wait in the daemon, so
through a socket it waits up to 10 minutes for a reply and the emulation stalls meanwhile. A refused command
gets a lone `e`, also where a read's data should be, and the link takes that as the whole reply. The link's
queue is bounded (524,288 writes): when it is full, the emulated board's next write waits. On exit, the link
gives the queue 2 s, then stops waiting for replies.

The daemon `sw/tools/host/z2bridged.py` lives in the main project. It owns the serial port and passes whole
commands from several clients to it in order. When the bridge refuses something, it answers `e` for every
command that was dropped.

Reads are always answered by the emulated board and never forwarded, since a USB round trip takes about 1 ms.

## Tests

The host tests live in the main project (`make -C sim host`):
- `sw/tests/test_z2rtg_core.py`: the model;
- `sw/tests/test_zboard_plugin.py`: the board through the plugin interface, as a host uses it;
- `sw/tests/test_z2rtg_link.py`: input polling during a long redraw;
- `sw/tests/test_z2bridged.py`: the link through the daemon, together with a second client.
