// Zorro2RTG as a zboard plugin (include/zboard_plugin.h): the board model (z2rtg_core), its timing, the
// board-in-the-loop link (z2rtg_link), the USB input sources, the boot ROM, the traces and the finished
// frame. Emulator-independent; hosts/amiberry/zorro2rtg.cpp is Amiberry's adapter to it.
//
// Time: the model's 64 us lines (blitter busy time, vertical blank, FRAME_COUNT) follow the host's time,
// passed with every call, so BLT_BUSY resolves to the CPU cycle inside a line.
// Bus time: each access charges a Zorro II cycle, 2 colour clocks (564 ns) for a write into the posted-
// write FIFO and 3 (846 ns) for a read, which holds XRDY until the data is back. Placeholders, VERIFY on
// the A2000 (z2test BENCH); bus=0 turns the charge off.
//
// Options (create's string, else the environment variable Z2RTG_<KEY>):
//   bridge=DEV        mirror every write to a real board: /dev/ttyUSBn, or unix:PATH / a socket path for
//                     z2bridged.py (README.md)
//   bridge_baud=N     serial port only, default 3000000
//   input=FIFO|bridge USB input from single-wire link frames in a named pipe, or from the real board's queue
//   bootrom=FILE      the boot ROM image for the BOOTROM window (default: none, the window reads 0)
//   bus=0             no bus time charged
//   trace=N, trace_blt=N, trace_tpl=1, trace_file=FILE, nocursor=1   debugging
// SPDX-License-Identifier: GPL-3.0-or-later
#include "zboard_plugin.h"
#include "z2rtg_core.h"
#include "z2rtg_link.h"
#include "zorro2rtg_regs.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <new>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint32_t BOARD_SIZE = 0x00400000;               // 4 MB Zorro II board
constexpr uint64_t LINE = Z2rtgCore::LINE_NS;
constexpr uint64_t RESYNC_NS = LINE * 100000;             // a jump this long is a resync (6.4 s)
constexpr uint64_t POLL_NS = 1000000;                     // input sources polled every 1 ms
constexpr uint32_t BUS_WRITE_NS = 564, BUS_READ_NS = 846; // 2 and 3 colour clocks
// board in the loop: wait for the real blitter after a job that may outlast the next command on the UART
constexpr uint32_t LINK_WAIT_BYTES = 16384;  // the bridge buffers 64 bytes (about 210 us at 3 Mbaud) while
                                             // a job holds its writes back; 16 KB is about 55 us
constexpr uint32_t LINK_WAIT_ROWS = 64;      // taller jobs too: each row costs the real blitter a fixed
                                             // setup however narrow it is, so a 1x708 border fill outlasts
                                             // the next command and that BLT_OP is refused
                                             // (STATUS.BLT_ERR). VERIFY the per-row cost

const zbp_region regions[] = {
    {0x000000, Z2R_VRAM_START, ZBP_REGION_IO, "Zorro2RTG"},
    {Z2R_VRAM_START, BOARD_SIZE - Z2R_VRAM_START, ZBP_REGION_RAM, "Zorro2RTG VRAM"},
};

const zbp_desc desc = {
    sizeof(zbp_desc), "Zorro2RTG", "Zorro2-Tang25k",
    0x5A32, 1,          // placeholder manufacturer ID (PROJECT.md: choose a real one before release)
    0xD7, 0x00,         // Zorro II, 4 MB, ERTF_DIAGVALID (docs/registers.md section 3)
    1, 0x8000,          // serial, the DiagArea in the boot ROM
    BOARD_SIZE, 2, ZBP_DESC_DISPLAY | ZBP_DESC_NOT_RAM,
    sizeof(regions) / sizeof(regions[0]), regions,
};

uint16_t rgb565(uint32_t c)
{
    return static_cast<uint16_t>(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
}

} // namespace

struct zbp_board {
    const zbp_host *host = nullptr;
    void *ctx = nullptr;
    Z2rtgCore core;
    Z2rtgLink link;
    std::map<std::string, std::string> opt;

    bool configured = false;
    bool have_time = false;
    uint64_t last_ns = 0;            // host time of the last whole line the model has done
    uint64_t next_poll = 0;
    bool rtg_on = false;             // last value passed to host->display
    uint32_t bus_write = BUS_WRITE_NS, bus_read = BUS_READ_NS;
    int in_fd = -1;                  // input=FIFO
    bool in_bridge = false;          // input=bridge

    std::vector<uint8_t> fb;
    int last_w = 0, last_h = 0;
    uint32_t last_format = 0;
    uint32_t last_jobs = 0;

    // debugging
    int trace = 0, trace_blt = 0;
    bool trace_tpl = false, no_cursor = false;
    FILE *tf = nullptr;
    uint32_t dump_dst = 0, dump_stride = 0, dump_w = 0, dump_h = 0, dump_fg = 0;
    uint32_t hsyncs = 0;
    uint32_t reg_reads[64] = {};

    const char *get(const char *key) const
    {
        auto it = opt.find(key);
        if (it != opt.end())
            return it->second.c_str();
        std::string env = "Z2RTG_";
        for (const char *p = key; *p; p++)
            env += static_cast<char>(toupper(static_cast<unsigned char>(*p)));
        return getenv(env.c_str());
    }
    int get_int(const char *key, int dflt) const
    {
        const char *v = get(key);
        return v ? atoi(v) : dflt;
    }
    void log(int level, const char *fmt, ...) __attribute__((format(printf, 3, 4)))
    {
        if (!host->log)
            return;
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        host->log(ctx, level, buf);
    }
    void charge(uint32_t ns)
    {
        if (ns && host->charge_ns)
            host->charge_ns(ctx, ns);
    }
    FILE *link_log() const { return tf ? tf : stderr; }

    void follow_video_en();
    void advance(uint64_t now);
    void do_trace(uint32_t off, uint32_t v, int size);
    void link_reg(uint32_t off, uint16_t v, uint32_t jobs_before, uint32_t bytes_before);
    void link_vram(uint32_t off);
};

// switch the host's display between the chipset and this board as CTRL.VIDEO_EN applies at vertical blank
void zbp_board::follow_video_en()
{
    const bool on = (core.ctrl_act & Z2R_CTRL_VIDEO_EN) != 0;
    if (on != rtg_on) {
        rtg_on = on;
        if (host->display)
            host->display(ctx, on);
    }
}

// advance the model to the host's time, one 64 us line at a time
void zbp_board::advance(uint64_t now)
{
    if (!have_time || now < last_ns || now - last_ns > RESYNC_NS) {
        have_time = true;                   // first call, or a jump (reset, state load): resync
        last_ns = now;
        next_poll = now;                    // after a rewind, the old deadline would hold the input back
        core.blt_busy_until = 0;
        return;
    }
    while (now - last_ns >= LINE) {
        last_ns += LINE;
        if (core.hsync())
            follow_video_en();
        if (tf && !(++hsyncs % 50000)) {
            fprintf(tf, "%u lines, frame %u, blitter jobs %u, reads:", hsyncs, core.frame_count, core.blt_jobs);
            for (int i = 0; i < 64; i++)
                if (reg_reads[i])
                    fprintf(tf, " %02x:%u", i * 2, reg_reads[i]);
            fprintf(tf, "\n");
            fflush(tf);
        }
    }
    // BLT_BUSY inside the line: model time = the lines done + the part of the current one
    core.now_ns = core.now_ns - core.now_ns % LINE + (now - last_ns);
}

void zbp_board::do_trace(uint32_t off, uint32_t v, int size)
{
    if (!tf)
        return;
    if (trace_blt > 0 && off == Z2R_REG_BLT_OP) {
        const Z2rtgCore &c = core;
        trace_blt--;
        if (dump_w && trace_tpl) {   // the last EXPAND's destination, as it is now
            fprintf(tf, "dst of last expand, 16-bit (# = its fg %04x):\n", dump_fg);
            for (unsigned y = 0; y < dump_h; y++) {
                fprintf(tf, "  %3u ", y);
                for (unsigned x = 0; x < dump_w; x++) {
                    const unsigned a = (dump_dst + y * dump_stride + x * 2) & (Z2rtgCore::VRAM_BYTES - 2);
                    fputc(static_cast<uint32_t>((c.vram[a] << 8) | c.vram[a + 1]) == dump_fg ? '#' : '.', tf);
                }
                fputc('\n', tf);
            }
            dump_w = 0;
        }
        if (v == Z2R_BLITTER_OP_EXPAND && c.blt_w <= 160 && c.blt_h <= 120) {
            dump_dst = c.blt_dst; dump_stride = c.blt_dst_stride; dump_w = c.blt_w; dump_h = c.blt_h; dump_fg = c.blt_fg & 0xffff;
        }
        fprintf(tf, "blt op %u dst %06x/%u src %06x/%u %ux%u fg %04x bg %04x mode %04x rop %u\n", v, c.blt_dst,
                c.blt_dst_stride, c.blt_src, c.blt_src_stride, c.blt_w, c.blt_h, c.blt_fg & 0xffff,
                c.blt_bg & 0xffff, c.blt_mode, c.blt_rop);
        if (v == Z2R_BLITTER_OP_EXPAND && trace_tpl && c.blt_w <= 160 && c.blt_h <= 120) {
            const unsigned ofs = (c.blt_mode >> Z2R_BLT_MODE_BITOFS_SHIFT) & 15;
            for (unsigned y = 0; y < c.blt_h; y++) {
                fprintf(tf, "  %3u ", y);
                for (unsigned x = 0; x < c.blt_w; x++) {
                    const unsigned b = ofs + x, a = c.sdram_addr(c.blt_src) + y * c.blt_src_stride + (b >> 3);
                    fputc(a < c.vram.size() && (c.vram[a] & (0x80 >> (b & 7))) ? '#' : '.', tf);
                }
                fputc('\n', tf);
            }
        }
        fflush(tf);
    }
    if (trace > 0 && off < Z2R_VRAM_START) {
        trace--;
        fprintf(tf, "frame %u line %d: wr%d %04x = %04x  busy %u\n", core.frame_count, core.line, size, off, v,
                core.blt_busy() ? 1u : 0u);
        fflush(tf);
    }
}

void zbp_board::link_reg(uint32_t off, uint16_t v, uint32_t jobs_before, uint32_t bytes_before)
{
    if (!link.is_open())
        return;
    link.write(off, v);
    // a job the model started: wait for the real one when it may outlast the next command on the UART
    if ((off & ~1u) == Z2R_REG_BLT_OP && core.blt_jobs != jobs_before &&
        (core.blt_bytes - bytes_before >= LINK_WAIT_BYTES || core.blt_h >= LINK_WAIT_ROWS))
        link.wait_blitter();
}

void zbp_board::link_vram(uint32_t off)
{
    if (link.is_open())
        link.write(off & ~1u, core.read16(off & ~1u));
}

// ---- the interface

static zbp_board *do_create(const zbp_host *host, void *host_ctx, const char *options)
{
    if (!host || host->struct_size < sizeof(zbp_host))
        return nullptr;
    auto *b = new (std::nothrow) zbp_board{};
    if (!b)
        return nullptr;
    b->host = host;
    b->ctx = host_ctx;
    // "key=value" pairs separated by spaces or commas
    for (const char *p = options ? options : ""; *p;) {
        while (*p == ' ' || *p == ',')
            p++;
        const char *e = p;
        while (*e && *e != ' ' && *e != ',')
            e++;
        if (e > p) {
            const std::string kv(p, e);
            const size_t eq = kv.find('=');
            if (eq == std::string::npos)
                b->opt[kv] = "1";
            else
                b->opt[kv.substr(0, eq)] = kv.substr(eq + 1);
        }
        p = e;
    }
    b->trace = b->get_int("trace", 0);
    b->trace_blt = b->get_int("trace_blt", 0);
    b->trace_tpl = b->get("trace_tpl") != nullptr;
    b->no_cursor = b->get_int("nocursor", 0) != 0;
    if (!b->get_int("bus", 1))
        b->bus_write = b->bus_read = 0;
    if (const char *f = b->get("trace_file"))
        b->tf = fopen(f, "w");
    if (const char *f = b->get("bootrom")) {
        if (FILE *rf = fopen(f, "rb")) {
            b->core.rom.resize(Z2R_BOOTROM_SIZE);
            b->core.rom.resize(fread(b->core.rom.data(), 1, Z2R_BOOTROM_SIZE, rf));
            fclose(rf);
            b->log(ZBP_LOG_INFO, "Zorro2RTG: boot ROM %s, %u bytes", f, static_cast<unsigned>(b->core.rom.size()));
        } else {
            b->log(ZBP_LOG_WARN, "Zorro2RTG: boot ROM %s not found", f);
        }
    }
    return b;
}

static void p_destroy(zbp_board *b)
{
    if (!b)
        return;
    b->link.close();
    if (b->in_fd >= 0)
        close(b->in_fd);
    if (b->tf)
        fclose(b->tf);
    delete b;
}

static void do_configured(zbp_board *b, uint32_t base, uint64_t now_ns)
{
    b->configured = true;
    b->have_time = true;
    b->last_ns = now_ns;
    b->next_poll = now_ns;
    b->log(ZBP_LOG_INFO, "Zorro2RTG configured at %08x", base);
    const char *in_src = b->get("input");
    b->in_bridge = in_src && !strcmp(in_src, "bridge");
    if (in_src && !b->in_bridge && b->in_fd < 0) {
        b->in_fd = open(in_src, O_RDONLY | O_NONBLOCK);
        b->log(ZBP_LOG_INFO, "Zorro2RTG: USB input from %s%s", in_src, b->in_fd < 0 ? " failed to open" : "");
    }
    if (const char *dev = b->get("bridge")) {
        if (b->link.is_open() || b->link.open(dev, b->get_int("bridge_baud", 3000000), b->link_log())) {
            // the real board starts from the model's power-on state
            b->link.write(Z2R_REG_CTRL, 0);
            b->link.write(Z2R_REG_BANK, 0);
            b->link.write(Z2R_REG_STATUS, Z2R_STATUS_VBLANK | Z2R_STATUS_BLT_ERR);
            if (b->in_bridge) {
                // keyboard and mouse on the real board's USB input (an RP2040 on pin 51) drive the emulated one
                b->link.enable_input();
                b->log(ZBP_LOG_INFO, "Zorro2RTG: USB input from the real board over the bridge");
            }
        }
    }
}

static void do_reset_board(zbp_board *b)
{
    b->core.reset();
    if (b->rtg_on) {
        b->rtg_on = false;
        if (b->host->display)
            b->host->display(b->ctx, 0);
    }
}

static uint32_t p_read(zbp_board *b, uint32_t off, int bytes, uint64_t now_ns)
{
    off &= BOARD_SIZE - 1;
    if (b->configured)
        b->advance(now_ns);
    b->charge(b->bus_read);
    if (off < 0x80)
        b->reg_reads[off >> 1]++;
    return bytes == 1 ? b->core.read8(off) : b->core.read16(off);
}

static void do_write(zbp_board *b, uint32_t off, uint32_t v, int bytes, uint64_t now_ns)
{
    off &= BOARD_SIZE - 1;
    if (b->configured)
        b->advance(now_ns);
    b->charge(b->bus_write);
    b->do_trace(off, v & (bytes == 1 ? 0xff : 0xffff), bytes);
    if (off >= Z2R_VRAM_START) {
        if (bytes == 1)
            b->core.write8(off, static_cast<uint8_t>(v));
        else
            b->core.write16(off, static_cast<uint16_t>(v), 3);
        b->link_vram(off);
        return;
    }
    const uint32_t jobs = b->core.blt_jobs, bbytes = b->core.blt_bytes;
    if (bytes != 1) {
        b->core.write16(off, static_cast<uint16_t>(v), 3);
        b->link_reg(off, static_cast<uint16_t>(v), jobs, bbytes);
        return;
    }
    b->core.write8(off, static_cast<uint8_t>(v));
    // the bridge writes whole words: the byte in its lane, the other lane as the register's write side now
    // holds it (write_base: a _HI half's staging value, not its committed one; 0 for the W1C registers)
    uint16_t w = b->core.write_base(off & ~1u);
    w = (off & 1) ? static_cast<uint16_t>((w & 0xFF00) | (v & 0xFF)) : static_cast<uint16_t>((w & 0x00FF) | ((v & 0xFF) << 8));
    b->link_reg(off & ~1u, w, jobs, bbytes);
}

static const uint8_t *p_ptr(zbp_board *b, uint32_t off, uint32_t len)
{
    if (off < Z2R_VRAM_START || off >= BOARD_SIZE || len > BOARD_SIZE - off)
        return nullptr;
    return &b->core.vram[b->core.sdram_addr(off) & (Z2rtgCore::VRAM_BYTES - 1)];
}

static uint32_t do_tick(zbp_board *b, uint64_t now_ns)
{
    if (!b->configured)
        return 0;
    b->advance(now_ns);
    if (b->next_poll > now_ns + POLL_NS)    // time went back a little, short of a resync
        b->next_poll = now_ns;
    if (now_ns >= b->next_poll) {
        b->next_poll = now_ns + POLL_NS;
        if (b->in_fd >= 0) {
            uint8_t buf[256];
            const ssize_t n = read(b->in_fd, buf, sizeof buf);
            for (ssize_t i = 0; i < n; i++)
                b->core.in_link_byte(buf[i]);
        }
        // input=bridge: only as many as the emulated queue has room for; the rest wait in the link, so an
        // emulated Amiga that is busy (a long redraw) delays events instead of losing them
        std::array<uint8_t, 7> ev;
        while (b->core.in_q.size() < Z2rtgCore::IN_DEPTH && b->link.take_event(ev))
            b->core.in_event(ev);
        if (b->core.blt_jobs != b->last_jobs && !(b->core.blt_jobs % 1000)) {
            b->last_jobs = b->core.blt_jobs;
            b->log(ZBP_LOG_INFO, "Zorro2RTG: %u blitter jobs, %u bytes", b->core.blt_jobs, b->core.blt_bytes);
        }
    }
    return b->core.int_req() ? 1u << 2 : 0;
}

static int do_frame(zbp_board *b, zbp_frame *f)
{
    if (!b->configured || !f || f->struct_size < sizeof(zbp_frame))
        return 0;
    Z2rtgCore &c = b->core;
    const int W = c.width(), H = c.height();   // the active MODE (applied at vertical blank)
    const size_t line_size = static_cast<size_t>(W) * 2;
    b->fb.resize(line_size * H);
    const bool show = c.showing();
    const bool clut = c.format_act == Z2R_PIXEL_FORMAT_CLUT8;
    const uint32_t base = c.scan_base_act & ~31u, stride = c.stride_act & ~31u;
    constexpr uint32_t MASK = Z2rtgCore::VRAM_BYTES - 1;
    const uint8_t *vram = c.vram.data();
    uint8_t lut[256][2];                        // CLUT8: the palette (8:8:8 on the board) reduced to RGB565
    if (show && clut)
        for (int i = 0; i < 256; i++) {
            const uint16_t p = rgb565(c.palette[i]);
            lut[i][0] = static_cast<uint8_t>(p >> 8);
            lut[i][1] = static_cast<uint8_t>(p);
        }
    for (int y = 0; y < H; y++) {
        uint8_t *line = &b->fb[y * line_size];
        const uint32_t a = (base + y * stride) & MASK;
        if (show && clut) {
            // a byte per pixel; a line that runs past the end of the SDRAM continues at its start
            const uint32_t first = std::min<uint32_t>(W, Z2rtgCore::VRAM_BYTES - a);
            for (uint32_t x = 0; x < first; x++)
                memcpy(line + 2 * x, lut[vram[a + x]], 2);
            for (uint32_t x = first; x < static_cast<uint32_t>(W); x++)
                memcpy(line + 2 * x, lut[vram[x - first]], 2);
        } else if (show) {
            const size_t first = std::min<size_t>(line_size, Z2rtgCore::VRAM_BYTES - a);
            memcpy(line, vram + a, first);
            memcpy(line + first, vram, line_size - first);
        } else {
            memset(line, 0, line_size);
        }
        // the hardware cursor over the line, reduced to RGB565
        if (show && c.cursor_en_act && !b->no_cursor && y >= c.cursor_y_act && y < c.cursor_y_act + 64)
            for (int x = std::max(0, static_cast<int>(c.cursor_x_act)); x < std::min(W, c.cursor_x_act + 64); x++) {
                uint32_t rgb;
                if (c.cursor_pixel(x, y, rgb)) {
                    const uint16_t p = rgb565(rgb);
                    line[2 * x] = static_cast<uint8_t>(p >> 8);
                    line[2 * x + 1] = static_cast<uint8_t>(p);
                }
            }
    }
    f->width = W;
    f->height = H;
    f->format = ZBP_FB_RGB565_BE;
    f->stride = static_cast<uint32_t>(line_size);
    f->pixels = b->fb.data();
    f->changed = W != b->last_w || H != b->last_h || f->format != b->last_format;
    b->last_w = W;
    b->last_h = H;
    b->last_format = f->format;
    return 1;
}

// The C interface lets no exception out (zboard_plugin.h, "Conventions"). The model allocates (a COPY's
// source, the frame, the input queues, the link's thread), so each call that can catches everything: the
// access or frame is lost and logged, the emulator goes on.
static void caught(zbp_board *b, const char *where)
{
    if (b && b->host && b->host->log) {
        char m[160];
        snprintf(m, sizeof m, "Zorro2RTG: %s failed (out of memory or another C++ exception); ignored", where);
        b->host->log(b->ctx, ZBP_LOG_WARN, m);
    }
}

static zbp_board *p_create(const zbp_host *host, void *host_ctx, const char *options)
{
    try {
        return do_create(host, host_ctx, options);
    } catch (...) {
        return nullptr;
    }
}

static void p_configured(zbp_board *b, uint32_t base, uint64_t now_ns)
{
    try { do_configured(b, base, now_ns); } catch (...) { caught(b, "configured"); }
}

static void p_reset_board(zbp_board *b)
{
    try { do_reset_board(b); } catch (...) { caught(b, "reset_board"); }
}

static void p_write(zbp_board *b, uint32_t off, uint32_t v, int bytes, uint64_t now_ns)
{
    try { do_write(b, off, v, bytes, now_ns); } catch (...) { caught(b, "write"); }
}

static uint32_t p_tick(zbp_board *b, uint64_t now_ns)
{
    try { return do_tick(b, now_ns); } catch (...) { caught(b, "tick"); return 0; }
}

static int p_frame(zbp_board *b, zbp_frame *f)
{
    try { return do_frame(b, f); } catch (...) { caught(b, "frame"); return 0; }
}

static const zbp_api api = {
    sizeof(zbp_api), ZBP_VERSION, &desc,
    p_create, p_destroy, p_configured, p_reset_board,
    p_read, p_write, p_ptr,
    static_cast<uint32_t>(POLL_NS), p_tick,
    p_frame,
};

extern "C" const zbp_api *zbp_get_api(uint32_t version)
{
    return version == ZBP_VERSION ? &api : nullptr;
}
