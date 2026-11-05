/*
 * Zorro2RTG (Tang Nano 20K Zorro II RTG card) board emulation for Amiberry.
 *
 * Glue between Amiberry's gfxboard_func interface and the emulator-independent board model in
 * z2rtg_core.cpp, which implements docs/registers.md of the Zorro2-Tang25k project: register file,
 * 8 MB VRAM window, scanout (1280x720 RGB565 only, as the gateware) and the blitter.
 * The register area (the first 64 KB) is an I/O bank through the model; the VRAM and staging window
 * above it is a RAM bank on the model's VRAM (with BANK), as the ZZ9000 model maps its VRAM.
 * The model's time (its 64 us lines: blitter busy time, vertical blank, FRAME_COUNT) follows the emulated
 * CPU clock, read at every board access and on each hsync callback. The hsync callback alone is not enough:
 * Amiberry stops calling it after the boot-time reset.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "memory.h"
#include "picasso96.h"
#include "gfxboard.h"
#include "custom.h"
#include "xwin.h"
#include "events.h"
#include "newcpu.h"

#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "z2rtg_core.h"
#include "zorro2rtg_regs.h"

static constexpr uae_u32 Z2RTG_SIZE = 0x00400000;   // 4 MB Zorro II board
static constexpr int Z2RTG_W = 1280, Z2RTG_H = 720;

struct z2rtg_state {
    Z2rtgCore core;
    int devnum = -1;
    int monitor_id = 0;
    uae_u32 configured = 0;
    bool enabled = false;       // Amiberry has this board's display selected (toggle)
    bool rtg_on = false;        // last value passed to gfxboard_set
    bool mode_changed = true;
    std::vector<uae_u8> line;
    uae_u32 last_jobs = 0;
    int trace = 0;               // Z2RTG_TRACE=N: log the first N register writes
    uae_u32 dump_dst = 0, dump_stride = 0, dump_w = 0, dump_h = 0, dump_fg = 0;   // Z2RTG_TRACE_TPL
    int trace_blt = 0;           // Z2RTG_TRACE_BLT=N: log the first N blitter jobs
    uae_u32 status_reads = 0;
    uae_u32 hsyncs = 0;
    uae_u32 reg_reads[64] = {};  // reads of each register word $00-$7E, for the trace
    FILE *tf = nullptr;          // Z2RTG_TRACE_FILE: trace written here, flushed per line
    evt_t last_cycles = 0;
};

static void z2_follow_video_en(z2rtg_state *d);

// advance the model to the emulated CPU time, one 64 us line (227 colour clocks) at a time
static void z2_advance(z2rtg_state *d)
{
    const evt_t line = static_cast<evt_t>(227) * CYCLE_UNIT;
    const evt_t now = get_cycles();
    if (now < d->last_cycles || now - d->last_cycles > line * 100000) {
        d->last_cycles = now;              // first call, or a jump (reset, state load): resync
        d->core.blt_busy_until = 0;
        return;
    }
    while (now - d->last_cycles >= line) {
        d->last_cycles += line;
        if (d->core.hsync())
            z2_follow_video_en(d);
        if (d->tf && !(++d->hsyncs % 50000)) {
            fprintf(d->tf, "%u lines, frame %u, blitter jobs %u, reads:", d->hsyncs, d->core.frame_count,
                    d->core.blt_jobs);
            for (int i = 0; i < 64; i++)
                if (d->reg_reads[i])
                    fprintf(d->tf, " %02x:%u", i * 2, d->reg_reads[i]);
            fprintf(d->tf, "\n");
            fflush(d->tf);
        }
    }
    // BLT_BUSY at CPU-cycle resolution: model time = the lines done + the part of the current one
    d->core.now_ns = d->core.now_ns - d->core.now_ns % Z2rtgCore::LINE_NS +
                     static_cast<uint64_t>(now - d->last_cycles) * Z2rtgCore::LINE_NS / static_cast<uint64_t>(line);
}

static void z2_trace(z2rtg_state *d, uae_u32 off, uae_u32 v, int size)
{
    if (d->trace_blt > 0 && off == Z2R_REG_BLT_OP && d->tf) {
        const Z2rtgCore &c = d->core;
        d->trace_blt--;
        if (d->dump_w && getenv("Z2RTG_TRACE_TPL")) {   // the last EXPAND's destination, as it is now
            fprintf(d->tf, "dst of last expand, 16-bit (# = its fg %04x):\n", d->dump_fg);
            for (unsigned y = 0; y < d->dump_h; y++) {
                fprintf(d->tf, "  %3u ", y);
                for (unsigned x = 0; x < d->dump_w; x++) {
                    const unsigned a = (d->dump_dst + y * d->dump_stride + x * 2) & (Z2rtgCore::VRAM_BYTES - 2);
                    fputc(((c.vram[a] << 8) | c.vram[a + 1]) == d->dump_fg ? '#' : '.', d->tf);
                }
                fputc('\n', d->tf);
            }
            d->dump_w = 0;
        }
        if (v == Z2R_BLITTER_OP_EXPAND && c.blt_w <= 160 && c.blt_h <= 120) {
            d->dump_dst = c.blt_dst; d->dump_stride = c.blt_dst_stride; d->dump_w = c.blt_w; d->dump_h = c.blt_h; d->dump_fg = c.blt_fg & 0xffff;
        }
        fprintf(d->tf, "blt op %u dst %06x/%u src %06x/%u %ux%u fg %04x bg %04x mode %04x rop %u\n", v, c.blt_dst,
                c.blt_dst_stride, c.blt_src, c.blt_src_stride, c.blt_w, c.blt_h, c.blt_fg & 0xffff,
                c.blt_bg & 0xffff, c.blt_mode, c.blt_rop);
        if (v == Z2R_BLITTER_OP_EXPAND && getenv("Z2RTG_TRACE_TPL") && c.blt_w <= 160 && c.blt_h <= 120) {
            const unsigned ofs = (c.blt_mode >> Z2R_BLT_MODE_BITOFS_SHIFT) & 15;
            for (unsigned y = 0; y < c.blt_h; y++) {
                fprintf(d->tf, "  %3u ", y);
                for (unsigned x = 0; x < c.blt_w; x++) {
                    const unsigned b = ofs + x, a = c.sdram_addr(c.blt_src) + y * c.blt_src_stride + (b >> 3);
                    fputc(a < c.vram.size() && (c.vram[a] & (0x80 >> (b & 7))) ? '#' : '.', d->tf);
                }
                fputc('\n', d->tf);
            }
        }
        fflush(d->tf);
    }
    if (d->trace > 0 && off < 0x10000 && d->tf) {
        d->trace--;
        fprintf(d->tf, "frame %u line %d: wr%d %04x = %04x  busy %u\n", d->core.frame_count, d->core.line,
                size, off, v, d->core.blt_busy() ? 1u : 0u);
        fflush(d->tf);
    }
}

static z2rtg_state *z2rtg_boards[MAX_RTG_BOARDS];

// Zorro II bus time per word access, in colour clocks (281.9 ns): a cycle is 4 clocks of 7.09 MHz,
// 2 colour clocks. Writes go into the card's posted-write FIFO, reads hold XRDY until the data is back.
// Placeholders, VERIFY on the A2000 (z2test BENCH). Z2RTG_BUS=0 turns the charge off.
static int z2_bus_write = 2, z2_bus_read = 3;
static inline void z2_bus(int ccks)
{
    if (ccks)
        x_do_cycles(ccks * CYCLE_UNIT);
}

static z2rtg_state *z2_find(uaecptr addr)
{
    for (auto *d : z2rtg_boards)
        if (d && d->configured && addr >= d->configured && addr < d->configured + Z2RTG_SIZE)
            return d;
    return nullptr;
}

static uae_u32 REGPARAM2 z2rtg_wget(uaecptr addr)
{
    z2rtg_state *d = z2_find(addr);
    if (!d)
        return 0;
    z2_advance(d);
    z2_bus(z2_bus_read);
    if (addr - d->configured == 8)
        d->status_reads++;
    if (addr - d->configured < 0x80)
        d->reg_reads[(addr - d->configured) >> 1]++;
    return d->core.read16(addr - d->configured);
}
static uae_u32 REGPARAM2 z2rtg_lget(uaecptr addr)
{
    return (z2rtg_wget(addr) << 16) | z2rtg_wget(addr + 2);
}
static uae_u32 REGPARAM2 z2rtg_bget(uaecptr addr)
{
    z2rtg_state *d = z2_find(addr);
    if (!d)
        return 0;
    z2_advance(d);
    z2_bus(z2_bus_read);
    return d->core.read8(addr - d->configured);
}
static void REGPARAM2 z2rtg_wput(uaecptr addr, uae_u32 v)
{
    z2rtg_state *d = z2_find(addr);
    if (d) {
        z2_advance(d);
        z2_bus(z2_bus_write);
        z2_trace(d, addr - d->configured, v & 0xffff, 2);
        d->core.write16(addr - d->configured, static_cast<uint16_t>(v), 3);
    }
}
static void REGPARAM2 z2rtg_lput(uaecptr addr, uae_u32 v)
{
    z2rtg_wput(addr, v >> 16);           // Zorro II: two word cycles, high word first
    z2rtg_wput(addr + 2, v & 0xffff);
}
static void REGPARAM2 z2rtg_bput(uaecptr addr, uae_u32 v)
{
    z2rtg_state *d = z2_find(addr);
    if (d) {
        z2_advance(d);
        z2_bus(z2_bus_write);
        z2_trace(d, addr - d->configured, v & 0xff, 1);
        d->core.write8(addr - d->configured, static_cast<uint8_t>(v));
    }
}

// VRAM and staging window, board offsets $10000-$3FFFFF: plain memory, big-endian
static inline uae_u8 *z2_vram_at(z2rtg_state *d, uaecptr addr)
{
    return &d->core.vram[d->core.sdram_addr(addr - d->configured) & (Z2rtgCore::VRAM_BYTES - 1)];
}
static uae_u32 REGPARAM2 z2rtg_vram_lget(uaecptr addr)
{
    z2rtg_state *d = z2_find(addr);
    z2_bus(2 * z2_bus_read);
    if (!d) return 0;
    const uae_u8 *p = z2_vram_at(d, addr);
    return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}
static uae_u32 REGPARAM2 z2rtg_vram_wget(uaecptr addr)
{
    z2rtg_state *d = z2_find(addr);
    z2_bus(z2_bus_read);
    if (!d) return 0;
    const uae_u8 *p = z2_vram_at(d, addr);
    return (p[0] << 8) | p[1];
}
static uae_u32 REGPARAM2 z2rtg_vram_bget(uaecptr addr)
{
    z2rtg_state *d = z2_find(addr);
    z2_bus(z2_bus_read);
    return d ? *z2_vram_at(d, addr) : 0;
}
static void REGPARAM2 z2rtg_vram_lput(uaecptr addr, uae_u32 v)
{
    z2rtg_state *d = z2_find(addr);
    z2_bus(2 * z2_bus_write);
    if (!d) return;
    uae_u8 *p = z2_vram_at(d, addr);
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static void REGPARAM2 z2rtg_vram_wput(uaecptr addr, uae_u32 v)
{
    z2rtg_state *d = z2_find(addr);
    z2_bus(z2_bus_write);
    if (!d) return;
    uae_u8 *p = z2_vram_at(d, addr);
    p[0] = v >> 8; p[1] = v;
}
static void REGPARAM2 z2rtg_vram_bput(uaecptr addr, uae_u32 v)
{
    z2rtg_state *d = z2_find(addr);
    z2_bus(z2_bus_write);
    if (d) *z2_vram_at(d, addr) = v;
}
static uae_u8 *REGPARAM2 z2rtg_vram_xlate(uaecptr addr)
{
    z2rtg_state *d = z2_find(addr);
    return d ? z2_vram_at(d, addr) : default_xlate(addr);
}
static int REGPARAM2 z2rtg_vram_check(uaecptr addr, uae_u32 size)
{
    z2rtg_state *d = z2_find(addr);
    if (!d) return 0;
    const uae_u32 off = addr - d->configured;
    return off >= 0x10000 && off + size <= Z2RTG_SIZE;
}
static addrbank z2rtg_vram_bank = {
    z2rtg_vram_lget, z2rtg_vram_wget, z2rtg_vram_bget,
    z2rtg_vram_lput, z2rtg_vram_wput, z2rtg_vram_bput,
    z2rtg_vram_xlate, z2rtg_vram_check, nullptr, _T("*"), _T("Zorro2RTG VRAM"),
    z2rtg_vram_lget, z2rtg_vram_wget,
    ABFLAG_RAM, S_READ, S_WRITE
};

static addrbank z2rtg_bank = {
    z2rtg_lget, z2rtg_wget, z2rtg_bget,
    z2rtg_lput, z2rtg_wput, z2rtg_bput,
    default_xlate, default_check, nullptr, nullptr, _T("Zorro2RTG"),
    z2rtg_lget, z2rtg_wget,
    ABFLAG_IO, S_READ, S_WRITE
};

// switch Amiberry's window between the chipset and this board as CTRL.VIDEO_EN applies at vertical blank
static void z2_follow_video_en(z2rtg_state *d)
{
    const bool on = (d->core.ctrl_act & 1) != 0;   // Z2R_CTRL_VIDEO_EN
    if (on != d->rtg_on) {
        d->rtg_on = on;
        d->mode_changed = true;
        gfxboard_set(d->monitor_id, on);
    }
}

static void z2rtg_hsync(void *userdata)
{
    auto *d = static_cast<z2rtg_state *>(userdata);
    if (!d->configured)
        return;
    z2_advance(d);
    if (d->core.blt_jobs != d->last_jobs && !(d->core.blt_jobs % 1000)) {
        d->last_jobs = d->core.blt_jobs;
        write_log(_T("Zorro2RTG: %u blitter jobs, %u bytes\n"), d->core.blt_jobs, d->core.blt_bytes);
    }
}

static bool z2rtg_vsync(void *userdata, gfxboard_mode *mode)
{
    auto *d = static_cast<z2rtg_state *>(userdata);
    if (!d->configured || !d->enabled)
        return false;
    mode->width = Z2RTG_W;
    mode->height = Z2RTG_H;
    mode->mode = RGBFB_R5G6B5;             // big-endian RGB565, as stored in VRAM
    mode->hlinedbl = 1;
    mode->vlinedbl = 1;
    const auto &state = picasso96_state[d->monitor_id];
    if (d->mode_changed && (state.Width != Z2RTG_W || state.Height != Z2RTG_H || state.RGBFormat != RGBFB_R5G6B5))
        return true;                       // let Amiberry resize first
    uae_u8 *surface = gfx_lock_picasso(d->monitor_id, false);
    if (!surface)
        return false;
    const size_t line_size = Z2RTG_W * 2;
    d->line.resize(line_size);
    const bool show = d->core.showing();
    const uint32_t base = d->core.scan_base_act & ~31u, stride = d->core.stride_act & ~31u;
    for (int y = 0; y < Z2RTG_H; y++) {
        if (show)
            for (size_t i = 0; i < line_size; i++)
                d->line[i] = d->core.vram[(base + y * stride + i) & (Z2rtgCore::VRAM_BYTES - 1)];
        else
            memset(d->line.data(), 0, line_size);
        fb_copyrow(d->monitor_id, d->line.data(), surface, 0, 0, Z2RTG_W, 2, y);
    }
    d->mode_changed = false;
    gfx_unlock_picasso(d->monitor_id, true);
    return true;
}

static bool z2rtg_toggle(void *userdata, int mode)
{
    auto *d = static_cast<z2rtg_state *>(userdata);
    if (!d->configured)
        return false;
    if (!mode) {
        if (!d->enabled)
            return false;
        d->enabled = false;
        return true;
    }
    if (d->enabled)
        return false;
    d->enabled = true;
    d->mode_changed = true;
    return true;
}

static void z2rtg_configured(void *userdata, uae_u32 address)
{
    auto *d = static_cast<z2rtg_state *>(userdata);
    d->configured = address;
    // registers stay in the I/O bank mapped by gfxboard.cpp; VRAM and staging become memory
    map_banks(&z2rtg_vram_bank, (address + 0x10000) >> 16, (Z2RTG_SIZE - 0x10000) >> 16, 0);
    write_log(_T("Zorro2RTG configured at %08x\n"), address);
}

static void z2rtg_refresh(void *userdata)
{
    static_cast<z2rtg_state *>(userdata)->mode_changed = true;
}

static void z2rtg_reset(void *userdata)
{
    auto *d = static_cast<z2rtg_state *>(userdata);
    d->core.reset();
    if (d->rtg_on) {
        d->rtg_on = false;
        gfxboard_set(d->monitor_id, false);
    }
}

static void z2rtg_free(void *userdata)
{
    auto *d = static_cast<z2rtg_state *>(userdata);
    if (!d)
        return;
    if (d->devnum >= 0 && d->devnum < MAX_RTG_BOARDS)
        z2rtg_boards[d->devnum] = nullptr;
    delete d;
}

static bool z2rtg_init(autoconfig_info *aci)
{
    aci->label = _T("Zorro2RTG");
    if (!aci->doinit)
        return true;
    auto *d = new (std::nothrow) z2rtg_state{};
    if (!d)
        return false;
    d->devnum = aci->devnum;
    d->monitor_id = aci->prefs->rtgboards[aci->devnum].monitor_id;
    if (const char *t = getenv("Z2RTG_TRACE"))
        d->trace = atoi(t);
    if (const char *t = getenv("Z2RTG_BUS"); t && !atoi(t))
        z2_bus_write = z2_bus_read = 0;
    if (const char *t = getenv("Z2RTG_TRACE_BLT"))
        d->trace_blt = atoi(t);
    if (const char *f = getenv("Z2RTG_TRACE_FILE"))
        d->tf = fopen(f, "w");
    z2rtg_boards[d->devnum] = d;
    aci->addrbank = &z2rtg_bank;
    aci->userdata = d;
    return true;
}

gfxboard_func zorro2rtg_func = {
    z2rtg_init,
    z2rtg_free,
    z2rtg_reset,
    z2rtg_hsync,
    z2rtg_vsync,
    z2rtg_refresh,
    z2rtg_toggle,
    z2rtg_configured
};
