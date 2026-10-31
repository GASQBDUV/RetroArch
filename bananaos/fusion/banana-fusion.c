// BananaOS: a companion screen on the bottom panel for Metroid Fusion in RetroArch (gpSP), preloaded into RetroArch.
// RetroArch holds DRM master and draws on the top panel through its KMS context; this takes its DRM fd the first
// time RetroArch flips a page or sets a mode, gives the bottom panel (DSI-1) a CRTC and a dumb buffer of its own,
// and draws there from a thread. The game's memory is read straight from the loaded libretro core
// (retro_get_memory_data), the core's path taken from RetroArch's command line.
// Build: aarch64-linux-gnu-gcc -O2 -fPIC -shared -I/usr/include/libdrm -o banana-fusion.so banana-fusion.c -ldl -lpthread -ldrm
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static int g_fd = -1;
static int g_started;
static char g_core[512], g_rom[1024];
static uint32_t* g_front;
static uint32_t* g_back;
static size_t g_bytes;
static int g_w, g_h, g_pitch;

#define RETRO_MEMORY_SYSTEM_RAM 2


// ---- drawing ---------------------------------------------------------------------------------------------------
#include <ctype.h>
#include <time.h>
#include <linux/input.h>
#include <math.h>
#include <sys/ioctl.h>
#include "pfont.h"
#include "fz_suit.h"

static int g_clipX0, g_clipY0, g_clipX1, g_clipY1;
static void fill(int x, int y, int w, int h, uint32_t c) {
    if (x < g_clipX0) { w -= g_clipX0 - x; x = g_clipX0; }
    if (y < g_clipY0) { h -= g_clipY0 - y; y = g_clipY0; }
    if (x + w > g_clipX1) w = g_clipX1 - x;
    if (y + h > g_clipY1) h = g_clipY1 - y;
    for (int j = 0; j < h; j++) {
        uint32_t* row = g_back + (size_t)(y + j) * g_pitch + x;
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

#define COLOR(r, g, b) (0xFF000000u | ((r) << 16) | ((g) << 8) | (b))
#define COL_BG COLOR(30, 33, 44)
#define COL_PANEL_BG COLOR(38, 42, 56)
#define COL_BORDER_DARK COLOR(58, 64, 86)
#define COL_DIM_GRAY COLOR(105, 110, 128)
#define COL_ENERGY_PIP COLOR(204, 71, 145)
#define COL_ACCENT COLOR(255, 158, 68)
#define COL_TAB_ACTIVE_BG COLOR(56, 61, 82)
#define COL_TAB_LABEL COLOR(205, 209, 225)
#define COL_SLOT_BG COLOR(48, 52, 68)
#define COL_WHITE COLOR(255, 255, 255)

typedef struct { float l, t, r, b; } RectF;
static float rw(RectF r) { return r.r - r.l; }
static float rh(RectF r) { return r.b - r.t; }
static float rcx(RectF r) { return (r.l + r.r) / 2; }
static float rcy(RectF r) { return (r.t + r.b) / 2; }
static int inrect(RectF r, float x, float y) { return x >= r.l && x < r.r && y >= r.t && y < r.b; }

static void rectf(float x0, float y0, float x1, float y1, uint32_t c) {
    int ix0 = (int)lroundf(x0), iy0 = (int)lroundf(y0), ix1 = (int)lroundf(x1), iy1 = (int)lroundf(y1);
    if (ix1 <= ix0) ix1 = ix0 + 1;
    if (iy1 <= iy0) iy1 = iy0 + 1;
    fill(ix0, iy0, ix1 - ix0, iy1 - iy0, c);
}

static void strokef(float x0, float y0, float x1, float y1, float sw, uint32_t c) {
    rectf(x0, y0, x1, y0 + sw, c);
    rectf(x0, y1 - sw, x1, y1, c);
    rectf(x0, y0, x0 + sw, y1, c);
    rectf(x1 - sw, y0, x1, y1, c);
}

static void roundFill(float x0, float y0, float x1, float y1, float rad, uint32_t c) {
    int iy0 = (int)lroundf(y0), iy1 = (int)lroundf(y1);
    for (int y = iy0; y < iy1; y++) {
        float dy = 0;
        if (y + 0.5f < y0 + rad) dy = y0 + rad - (y + 0.5f);
        else if (y + 0.5f > y1 - rad) dy = (y + 0.5f) - (y1 - rad);
        float inset = rad > 0 && dy > 0 ? rad - sqrtf(fmaxf(0, rad * rad - dy * dy)) : 0;
        rectf(x0 + inset, (float)y, x1 - inset, (float)y + 1, c);
    }
}

static void roundBox(RectF r, float rad, uint32_t fillc, uint32_t borderc, float sw) {
    roundFill(r.l, r.t, r.r, r.b, rad, borderc);
    roundFill(r.l + sw, r.t + sw, r.r - sw, r.b - sw, fmaxf(0, rad - sw), fillc);
}

static void pixelBox(RectF r, uint32_t fillc, uint32_t borderc) {
    float inset = fminf(rw(r), rh(r)) * 0.02f + 1.5f;
    rectf(r.l + inset, r.t + inset, r.r - inset, r.b - inset, fillc);
    strokef(r.l, r.t, r.r, r.b, inset, borderc);
}

static void circlef(float cx, float cy, float rad, uint32_t c) {
    for (int y = (int)floorf(cy - rad); y <= (int)ceilf(cy + rad); y++) {
        float dy = y + 0.5f - cy;
        if (fabsf(dy) > rad) continue;
        float dx = sqrtf(rad * rad - dy * dy);
        rectf(cx - dx, (float)y, cx + dx, (float)y + 1, c);
    }
}

// the 5x7 pixel font; align 0 left, 1 centre, 2 right. y is the top of the glyphs.
static const uint8_t* glyph(char c) {
    if (c >= 'a' && c <= 'z') c -= 32;
    for (size_t i = 0; i < sizeof kFont / sizeof *kFont; i++)
        if (kFont[i].c == c) return kFont[i].rows;
    return NULL;
}
static float textW(const char* s, float ps) { return ((float)strlen(s) * 6 - 1) * ps; }
static float pixelSizeFor(float h) { return h / 7.0f; }
static void text(const char* s, float x, float y, float ps, uint32_t c, int align) {
    float w = textW(s, ps);
    float cx = align == 1 ? x - w / 2 : align == 2 ? x - w : x;
    for (; *s; s++) {
        const uint8_t* g = glyph(*s);
        if (g)
            for (int r = 0; r < 7; r++)
                for (int k = 0; k < 5; k++)
                    if (g[r] & (0x10 >> k)) rectf(cx + k * ps, y + r * ps, cx + (k + 1) * ps, y + (r + 1) * ps, c);
        cx += 6 * ps;
    }
}

typedef struct { int w, h; uint32_t* px; } Img;
// nearest-neighbour, 0 alpha = transparent
static void blit(const Img* im, float x0, float y0, float x1, float y1) {
    if (!im->px || x1 <= x0 || y1 <= y0) return;
    int ix0 = (int)lroundf(x0), ix1 = (int)lroundf(x1), iy0 = (int)lroundf(y0), iy1 = (int)lroundf(y1);
    for (int y = iy0; y < iy1; y++) {
        if (y < g_clipY0 || y >= g_clipY1) continue;
        int sy = (int)((y + 0.5f - y0) * im->h / (y1 - y0));
        if (sy < 0) sy = 0;
        if (sy >= im->h) sy = im->h - 1;
        for (int x = ix0; x < ix1; x++) {
            if (x < g_clipX0 || x >= g_clipX1) continue;
            int sx = (int)((x + 0.5f - x0) * im->w / (x1 - x0));
            if (sx < 0) sx = 0;
            if (sx >= im->w) sx = im->w - 1;
            uint32_t p = im->px[sy * im->w + sx];
            if (p >> 24) g_back[(size_t)y * g_pitch + x] = p;
        }
    }
}


// ---- the game memory: EWRAM from libretro, IWRAM 0x20000 below it in the core data ---------------------------------------
typedef void* (*get_data_t)(unsigned);
typedef size_t (*get_size_t)(unsigned);
static const uint8_t *g_iw, *g_ew;

static int find_ram(void) {
    void* h = dlopen(g_core, RTLD_NOLOAD | RTLD_LAZY);
    if (!h) return 0;
    get_data_t d = (get_data_t)dlsym(h, "retro_get_memory_data");
    get_size_t s = (get_size_t)dlsym(h, "retro_get_memory_size");
    if (!d || !s) return 0;
    for (unsigned id = 0; id < 8; id++) {
        size_t sz = s(id);
        void* p = sz ? d(id) : NULL;
        fprintf(stderr, "banana-fusion: memory %u: %zu bytes at %p\n", id, sz, p);
        if (p && sz == 0x40000) g_ew = p;
        if (p && sz == 0x8000) g_iw = p;
    }
    if (g_ew && !g_iw) g_iw = g_ew - 0x20000; // gpSP keeps IWRAM right below EWRAM
    fprintf(stderr, "banana-fusion: ewram %p iwram %p\n", (void*)g_ew, (void*)g_iw);
    return g_ew && g_iw;
}

static int rd(uint32_t a, int n) {
    const uint8_t* p = (a >> 24) == 3 ? g_iw + (a & 0x7FFF) : g_ew + (a & 0x3FFFF);
    int v = 0;
    for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
#define U8(a) rd(a, 1)
#define U16(a) rd(a, 2)

#define A_EN 0x03001310
#define A_ENMAX 0x03001312
#define A_MIS 0x03001314
#define A_MISMAX 0x03001316
#define A_PB 0x03001318
#define A_PBMAX 0x03001319
#define A_BEAM 0x0300131A
#define A_MTYPE 0x0300131B
#define A_SUIT 0x0300131C
#define A_LOCKS 0x0300131D
#define A_MAPS 0x0300131E
#define A_AREA 0x0300002C
#define A_ROOM 0x0300002D
#define A_TH 0x03000138
#define A_MAPX 0x03000031
#define A_MAPY 0x03000032
#define A_RMX 0x030000B2
#define A_RMY 0x030000B3
#define A_LW 0x03000090
#define A_LH 0x03000092
#define A_EXPL 0x0300003B

// ---- what the player has set ----------------------------------------------------------------------------------------
enum { TAB_MAP, TAB_ITEMS, TAB_SETUP };
static int g_tab = TAB_MAP;
static int g_showBar[3] = { 1, 1, 1 };
static int g_showTarget = 1;
static char g_prefs[512];

static void prefs_load(void) {
    FILE* f = fopen(g_prefs, "r");
    if (!f) return;
    int a, b, c, t, tg = 1;
    int n = fscanf(f, "%d %d %d %d %d", &a, &b, &c, &t, &tg);
    if (n >= 4) { g_showBar[0] = a; g_showBar[1] = b; g_showBar[2] = c; if (t >= 0 && t < 3) g_tab = t; }
    if (n == 5) g_showTarget = tg != 0;
    fclose(f);
}
static void prefs_save(void) {
    FILE* f = fopen(g_prefs, "w");
    if (!f) return;
    fprintf(f, "%d %d %d %d %d\n", g_showBar[0], g_showBar[1], g_showBar[2], g_tab, g_showTarget);
    fclose(f);
}

// ---- touch: the bottom panel's Goodix screen, read straight from evdev -------------------------------------------------
static volatile int g_tapX = -1, g_tapY = -1;
static volatile int g_touchX = -1, g_touchY = -1, g_touchDown;
static volatile int g_longX = -1, g_longY = -1;
static volatile int g_dragDX, g_dragDY, g_downX = -1, g_downY = -1;

static void* touch_thread(void* arg) {
    (void)arg;
    int fd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
    if (fd < 0) return NULL;
    struct input_absinfo ax = { 0 }, ay = { 0 };
    ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax);
    ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay);
    int rx = 0, ry = 0, sx = 0, sy = 0, moved = 0, down = 0, lastX = 0, lastY = 0, longFired = 0;
    long downMs = 0;
    for (;;) {
        struct input_event ev;
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long nowMs = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
        if (down && !moved && !longFired && nowMs - downMs > 600 && ax.maximum > ax.minimum && ay.maximum > ay.minimum) {
            longFired = 1;
            g_longX = (sx - ax.minimum) * g_w / (ax.maximum - ax.minimum);
            g_longY = (sy - ay.minimum) * g_h / (ay.maximum - ay.minimum);
        }
        ssize_t n = read(fd, &ev, sizeof ev);
        if (n != (ssize_t)sizeof ev) { usleep(4000); continue; }
        if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_X) rx = ev.value;
        else if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_Y) ry = ev.value;
        else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            if (ev.value && !down && ax.maximum > ax.minimum && ay.maximum > ay.minimum) {
                sx = rx; sy = ry; moved = 0; longFired = 0; downMs = nowMs;
                g_downX = lastX = (rx - ax.minimum) * g_w / (ax.maximum - ax.minimum);
                g_downY = lastY = (ry - ay.minimum) * g_h / (ay.maximum - ay.minimum);
            }
            if (!ev.value && down && !moved && !longFired && ax.maximum > ax.minimum && ay.maximum > ay.minimum) {
                g_tapX = (sx - ax.minimum) * g_w / (ax.maximum - ax.minimum);
                g_tapY = (sy - ay.minimum) * g_h / (ay.maximum - ay.minimum);
            }
            down = ev.value != 0;
            g_touchDown = down;
        } else if (ev.type == EV_SYN) {
            if (down && (abs(rx - sx) > (ax.maximum - ax.minimum) / 40 || abs(ry - sy) > (ay.maximum - ay.minimum) / 40)) moved = 1;
            if (ax.maximum > ax.minimum && ay.maximum > ay.minimum) {
                g_touchX = (rx - ax.minimum) * g_w / (ax.maximum - ax.minimum);
                g_touchY = (ry - ay.minimum) * g_h / (ay.maximum - ay.minimum);
                if (down && moved) {
                    g_dragDX += g_touchX - lastX; g_dragDY += g_touchY - lastY;
                    lastX = g_touchX; lastY = g_touchY;
                }
            }
        }
    }
    return NULL;
}

// ---- the screens -----------------------------------------------------------------------------------------------------
static RectF g_tabRect[3], g_setupRow[4], g_mapHdr;
static const char* kTabLabel[3] = { "MAP", "ITEMS", "SETUP" };
#define COL_LOW COLOR(230, 57, 57)

static void layout_tabs(RectF bar) {
    float gap = rw(bar) * 0.012f, tw = (rw(bar) - gap * 2) / 3;
    for (int i = 0; i < 3; i++) {
        float x0 = bar.l + i * (tw + gap);
        g_tabRect[i] = (RectF){ x0, bar.t, x0 + tw, bar.b };
    }
}

static void draw_tab_bar(void) {
    for (int i = 0; i < 3; i++) {
        RectF r = g_tabRect[i];
        int active = g_tab == i;
        pixelBox(r, active ? COL_TAB_ACTIVE_BG : COL_PANEL_BG, active ? COL_ACCENT : COL_BORDER_DARK);
        float ts = rh(r) * 0.4f;
        text(kTabLabel[i], rcx(r), rcy(r) - ts / 2, pixelSizeFor(ts), COL_TAB_LABEL, 1);
    }
}

static float draw_ammo(const char* label, int count, float x, float midY, float textSize, float stripH) {
    float ps = pixelSizeFor(textSize), ls = pixelSizeFor(textSize * 0.6f);
    char buf[16];
    text(label, x, midY - textSize * 0.3f, ls, COL_ACCENT, 0);
    x += textW(label, ls) + stripH * 0.15f;
    snprintf(buf, sizeof buf, "%d", count);
    text(buf, x, midY - textSize / 2, ps, COL_WHITE, 0);
    return x + textW(buf, ps) + stripH * 0.5f;
}

static void draw_strip(RectF strip, float stripH) {
    roundBox(strip, stripH * 0.1f, COL_PANEL_BG, COL_BORDER_DARK, fmaxf(2, stripH * 0.02f));
    int hp = U16(A_EN), maxHp = U16(A_ENMAX);
    float textSize = stripH * 0.32f, pipSize = stripH * 0.28f, pipGap = stripH * 0.07f;
    int maxTanks = (maxHp - 99) / 100;
    if (maxTanks < 0) maxTanks = 0;
    if (maxTanks > 14) maxTanks = 14;
    int filled = hp / 100 > maxTanks ? maxTanks : hp / 100;
    int cols = maxTanks < 7 ? maxTanks : 7, rowsN = maxTanks <= 7 ? 1 : 2;
    float rowStep = pipSize * 1.3f, blockH = rowStep * (rowsN - 1) + pipSize;
    float blockTop = strip.t + (stripH - blockH) * 0.5f, pipLeft = strip.l + stripH * 0.3f;
    uint32_t pipCol = hp < 30 ? COL_LOW : COL_ENERGY_PIP;
    for (int i = 0; i < maxTanks; i++) {
        int row = i / 7, col = i % 7;
        float midY = blockTop + rowStep * row + pipSize * 0.5f, px = pipLeft + col * (pipSize + pipGap);
        rectf(px, midY - pipSize / 2, px + pipSize, midY + pipSize / 2, i < filled ? pipCol : COL_BORDER_DARK);
        if (i < filled) {
            float th = pipSize * 0.16f;
            rectf(px, midY - pipSize / 2, px + pipSize, midY - pipSize / 2 + th, COL_WHITE);
            rectf(px, midY - pipSize / 2, px + th, midY + pipSize / 2, COL_WHITE);
        }
    }
    float midY = strip.t + stripH * 0.5f, ps = pixelSizeFor(textSize);
    float x = pipLeft + cols * (pipSize + pipGap) + stripH * 0.2f;
    char buf[16];
    snprintf(buf, sizeof buf, "%d", maxTanks > 0 ? hp % 100 : hp);
    text(buf, x, midY - textSize / 2, ps, COL_WHITE, 0);
    x += textW(buf, ps) + stripH * 0.6f;
    if (U16(A_MISMAX) > 0) x = draw_ammo("MIS", U16(A_MIS), x, midY, textSize, stripH);
    if (U8(A_PBMAX) > 0) draw_ammo("PB", U8(A_PB), x, midY, textSize, stripH);
}

static void stat_box(float x, float top, float w, float h, const char* label, const char* value) {
    RectF box = { x, top, x + w, top + h };
    pixelBox(box, COL_SLOT_BG, COL_BORDER_DARK);
    text(label, box.l + w * 0.06f, box.t + h * 0.12f, pixelSizeFor(h * 0.22f), COL_DIM_GRAY, 0);
    text(value, box.l + w * 0.06f, box.b - h * 0.14f - h * 0.36f, pixelSizeFor(h * 0.36f), COL_WHITE, 0);
}

typedef struct { const char* title; int n; int bits[8]; const char* labels[8]; int* src; } Equip;
static int g_beam, g_mtype, g_suit;
static const Equip kBeam = { "BEAM", 5, { 1, 2, 4, 8, 0x10 }, { "CHARGE", "WIDE", "PLASMA", "WAVE", "ICE" }, &g_beam };
static const Equip kMissile = { "MISSILE", 3, { 2, 4, 8 }, { "SUPER", "ICE", "DIFFUSION" }, &g_mtype };
static const Equip kBomb = { "BOMB", 1, { 0x20 }, { "POWER" }, &g_mtype };
static const Equip kSuit = { "SUIT", 2, { 0x10, 0x20 }, { "VARIA", "GRAVITY" }, &g_suit };
static const Equip kMisc = { "MISC.", 5, { 0x40, 1, 2, 4, 8 }, { "MORPH BALL", "HI-JUMP", "SPEED BOOSTER", "SPACE JUMP", "SCREW ATTACK" }, &g_suit };

static void equip_column(float x, float top, float w, float h, const Equip* const* groups, int ng) {
    int total = 0;
    for (int g = 0; g < ng; g++) total += groups[g]->n;
    float gap = h * 0.04f, unitH = (h - gap * (ng - 1)) / total;
    float titleSize = unitH * 0.34f, entrySize = unitH * 0.40f, maxLabelW = w * 0.80f;
    float eps = pixelSizeFor(entrySize), widest = 0;
    for (int g = 0; g < ng; g++)
        for (int i = 0; i < groups[g]->n; i++) widest = fmaxf(widest, textW(groups[g]->labels[i], eps));
    if (widest > maxLabelW) { float k = maxLabelW / widest; eps *= k; entrySize *= k; }
    float y = top;
    for (int g = 0; g < ng; g++) {
        const Equip* e = groups[g];
        float boxH = unitH * e->n;
        RectF box = { x, y, x + w, y + boxH };
        pixelBox(box, COL_SLOT_BG, COL_BORDER_DARK);
        text(e->title, box.l + w * 0.06f, box.t + titleSize * 0.5f, pixelSizeFor(titleSize), COL_ACCENT, 0);
        float rowH = (boxH - titleSize * 1.6f) / e->n, rowY = box.t + titleSize * 1.6f;
        for (int i = 0; i < e->n; i++) {
            int got = (*e->src & e->bits[i]) != 0;
            float cy = rowY + i * rowH + rowH * 0.65f, dotR = entrySize * 0.28f;
            float dcx = box.l + w * 0.09f, dcy = cy - entrySize * 0.32f;
            if (got) circlef(dcx, dcy, dotR, COL_ACCENT);
            else { circlef(dcx, dcy, dotR, COL_BORDER_DARK); circlef(dcx, dcy, dotR - fmaxf(1.5f, dotR * 0.25f), COL_SLOT_BG); }
            text(got ? e->labels[i] : "???", box.l + w * 0.16f, cy - entrySize * 0.7f, eps, got ? COL_WHITE : COL_DIM_GRAY, 0);
        }
        y += boxH + gap;
    }
}

static void draw_items(RectF area) {
    roundBox(area, rh(area) * 0.02f, COL_PANEL_BG, COL_BORDER_DARK, fmaxf(2, rh(area) * 0.01f));
    g_beam = U8(A_BEAM); g_mtype = U8(A_MTYPE); g_suit = U8(A_SUIT);
    float pad = rw(area) * 0.03f, left = area.l + pad, right = area.r - pad, top = area.t + pad, bottom = area.b - pad;
    float colGap = pad * 0.8f, headerH = (bottom - top) * 0.16f, hgap = pad * 0.4f;
    char tm[24], tk[24];
    int tanks = (U16(A_ENMAX) - 99) / 100;
    snprintf(tm, sizeof tm, "%d:%02d", U8(A_TH), U8(A_TH + 1));
    snprintf(tk, sizeof tk, "%d", tanks < 0 ? 0 : tanks);
    float bw = (right - left - hgap) / 2;
    stat_box(left, top, bw, headerH, "ENERGY TANKS", tk);
    stat_box(left + bw + hgap, top, bw, headerH, "TIME", tm);
    float bodyTop = top + headerH + pad * 0.6f, bodyH = bottom - bodyTop;
    float aspect = (float)FZ_SUIT_W / FZ_SUIT_H;
    float suitW = fminf((right - left) * 0.3f, bodyH * aspect);
    float colW = (right - left - colGap * 2 - suitW) / 2;
    const Equip* c1[2] = { &kBeam, &kMissile };
    const Equip* c2[3] = { &kBomb, &kSuit, &kMisc };
    equip_column(left, bodyTop, colW, bodyH, c1, 2);
    float dh = bodyH, dw = dh * aspect;
    if (dw > suitW) { dw = suitW; dh = dw / aspect; }
    float dl = left + colW + colGap + (suitW - dw) / 2, dt = bodyTop + (bodyH - dh) / 2;
    static Img suit;
    if (!suit.px) { suit.w = FZ_SUIT_W; suit.h = FZ_SUIT_H; suit.px = (uint32_t*)fz_suit; }
    blit(&suit, dl, dt, dl + dw, dt + dh);
    equip_column(left + colW + colGap + suitW + colGap, bodyTop, colW, bodyH, c2, 3);
}

// ---- the map: the game keeps the whole sector as a 32x32 tilemap at 0x02034800 (entry = x + y * 32, the same x and y the game uses for
// the map position at 0x03000031). The tiles are 8x8 4bpp graphics in the ROM, drawn the way the game draws them. Only the
// sector Samus is in is in memory, so every sector is remembered in a file the last time it was seen. -------------------
#define A_MAPDATA 0x02034800   // the whole sector, hidden rooms included
#define A_MAPICONS 0x03001690
#define A_GMODE 0x03000BDE
#define A_EVENT 0x03000B87
#define A_MAPSHOWN 0x02034000   // what the game shows: the rooms it has revealed, the objective path in palette 1
#define ADDR_MAP_GFX 0x561FA8
#define ADDR_MAP_PAL 0x5657A8
#define MAP_TILES 0x1C0
#define MAP_BLANK 0x00A0
static uint8_t* g_romData;
static long g_romSize;
static int g_romOk;
static uint16_t g_mapCache[7][1024];
// where the objective is, per sector, as last seen while the game's own map screen was up (the icon list only exists then)
static struct { int8_t x[7], y[7]; uint8_t ev; } g_tgt = { { -1, -1, -1, -1, -1, -1, -1 }, { -1, -1, -1, -1, -1, -1, -1 }, 0 };
static void tgt_clear(void) { memset(g_tgt.x, -1, sizeof g_tgt.x); memset(g_tgt.y, -1, sizeof g_tgt.y); }
static char g_mapFile[512];
static int g_mapSel = -1;
static Img g_mapImg;

static void load_rom(void) {
    if (g_romData || !g_rom[0]) return;
    FILE* f = fopen(g_rom, "rb");
    if (!f) { fprintf(stderr, "banana-fusion: cannot open the ROM %s\n", g_rom); return; }
    fseek(f, 0, SEEK_END);
    g_romSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_romData = malloc(g_romSize);
    if (!g_romData || fread(g_romData, 1, g_romSize, f) != (size_t)g_romSize) { free(g_romData); g_romData = NULL; }
    fclose(f);
    // the US ROM keeps the map palette first colour here; anything else means other addresses
    g_romOk = g_romData && g_romSize >= 0x566000 && g_romData[ADDR_MAP_PAL] == 0x25 && g_romData[ADDR_MAP_PAL + 1] == 0x2A;
    if (!g_romOk) fprintf(stderr, "banana-fusion: this ROM is not the US version, no map graphics\n");
}

static uint32_t bgr555(int c) {
    return COLOR((c & 31) * 255 / 31, ((c >> 5) & 31) * 255 / 31, ((c >> 10) & 31) * 255 / 31);
}

// bank 2 tiles are rooms the game keeps hidden (they light up as the objective path) and the little labels (elevator numbers):
// only the labels, which have no fill colour (2 or 3), are shown
static int tile_label(int id) {
    for (int i = 0; i < 64; i++) {
        int b = g_romData[ADDR_MAP_GFX + id * 32 + i / 2], ci = (i & 1) ? (b >> 4) : (b & 15);
        if (ci == 2 || ci == 3) return 0;
    }
    return 1;
}

static uint32_t map_color(int bank, int ci) {
    if (bank > 6) bank = 0;
    int o = ADDR_MAP_PAL + bank * 32 + ci * 2;
    return bgr555(g_romData[o] | (g_romData[o + 1] << 8));
}

static void map_cache_load(void) {
    FILE* f = fopen(g_mapFile, "rb");
    if (!f) return;
    if (fread(g_mapCache, 1, sizeof g_mapCache, f) != sizeof g_mapCache) memset(g_mapCache, 0, sizeof g_mapCache);
    else if (fread(&g_tgt, 1, sizeof g_tgt, f) != sizeof g_tgt) tgt_clear();
    fclose(f);
}

static void map_cache_save(void) {
    FILE* f = fopen(g_mapFile, "wb");
    if (!f) return;
    fwrite(g_mapCache, 1, sizeof g_mapCache, f);
    fwrite(&g_tgt, 1, sizeof g_tgt, f);
    fclose(f);
}

// the map as the game shows it: the revealed rooms (with the objective path), plus the small labels (elevator numbers) that
// the game keeps among the hidden rooms
static int read_grid(uint16_t* out) {
    int used = 0;
    for (int i = 0; i < 1024; i++) {
        int a = (int)rd(A_MAPSHOWN + i * 2, 2), b = (int)rd(A_MAPDATA + i * 2, 2);
        if (a == MAP_BLANK && b != MAP_BLANK && ((b >> 12) & 15) == 2 && (b & 0x3FF) < MAP_TILES && tile_label(b & 0x3FF)) a = b;
        out[i] = (uint16_t)a;
        if (a != MAP_BLANK) used++;
    }
    return used;
}

// the objective ring is a sprite whose list (x, y, 0, type per icon: the objective first, then Samus) only holds real data while the game's
// map screen is open, so it is picked up then and remembered; a new story event makes it stale
static void target_track(void) {
    int ar = U8(A_AREA);
    if (ar >= 7) return;
    int dirty = 0;
    if (U8(A_EVENT) != g_tgt.ev) { tgt_clear(); g_tgt.ev = U8(A_EVENT); dirty = 1; }
    if (U8(A_GMODE) == 3) {
        // entry 0 is the objective and entry 1 is Samus (the type byte only blinks); the list is real when entry 1 is where Samus is
        int tx = U8(A_MAPICONS), ty = U8(A_MAPICONS + 1);
        int valid = U8(A_MAPICONS + 4) == U8(A_MAPX) && U8(A_MAPICONS + 5) == U8(A_MAPY) && U8(A_MAPICONS + 7) != 0;
        if (tx > 31 || ty > 31) valid = 0;
        if (valid && (g_tgt.x[ar] != tx || g_tgt.y[ar] != ty)) { g_tgt.x[ar] = (int8_t)tx; g_tgt.y[ar] = (int8_t)ty; dirty = 1; }
    }
    if (dirty) map_cache_save();
}

// every live frame: keep the map of the sector Samus is in, once the sector has stayed the same for a second
static void map_track(void) {
    static int lastArea = -1, stable;
    int ar = U8(A_AREA);
    if (ar >= 7) return;
    if (ar != lastArea) { lastArea = ar; stable = 0; return; }
    if (++stable < 60) return;
    stable = 60;
    load_rom();
    if (!g_romOk) return;
    uint16_t now[1024];
    int used = read_grid(now);
    if (used < 8 || !memcmp(now, g_mapCache[ar], sizeof now)) return;
    static time_t lastSave;
    memcpy(g_mapCache[ar], now, sizeof now);
    time_t t = time(NULL);
    if (t != lastSave) { lastSave = t; map_cache_save(); }
}

// zoom and pan like the other ports: buttons under the map, drag to move; g_zoom 1 = about 16 tiles across
static float g_zoom = 1.0f, g_panX, g_panY, g_cellsPerPx;
static RectF g_mapRect, g_resetBtn, g_zoomOutBtn, g_zoomInBtn;

static void blit_view(const Img* im, float dl, float dt, float sc, RectF v) {
    int x0 = (int)fmaxf(fmaxf(ceilf(v.l), ceilf(dl)), 0), x1 = (int)fminf(fminf(floorf(v.r), floorf(dl + im->w * sc)), (float)g_w);
    int y0 = (int)fmaxf(fmaxf(ceilf(v.t), ceilf(dt)), 0), y1 = (int)fminf(fminf(floorf(v.b), floorf(dt + im->h * sc)), (float)g_h);
    for (int y = y0; y < y1; y++) {
        int sy = (int)((y + 0.5f - dt) / sc);
        if (sy < 0 || sy >= im->h) continue;
        for (int x = x0; x < x1; x++) {
            int sx = (int)((x + 0.5f - dl) / sc);
            if (sx < 0 || sx >= im->w) continue;
            uint32_t p = im->px[sy * im->w + sx];
            if (p >> 24) g_back[(size_t)y * g_pitch + x] = p;
        }
    }
}

static void layout_map_buttons(RectF bar) {
    float gap = rw(bar) * 0.02f, bw = (rw(bar) - gap * 2) / 3;
    g_resetBtn = (RectF){ bar.l, bar.t, bar.l + bw, bar.b };
    g_zoomOutBtn = (RectF){ bar.l + bw + gap, bar.t, bar.l + 2 * bw + gap, bar.b };
    g_zoomInBtn = (RectF){ bar.l + 2 * (bw + gap), bar.t, bar.r, bar.b };
}

static void draw_map_buttons(void) {
    pixelBox(g_resetBtn, COL_PANEL_BG, COL_BORDER_DARK);
    float cx = rcx(g_resetBtn), cy = rcy(g_resetBtn), m = fminf(rw(g_resetBtn), rh(g_resetBtn));
    float r = m * 0.26f, gp = r * 0.4f, t = fmaxf(2, m * 0.08f);
    circlef(cx, cy, r, COL_ACCENT);
    circlef(cx, cy, r - t, COL_PANEL_BG);
    rectf(cx - r - gp, cy - t / 2, cx - r + gp, cy + t / 2, COL_ACCENT);
    rectf(cx + r - gp, cy - t / 2, cx + r + gp, cy + t / 2, COL_ACCENT);
    rectf(cx - t / 2, cy - r - gp, cx + t / 2, cy - r + gp, COL_ACCENT);
    rectf(cx - t / 2, cy + r - gp, cx + t / 2, cy + r + gp, COL_ACCENT);
    pixelBox(g_zoomInBtn, COL_PANEL_BG, COL_BORDER_DARK);
    pixelBox(g_zoomOutBtn, COL_PANEL_BG, COL_BORDER_DARK);
    float half = fminf(rw(g_zoomInBtn), rh(g_zoomInBtn)) * 0.28f, tt = fmaxf(2, fminf(rw(g_zoomInBtn), rh(g_zoomInBtn)) * 0.08f);
    float x1 = rcx(g_zoomInBtn), y1 = rcy(g_zoomInBtn), x2 = rcx(g_zoomOutBtn), y2 = rcy(g_zoomOutBtn);
    rectf(x1 - half, y1 - tt / 2, x1 + half, y1 + tt / 2, COL_ACCENT);
    rectf(x1 - tt / 2, y1 - half, x1 + tt / 2, y1 + half, COL_ACCENT);
    rectf(x2 - half, y2 - tt / 2, x2 + half, y2 + tt / 2, COL_ACCENT);
}

static const char* kAreaName[7] = { "MAIN DECK", "SECTOR 1 SRX", "SECTOR 2 TRO", "SECTOR 3 PYR", "SECTOR 4 AQA", "SECTOR 5 ARC", "SECTOR 6 NOC" };

static void draw_map(RectF area) {
    roundBox(area, rh(area) * 0.02f, COL_PANEL_BG, COL_BORDER_DARK, fmaxf(2, rh(area) * 0.01f));
    float pad = rw(area) * 0.03f, left = area.l + pad, right = area.r - pad, top = area.t + pad;
    float bh = rh(area) * 0.13f, gap = pad * 0.5f;
    int cur = U8(A_AREA);
    static int lastCur = -1;
    if (cur != lastCur) { lastCur = cur; g_mapSel = -1; }
    int sel = g_mapSel >= 0 ? g_mapSel : cur;
    if (sel > 6) sel = 0;
    char b1[32], b2[32];
    snprintf(b1, sizeof b1, "%d:%02d", U8(A_TH), U8(A_TH + 1));
    snprintf(b2, sizeof b2, "%d%%", U8(A_EXPL + sel));
    float w1 = (right - left - gap * 2) * 0.5f, w2 = (right - left - gap * 2) * 0.25f;
    g_mapHdr = (RectF){ left, top, left + w1, top + bh };
    stat_box(left, top, w1, bh, "SECTOR (TAP)", kAreaName[sel]);
    stat_box(left + w1 + gap, top, w2, bh, "EXPLORED", b2);
    stat_box(left + w1 + w2 + gap * 2, top, w2, bh, "TIME", b1);
    top += bh + gap;

    RectF box = { left, top, right, area.b - pad };
    pixelBox(box, COL_SLOT_BG, COL_BORDER_DARK);
    load_rom();
    const uint16_t* grid = g_mapCache[sel];
    uint16_t live[1024];
    if (sel == cur) {   // the sector Samus is in is read straight from the game
        read_grid(live);
        grid = live;
    }
    float ts = rh(box) * 0.07f;
    if (!g_romOk) {
        text(g_romData ? "ROM NOT SUPPORTED" : "ROM NOT FOUND", rcx(box), rcy(box) - ts / 2, pixelSizeFor(ts), COL_DIM_GRAY, 1);
        return;
    }
    int x0 = 32, y0 = 32, x1 = -1, y1 = -1;
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++)
            if (grid[y * 32 + x] != MAP_BLANK && (grid[y * 32 + x] & 0x3FF) < MAP_TILES) {
                if (x < x0) x0 = x;
                if (y < y0) y0 = y;
                if (x > x1) x1 = x;
                if (y > y1) y1 = y;
            }
    if (x1 < 0) {
        text("NOT SEEN YET", rcx(box), rcy(box) - ts / 2, pixelSizeFor(ts), COL_DIM_GRAY, 1);
        return;
    }
    int cols = x1 - x0 + 1, rows = y1 - y0 + 1;
    if (!g_mapImg.px) { g_mapImg.px = calloc(256 * 256, 4); }
    g_mapImg.w = cols * 8; g_mapImg.h = rows * 8;
    memset(g_mapImg.px, 0, (size_t)g_mapImg.w * g_mapImg.h * 4);
    for (int gy = y0; gy <= y1; gy++)
        for (int gx = x0; gx <= x1; gx++) {
            int v = grid[gy * 32 + gx];
            int id = v & 0x3FF, flip = (v >> 10) & 3, bank = (v >> 12) & 15;
            if (v == MAP_BLANK || id >= MAP_TILES) continue;
            for (int row = 0; row < 8; row++) {
                int sr = (flip & 2) ? 7 - row : row;
                for (int col = 0; col < 8; col++) {
                    int sc = (flip & 1) ? 7 - col : col;
                    int b = g_romData[ADDR_MAP_GFX + id * 32 + sr * 4 + (sc >> 1)];
                    int ci = (sc & 1) == 0 ? (b & 15) : (b >> 4);
                    if (ci) g_mapImg.px[((gy - y0) * 8 + row) * g_mapImg.w + (gx - x0) * 8 + col] = map_color(bank, ci);
                }
            }
        }
    static int lastSel = -1;
    if (sel != lastSel) { lastSel = sel; g_panX = g_panY = 0; }
    g_mapRect = box;
    float fit = fminf(rw(box) / g_mapImg.w, rh(box) / g_mapImg.h);   // the whole sector in view
    float def = rw(box) / 152.0f, maxSc = rw(box) / 48.0f;
    if (fit > maxSc) fit = maxSc;
    float sc = def * g_zoom;
    if (sc < fit) sc = fit;
    if (sc > maxSc) sc = maxSc;
    g_zoom = sc / def;
    g_cellsPerPx = 1.0f / (8 * sc);
    int mx = U8(A_MAPX), my = U8(A_MAPY);
    int onMap = sel == cur && mx >= x0 && mx <= x1 && my >= y0 && my <= y1;
    float ctrX = onMap ? mx - x0 + 0.5f : cols / 2.0f, ctrY = onMap ? my - y0 + 0.5f : rows / 2.0f;
    float cx = fmaxf(0, fminf((float)cols, ctrX + g_panX)), cy = fmaxf(0, fminf((float)rows, ctrY + g_panY));
    g_panX = cx - ctrX; g_panY = cy - ctrY;
    float dl = rcx(box) - cx * 8 * sc, dt = rcy(box) - cy * 8 * sc;
    RectF view = { box.l + 2, box.t + 2, box.r - 2, box.b - 2 };
    blit_view(&g_mapImg, dl, dt, sc, view);
    if (onMap) {
        struct timespec tsp;
        clock_gettime(CLOCK_MONOTONIC, &tsp);
        if ((tsp.tv_nsec / 400000000) % 2 == 0) {
            float cs = 8 * sc, cl = dl + (mx - x0) * cs, ct = dt + (my - y0) * cs, d = cs * 0.22f;
            if (cl + d >= view.l && cl + cs - d <= view.r && ct + d >= view.t && ct + cs - d <= view.b)
                rectf(cl + d, ct + d, cl + cs - d, ct + cs - d, COLOR(250, 220, 40));
        }
    }
    {   // the objective, remembered from the game's own map screen
        int gx = g_tgt.x[sel], gy = g_tgt.y[sel];
        if (!g_showTarget || gx < x0 || gx > x1 || gy < y0 || gy > y1) goto no_target;
        float cs = 8 * sc, cl = dl + (gx - x0) * cs, ct = dt + (gy - y0) * cs;
        struct timespec tsp;
        clock_gettime(CLOCK_MONOTONIC, &tsp);
        float k = 0.5f - 0.5f * cosf((tsp.tv_nsec / 1e9f + tsp.tv_sec % 2) * 3.1415927f), ex = cs * (0.2f + 0.3f * k), th = fmaxf(2, cs * 0.12f);
        if (cl - ex < view.l || ct - ex < view.t || cl + cs + ex > view.r || ct + cs + ex > view.b) goto no_target;
        uint32_t col = COLOR(255, 70, 200);
        rectf(cl - ex, ct - ex, cl + cs + ex, ct - ex + th, col);
        rectf(cl - ex, ct + cs + ex - th, cl + cs + ex, ct + cs + ex, col);
        rectf(cl - ex, ct - ex, cl - ex + th, ct + cs + ex, col);
        rectf(cl + cs + ex - th, ct - ex, cl + cs + ex, ct + cs + ex, col);
        float lps = pixelSizeFor(fmaxf(10, cs * 0.9f)), ty = ct - ex - 7 * lps - 2;
        if (ty >= view.t) text("TARGET", cl + cs / 2, ty, lps, col, 1);
    }
    no_target:;
    // the security levels, as on the game's map screen: level n is open once the security status has reached it (255 = none)
    static const uint32_t kLockCol[4] = { COLOR(40, 130, 255), COLOR(40, 200, 80), COLOR(240, 200, 30), COLOR(230, 50, 50) };
    int sec = U8(A_LOCKS);
    float lh = rh(box) * 0.065f, ps = pixelSizeFor(lh * 0.8f), lw = lh * 5.2f, lx = box.r - lw - 4, ly = box.t + 4;
    rectf(lx - 3, ly - 2, box.r - 2, ly + lh * 4 + 3, COL_SLOT_BG);
    for (int n = 1; n <= 4; n++) {
        int open = sec != 255 && sec >= n;
        float y = ly + (n - 1) * lh;
        rectf(lx, y + lh * 0.15f, lx + lh * 1.3f, y + lh * 0.85f, kLockCol[n - 1]);
        text(open ? "OPEN" : "LOCK", box.r - 6, y + lh * 0.5f - 7 * ps / 2, ps, open ? COL_WHITE : COLOR(230, 60, 200), 2);
    }
}

static void settings_row(RectF row, RectF* tap, const char* label, const char* value, uint32_t valueCol) {
    *tap = row;
    pixelBox(row, COL_SLOT_BG, COL_BORDER_DARK);
    float h = rh(row), ps = pixelSizeFor(h * 0.38f);
    text(label, row.l + h * 0.25f, rcy(row) - 7 * ps / 2, ps, COL_WHITE, 0);
    text(value, row.r - h * 0.25f, rcy(row) - 7 * ps / 2, ps, valueCol, 2);
}

static void draw_setup(RectF area) {
    roundBox(area, rh(area) * 0.02f, COL_PANEL_BG, COL_BORDER_DARK, fmaxf(2, rh(area) * 0.01f));
    float pad = rw(area) * 0.03f, rowH = rh(area) * 0.14f, gap = rowH * 0.2f, y = area.t + pad;
    static const char* labels[3] = { "STATUS BAR MAP", "STATUS BAR ITEMS", "STATUS BAR SETUP" };
    for (int i = 0; i < 3; i++) {
        RectF row = { area.l + pad, y, area.r - pad, y + rowH };
        settings_row(row, &g_setupRow[i], labels[i], g_showBar[i] ? "ON" : "OFF", g_showBar[i] ? COL_ACCENT : COL_DIM_GRAY);
        y = row.b + gap;
    }
    RectF row = { area.l + pad, y, area.r - pad, y + rowH };
    settings_row(row, &g_setupRow[3], "MAP TARGET MARKER", g_showTarget ? "ON" : "OFF", g_showTarget ? COL_ACCENT : COL_DIM_GRAY);
}

static void draw_dim(void) {
    float cx = g_w / 2.0f, cy = g_h / 2.0f, th = g_w * 0.11f, ps = pixelSizeFor(th);
    uint32_t faded = COLOR(110, 78, 40);
    float tw = textW("METROID", ps), half = tw * 0.55f;
    rectf(cx - half, cy - th * 0.9f, cx + half, cy - th * 0.9f + 3, faded);
    rectf(cx - half, cy + th * 0.7f, cx + half, cy + th * 0.7f + 3, faded);
    text("METROID", cx, cy - th / 2, ps, faded, 1);
}

static void handle_tap(int ix, int iy) {
    float x = (float)ix, y = (float)iy;
    for (int i = 0; i < 3; i++)
        if (inrect(g_tabRect[i], x, y)) { g_tab = i; prefs_save(); return; }
    if (g_tab == TAB_SETUP) {
        for (int i = 0; i < 3; i++)
            if (inrect(g_setupRow[i], x, y)) { g_showBar[i] = !g_showBar[i]; prefs_save(); return; }
        if (inrect(g_setupRow[3], x, y)) { g_showTarget = !g_showTarget; prefs_save(); return; }
    }
    if (g_tab == TAB_MAP) {
        if (inrect(g_resetBtn, x, y)) { g_panX = g_panY = 0; g_zoom = 1.0f; return; }
        if (inrect(g_zoomInBtn, x, y)) { g_zoom *= 1.25f; return; }
        if (inrect(g_zoomOutBtn, x, y)) { g_zoom /= 1.25f; return; }
    }
    if (g_tab == TAB_MAP && inrect(g_mapHdr, x, y)) {
        int cur = U8(A_AREA), sel = g_mapSel >= 0 ? g_mapSel : cur;
        g_mapSel = (sel + 1) % 7;
    }
}

static void low_energy_flash(void) {
    int hp = U16(A_EN);
    if (hp <= 0 || hp >= 30) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    float phase = (ts.tv_sec % 2 + ts.tv_nsec / 1e9f) / 2.0f;
    float k = 0.5f - 0.5f * cosf(phase * 6.2831853f);
    int a = (int)(k * 90);
    for (int y = 0; y < g_h; y++) {
        uint32_t* row = g_back + (size_t)y * g_pitch;
        for (int x = 0; x < g_w; x++) {
            uint32_t p = row[x];
            int r = (((p >> 16) & 255) * (255 - a) + 255 * a) / 255, gr = (((p >> 8) & 255) * (255 - a)) / 255, b = ((p & 255) * (255 - a)) / 255;
            row[x] = 0xFF000000u | (r << 16) | (gr << 8) | b;
        }
    }
}

// energy and its maximum only make sense in a running game (the maximum is 99 plus 100 per tank)
static int playing_live(void) {
    int mx = U16(A_ENMAX), en = U16(A_EN);
    return mx >= 99 && mx <= 1499 && mx % 100 == 99 && en > 0 && en <= mx && U8(A_AREA) < 7;
}

static void draw(unsigned frame) {
    (void)frame;
    g_clipX0 = 0; g_clipY0 = 0; g_clipX1 = g_w; g_clipY1 = g_h;
    fill(0, 0, g_w, g_h, COL_BG);
    static time_t lastFind;
    if (!g_ew && time(NULL) != lastFind) { lastFind = time(NULL); find_ram(); }
    if (!g_ew) { draw_dim(); return; }
    if (!playing_live()) {
        memset(g_tabRect, 0, sizeof g_tabRect);
        g_dragDX = g_dragDY = 0; g_tapX = g_longX = -1;
        draw_dim();
        return;
    }
    g_longX = -1;
    if (g_tab == TAB_MAP) {
        int ddx = g_dragDX, ddy = g_dragDY;
        g_dragDX = g_dragDY = 0;
        if ((ddx || ddy) && inrect(g_mapRect, (float)g_downX, (float)g_downY)) { g_panX -= ddx * g_cellsPerPx; g_panY -= ddy * g_cellsPerPx; }
    } else g_dragDX = g_dragDY = 0;
    target_track();
    map_track();
    int tapx = g_tapX, tapy = g_tapY;
    if (tapx >= 0) { g_tapX = -1; handle_tap(tapx, tapy); }
    float w = (float)g_w, h = (float)g_h, margin = w * 0.025f, stripH = fminf(h * 0.16f, w * 0.11f);
    int bar = g_showBar[g_tab];
    RectF strip = bar ? (RectF){ margin, margin, w - margin, margin + stripH } : (RectF){ margin, margin, w - margin, margin };
    float gapTop = bar ? stripH * 0.15f : 0;
    if (bar) draw_strip(strip, stripH);
    float tabBarH = stripH * 0.85f;
    RectF tabBar = { margin, h - tabBarH - margin * 0.3f, w - margin, h - margin * 0.3f };
    layout_tabs(tabBar);
    draw_tab_bar();
    float ctlH = g_tab == TAB_MAP ? stripH * 0.85f : 0;
    RectF area = { strip.l, strip.b + gapTop, w - strip.l, tabBar.t - ctlH - (g_tab == TAB_MAP ? stripH * 0.1f : 0) - stripH * 0.15f };
    if (g_tab == TAB_MAP) {
        layout_map_buttons((RectF){ margin, tabBar.t - ctlH - stripH * 0.1f, w - margin, tabBar.t - stripH * 0.1f });
        draw_map_buttons();
    }
    if (g_tab == TAB_ITEMS) draw_items(area);
    else if (g_tab == TAB_SETUP) draw_setup(area);
    else draw_map(area);
    low_energy_flash();
}

// ---- the bottom panel ------------------------------------------------------------------------------------------
static int setup_panel(void) {
    drmModeRes* res = drmModeGetResources(g_fd);
    if (!res) return 0;
    drmModeConnector* conn = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector* c = drmModeGetConnector(g_fd, res->connectors[i]);
        if (!c) continue;
        if (c->connector_type == DRM_MODE_CONNECTOR_DSI && c->connector_type_id == 1 && c->connection == DRM_MODE_CONNECTED &&
            c->count_modes > 0)
            conn = c;
        else
            drmModeFreeConnector(c);
    }
    if (!conn) { fprintf(stderr, "banana-fusion: DSI-1 not found\n"); drmModeFreeResources(res); return 0; }
    uint32_t busy = 0, possible = 0, cur = 0;
    for (int i = 0; i < res->count_connectors; i++) {
        if (res->connectors[i] == conn->connector_id) continue;
        drmModeConnector* c = drmModeGetConnector(g_fd, res->connectors[i]);
        if (!c) continue;
        if (c->encoder_id) {
            drmModeEncoder* e = drmModeGetEncoder(g_fd, c->encoder_id);
            if (e) {
                for (int k = 0; k < res->count_crtcs; k++) if (res->crtcs[k] == e->crtc_id) busy |= 1u << k;
                drmModeFreeEncoder(e);
            }
        }
        drmModeFreeConnector(c);
    }
    for (int i = 0; i < conn->count_encoders; i++) {
        drmModeEncoder* e = drmModeGetEncoder(g_fd, conn->encoders[i]);
        if (!e) continue;
        possible |= e->possible_crtcs;
        if (conn->encoder_id == e->encoder_id) cur = e->crtc_id;
        drmModeFreeEncoder(e);
    }
    uint32_t crtc = 0;
    for (int k = 0; k < res->count_crtcs && !crtc; k++)
        if ((possible & (1u << k)) && !(busy & (1u << k)) && (!cur || res->crtcs[k] == cur)) crtc = res->crtcs[k];
    for (int k = 0; k < res->count_crtcs && !crtc; k++)
        if ((possible & (1u << k)) && !(busy & (1u << k))) crtc = res->crtcs[k];
    drmModeModeInfo mode = conn->modes[0];
    uint32_t conn_id = conn->connector_id;
    g_w = mode.hdisplay; g_h = mode.vdisplay;
    fprintf(stderr, "banana-fusion: DSI-1 connector %u %dx%d, crtc %u (possible 0x%x busy 0x%x current %u)\n", conn_id, g_w, g_h,
            crtc, possible, busy, cur);
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    if (!crtc) return 0;

    struct drm_mode_create_dumb cd = { .width = g_w, .height = g_h, .bpp = 32 };
    if (drmIoctl(g_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) return 0;
    uint32_t fb;
    if (drmModeAddFB(g_fd, g_w, g_h, 24, 32, cd.pitch, cd.handle, &fb)) return 0;
    struct drm_mode_map_dumb md = { .handle = cd.handle };
    if (drmIoctl(g_fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) return 0;
    void* m = mmap(NULL, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, md.offset);
    if (m == MAP_FAILED) return 0;
    g_front = m; g_bytes = cd.size; g_pitch = cd.pitch / 4;
    g_back = calloc(1, cd.size);
    memset(m, 0, cd.size);
    if (drmModeSetCrtc(g_fd, crtc, fb, 0, 0, &conn_id, 1, &mode)) { perror("banana-fusion: setcrtc"); return 0; }
    return 1;
}

static void* thread_main(void* arg) {
    (void)arg;
    sleep(2); // RetroArch's own set-up first
    if (!setup_panel()) { fprintf(stderr, "banana-fusion: panel setup failed\n"); return NULL; }
    // the menu keeps the bottom backlight off while a game runs
    system(". /usr/libexec/banana/one-panel; one_panel_dark top; one_panel_restore");
    fprintf(stderr, "banana-fusion: running, core %s, rom %s\n", g_core, g_rom);
    prefs_load();
    map_cache_load();
    pthread_t tt;
    pthread_create(&tt, NULL, touch_thread, NULL);
    for (unsigned frame = 0;; frame++) {
        draw(frame);
        memcpy(g_front, g_back, g_bytes);
        usleep(66000);
    }
    return NULL;
}

static void started(int fd) {
    if (g_started || fd < 0 || !g_core[0]) return;
    g_started = 1;
    g_fd = fd;
    pthread_t t;
    pthread_create(&t, NULL, thread_main, NULL);
}

// ---- the hooks: RetroArch's first page flip, mode set or atomic commit ------------------------------------------
int drmModePageFlip(int fd, uint32_t crtc, uint32_t fb, uint32_t flags, void* user) {
    static int (*real)(int, uint32_t, uint32_t, uint32_t, void*);
    if (!real) real = dlsym(RTLD_NEXT, "drmModePageFlip");
    started(fd);
    return real(fd, crtc, fb, flags, user);
}

int drmModeSetCrtc(int fd, uint32_t crtc, uint32_t buf, uint32_t x, uint32_t y, uint32_t* conns, int count, drmModeModeInfoPtr mode) {
    static int (*real)(int, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t*, int, drmModeModeInfoPtr);
    if (!real) real = dlsym(RTLD_NEXT, "drmModeSetCrtc");
    int r = real(fd, crtc, buf, x, y, conns, count, mode);
    if (!g_started && mode) started(fd); // (our own call comes after g_started is set)
    return r;
}

int drmModeAtomicCommit(int fd, drmModeAtomicReqPtr req, uint32_t flags, void* user) {
    static int (*real)(int, drmModeAtomicReqPtr, uint32_t, void*);
    if (!real) real = dlsym(RTLD_NEXT, "drmModeAtomicCommit");
    started(fd);
    return real(fd, req, flags, user);
}

__attribute__((constructor)) static void init(void) {
    char exe[256] = "";
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return;
    exe[n] = 0;
    const char* base = strrchr(exe, '/');
    base = base ? base + 1 : exe;
    if (strcmp(base, "retroarch") != 0) return; // preloaded into the launcher's other processes too: inert there
    FILE* f = fopen("/proc/self/cmdline", "rb");
    if (!f) return;
    char buf[4096];
    size_t len = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[len] = 0;
    for (size_t i = 0; i < len;) {
        const char* a = buf + i;
        size_t al = strlen(a);
        if (!strcmp(a, "-L") && i + al + 1 < len) snprintf(g_core, sizeof g_core, "%s", a + al + 1);
        else if (a[0] != '-' && i > 0 && strcmp(a, g_core)) snprintf(g_rom, sizeof g_rom, "%s", a);
        i += al + 1;
    }
    snprintf(g_prefs, sizeof g_prefs, "/data/roms/ports/fusion/prefs.txt");
    snprintf(g_mapFile, sizeof g_mapFile, "/data/roms/ports/fusion/maps2.bin");
    unsetenv("LD_PRELOAD"); // not into what RetroArch starts
}
