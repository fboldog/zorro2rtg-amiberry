// Zorro2RTG board model, emulator-independent core (docs/driver.md).
// The register file, scanout state, 8 MB VRAM and blitter of the gateware, as docs/registers.md defines
// them. Offsets and bit positions come from the generated sw/p96/zorro2rtg_regs.h.
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef Z2RTG_CORE_H
#define Z2RTG_CORE_H

#include <cstdint>
#include <vector>

struct Z2rtgCore {
    static constexpr uint32_t VRAM_BYTES = 8u << 20;
    static constexpr uint16_t K_ID = 0x5A32, K_VERSION = 0x0001;
    static constexpr uint16_t K_CAPS = 0x01EB;          // as rtl/top/zorro2rtg_top.v (CLUT8, RGB565, CURSOR, blitter)
    static constexpr uint16_t K_MODES = 0x000F;         // 640x480, 800x600, 1024x768, 1280x720, as the gateware
    static constexpr int LINES_PER_FRAME = 260;       // Amiga lines (64 us) per 720p60 frame, about
    static constexpr int VBLANK_LINES = 10;           // 30 of 750 video lines
    static constexpr uint64_t LINE_NS = 64000;        // one Amiga line
    static constexpr uint64_t BLT_SETUP_NS = 200;     // per job, a placeholder (VERIFY on the board)
    // BLT_BUSY time: BLT_SETUP_NS + bytes moved * 10 / 3 ns, about 300 MB/s, the measured fill rate

    std::vector<uint8_t> vram;                        // big-endian, byte at address A is vram[A]

    // registers (CPU view)
    uint16_t ctrl = 0, mode = 0, format = 1, stride = 0x0A00, bank = 0, frame_count = 0, scratch = 0;
    uint32_t scan_base = 0;
    uint16_t scan_base_hi_st = 0;
    bool vblank_latch = false, blt_err = false, scan_pending = false;
    // active copies, loaded at vertical blank
    uint16_t ctrl_act = 0, mode_act = 0, format_act = 1, stride_act = 0x0A00;
    uint32_t scan_base_act = 0;
    // cursor (shown over the scanout, as rtl/video/cursor.v) and palette (8:8:8, used by CLUT8 scanout)
    uint16_t cursor_x = 0, cursor_y = 0;
    uint32_t cursor_col[3] = {0, 0, 0};
    uint16_t cursor_col_hi_st[3] = {0, 0, 0};
    uint32_t palette[256] = {};
    uint16_t pal_hi_st = 0;
    uint8_t cursor_img[0x400] = {};
    // the cursor's frame values, taken at vblank_start (the image is read live, as the BSRAM is)
    bool cursor_en_act = false;
    int16_t cursor_x_act = 0, cursor_y_act = 0;
    uint32_t cursor_col_act[3] = {0, 0, 0};

    // blitter
    uint32_t blt_dst = 0, blt_src = 0, blt_fg = 0, blt_bg = 0;
    uint16_t blt_dst_stride = 0, blt_src_stride = 0, blt_w = 0, blt_h = 0, blt_mode = 0, blt_rop = 0;
    uint16_t dst_hi_st = 0, src_hi_st = 0, fg_hi_st = 0, bg_hi_st = 0;
    uint64_t now_ns = 0;                              // model time: hsync() adds a line, an emulator may set it
    uint64_t blt_busy_until = 0;                      // BLT_BUSY is set while now_ns is below this
    uint32_t blt_jobs = 0, blt_bytes = 0;             // statistics, for tests and the emulator log

    int line = 0;                                     // position within the emulated video frame

    Z2rtgCore();
    void reset();

    // CPU access to the 4 MB board window. be: bit 1 = high byte (even address), bit 0 = low byte.
    uint16_t read16(uint32_t offset);
    void write16(uint32_t offset, uint16_t value, unsigned be = 3);
    uint8_t read8(uint32_t offset);
    void write8(uint32_t offset, uint8_t value);

    // board offset in the VRAM or staging window to an SDRAM address (BANK applies)
    uint32_t sdram_addr(uint32_t offset) const { return (bank & 1u) * 0x400000u + offset; }

    // one Amiga line; returns true at vblank_start
    bool hsync();
    bool in_vblank() const { return line >= LINES_PER_FRAME - VBLANK_LINES; }
    bool blt_busy() const { return now_ns < blt_busy_until; }

    // what the HDMI output shows: false means black (disabled, blanked, unsupported mode or format)
    bool showing() const;
    // the active mode's size (MODE_PRESET, rtl/video/video_mode.v); 1280x720 for an unsupported one
    int width() const;
    int height() const;

    // the cursor's 8:8:8 colour at screen pixel (x, y) of the shown frame, or false where it is transparent
    bool cursor_pixel(int x, int y, uint32_t &rgb) const;

    // the blitter job as BLT_OP starts it (immediately; BLT_BUSY then models its duration)
    void blit(unsigned op);

private:
    uint16_t read_reg(uint32_t offset);
    void write_reg(uint32_t offset, uint16_t value, unsigned be);
    void vblank_start();
};

#endif
