# Zorro board plugins for Amiberry

This repo has three parts:
- **A board plugin interface** (`include/zboard_plugin.h`): any Zorro expansion board behind a versioned C
  interface that names no emulator type. An RTG card is one kind of board; a RAM, I/O or controller board
  fits the same interface, since producing frames is optional (`ZBP_DESC_DISPLAY`).
- **Hosts:** two adapters for [Amiberry](https://github.com/BlitterStudio/amiberry) v8.5.0 (RTG cards, and every
  other Zorro II board), and the channel every host uses to reach the zboard daemon.
- **Plugins:** each board as a plugin for the emulator and, when it talks to outside hardware, a peer for
  the daemon: Zorro2RTG, and `zbexample`, the example to start a new plugin from.

**Writing a plugin:** [`docs/writing-a-plugin.md`](docs/writing-a-plugin.md) (the interface in practice, reads
that depend on hardware, the channel, the peer's contract, `plugin.json`, building, testing, a checklist).

A plugin never opens a port or a socket. The host holds one connection per board to the daemon
(`z2bridged.py` in the Zorro2-Tang25k project) and gives the plugin a message channel over it; in the daemon,
the board's peer gets the messages and talks to the hardware. The board's model stays in the emulator, so
reads never wait for a round trip: only messages cross.

```
Amiberry                                    z2bridged.py                      hardware
  hosts/amiberry/zboard_amiberry.cpp          zboard client
    hosts/common/zboard_channel.cpp  ------>    plugins/zorro2rtg/peer.py  -->  Nano 20K (UART bridge)
  plugins/zorro2rtg (model, stream) <------     (bursts, blitter waits, input)
```

Zorro2RTG is a Zorro II RTG card built on a Sipeed Tang Nano 20K (the Zorro2-Tang25k project); its
emulated board runs the card's own Picasso96 and PrismRTG drivers unchanged (`plugins/zorro2rtg/README.md`).

The repo's name, `zorro2rtg-amiberry`, comes from the time before the interface, when it held only the
Zorro2RTG board; it may change when the plugin loader is proposed to Amiberry upstream.

Paths such as `docs/registers.md` or `sw/p96/` in the comments refer to the Zorro2-Tang25k project, which
uses this repo as the submodule `sw/emu/zorro2rtg-amiberry`.

Licence: GPL-3.0-or-later (`LICENSE`).

## Layout

| Path | What it is |
|---|---|
| `include/zboard_plugin.h` | The plugin interface: a Zorro board behind a versioned C interface that names no emulator type |
| `hosts/common/zboard_channel.{h,cpp}` | The host's connection to the daemon for one board: hello, framed messages both ways, a bounded outgoing buffer |
| `hosts/amiberry/zboard_amiberry_common.h` | What both Amiberry adapters share, naming no board: memory banks, CPU time and stalls, /INT2 and /INT6, the daemon channel |
| `hosts/amiberry/zboard_amiberry.cpp` | Amiberry's adapter for RTG cards (`ZBP_DESC_DISPLAY`): a gfxboard, display switching, frame copy |
| `hosts/amiberry/zboard_expansion.cpp` | Amiberry's adapter for every other Zorro II board: an expansion board whose Autoconfig bytes come from the plugin's description |
| `hosts/amiberry/integrate.py` | Puts one plugin into an Amiberry v8.5.0 checkout from its `plugin.json`, starting again from v8.5.0 each time (plugins can be switched) |
| `hosts/amiberry/build.sh` | Clones v8.5.0 if needed, integrates a plugin, builds with CMake and Ninja |
| `plugins/<name>/plugin.json` | What `integrate.py` needs: sources, headers, which adapter, its table row |
| `plugins/zorro2rtg/` | The Zorro2RTG board as a plugin, and its peer for the daemon (`peer.py`; its own `README.md`) |
| `plugins/zbexample/` | The example board: a 64 KB packet mailbox whose peer owns its own device (a UDP socket), with an Amiga tool |
| `docs/writing-a-plugin.md` | How to write a plugin and its peer |

## The plugin interface

`include/zboard_plugin.h` (version 1) is the only interface between the board and the emulator. One function,
`zbp_get_api(version)`, returns the board's description (Autoconfig bytes and memory regions) and its calls:
create and destroy, `configured`, `reset_board`, 8- and 16-bit `read` and `write`, a direct pointer into its RAM
regions, `tick` (timing and side channels; returns the interrupt levels it requests) and `frame` (the finished
picture). The host passes its emulated time in ns with every call and supplies its callbacks: charge bus
time, switch the display, log, and the channel to the board's peer (`channel_up`, `send`, `recv`). The
description names the peer (`peer`, for example `zorro2rtg`). Version 1 is Zorro II only and has no save
states, since a board in the loop depends on the real board's state. Structs grow at their end and start with
their size, so a plugin built for a smaller `zbp_host` still works.

The daemon protocol (`hosts/common/zboard_channel.h`): the host says hello (`H`, version 1, the peer's name)
and the daemon answers `H`, or `e` when it has no such peer. Then both ways carry messages, a 4-byte
big-endian length and the bytes, which only the plugin and its peer read. Amiberry connects when
`ZBOARD_DAEMON` is set (`unix:PATH`, a path, or `default`).

What is generic today and what is not:

| Part | Generic | Limit |
|---|---|---|
| `zboard_plugin.h` | Yes: any Zorro board, emulator-neutral | Version 1: Zorro II only, 8- and 16-bit accesses, no save states. The `zorro` field already takes 3, but Zorro III needs a later version (32-bit accesses) |
| Amiberry adapters (`hosts/amiberry/`) | Name no board: bank names come from the plugin's regions, the label from its description | RTG cards run as a gfxboard (`zboard_func`, the RTG board slots, `gfxboard_set`), with their row in `gfxboard.cpp` from `plugin.json`; every other board as an expansion board (`zboard_expansion.cpp`, `ROMTYPE_ZBOARD`, switched on with `<config_name>_rom_file=:ENABLED`), Zorro II, one per configuration. One plugin at a time is compiled in |
| `hosts/amiberry/integrate.py` | Yes, from `plugin.json` | Needs a git checkout of v8.5.0; the anchors it edits are that version's |
| `hosts/common/zboard_channel` | Yes: any board, any host | POSIX, a Unix socket |
| `plugins/zorro2rtg/` | No | The Zorro2RTG board itself, and its peer |

Amiberry has no loader for expansion boards, so the board is compiled in, like its other boards. For an RTG
card `integrate.py` adds its Autoconfig row to `gfxboard.cpp` from `plugin.json`, which must match the plugin's
description; the expansion adapter builds the Autoconfig bytes from the description itself. A loader that does
this for RTG cards too is an Amiberry change for later.

## Build

```
hosts/amiberry/build.sh [AMIBERRY_SRC [PLUGIN]]   # default ~/amiberry-zorro2/src and zorro2rtg; the binary is AMIBERRY_SRC/build/amiberry
```

With Zorro2RTG, select it in the config with `gfxcard_type=Zorro2RTG` and `gfxcard_size=4` (or the GUI's RTG
board list); it is a 4 MB Zorro II board. With `zbexample` (or another expansion-adapter plugin), add
`zbexample_rom_file=:ENABLED` to the config.

## Plugins

| Plugin | Board |
|---|---|
| [`plugins/zorro2rtg`](plugins/zorro2rtg/README.md) | Zorro2RTG, a Zorro II RTG card: its options, its messages and peer (board in the loop) and its tests |
| [`plugins/zbexample`](plugins/zbexample/README.md) | The example: a 64 KB Zorro II packet mailbox, its peer with its own device, and `zbping` |

## Tests

The host tests live in the main project (`make -C sim host`). `sw/tests/test_zboard_plugin.py` drives a board
through the plugin interface, as a host uses it, with a fake channel; `sw/tests/test_z2bridged.py` runs a
test host (`sw/tests/zboard_testhost.cpp`, the plugin and `hosts/common/zboard_channel.cpp`) against the daemon
and the peer; `sw/tests/test_zboard_example.py` checks the example board's plugin and, through the daemon
without a Nano, its peer. The plugins' own tests are in their `README.md`.
