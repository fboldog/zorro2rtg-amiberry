// Zorro2RTG board model, emulator-independent core (docs/driver.md).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "z2rtg_core.h"

#include <algorithm>
#include <cstring>

#include "zorro2rtg_regs.h"

static constexpr uint32_t SDRAM_MASK = Z2rtgCore::VRAM_BYTES - 1;

// byte-lane merge, as the gateware's bw(): be bit 1 = bits 15:8, bit 0 = bits 7:0
static uint16_t bw(uint16_t old, uint16_t v, unsigned be)
{
    uint16_t m = (be & 2 ? 0xFF00 : 0) | (be & 1 ? 0x00FF : 0);
    return static_cast<uint16_t>((old & ~m) | (v & m));
}

Z2rtgCore::Z2rtgCore() : vram(VRAM_BYTES, 0) { reset(); }

void Z2rtgCore::reset()
{
    ctrl = 0; mode = 0; format = 1; stride = 0x0A00; bank = 0; frame_count = 0; scratch = 0;
    scan_base = 0; scan_base_hi_st = 0; vblank_latch = blt_err = scan_pending = false;
    ctrl_act = 0; mode_act = 0; format_act = 1; stride_act = 0x0A00; scan_base_act = 0;
    pad = 0; pad_act = 0;
    cursor_x = cursor_y = 0;
    cursor_en_act = false; cursor_x_act = cursor_y_act = 0;
    for (int i = 0; i < 3; i++) cursor_col_act[i] = 0;
    for (int i = 0; i < 3; i++) { cursor_col[i] = 0; cursor_col_hi_st[i] = 0; }
    blt_dst = blt_src = blt_fg = blt_bg = 0;
    blt_dst_stride = blt_src_stride = blt_w = blt_h = blt_mode = blt_rop = 0;
    dst_hi_st = src_hi_st = fg_hi_st = bg_hi_st = 0;
    blt_busy_until = 0;
    line = 0;
    in_irq_en = false;                                // the queue itself keeps its events, as the gateware does
}

// ------------------------------------------------------------------ USB input
static uint8_t crc8(const uint8_t *p, int n)          // polynomial 0x07, initial 0 (docs/usb_input.md)
{
    uint8_t c = 0;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++)
            c = static_cast<uint8_t>(c & 0x80 ? (c << 1) ^ 0x07 : c << 1);
    }
    return c;
}

void Z2rtgCore::in_event(const std::array<uint8_t, 7> &ev)
{
    if (in_q.size() >= IN_DEPTH) {
        in_ovf = true;
        return;
    }
    in_q.push_back(ev);
}

void Z2rtgCore::in_link_byte(uint8_t b)
{
    if (in_n == 0 && b != 0xA5) {                     // a stray byte between frames
        in_ferr = true;
        return;
    }
    in_frame[in_n++] = b;
    if (in_n < 9)
        return;
    in_n = 0;
    if (crc8(in_frame + 1, 7) != in_frame[8]) {
        in_ferr = true;
        return;
    }
    std::array<uint8_t, 7> ev;
    std::memcpy(ev.data(), in_frame + 1, 7);
    in_event(ev);
}

bool Z2rtgCore::int_req() const
{
    return (vblank_latch && (ctrl & Z2R_CTRL_VBL_IRQ_EN)) || (!in_q.empty() && in_irq_en);
}

// ------------------------------------------------------------------ board window
uint16_t Z2rtgCore::read16(uint32_t offset)
{
    offset &= 0x3FFFFE;
    if (offset >= Z2R_VRAM_START) {
        uint32_t a = sdram_addr(offset) & SDRAM_MASK;
        return static_cast<uint16_t>((vram[a] << 8) | vram[(a + 1) & SDRAM_MASK]);
    }
    return read_reg(offset);
}

void Z2rtgCore::write16(uint32_t offset, uint16_t value, unsigned be)
{
    offset &= 0x3FFFFE;
    if (offset >= Z2R_VRAM_START) {
        uint32_t a = sdram_addr(offset) & SDRAM_MASK;
        if (be & 2) vram[a] = static_cast<uint8_t>(value >> 8);
        if (be & 1) vram[(a + 1) & SDRAM_MASK] = static_cast<uint8_t>(value);
        return;
    }
    write_reg(offset, value, be);
}

uint8_t Z2rtgCore::read8(uint32_t offset)
{
    uint16_t w = read16(offset & ~1u);
    return static_cast<uint8_t>(offset & 1 ? w : w >> 8);
}

void Z2rtgCore::write8(uint32_t offset, uint8_t value)
{
    uint16_t w = static_cast<uint16_t>((value << 8) | value);
    write16(offset & ~1u, w, offset & 1 ? 1 : 2);
}

// ------------------------------------------------------------------ registers
uint16_t Z2rtgCore::read_reg(uint32_t o)
{
    if (o >= Z2R_PALETTE_START && o < Z2R_PALETTE_START + Z2R_PALETTE_SIZE) {
        uint32_t e = palette[(o - Z2R_PALETTE_START) >> 2];
        return (o & 2) ? static_cast<uint16_t>(e & 0xFFFF) : static_cast<uint16_t>((e >> 16) & 0xFF);
    }
    if (o >= Z2R_CURSOR_IMG_START && o < Z2R_CURSOR_IMG_START + Z2R_CURSOR_IMG_SIZE) {
        uint32_t i = o - Z2R_CURSOR_IMG_START;
        return static_cast<uint16_t>((cursor_img[i] << 8) | cursor_img[i + 1]);
    }
    if (o >= Z2R_BOOTROM_START && o < Z2R_BOOTROM_START + Z2R_BOOTROM_SIZE) {
        uint32_t i = o - Z2R_BOOTROM_START;
        return i + 1 < rom.size() ? static_cast<uint16_t>((rom[i] << 8) | rom[i + 1]) : 0;
    }
    switch (o) {
    case Z2R_REG_ID:           return K_ID;
    case Z2R_REG_VERSION:      return K_VERSION;
    case Z2R_REG_CAPS:         return K_CAPS;
    case Z2R_REG_CTRL:         return ctrl;
    case Z2R_REG_STATUS:
        // SCAN_UNDERRUN and SCAN_LINES stay 0: the emulated board has no scanout that can fall behind
        return static_cast<uint16_t>((vblank_latch ? Z2R_STATUS_VBLANK : 0) |
                                     (in_vblank() ? Z2R_STATUS_IN_VBLANK : 0) |
                                     (blt_busy() ? Z2R_STATUS_BLT_BUSY : 0) |
                                     (blt_err ? Z2R_STATUS_BLT_ERR : 0) |
                                     (scan_pending ? Z2R_STATUS_SCAN_PENDING : 0) |
                                     Z2R_STATUS_PLL_LOCK);
    case Z2R_REG_MODE:         return mode;
    case Z2R_REG_FORMAT:       return format;
    case Z2R_REG_SCAN_BASE_HI: return static_cast<uint16_t>((scan_base >> 16) & 0x7F);
    case Z2R_REG_SCAN_BASE_LO: return static_cast<uint16_t>(scan_base & 0xFFFF);
    case Z2R_REG_STRIDE:       return stride;
    case Z2R_REG_BANK:         return bank;
    case Z2R_REG_FRAME_COUNT:  return frame_count;
    case Z2R_REG_SCRATCH:      return scratch;
    case Z2R_REG_MODES:        return K_MODES;
    case Z2R_REG_IN_STATUS:
        return static_cast<uint16_t>((in_q.empty() ? 0 : Z2R_IN_STATUS_AVAIL) | (in_ovf ? Z2R_IN_STATUS_OVERFLOW : 0) |
                                     (in_ferr ? Z2R_IN_STATUS_FRAME_ERR : 0) |
                                     (in_q.size() << Z2R_IN_STATUS_LEVEL_SHIFT));
    case Z2R_REG_IN_CTRL:      return in_irq_en ? Z2R_IN_CTRL_IRQ_EN : 0;
    case Z2R_REG_IN_EV0: case Z2R_REG_IN_EV1: case Z2R_REG_IN_EV2: case Z2R_REG_IN_EV3: {
        if (in_q.empty())
            return 0;
        const auto &e = in_q.front();
        int i = (o - Z2R_REG_IN_EV0) / 2;             // EV0 = TYPE:P0, EV1 = P1:P2, EV2 = P3:P4, EV3 = P5:0
        return static_cast<uint16_t>((e[2 * i] << 8) | (2 * i + 1 < 7 ? e[2 * i + 1] : 0));
    }
    case Z2R_REG_DISPLAY:
        return static_cast<uint16_t>(pad | (out_preset() << Z2R_DISPLAY_OUT_PRESET_SHIFT) |
                                     (out_preset() != mode_act ? Z2R_DISPLAY_PADDED : 0));
    case Z2R_REG_CURSOR_X:     return cursor_x;
    case Z2R_REG_CURSOR_Y:     return cursor_y;
    case Z2R_REG_CURSOR_COL1_HI: return static_cast<uint16_t>(cursor_col[0] >> 16);
    case Z2R_REG_CURSOR_COL1_LO: return static_cast<uint16_t>(cursor_col[0]);
    case Z2R_REG_CURSOR_COL2_HI: return static_cast<uint16_t>(cursor_col[1] >> 16);
    case Z2R_REG_CURSOR_COL2_LO: return static_cast<uint16_t>(cursor_col[1]);
    case Z2R_REG_CURSOR_COL3_HI: return static_cast<uint16_t>(cursor_col[2] >> 16);
    case Z2R_REG_CURSOR_COL3_LO: return static_cast<uint16_t>(cursor_col[2]);
    case Z2R_REG_BLT_DST_HI:   return static_cast<uint16_t>((blt_dst >> 16) & 0x7F);
    case Z2R_REG_BLT_DST_LO:   return static_cast<uint16_t>(blt_dst);
    case Z2R_REG_BLT_DST_STRIDE: return blt_dst_stride;
    case Z2R_REG_BLT_SRC_HI:   return static_cast<uint16_t>((blt_src >> 16) & 0x7F);
    case Z2R_REG_BLT_SRC_LO:   return static_cast<uint16_t>(blt_src);
    case Z2R_REG_BLT_SRC_STRIDE: return blt_src_stride;
    case Z2R_REG_BLT_W:        return blt_w;
    case Z2R_REG_BLT_H:        return blt_h;
    case Z2R_REG_BLT_FG_HI:    return static_cast<uint16_t>(blt_fg >> 16);
    case Z2R_REG_BLT_FG_LO:    return static_cast<uint16_t>(blt_fg);
    case Z2R_REG_BLT_BG_HI:    return static_cast<uint16_t>(blt_bg >> 16);
    case Z2R_REG_BLT_BG_LO:    return static_cast<uint16_t>(blt_bg);
    case Z2R_REG_BLT_MODE:     return blt_mode;
    case Z2R_REG_BLT_ROP:      return blt_rop;
    default:                   return 0;    // reserved, and BLT_OP reads 0
    }
}

void Z2rtgCore::write_reg(uint32_t o, uint16_t v, unsigned be)
{
    if (o >= Z2R_PALETTE_START && o < Z2R_PALETTE_START + Z2R_PALETTE_SIZE) {
        uint32_t i = (o - Z2R_PALETTE_START) >> 2;
        if (!(o & 2)) pal_hi_st = bw(pal_hi_st, v, be);
        else palette[i] = (static_cast<uint32_t>(pal_hi_st & 0xFF) << 16) |
                          bw(static_cast<uint16_t>(palette[i]), v, be);
        return;
    }
    if (o >= Z2R_CURSOR_IMG_START && o < Z2R_CURSOR_IMG_START + Z2R_CURSOR_IMG_SIZE) {
        uint32_t i = o - Z2R_CURSOR_IMG_START;
        if (be & 2) cursor_img[i] = static_cast<uint8_t>(v >> 8);
        if (be & 1) cursor_img[i + 1] = static_cast<uint8_t>(v);
        return;
    }
    switch (o) {
    case Z2R_REG_CTRL:   ctrl = bw(ctrl, v, be) & Z2R_CTRL_FIELDS; break;
    case Z2R_REG_STATUS:                                         // W1C, low byte lane
        if (be & 1) {
            if (v & Z2R_STATUS_VBLANK)  vblank_latch = false;
            if (v & Z2R_STATUS_BLT_ERR) blt_err = false;
        }
        break;
    case Z2R_REG_IN_STATUS:                                      // W1C, low byte lane
        if (be & 1) {
            if (v & Z2R_IN_STATUS_OVERFLOW)  in_ovf = false;
            if (v & Z2R_IN_STATUS_FRAME_ERR) in_ferr = false;
        }
        break;
    case Z2R_REG_IN_CTRL: if (be & 1) in_irq_en = v & Z2R_IN_CTRL_IRQ_EN; break;
    case Z2R_REG_IN_POP:  if (!in_q.empty()) in_q.pop_front(); break;   // any write
    case Z2R_REG_MODE:   mode = bw(mode, v, be) & Z2R_MODE_PRESET_MASK; break;
    case Z2R_REG_DISPLAY: pad = bw(pad, v, be) & Z2R_DISPLAY_PAD_MASK; break;   // OUT_PRESET, PADDED read-only
    case Z2R_REG_FORMAT: format = bw(format, v, be) & 0x3; break;
    case Z2R_REG_SCAN_BASE_HI: scan_base_hi_st = bw(scan_base_hi_st, v, be); break;
    case Z2R_REG_SCAN_BASE_LO:
        scan_base = (static_cast<uint32_t>(scan_base_hi_st & 0x7F) << 16) |
                    bw(static_cast<uint16_t>(scan_base), v, be);
        scan_pending = true;
        break;
    case Z2R_REG_STRIDE:  stride = bw(stride, v, be); break;
    case Z2R_REG_BANK:    bank = bw(bank, v, be) & 1; break;
    case Z2R_REG_SCRATCH: scratch = bw(scratch, v, be); break;
    case Z2R_REG_CURSOR_X: cursor_x = bw(cursor_x, v, be); break;
    case Z2R_REG_CURSOR_Y: cursor_y = bw(cursor_y, v, be); break;
    case Z2R_REG_CURSOR_COL1_HI: cursor_col_hi_st[0] = bw(cursor_col_hi_st[0], v, be); break;
    case Z2R_REG_CURSOR_COL2_HI: cursor_col_hi_st[1] = bw(cursor_col_hi_st[1], v, be); break;
    case Z2R_REG_CURSOR_COL3_HI: cursor_col_hi_st[2] = bw(cursor_col_hi_st[2], v, be); break;
    case Z2R_REG_CURSOR_COL1_LO: case Z2R_REG_CURSOR_COL2_LO: case Z2R_REG_CURSOR_COL3_LO: {
        int i = (o - Z2R_REG_CURSOR_COL1_LO) / 4;
        cursor_col[i] = (static_cast<uint32_t>(cursor_col_hi_st[i] & 0xFF) << 16) |
                        bw(static_cast<uint16_t>(cursor_col[i]), v, be);
        break;
    }
    case Z2R_REG_BLT_DST_HI: dst_hi_st = bw(dst_hi_st, v, be); break;
    case Z2R_REG_BLT_DST_LO:
        blt_dst = (static_cast<uint32_t>(dst_hi_st & 0x7F) << 16) | bw(static_cast<uint16_t>(blt_dst), v, be);
        break;
    case Z2R_REG_BLT_DST_STRIDE: blt_dst_stride = bw(blt_dst_stride, v, be); break;
    case Z2R_REG_BLT_SRC_HI: src_hi_st = bw(src_hi_st, v, be); break;
    case Z2R_REG_BLT_SRC_LO:
        blt_src = (static_cast<uint32_t>(src_hi_st & 0x7F) << 16) | bw(static_cast<uint16_t>(blt_src), v, be);
        break;
    case Z2R_REG_BLT_SRC_STRIDE: blt_src_stride = bw(blt_src_stride, v, be); break;
    case Z2R_REG_BLT_W: blt_w = bw(blt_w, v, be); break;
    case Z2R_REG_BLT_H: blt_h = bw(blt_h, v, be); break;
    case Z2R_REG_BLT_FG_HI: fg_hi_st = bw(fg_hi_st, v, be); break;
    case Z2R_REG_BLT_FG_LO:
        blt_fg = (static_cast<uint32_t>(fg_hi_st) << 16) | bw(static_cast<uint16_t>(blt_fg), v, be);
        break;
    case Z2R_REG_BLT_BG_HI: bg_hi_st = bw(bg_hi_st, v, be); break;
    case Z2R_REG_BLT_BG_LO:
        blt_bg = (static_cast<uint32_t>(bg_hi_st) << 16) | bw(static_cast<uint16_t>(blt_bg), v, be);
        break;
    case Z2R_REG_BLT_MODE: blt_mode = bw(blt_mode, v, be) & Z2R_BLT_MODE_FIELDS; break;
    case Z2R_REG_BLT_ROP:  if (be & 1) blt_rop = v & 0xF; break;
    case Z2R_REG_BLT_OP:
        if ((be & 1) && (v & 0xF) != Z2R_BLITTER_OP_NOP) {
            unsigned op = v & 0xF;
            bool valid = op >= Z2R_BLITTER_OP_FILL && op <= Z2R_BLITTER_OP_INVERT;   // all four in CAPS
            if (valid && !blt_busy()) blit(op);
            else blt_err = true;
        }
        break;
    default: break;                                              // read-only or reserved
    }
}

// ------------------------------------------------------------------ timing
bool Z2rtgCore::hsync()
{
    now_ns += LINE_NS;
    if (++line >= LINES_PER_FRAME) line = 0;
    if (line == LINES_PER_FRAME - VBLANK_LINES) { vblank_start(); return true; }
    return false;
}

void Z2rtgCore::vblank_start()
{
    ctrl_act = ctrl; mode_act = mode; format_act = format; stride_act = stride; pad_act = pad;
    scan_base_act = scan_base;
    cursor_en_act = (ctrl & Z2R_CTRL_CURSOR_EN) != 0;
    cursor_x_act = static_cast<int16_t>(cursor_x);
    cursor_y_act = static_cast<int16_t>(cursor_y);
    for (int i = 0; i < 3; i++) cursor_col_act[i] = cursor_col[i];
    scan_pending = false;
    vblank_latch = true;
    frame_count++;
}

bool Z2rtgCore::cursor_pixel(int x, int y, uint32_t &rgb) const
{
    const int dx = x - cursor_x_act, dy = y - cursor_y_act;
    if (!cursor_en_act || dx < 0 || dx >= 64 || dy < 0 || dy >= 64)
        return false;
    // CURSOR_IMG: per row four words of plane 0, then four of plane 1, MSB first
    const unsigned byte = dy * 16 + (dx >> 3), bit = 7 - (dx & 7);
    const unsigned idx = ((cursor_img[byte] >> bit) & 1) | (((cursor_img[byte + 8] >> bit) & 1) << 1);
    if (!idx)
        return false;
    rgb = cursor_col_act[idx - 1];
    return true;
}

bool Z2rtgCore::showing() const
{
    return (ctrl_act & Z2R_CTRL_VIDEO_EN) && !(ctrl_act & Z2R_CTRL_BLANK) &&
           mode_act < 16 && ((K_MODES >> mode_act) & 1) &&
           (format_act == Z2R_PIXEL_FORMAT_RGB565 || format_act == Z2R_PIXEL_FORMAT_CLUT8);
}

// MODE_PRESET sizes; 4 (1280x1024) is not supported and shows as 1280x720 here
static const int mode_w[8] = {640, 800, 1024, 1280, 1280, 848, 1360, 1280};
static const int mode_h[8] = {480, 600, 768, 720, 720, 480, 768, 800};
int Z2rtgCore::width() const { return mode_act < 8 ? mode_w[mode_act] : 1280; }
int Z2rtgCore::height() const { return mode_act < 8 ? mode_h[mode_act] : 720; }

int Z2rtgCore::out_preset() const
{
    int out = mode_act;
    if (pad_act == Z2R_PAD_ASPECT_R16X9 && mode_act <= 2)
        out = mode_act == 0 ? Z2R_MODE_PRESET_M848X480 : mode_act == 1 ? Z2R_MODE_PRESET_M1280X720 : Z2R_MODE_PRESET_M1360X768;
    else if (pad_act == Z2R_PAD_ASPECT_R16X10 && mode_act <= 3)
        out = Z2R_MODE_PRESET_M1280X800;
    return ((K_MODES >> out) & 1) ? out : mode_act;
}

// ------------------------------------------------------------------ blitter (as sim/blitter/tb_blitter.v)
static uint8_t rop8(unsigned rop, uint8_t s, uint8_t d)
{
    switch (rop) {
    case Z2R_RASTER_OP_SRC:     return s;
    case Z2R_RASTER_OP_XOR:     return s ^ d;
    case Z2R_RASTER_OP_AND:     return s & d;
    case Z2R_RASTER_OP_OR:      return s | d;
    case Z2R_RASTER_OP_NOT_DST: return static_cast<uint8_t>(~d);
    default:                    return d;           // unknown op leaves the destination
    }
}

void Z2rtgCore::blit(unsigned op)
{
    const unsigned pix = blt_mode & 3;
    const int bpp = pix == 0 ? 1 : pix == 1 ? 2 : 4;
    const uint32_t w = blt_w, h = blt_h, rb = w * bpp;
    const uint32_t ds = blt_dst_stride, ss = blt_src_stride;
    const unsigned rop = op == Z2R_BLITTER_OP_INVERT ? Z2R_RASTER_OP_NOT_DST : blt_rop;
    blt_jobs++;
    if (!w || !h)
        return;
    auto at = [this](uint32_t a) -> uint8_t & { return vram[a & SDRAM_MASK]; };
    if (op == Z2R_BLITTER_OP_EXPAND) {
        const bool transp = blt_mode & Z2R_BLT_MODE_TRANSPARENT, inv = blt_mode & Z2R_BLT_MODE_INVERSE;
        const unsigned bitofs = (blt_mode >> Z2R_BLT_MODE_BITOFS_SHIFT) & 0xF;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint32_t bi = bitofs + x;
                bool b = ((at(blt_src + y * ss + bi / 8) >> (7 - bi % 8)) & 1) ^ inv;
                if (transp && !b)
                    continue;
                uint32_t col = b ? blt_fg : blt_bg;
                for (int k = 0; k < bpp; k++) {
                    uint8_t &d = at(blt_dst + y * ds + x * bpp + k);
                    d = rop8(rop, static_cast<uint8_t>(col >> (8 * (bpp - 1 - k))), d);
                }
            }
    } else {
        // COPY reads the whole source first (memmove semantics; the gateware needs REVERSE for that)
        std::vector<uint8_t> src;
        if (op == Z2R_BLITTER_OP_COPY) {
            src.resize(static_cast<size_t>(rb) * h);
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < rb; x++)
                    src[static_cast<size_t>(y) * rb + x] = at(blt_src + y * ss + x);
        }
        const uint32_t fgr = bpp == 1 ? (blt_fg & 0xFF) * 0x01010101u :
                             bpp == 2 ? (blt_fg & 0xFFFF) * 0x00010001u : blt_fg;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < rb; x++) {
                uint32_t a = blt_dst + y * ds + x;
                uint8_t s = op == Z2R_BLITTER_OP_COPY ? src[static_cast<size_t>(y) * rb + x]
                                                      : static_cast<uint8_t>(fgr >> (8 * (3 - (a & 3))));
                uint8_t &d = at(a);
                d = rop8(rop, s, d);
            }
    }
    const uint32_t bytes = rb * h * (op == Z2R_BLITTER_OP_COPY ? 2 : 1);
    blt_bytes += bytes;
    blt_busy_until = now_ns + BLT_SETUP_NS + static_cast<uint64_t>(bytes) * 10 / 3;
}
