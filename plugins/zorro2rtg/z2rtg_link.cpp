// Board in the loop, the plugin's half (z2rtg_link.h).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "z2rtg_link.h"

#include <algorithm>

#include "zorro2rtg_regs.h"

static constexpr uint32_t VRAM_START = Z2R_VRAM_START;   // below: registers, palette, cursor image (in order)
static constexpr uint32_t WINDOW = 0x400000u;             // the 4 MB board window
static constexpr uint32_t VRAM_WORDS = (WINDOW - VRAM_START) / 2;

void Z2rtgLink::start(SendFn send, ReadFn read, bool in)
{
    q.clear();
    msg.clear();
    ptr_new = false;
    dirty.assign((VRAM_WORDS + 63) / 64, 0);
    groups.clear();
    out = std::move(send);
    rd = std::move(read);
    input = in;
    epoch++;
    sent = acked = 0;
    put('Z');
    msg.push_back(epoch);
    if (input)
        put('I');
    send_msg();
}

void Z2rtgLink::stop()
{
    out = nullptr;
    rd = nullptr;
    input = false;
    ptr_new = false;
    q.clear();
    msg.clear();
    groups.clear();
    std::fill(dirty.begin(), dirty.end(), 0);
}

void Z2rtgLink::write(uint32_t offset, uint16_t value)
{
    if (!out)
        return;
    offset &= 0x3FFFFEu;
    if (offset >= VRAM_START) {
        const uint32_t i = (offset - VRAM_START) / 2;
        uint64_t &g = dirty[i / 64];
        if (!g)
            groups.push_back(i / 64);
        g |= 1ull << (i % 64);
        return;
    }
    if (input && offset >= Z2R_REG_IN_STATUS && offset <= Z2R_REG_IN_POP)
        return;                                  // the real queue belongs to the peer (input on)
    if (offset == Z2R_REG_CURSOR_X || offset == Z2R_REG_CURSOR_Y) {
        (offset == Z2R_REG_CURSOR_X ? ptr_x : ptr_y) = value;   // the fast lane: the latest position only
        ptr_new = true;
        return;
    }
    q.push_back({offset, value, false});
    if (q.size() >= FLUSH_AT)
        flush();
}

void Z2rtgLink::wait_blitter()
{
    if (!out)
        return;
    q.push_back({0, 0, true});
}

void Z2rtgLink::send_msg()
{
    if (msg.empty())
        return;
    if (out && !out(msg.data(), static_cast<uint32_t>(msg.size())))
        stop();                                  // the channel went away under us
    messages++;
    sent++;
    msg.clear();
}

void Z2rtgLink::put(uint8_t rec)
{
    if (msg.size() + 5 > MSG_MAX)
        send_msg();
    msg.push_back(rec);
}

void Z2rtgLink::put_run(uint32_t off, const uint16_t *w, uint32_t n)
{
    while (n && out) {
        // as many words as fit in this message, at least one; else a new message
        uint32_t room = msg.size() + 6 + 2 < MSG_MAX ? static_cast<uint32_t>((MSG_MAX - msg.size() - 6) / 2) : 0;
        if (!room) {
            send_msg();
            continue;
        }
        const uint32_t k = std::min({n, room, RUN_MAX});
        msg.push_back('W');
        msg.push_back(static_cast<uint8_t>(off >> 16));
        msg.push_back(static_cast<uint8_t>(off >> 8));
        msg.push_back(static_cast<uint8_t>(off));
        msg.push_back(static_cast<uint8_t>(k >> 8));
        msg.push_back(static_cast<uint8_t>(k));
        for (uint32_t i = 0; i < k; i++) {
            msg.push_back(static_cast<uint8_t>(w[i] >> 8));
            msg.push_back(static_cast<uint8_t>(w[i]));
        }
        words += k;
        off += 2 * k;
        w += k;
        n -= k;
    }
}

void Z2rtgLink::put_vram()
{
    // every changed word, in address order, as runs of consecutive words with the model's values now
    std::sort(groups.begin(), groups.end());
    uint32_t start = 0;
    run.clear();
    for (const uint32_t gi : groups) {
        uint64_t bits = dirty[gi];
        dirty[gi] = 0;
        while (bits && out) {
            const uint32_t b = static_cast<uint32_t>(__builtin_ctzll(bits));
            bits &= bits - 1;
            const uint32_t off = VRAM_START + 2 * (gi * 64 + b);
            if (!run.empty() && (off != start + 2 * run.size() || run.size() >= RUN_MAX)) {
                put_run(start, run.data(), static_cast<uint32_t>(run.size()));
                run.clear();
            }
            if (run.empty())
                start = off;
            run.push_back(rd(off));
        }
    }
    if (!run.empty() && out)
        put_run(start, run.data(), static_cast<uint32_t>(run.size()));
    groups.clear();
}

void Z2rtgLink::flush(bool force)
{
    if (!out)
        return;
    if (ptr_new) {
        put('P');
        msg.insert(msg.end(), {static_cast<uint8_t>(ptr_x >> 8), static_cast<uint8_t>(ptr_x),
                               static_cast<uint8_t>(ptr_y >> 8), static_cast<uint8_t>(ptr_y)});
        ptr_new = false;
    }
    std::vector<uint16_t> regs;
    for (size_t i = 0; i < q.size() && out;) {
        if (q[i].wait) {
            put('B');
            barriers++;
            i++;
            continue;
        }
        // registers in order; consecutive offsets become one run
        const uint32_t start = q[i].off;
        regs.clear();
        while (i < q.size() && !q[i].wait && regs.size() < RUN_MAX && q[i].off == start + 2 * regs.size())
            regs.push_back(q[i++].v);
        put_run(start, regs.data(), static_cast<uint32_t>(regs.size()));
    }
    q.clear();
    if (!groups.empty()) {
        if (force || room())
            put_vram();
        else
            held++;
    }
    send_msg();
}

bool Z2rtgLink::receive(const uint8_t *m, uint32_t len, std::array<uint8_t, 7> &ev)
{
    if (len == 8 && m[0] == 'E') {
        std::copy(m + 1, m + 8, ev.begin());
        return true;
    }
    if (len == 6 && m[0] == 'A' && m[1] == epoch) {
        const uint32_t n = (static_cast<uint32_t>(m[2]) << 24) | (m[3] << 16) | (m[4] << 8) | m[5];
        acked = std::min(n, sent);
    }
    return false;
}
