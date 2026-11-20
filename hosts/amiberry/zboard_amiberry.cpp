/*
 * Amiberry adapter for a zboard plugin that is an RTG card (include/zboard_plugin.h, ZBP_DESC_DISPLAY), compiled
 * in with the plugin that integrate.py copies next to it (its plugin.json says "adapter": "rtg"). It names no
 * board: the bank names come from the plugin's regions and the label from its description. What both adapters
 * share (time, bus time, banks, interrupts, the daemon channel) is in zboard_amiberry_common.h; this file is
 * what an RTG card needs on top: Amiberry runs it as a gfxboard (an RTG board slot, gfxboard_set for the
 * display), switches the display between the chipset and the board, and copies its frames to the RTG surface.
 * A Zorro board that is not an RTG card uses zboard_expansion.cpp instead.
 * - The Autoconfig data is the board's row in gfxboard.cpp, added by integrate.py from plugin.json; it must
 *   match the plugin's zbp_desc. (The expansion adapter builds it from zbp_desc itself.)
 * - The hsync callback ticks the plugin every line (it runs every line after the boot reset and after a
 *   reboot, instrumented on v8.5.0 on 2026-11-14).
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

#define ZB_MAX_BOARDS MAX_RTG_BOARDS
#include "zboard_amiberry_common.h"

static void zb_display(void *ctx, int on)
{
    auto *d = static_cast<zb_state *>(ctx);
    d->rtg_on = on != 0;
    d->mode_changed = true;
    gfxboard_set(d->monitor_id, d->rtg_on);
}

static void zb_hsync(void *userdata)
{
    zb_tick(static_cast<zb_state *>(userdata));
}

static bool zb_vsync(void *userdata, gfxboard_mode *mode)
{
    auto *d = static_cast<zb_state *>(userdata);
    if (!d->base || !d->enabled || !(d->api->desc->flags & ZBP_DESC_DISPLAY))
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
    if (!d->base || !(d->api->desc->flags & ZBP_DESC_DISPLAY))
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
    zb_set_configured(static_cast<zb_state *>(userdata), address);
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
    if (auto *d = static_cast<zb_state *>(userdata))
        zb_destroy(d);
}

static bool zb_init(autoconfig_info *aci)
{
    const zbp_api *api = zb_get_api();
    if (!api)
        return false;
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
    zb_host_init(d, zb_display);
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

gfxboard_func zboard_func = {
    zb_init,
    zb_free,
    zb_reset,
    zb_hsync,
    zb_vsync,
    zb_refresh,
    zb_toggle,
    zb_configured
};
