# Zorro board plugins for Amiberry

This repo has two parts:
- **A board plugin interface** (`include/zboard_plugin.h`): any Zorro expansion board behind a versioned C
  interface that names no emulator type. An RTG card is one kind of board; a RAM, I/O or controller board
  fits the same interface, since producing frames is optional (`ZBP_DESC_DISPLAY`).
- **The Zorro2RTG board as its first plugin**, with an adapter for
  [Amiberry](https://github.com/BlitterStudio/amiberry) v8.5.0.

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
| `hosts/amiberry/zboard_amiberry.cpp` | Amiberry's adapter, using only the plugin interface and naming no board: memory banks, CPU time and stalls, /INT2, display switching, frame copy |
| `hosts/amiberry/integrate.py` | Copies the adapter and the plugin into an Amiberry v8.5.0 tree and makes the small table edits (safe to run again) |
| `hosts/amiberry/build.sh` | Clones v8.5.0 if needed, integrates, builds with CMake and Ninja |
| `plugins/zorro2rtg/` | The Zorro2RTG board as a plugin (its own `README.md`) |

## The plugin interface

`include/zboard_plugin.h` (version 1) is the only interface between the board and the emulator. One function,
`zbp_get_api(version)`, returns the board's description (Autoconfig bytes and memory regions) and its calls:
create and destroy, `configured`, `reset_board`, 8- and 16-bit `read` and `write`, a direct pointer into its RAM
regions, `tick` (timing and side channels; returns the interrupt levels it requests) and `frame` (the finished
picture). The host passes its emulated time in ns with every call and supplies three callbacks: charge bus
time, switch the display, log. Version 1 is Zorro II only and has no save states, since a board in the loop
depends on the real board's state.

What is generic today and what is not:

| Part | Generic | Limit |
|---|---|---|
| `zboard_plugin.h` | Yes: any Zorro board, emulator-neutral | Version 1: Zorro II only, 8- and 16-bit accesses, no save states. The `zorro` field already takes 3, but Zorro III needs a later version (32-bit accesses) |
| Amiberry adapter (`hosts/amiberry/zboard_amiberry.cpp`) | Names no board: bank names come from the plugin's regions, the label from its description | An RTG board adapter: registers as a `gfxboard_func` (`zboard_func`), uses the RTG board slots (`rtgboards[]`, `MAX_RTG_BOARDS`) and `gfxboard_set`. A board without `ZBP_DESC_DISPLAY` runs but never shows a picture; a board that is not an RTG card needs a second adapter on `expansion.cpp` or an Amiberry loader. One plugin at a time is compiled in |
| `hosts/amiberry/integrate.py` | No | Adds Zorro2RTG's Autoconfig row and boot ROM fields to `gfxboard.cpp`, its board ID to a check in `expansion.cpp`, and its sources to the CMake list |
| `plugins/zorro2rtg/` | No | The Zorro2RTG board itself |

Amiberry has no loader for expansion boards, so the board is compiled in, like its other gfxboards, and
`integrate.py` adds its Autoconfig row to `gfxboard.cpp`, which must match the plugin's description. A loader
that builds the row from the description is an Amiberry change for later.

## Build

```
hosts/amiberry/build.sh [AMIBERRY_SRC]      # default ~/amiberry-zorro2/src; the binary is AMIBERRY_SRC/build/amiberry
```

This builds Amiberry with the plugin in `plugins/zorro2rtg`, today the only one. Select it in the config with
`gfxcard_type=Zorro2RTG` and `gfxcard_size=4` (or the GUI's RTG board list); it is a 4 MB Zorro II board.

## Plugins

| Plugin | Board |
|---|---|
| [`plugins/zorro2rtg`](plugins/zorro2rtg/README.md) | Zorro2RTG, a Zorro II RTG card: its options, the board-in-the-loop link and its tests |

## Tests

The host tests live in the main project (`make -C sim host`). `sw/tests/test_zboard_plugin.py` drives a board
through the plugin interface, as a host uses it; the Zorro2RTG plugin's own tests are in its `README.md`.
