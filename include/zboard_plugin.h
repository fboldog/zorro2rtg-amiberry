/* zboard_plugin.h: a Zorro expansion board as a plugin for an Amiga emulator. Interface version 1.
 *
 * The plugin is the whole board: its registers, memory, timing, display and whatever it does on the side
 * (Zorro2RTG: the USB input queue, traces). The emulator (the host) keeps what only it can do: mapping
 * memory banks, charging bus time to the CPU, raising interrupt levels and showing frames. Nothing here
 * names an emulator type, so the same plugin can serve Amiberry, WinUAE or a test.
 *
 * Outside hardware (Zorro2RTG: a real Nano 20K the emulated board mirrors to) is never reached by the
 * plugin itself. The host holds one connection to a daemon per board and gives the plugin a message
 * channel over it (zbp_host.send and recv); in the daemon, the board's peer (zbp_desc.peer) gets the
 * messages and talks to the hardware. The model stays in the emulator, so reads never wait for a round
 * trip; only the messages leave. The messages are the plugin's and its peer's own: the host and the
 * daemon pass them on unread.
 *
 * Use, either compiled in or loaded with dlopen:
 *   const zbp_api *api = zbp_get_api(ZBP_VERSION);   NULL: the plugin doesn't speak this version
 *   api->desc                                        Autoconfig data and memory regions, before any board
 *   zbp_board *b = api->create(&host, host_ctx, options);
 *   ...accesses, api->tick and api->frame as the emulation runs...
 *   api->destroy(b);
 * Version 1 has no save states: a board can depend on outside hardware (Zorro2RTG mirrors to a real
 * Nano 20K), whose state a save cannot capture. A later version may add them at the end of zbp_api.
 *
 * Conventions:
 * - Plain C, no exceptions across the boundary, every call on the emulation thread.
 * - Offsets are relative to the board's base, from 0 to desc->size - 1. Data is as the Amiga sees it: a
 *   16-bit read at an even offset returns the byte at that offset in bits 15:8.
 * - Time is the host's emulated time in nanoseconds, never decreasing except across reset_board. The
 *   plugin treats a backward step or a jump of several seconds as a resync, not as time passing.
 * - A struct that may grow starts with its size; the reader uses only the fields that size covers.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef ZBOARD_PLUGIN_H
#define ZBOARD_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZBP_VERSION 1

typedef struct zbp_board zbp_board;   /* opaque, one per board instance */

/* ---- What the board is ------------------------------------------------------------------------------ */

enum {
    ZBP_REGION_IO = 0,      /* every access goes through read/write: registers, side effects */
    ZBP_REGION_RAM = 1,     /* memory: reads and writes still go through the plugin (it may mirror them),
                               but ptr() gives the host a direct pointer for instruction fetch and copies */
};

typedef struct zbp_region {
    uint32_t offset, size;  /* 64 KB aligned, so a host with 64 KB banks can map it */
    uint32_t kind;          /* ZBP_REGION_* */
    const char *name;
} zbp_region;

typedef struct zbp_desc {
    uint32_t struct_size;
    const char *name;               /* "Zorro2RTG", for menus and config files */
    const char *vendor;             /* "Zorro2-Tang25k" */
    /* Autoconfig (Amiga Hardware Reference Manual): the host builds the configuration ROM from these */
    uint16_t manufacturer;          /* placeholder 0x5A32 until a real ID exists */
    uint8_t product;
    uint8_t er_type;                /* complete byte, with the size code and ERTF_DIAGVALID (0xD7) */
    uint8_t er_flags;
    uint32_t serial;
    uint16_t diag_vec;              /* er_InitDiagVec, used when er_type has ERTF_DIAGVALID: 0x8000 */
    uint32_t size;                  /* board size in bytes, 0x400000 */
    uint32_t zorro;                 /* 2 or 3 */
    uint32_t flags;                 /* ZBP_DESC_* */
    uint32_t n_regions;
    const zbp_region *regions;      /* sorted, covering 0 to size - 1 without gaps */
    /* the board's peer in the daemon, a name of [a-z0-9_] ("zorro2rtg"), or NULL for none; the host
       connects only when its user asks for the daemon */
    const char *peer;
} zbp_desc;

enum {
    ZBP_DESC_DISPLAY = 1u << 0,     /* the board makes frames (an RTG card): the host calls frame() */
    ZBP_DESC_NOT_RAM = 1u << 1,     /* never add the board to the free memory list (always for RTG) */
};

/* ---- What the host gives the board ------------------------------------------------------------------ */

enum { ZBP_LOG_INFO = 0, ZBP_LOG_WARN = 1, ZBP_LOG_ERROR = 2 };

typedef struct zbp_host {
    uint32_t struct_size;
    /* the access in progress takes ns longer on the bus (wait states, the Zorro cycle itself); the host
       stalls the CPU by that much. May be called several times per access, or not at all. */
    void (*charge_ns)(void *ctx, uint32_t ns);
    /* the board's display wants to be shown (on) or gives the monitor back to the chipset (off) */
    void (*display)(void *ctx, int on);
    void (*log)(void *ctx, int level, const char *msg);

    /* The channel to the board's peer in the daemon (zbp_desc.peer). Messages arrive whole and in order,
       1 to ZBP_MSG_MAX bytes. The host opens it before configured() and again before each later
       configured(), so a lost daemon is retried at the Amiga's next reset. */
    /* nonzero while the channel is open */
    int (*channel_up)(void *ctx);
    /* queue a message for the peer; 0 when there is no channel (the message is dropped). Waits while the
       host's outgoing buffer is full, so a peer that cannot keep up slows the emulated board down */
    int (*send)(void *ctx, const void *msg, uint32_t len);
    /* the next message from the peer into buf: its length, 0 when there is none */
    uint32_t (*recv)(void *ctx, void *buf, uint32_t cap);
} zbp_host;

#define ZBP_MSG_MAX 65536u          /* the longest message either way (recv's buf needs this much) */

/* ---- Frames ----------------------------------------------------------------------------------------- */

enum {
    ZBP_FB_RGB565_BE = 1,           /* 2 bytes a pixel, big-endian, as Zorro2RTG stores them */
    ZBP_FB_XRGB8888_BE = 2,
};

typedef struct zbp_frame {
    uint32_t struct_size;
    int32_t width, height;
    uint32_t format;                /* ZBP_FB_* */
    uint32_t stride;                /* bytes from one row to the next */
    const uint8_t *pixels;          /* the finished picture (palette and cursor applied), valid until the
                                       next call into the plugin */
    uint32_t changed;               /* nonzero when width, height or format differ from the last frame */
} zbp_frame;

/* ---- What the board gives the host ------------------------------------------------------------------ */

typedef struct zbp_api {
    uint32_t struct_size;
    uint32_t version;               /* ZBP_VERSION it was built for */
    const zbp_desc *desc;

    /* options: "key=value" pairs separated by spaces or commas, from the host's config ("" for none); a
       plugin may also read its own environment variables. NULL on failure, after a log message. */
    zbp_board *(*create)(const zbp_host *host, void *host_ctx, const char *options);
    void (*destroy)(zbp_board *b);

    /* Autoconfig done: the board is at base. Called again after a reset that reconfigures it. */
    void (*configured)(zbp_board *b, uint32_t base, uint64_t now_ns);
    /* the Amiga's /RESET: back to the power-on state; Autoconfig follows, then configured again */
    void (*reset_board)(zbp_board *b);

    /* CPU accesses, bytes 1 or 2. A longword on Zorro II is two word cycles, high word first, and the
       host splits it so. (32-bit accesses are for a Zorro III version of this interface.) */
    uint32_t (*read)(zbp_board *b, uint32_t offset, int bytes, uint64_t now_ns);
    void (*write)(zbp_board *b, uint32_t offset, uint32_t value, int bytes, uint64_t now_ns);
    /* direct pointer into a ZBP_REGION_RAM region, valid for len bytes until the board's memory mapping
       changes (Zorro2RTG: a BANK write); NULL where there is none (the host then uses read). Read only:
       writes must go through write, so the board sees them. */
    const uint8_t *(*ptr)(zbp_board *b, uint32_t offset, uint32_t len);

    /* Call at least every tick_ns of emulated time (the host's hsync is fine). The board advances its
       own timing and polls its side channels. Returns the interrupt levels it requests as a bit mask
       (bit 2: /INT2, bit 6: /INT6); level-sensitive, so the host keeps raising them while set. */
    uint32_t tick_ns;
    uint32_t (*tick)(zbp_board *b, uint64_t now_ns);

    /* The host's vertical blank, while the host shows this board (host->display asked for it): fills f
       with the picture, black when the board's output is blanked. Returns 0 before Autoconfig. */
    int (*frame)(zbp_board *b, zbp_frame *f);
} zbp_api;

/* The one exported symbol. A plugin built as a shared object exports exactly this, with C linkage. */
const zbp_api *zbp_get_api(uint32_t version);
typedef const zbp_api *(*zbp_get_api_fn)(uint32_t version);

#ifdef __cplusplus
}
#endif

#endif
