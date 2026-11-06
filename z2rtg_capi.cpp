// C interface to the Zorro2RTG core model, for the host test (sw/tests/test_z2rtg_core.py).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "z2rtg_core.h"

extern "C" {
void *z2_new() { return new Z2rtgCore(); }
void z2_free(void *c) { delete static_cast<Z2rtgCore *>(c); }
unsigned z2_rd16(void *c, unsigned off) { return static_cast<Z2rtgCore *>(c)->read16(off); }
void z2_wr16(void *c, unsigned off, unsigned v, unsigned be) { static_cast<Z2rtgCore *>(c)->write16(off, v, be); }
unsigned z2_rd8(void *c, unsigned off) { return static_cast<Z2rtgCore *>(c)->read8(off); }
void z2_wr8(void *c, unsigned off, unsigned v) { static_cast<Z2rtgCore *>(c)->write8(off, v); }
int z2_hsync(void *c) { return static_cast<Z2rtgCore *>(c)->hsync(); }
int z2_showing(void *c) { return static_cast<Z2rtgCore *>(c)->showing(); }
unsigned char *z2_vram(void *c) { return static_cast<Z2rtgCore *>(c)->vram.data(); }
unsigned z2_scan_base_act(void *c) { return static_cast<Z2rtgCore *>(c)->scan_base_act; }
// the cursor's colour at (x, y) of the shown frame, -1 where it is transparent or off
long z2_cursor_pixel(void *c, int x, int y) { uint32_t v; return static_cast<Z2rtgCore *>(c)->cursor_pixel(x, y, v) ? static_cast<long>(v) : -1; }
}
