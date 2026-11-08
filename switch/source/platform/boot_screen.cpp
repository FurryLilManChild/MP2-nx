// mp2-nx: the boot screen's picture (the boot-time build), shared by the deko3d boot screen
// (switch/deko/dk_surface.cpp) and the Vulkan one (Aurora's webgpu/gpu.cpp): the title, what the game thread is
// doing (load_boost.cpp's mp2nx_boot_mark), the stored pipelines built so far and the seconds since the start,
// drawn by the CPU in RGBA8 with a 5x7 font.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" const char* mp2nx_boot_stage(void);                                   // load_boost.cpp
extern "C" uint64_t mp2nx_boot_ms(void);                                         // load_boost.cpp
extern "C" void aurora_pipeline_boot_progress(uint32_t* done, uint32_t* total);  // Aurora's pipeline_cache.cpp
extern "C" const char mp2nx_build_label[];                                       // <build>/mp2nx_build_id.c
extern "C" bool mp2nx_logs_ok(void);                                             // startup.cpp

namespace {
// 5x7 capitals, digits and a few marks; row bits, the leftmost column in bit 4
struct Glyph {
    char c;
    uint8_t rows[7];
};
constexpr Glyph kFont[] = {
    {'A', {14, 17, 17, 31, 17, 17, 17}}, {'B', {30, 17, 17, 30, 17, 17, 30}}, {'C', {14, 17, 16, 16, 16, 17, 14}},
    {'D', {30, 17, 17, 17, 17, 17, 30}}, {'E', {31, 16, 16, 30, 16, 16, 31}}, {'F', {31, 16, 16, 30, 16, 16, 16}},
    {'G', {14, 17, 16, 23, 17, 17, 15}}, {'H', {17, 17, 17, 31, 17, 17, 17}}, {'I', {14, 4, 4, 4, 4, 4, 14}},
    {'J', {7, 2, 2, 2, 2, 18, 12}},      {'K', {17, 18, 20, 24, 20, 18, 17}}, {'L', {16, 16, 16, 16, 16, 16, 31}},
    {'M', {17, 27, 21, 21, 17, 17, 17}}, {'N', {17, 17, 25, 21, 19, 17, 17}}, {'O', {14, 17, 17, 17, 17, 17, 14}},
    {'P', {30, 17, 17, 30, 16, 16, 16}}, {'Q', {14, 17, 17, 17, 21, 18, 13}}, {'R', {30, 17, 17, 30, 20, 18, 17}},
    {'S', {15, 16, 16, 14, 1, 1, 30}},   {'T', {31, 4, 4, 4, 4, 4, 4}},       {'U', {17, 17, 17, 17, 17, 17, 14}},
    {'V', {17, 17, 17, 17, 17, 10, 4}},  {'W', {17, 17, 17, 21, 21, 21, 10}}, {'X', {17, 17, 10, 4, 10, 17, 17}},
    {'Y', {17, 17, 17, 10, 4, 4, 4}},    {'Z', {31, 1, 2, 4, 8, 16, 31}},     {'0', {14, 17, 19, 21, 25, 17, 14}},
    {'1', {4, 12, 4, 4, 4, 4, 14}},      {'2', {14, 17, 1, 2, 4, 8, 31}},     {'3', {31, 2, 4, 2, 1, 17, 14}},
    {'4', {2, 6, 10, 18, 31, 2, 2}},     {'5', {31, 16, 30, 1, 1, 17, 14}},   {'6', {6, 8, 16, 30, 17, 17, 14}},
    {'7', {31, 1, 2, 4, 8, 8, 8}},       {'8', {14, 17, 17, 14, 17, 17, 14}}, {'9', {14, 17, 17, 15, 1, 2, 12}},
    {'/', {1, 1, 2, 4, 8, 16, 16}},      {'.', {0, 0, 0, 0, 0, 12, 12}},      {'-', {0, 0, 0, 31, 0, 0, 0}},
    {':', {0, 12, 12, 0, 12, 12, 0}},
};

struct Canvas {
    uint32_t* px = nullptr;
    uint32_t w = 0, h = 0;
    void rect(int x, int y, int rw, int rh, uint32_t c) {
        const int x0 = std::max(x, 0), y0 = std::max(y, 0);
        const int x1 = std::min<int>(x + rw, int(w)), y1 = std::min<int>(y + rh, int(h));
        for (int yy = y0; yy < y1; yy++)
            for (int xx = x0; xx < x1; xx++) px[size_t(yy) * w + size_t(xx)] = c;
    }
    static int text_width(const char* t, int scale) { return int(strlen(t)) * 6 * scale - scale; }
    void text(const char* t, int cx, int y, int scale, uint32_t c) {  // centred on cx
        int x = cx - text_width(t, scale) / 2;
        for (; *t; t++, x += 6 * scale) {
            char ch = *t >= 'a' && *t <= 'z' ? char(*t - 32) : *t;
            const Glyph* g = nullptr;
            for (const Glyph& k : kFont)
                if (k.c == ch) g = &k;
            if (!g) continue;  // (a space, or a mark the font lacks)
            for (int r = 0; r < 7; r++)
                for (int col = 0; col < 5; col++)
                    if (g->rows[r] & (16 >> col)) rect(x + col * scale, y + r * scale, scale, scale, c);
        }
    }
};

constexpr uint32_t rgba(uint32_t r, uint32_t g, uint32_t b) { return r | g << 8 | b << 16 | 0xFF000000u; }

void draw_boot_screen(Canvas& cv, const char* stage, uint32_t done, uint32_t total, uint64_t ms) {
    const float u = float(cv.h) / 720.f;  // (720p units)
    auto S = [u](float v) { return int(v * u + 0.5f); };
    cv.rect(0, 0, int(cv.w), int(cv.h), rgba(4, 6, 10));
    const int cx = int(cv.w) / 2;
    cv.text("METROID PRIME 2: ECHOES", cx, S(230), std::max(1, S(5)), rgba(210, 220, 230));
    cv.text(stage, cx, S(360), std::max(1, S(3)), rgba(160, 175, 190));
    const int bw = S(640), bh = S(14), bx = cx - bw / 2, by = S(430);
    const int edge = std::max(1, S(2));
    cv.rect(bx - edge, by - edge, bw + 2 * edge, bh + 2 * edge, rgba(60, 70, 85));
    cv.rect(bx, by, bw, bh, rgba(4, 6, 10));
    char line[96];
    if (total > 0) {
        const uint32_t d = std::min(done, total);
        cv.rect(bx, by, int(uint64_t(bw) * d / total), bh, rgba(40, 190, 200));
        if (d < total)
            snprintf(line, sizeof line, "PREPARING GRAPHICS %u / %u", d, total);
        else
            snprintf(line, sizeof line, "GRAPHICS READY");
    } else {
        // (no stored pipelines yet, or none at all: a moving block instead of a count)
        const int seg = bw / 6;
        const int pos = int((ms / 8) % uint64_t(bw + seg)) - seg;
        cv.rect(std::max(bx, bx + pos), by, std::min(seg, bw - std::max(pos, 0)) + std::min(pos, 0), bh,
                rgba(40, 190, 200));
        snprintf(line, sizeof line, "PREPARING GRAPHICS");
    }
    cv.text(line, cx, S(472), std::max(1, S(2)), rgba(120, 135, 150));
    snprintf(line, sizeof line, "%llu.%llu S", (unsigned long long)(ms / 1000), (unsigned long long)(ms / 100 % 10));
    cv.text(line, cx, S(640), std::max(1, S(2)), rgba(70, 80, 95));
    // which build this is, and whether its logs opened (the build-stamp build)
    snprintf(line, sizeof line, "%s - %s", mp2nx_build_label, mp2nx_logs_ok() ? "LOGS OK" : "LOG PROBLEM - SEE THE MENU");
    cv.text(line, cx, S(680), std::max(1, S(2)), mp2nx_logs_ok() ? rgba(70, 80, 95) : rgba(230, 90, 70));
}

}  // namespace

extern "C" void mp2nx_boot_screen_draw(uint32_t* px, uint32_t w, uint32_t h) {
    uint32_t done = 0, total = 0;
    aurora_pipeline_boot_progress(&done, &total);
    Canvas cv{px, w, h};
    draw_boot_screen(cv, mp2nx_boot_stage(), done, total, mp2nx_boot_ms());
}
