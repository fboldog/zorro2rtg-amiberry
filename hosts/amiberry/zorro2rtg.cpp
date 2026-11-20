/*
 * Amiberry adapter for a zboard plugin (include/zboard_plugin.h), here the Zorro2RTG board in
 * plugins/zorro2rtg/z2rtg_plugin.cpp, compiled in. Only what needs Amiberry is here: the memory banks, bus time charged to
 * the CPU, /INT2, switching the display between the chipset and the board, and copying its frames to the
 * RTG surface. Everything the board does, its options included, is in the plugin (z2rtg_plugin.cpp).
 *
 * - The board's IO regions use the I/O bank gfxboard.cpp maps at Autoconfig; its RAM regions (VRAM and
 *   staging) get a RAM bank with a direct pointer, as the ZZ9000 model maps its VRAM. Every access still
 *   goes through the plugin. Longwords are two word cycles, high word first, as on Zorro II.
 * - Time: the plugin gets the emulated CPU time in ns with every call, from get_cycles() at 227 colour
 *   clocks per 64 us line, so the model's lines stay in step with the chipset's.
 * - The hsync callback ticks the plugin every line (it runs every line after the boot reset and after a
 *   reboot, instrumented on v8.5.0 on 2026-11-14); the levels it returns are raised as long as it requests
 *   them, since /INT2 is level-triggered.
 * - The Autoconfig data is the board's row in gfxboard.cpp, added by integrate.py; it must match the
 *   plugin's zbp_desc. A loader that reads the description instead is for a later Amiberry change.
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
#include "devices.h"

#include <new>

#include "zboard_plugin.h"

struct zb_state {
    const zbp_api *api = nullptr;
    zbp_board *b = nullptr;
    zbp_host host {};
    int devnum = -1;
    int monitor_id = 0;
    uae_u32 base = 0;            // configured address, 0 before Autoconfig
    uae_u32 size = 0;
    bool enabled = false;        // Amiberry has this board's display selected (toggle)
    bool rtg_on = false;         // what the board last asked for (host.display)
    bool mode_changed = true;
    int shown_w = 0, shown_h = 0;
};

static zb_state *zb_boards[MAX_RTG_BOARDS];

static constexpr uint64_t LINE_NS = 64000, LINE_CCK = 227;

// emulated time in ns: colour clocks at 227 per 64 us line
static uint64_t zb_now()
{
    return static_cast<uint64_t>(static_cast<unsigned __int128>(get_cycles()) * LINE_NS / (LINE_CCK * CYCLE_UNIT));
}

static void zb_charge(void *, uint32_t ns)
{
    const evt_t cck = (static_cast<evt_t>(ns) * LINE_CCK + LINE_NS / 2) / LINE_NS;
    if (cck)
        x_do_cycles(cck * CYCLE_UNIT);
}

static void zb_display(void *ctx, int on)
{
    auto *d = static_cast<zb_state *>(ctx);
    d->rtg_on = on != 0;
    d->mode_changed = true;
    gfxboard_set(d->monitor_id, d->rtg_on);
}

static void zb_log(void *, int, const char *msg)
{
    write_log(_T("%s\n"), msg);
}

static zb_state *zb_find(uaecptr addr)
{
    for (auto *d : zb_boards)
        if (d && d->base && addr >= d->base && addr < d->base + d->size)
            return d;
    return nullptr;
}

static uae_u32 REGPARAM2 zb_wget(uaecptr addr)
{
    zb_state *d = zb_find(addr);
    return d ? d->api->read(d->b, addr - d->base, 2, zb_now()) : 0;
}
static uae_u32 REGPARAM2 zb_lget(uaecptr addr)
{
    return (zb_wget(addr) << 16) | zb_wget(addr + 2);
}
static uae_u32 REGPARAM2 zb_bget(uaecptr addr)
{
    zb_state *d = zb_find(addr);
    return d ? d->api->read(d->b, addr - d->base, 1, zb_now()) : 0;
}
static void REGPARAM2 zb_wput(uaecptr addr, uae_u32 v)
{
    if (zb_state *d = zb_find(addr))
        d->api->write(d->b, addr - d->base, v & 0xffff, 2, zb_now());
}
static void REGPARAM2 zb_lput(uaecptr addr, uae_u32 v)
{
    zb_wput(addr, v >> 16);
    zb_wput(addr + 2, v & 0xffff);
}
static void REGPARAM2 zb_bput(uaecptr addr, uae_u32 v)
{
    if (zb_state *d = zb_find(addr))
        d->api->write(d->b, addr - d->base, v & 0xff, 1, zb_now());
}
static uae_u8 *REGPARAM2 zb_xlate(uaecptr addr)
{
    zb_state *d = zb_find(addr);
    const uint8_t *p = d ? d->api->ptr(d->b, addr - d->base, 1) : nullptr;
    // Amiberry's xlate is not const; the CPU only fetches through it
    return p ? const_cast<uae_u8 *>(p) : default_xlate(addr);
}
static int REGPARAM2 zb_check(uaecptr addr, uae_u32 size)
{
    zb_state *d = zb_find(addr);
    if (!d)
        return 0;
    const uae_u32 off = addr - d->base;
    const zbp_desc *desc = d->api->desc;
    for (uint32_t i = 0; i < desc->n_regions; i++) {
        const zbp_region &r = desc->regions[i];
        if (r.kind == ZBP_REGION_RAM && off >= r.offset && size <= r.size && off - r.offset <= r.size - size)
            return 1;
    }
    return 0;
}

static addrbank zb_ram_bank = {
    zb_lget, zb_wget, zb_bget,
    zb_lput, zb_wput, zb_bput,
    zb_xlate, zb_check, nullptr, _T("*"), _T("Zorro2RTG VRAM"),
    zb_lget, zb_wget,
    ABFLAG_RAM, S_READ, S_WRITE
};

static addrbank zb_io_bank = {
    zb_lget, zb_wget, zb_bget,
    zb_lput, zb_wput, zb_bput,
    default_xlate, default_check, nullptr, nullptr, _T("Zorro2RTG"),
    zb_lget, zb_wget,
    ABFLAG_IO, S_READ, S_WRITE
};

static void zb_hsync(void *userdata)
{
    auto *d = static_cast<zb_state *>(userdata);
    if (!d->base)
        return;
    const uint32_t irq = d->api->tick(d->b, zb_now());
    if (irq & (1u << 2))
        safe_interrupt_set(IRQ_SOURCE_GFX, d->monitor_id, false);
    if (irq & (1u << 6))
        safe_interrupt_set(IRQ_SOURCE_GFX, d->monitor_id, true);
}

static bool zb_vsync(void *userdata, gfxboard_mode *mode)
{
    auto *d = static_cast<zb_state *>(userdata);
    if (!d->base || !d->enabled)
        return false;
    zbp_frame f {};
    f.struct_size = sizeof f;
    if (!d->api->frame(d->b, &f) || f.format != ZBP_FB_RGB565_BE)
        return false;
    const int W = f.width, H = f.height;
    if (f.changed || W != d->shown_w || H != d->shown_h) {
        d->shown_w = W;
        d->shown_h = H;
        d->mode_changed = true;
    }
    mode->width = W;
    mode->height = H;
    mode->mode = RGBFB_R5G6B5;             // big-endian RGB565
    mode->hlinedbl = 1;
    mode->vlinedbl = 1;
    const auto &state = picasso96_state[d->monitor_id];
    if (d->mode_changed && (state.Width != W || state.Height != H || state.RGBFormat != RGBFB_R5G6B5))
        return true;                       // let Amiberry resize first
    uae_u8 *surface = gfx_lock_picasso(d->monitor_id, false);
    if (!surface)
        return false;
    for (int y = 0; y < H; y++)
        fb_copyrow(d->monitor_id, const_cast<uae_u8 *>(f.pixels + static_cast<size_t>(y) * f.stride), surface, 0, 0,
                   W, 2, y);
    d->mode_changed = false;
    gfx_unlock_picasso(d->monitor_id, true);
    return true;
}

static bool zb_toggle(void *userdata, int mode)
{
    auto *d = static_cast<zb_state *>(userdata);
    if (!d->base)
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

static void zb_configured(void *userdata, uae_u32 address)
{
    auto *d = static_cast<zb_state *>(userdata);
    d->base = address;
    const zbp_desc *desc = d->api->desc;
    for (uint32_t i = 0; i < desc->n_regions; i++) {
        const zbp_region &r = desc->regions[i];
        if (r.kind == ZBP_REGION_RAM)
            map_banks(&zb_ram_bank, (address + r.offset) >> 16, r.size >> 16, 0);
    }
    d->api->configured(d->b, address, zb_now());
}

static void zb_refresh(void *userdata)
{
    static_cast<zb_state *>(userdata)->mode_changed = true;
}

static void zb_reset(void *userdata)
{
    auto *d = static_cast<zb_state *>(userdata);
    d->api->reset_board(d->b);
}

static void zb_free(void *userdata)
{
    auto *d = static_cast<zb_state *>(userdata);
    if (!d)
        return;
    if (d->devnum >= 0 && d->devnum < MAX_RTG_BOARDS)
        zb_boards[d->devnum] = nullptr;
    if (d->b)
        d->api->destroy(d->b);
    delete d;
}

static bool zb_init(autoconfig_info *aci)
{
    const zbp_api *api = zbp_get_api(ZBP_VERSION);
    if (!api || api->struct_size < sizeof(zbp_api)) {
        write_log(_T("zboard: plugin does not offer interface version %d\n"), ZBP_VERSION);
        return false;
    }
    aci->label = api->desc->name;
    if (!aci->doinit)
        return true;
    auto *d = new (std::nothrow) zb_state{};
    if (!d)
        return false;
    d->api = api;
    d->size = api->desc->size;
    d->devnum = aci->devnum;
    d->monitor_id = aci->prefs->rtgboards[aci->devnum].monitor_id;
    d->host.struct_size = sizeof d->host;
    d->host.charge_ns = zb_charge;
    d->host.display = zb_display;
    d->host.log = zb_log;
    d->b = api->create(&d->host, d, "");   // options: the plugin's environment variables for now
    if (!d->b) {
        delete d;
        return false;
    }
    zb_boards[d->devnum] = d;
    aci->addrbank = &zb_io_bank;
    aci->userdata = d;
    return true;
}

gfxboard_func zorro2rtg_func = {
    zb_init,
    zb_free,
    zb_reset,
    zb_hsync,
    zb_vsync,
    zb_refresh,
    zb_toggle,
    zb_configured
};
