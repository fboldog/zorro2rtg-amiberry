/*
 * Amiberry adapter for a zboard plugin that is not an RTG card (a network, storage, I/O or memory board),
 * compiled in with the plugin that integrate.py copies next to it (its plugin.json says "adapter":
 * "expansion"). Amiberry runs it as an expansion board: integrate.py adds one row to expansion.cpp's table
 * (ROMTYPE_ZBOARD, Zorro II, the config name from plugin.json), and the board is switched on in a config with
 * <config_name>_rom_file=:ENABLED. Everything else comes from the plugin:
 * - The Autoconfig bytes are built here from zbp_desc (type, product, flags, manufacturer, serial, DiagArea
 *   offset), so there is no copy of them to keep in step, unlike the RTG adapter's gfxboard row.
 * - Amiberry configures it automatically (autoconfig_automatic): it calls zboard_exp_init again with postinit
 *   once the board has its address, which is where the plugin gets configured().
 * - What both adapters share (time, bus time, banks, interrupts, the daemon channel) is in
 *   zboard_amiberry_common.h. The plugin is ticked every line (device_add_hsync).
 * - Zorro II only, one board of this plugin per configuration (version 1 of the interface).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "memory.h"
#include "autoconf.h"
#include "rommgr.h"
#include "custom.h"
#include "events.h"
#include "newcpu.h"
#include "devices.h"

#include <new>

#define ZB_MAX_BOARDS 1
#include "zboard_amiberry_common.h"

static void zbx_display(void *, int) {}     // no display: ZBP_DESC_DISPLAY boards use the RTG adapter

static void zbx_hsync()
{
    for (auto *d : zb_boards)
        if (d)
            zb_tick(d);
}

static void zbx_reset(int)
{
    for (auto *d : zb_boards)
        if (d) {
            d->base = 0;                     // unmapped until Autoconfig configures it again
            d->api->reset_board(d->b);
        }
}

static void zbx_free()
{
    for (auto *d : zb_boards)
        if (d)
            zb_destroy(d);
}

// the 16 bytes Amiberry's automatic Autoconfig expands, one per register at offset 4 * i (expansion.cpp)
static void zbx_autoconfig(const zbp_desc *desc, uae_u8 *b)
{
    memset(b, 0, 16);
    b[0] = desc->er_type;
    b[1] = desc->product;
    b[2] = desc->er_flags;
    b[4] = static_cast<uae_u8>(desc->manufacturer >> 8);
    b[5] = static_cast<uae_u8>(desc->manufacturer);
    b[6] = static_cast<uae_u8>(desc->serial >> 24);
    b[7] = static_cast<uae_u8>(desc->serial >> 16);
    b[8] = static_cast<uae_u8>(desc->serial >> 8);
    b[9] = static_cast<uae_u8>(desc->serial);
    if (desc->er_type & 0x10) {              // ERTF_DIAGVALID: er_InitDiagVec at $28/$2C
        b[10] = static_cast<uae_u8>(desc->diag_vec >> 8);
        b[11] = static_cast<uae_u8>(desc->diag_vec);
    }
}

bool zboard_exp_init(struct autoconfig_info *aci)
{
    if (aci->postinit) {                     // Autoconfig gave the board its address
        if (zb_state *d = zb_boards[0])
            zb_set_configured(d, expamem_board_pointer);
        return true;
    }
    const zbp_api *api = zb_get_api();
    if (!api)
        return false;
    if (api->desc->zorro != 2) {
        write_log(_T("zboard: %s is a Zorro %u board; this adapter serves Zorro II only\n"), api->desc->name,
                  api->desc->zorro);
        return false;
    }
    aci->label = api->desc->name;
    zbx_autoconfig(api->desc, aci->autoconfig_bytes);
    aci->autoconfigp = aci->autoconfig_bytes;
    aci->autoconfig_automatic = true;
    aci->addrbank = &zb_io_bank;
    aci->zorro = 2;
    if (!aci->doinit)
        return true;
    if (zb_boards[0]) {
        write_log(_T("zboard: one %s per configuration\n"), api->desc->name);
        return false;
    }
    auto *d = new (std::nothrow) zb_state{};
    if (!d)
        return false;
    d->api = api;
    d->size = api->desc->size;
    d->devnum = 0;
    zb_host_init(d, zbx_display);
    d->b = api->create(&d->host, d, "");   // options: the plugin's environment variables for now
    if (!d->b) {
        delete d;
        return false;
    }
    zb_boards[0] = d;
    device_add_hsync(zbx_hsync);
    device_add_reset(zbx_reset);
    device_add_exit(zbx_free, nullptr);
    return true;
}
