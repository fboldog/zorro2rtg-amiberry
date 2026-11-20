/*
 * What both Amiberry adapters share (zboard_amiberry.cpp for RTG cards, zboard_expansion.cpp for every other
 * Zorro board): the board's state, emulated time, bus time charged to the CPU, the memory banks, /INT2 and
 * /INT6, the log and the channel to the board's peer. Included by exactly one adapter per build (one plugin is
 * compiled in), so everything here is static. Define ZB_MAX_BOARDS before including it.
 *
 * - Time: the plugin gets the emulated CPU time in ns with every call, from get_cycles() at 227 colour clocks
 *   per 64 us line, so the model's lines stay in step with the chipset's.
 * - Banks: IO regions use the bank the adapter gives Autoconfig; RAM regions get a RAM bank with a direct
 *   pointer for instruction fetch. Every access still goes through the plugin. Longwords are two word cycles,
 *   high word first, as on Zorro II.
 * - Interrupts: the levels tick() returns are raised every line while it requests them (level-triggered).
 * - The daemon: with ZBOARD_DAEMON set (unix:PATH, a path, or "default" for z2bridged's default socket; unset or
 *   "none": no daemon), the plugin's channel to its peer (zbp_desc.peer) opens at each Autoconfig, before
 *   configured(), through hosts/common/zboard_channel. The plugin itself never opens a port.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZBOARD_AMIBERRY_COMMON_H
#define ZBOARD_AMIBERRY_COMMON_H

#ifndef ZB_MAX_BOARDS
#error "define ZB_MAX_BOARDS before including zboard_amiberry_common.h"
#endif

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "zboard_plugin.h"
#include "zboard_channel.h"

struct zb_state {
    const zbp_api *api = nullptr;
    zbp_board *b = nullptr;
    zbp_host host {};
    int devnum = -1;
    int monitor_id = 0;
    uae_u32 base = 0;            // configured address, 0 before Autoconfig
    uae_u32 size = 0;
    bool enabled = false;        // RTG: Amiberry has this board's display selected (toggle)
    bool rtg_on = false;         // RTG: what the board last asked for (host.display)
    bool mode_changed = true;
    int shown_w = 0, shown_h = 0;
    ZbChannel ch;                // to the board's peer in the daemon (ZBOARD_DAEMON)
};

static zb_state *zb_boards[ZB_MAX_BOARDS];

// The banks are shared by every board the adapter serves; one plugin is compiled in, so its region names label
// them.
static const char *zb_name(const zbp_desc *desc, uint32_t kind, const char *fallback)
{
    for (uint32_t i = 0; i < desc->n_regions; i++)
        if (desc->regions[i].kind == kind && desc->regions[i].name)
            return desc->regions[i].name;
    return fallback;
}

static constexpr uint64_t ZB_LINE_NS = 64000, ZB_LINE_CCK = 227;

// emulated time in ns: colour clocks at 227 per 64 us line
static uint64_t zb_now()
{
    return static_cast<uint64_t>(static_cast<unsigned __int128>(get_cycles()) * ZB_LINE_NS /
                                 (ZB_LINE_CCK * CYCLE_UNIT));
}

static void zb_charge(void *, uint32_t ns)
{
    const evt_t cck = (static_cast<evt_t>(ns) * ZB_LINE_CCK + ZB_LINE_NS / 2) / ZB_LINE_NS;
    if (cck)
        x_do_cycles(cck * CYCLE_UNIT);
}

static void zb_log(void *, int, const char *msg)
{
    write_log(_T("%s\n"), msg);
}

static int zb_channel_up(void *ctx)
{
    return static_cast<zb_state *>(ctx)->ch.up();
}

static int zb_send(void *ctx, const void *msg, uint32_t len)
{
    return static_cast<zb_state *>(ctx)->ch.send(msg, len);
}

static uint32_t zb_recv(void *ctx, void *buf, uint32_t cap)
{
    return static_cast<zb_state *>(ctx)->ch.recv(buf, cap);
}

// the host callbacks every adapter gives the plugin; display is the RTG adapter's own
static void zb_host_init(zb_state *d, void (*display)(void *, int))
{
    d->host.struct_size = sizeof d->host;
    d->host.charge_ns = zb_charge;
    d->host.display = display;
    d->host.log = zb_log;
    d->host.channel_up = zb_channel_up;
    d->host.send = zb_send;
    d->host.recv = zb_recv;
}

// the plugin's peer, when its description names one and the user asked for the daemon
static void zb_open_channel(zb_state *d)
{
    const zbp_desc *desc = d->api->desc;
    const char *spec = getenv("ZBOARD_DAEMON");
    if (!spec || !*spec || !strcmp(spec, "none") || d->ch.up())
        return;
    if (desc->struct_size < offsetof(zbp_desc, peer) + sizeof desc->peer || !desc->peer) {
        write_log(_T("zboard: %s has no peer for the daemon\n"), desc->name);
        return;
    }
    d->ch.open(spec, desc->peer, [](int, const std::string &m) {
        write_log(_T("%s\n"), m.c_str());
        fprintf(stderr, "%s\n", m.c_str());
    });
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
    zb_xlate, zb_check, nullptr, _T("*"), _T("zboard RAM"),
    zb_lget, zb_wget,
    ABFLAG_RAM, S_READ, S_WRITE
};

static addrbank zb_io_bank = {
    zb_lget, zb_wget, zb_bget,
    zb_lput, zb_wput, zb_bput,
    default_xlate, default_check, nullptr, nullptr, _T("zboard"),
    zb_lget, zb_wget,
    ABFLAG_IO, S_READ, S_WRITE
};

// the plugin's tick, once per line; the levels it requests are raised while it requests them
static void zb_tick(zb_state *d)
{
    if (!d->base)
        return;
    const uint32_t irq = d->api->tick(d->b, zb_now());
    if (irq & (1u << 2))
        safe_interrupt_set(IRQ_SOURCE_GFX, d->monitor_id, false);
    if (irq & (1u << 6))
        safe_interrupt_set(IRQ_SOURCE_GFX, d->monitor_id, true);
}

// Autoconfig done at base: the channel first (the plugin sees it from configured() on), the RAM banks, then
// the plugin
static void zb_set_configured(zb_state *d, uae_u32 address)
{
    d->base = address;
    zb_open_channel(d);
    const zbp_desc *desc = d->api->desc;
    for (uint32_t i = 0; i < desc->n_regions; i++) {
        const zbp_region &r = desc->regions[i];
        if (r.kind == ZBP_REGION_RAM)
            map_banks(&zb_ram_bank, (address + r.offset) >> 16, r.size >> 16, 0);
    }
    d->api->configured(d->b, address, zb_now());
}

// the plugin, if it speaks this interface version; labels the banks
static const zbp_api *zb_get_api()
{
    const zbp_api *api = zbp_get_api(ZBP_VERSION);
    if (!api || api->struct_size < sizeof(zbp_api)) {
        write_log(_T("zboard: plugin does not offer interface version %d\n"), ZBP_VERSION);
        return nullptr;
    }
    zb_io_bank.name = zb_name(api->desc, ZBP_REGION_IO, api->desc->name);
    zb_ram_bank.name = zb_name(api->desc, ZBP_REGION_RAM, api->desc->name);
    return api;
}

static void zb_destroy(zb_state *d)
{
    if (d->devnum >= 0 && d->devnum < ZB_MAX_BOARDS)
        zb_boards[d->devnum] = nullptr;
    if (d->b)
        d->api->destroy(d->b);  // the plugin flushes what it has into the channel
    d->ch.close();
    delete d;
}

#endif
