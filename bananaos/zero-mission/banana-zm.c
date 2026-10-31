// BananaOS: a companion screen on the bottom panel for Metroid Zero Mission in RetroArch (gpSP), preloaded into RetroArch.
// RetroArch holds DRM master and draws on the top panel through its KMS context; this takes its DRM fd the first
// time RetroArch flips a page or sets a mode, gives the bottom panel (DSI-1) a CRTC and a dumb buffer of its own,
// and draws there from a thread. The game's memory is read straight from the loaded libretro core
// (retro_get_memory_data), the core's path taken from RetroArch's command line.
// Build: sh build.sh
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
#include "zm_suit.h"

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

// ---- the ROM: the minimap tiles and the HUD icons, read from the player's own Zero Mission ROM -------------------------
static uint8_t* g_romData;
static long g_romSize;

static void load_rom(void) {
    if (g_romData || !g_rom[0]) return;
    FILE* f = fopen(g_rom, "rb");
    if (!f) { fprintf(stderr, "banana-zm: cannot open the ROM %s\n", g_rom); return; }
    fseek(f, 0, SEEK_END);
    g_romSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_romData = malloc(g_romSize);
    if (!g_romData || fread(g_romData, 1, g_romSize, f) != (size_t)g_romSize) { free(g_romData); g_romData = NULL; }
    fclose(f);
}

static uint32_t bgr555(int c) {
    int r5 = c & 31, g5 = (c >> 5) & 31, b5 = (c >> 10) & 31;
    return COLOR(r5 * 255 / 31, g5 * 255 / 31, b5 * 255 / 31);
}

#define ADDR_TILES_GFX 0x40E0C4
#define ADDR_TILES_PAL 0x411360
#define TILE_COUNT 320
#define ADDR_HUD_PAL 0x32BA08

static uint32_t g_tilePal[80];
static Img g_icon[3]; // missile, super missile, power bomb
static int g_romReady;

static void decode_icon(Img* im, uint32_t addr, int bank, int backdrop) {
    im->w = 16; im->h = 8;
    im->px = calloc(16 * 8, 4);
    uint32_t pal[96];
    for (int i = 0; i < 96; i++) pal[i] = bgr555(g_romData[ADDR_HUD_PAL + i * 2] | (g_romData[ADDR_HUD_PAL + i * 2 + 1] << 8));
    for (int t = 0; t < 2; t++)
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < 8; col++) {
                int b = g_romData[addr + t * 32 + row * 4 + (col >> 1)];
                int ci = (col & 1) == 0 ? (b & 15) : (b >> 4);
                if (ci == 0 || ci == backdrop) continue;
                im->px[row * 16 + t * 8 + col] = pal[bank * 16 + ci];
            }
}

static void ensure_rom(void) {
    if (g_romReady) return;
    load_rom();
    if (!g_romData || g_romSize < 0x420000) return;
    for (int i = 0; i < 80; i++) g_tilePal[i] = bgr555(g_romData[ADDR_TILES_PAL + i * 2] | (g_romData[ADDR_TILES_PAL + i * 2 + 1] << 8));
    decode_icon(&g_icon[0], 0x330548, 1, 15);
    decode_icon(&g_icon[1], 0x3306C8, 2, 15);
    decode_icon(&g_icon[2], 0x330848, 2, 7);
    g_romReady = 1;
}

// ---- the suit picture of the equipment screen (baked-in, run-length coded) -----------------------------------------------
static Img g_suit;
static int g_suitFor = -1;

static void render_suit(int kind) { // 0 power, 1 varia, 2 gravity, 3 zero suit
    if (g_suit.px && g_suitFor == kind) return;
    g_suitFor = kind;
    if (!g_suit.px) { g_suit.w = 48; g_suit.h = 88; g_suit.px = calloc(48 * 88, 4); }
    memset(g_suit.px, 0, 48 * 88 * 4);
    const uint16_t* rle = kind == 1 ? rle_varia : kind == 2 ? rle_gravity : kind == 3 ? rle_zero : rle_power;
    size_t n = kind == 1 ? sizeof rle_varia / 2 : kind == 2 ? sizeof rle_gravity / 2 : kind == 3 ? sizeof rle_zero / 2 : sizeof rle_power / 2;
    const uint32_t* pal = kind == 1 ? pal_varia : kind == 2 ? pal_gravity : kind == 3 ? pal_zero : pal_power;
    size_t pos = 0;
    for (size_t i = 0; i < n; i++) {
        int run = rle[i] >> 4, pi = rle[i] & 15;
        for (int k = 0; k < run && pos < 48 * 88; k++) g_suit.px[pos++] = pi ? pal[pi] : 0;
    }
}

// ---- the game's memory: EWRAM from libretro, IWRAM 0x20000 below it in the core's data ---------------------------------
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
        fprintf(stderr, "banana-zm: memory %u: %zu bytes at %p\n", id, sz, p);
        if (p && sz == 0x40000) g_ew = p;
        if (p && sz == 0x8000) g_iw = p;
    }
    if (g_ew && !g_iw) g_iw = g_ew - 0x20000; // gpSP keeps IWRAM right below EWRAM
    fprintf(stderr, "banana-zm: ewram %p iwram %p\n", (void*)g_ew, (void*)g_iw);
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

#define A_AREA 0x03000054
#define A_ROOM 0x03000055
#define A_MODE 0x03000C70
#define A_MAXHP 0x03001530
#define A_MAXMIS 0x03001532
#define A_MAXSUP 0x03001534
#define A_MAXPB 0x03001535
#define A_HP 0x03001536
#define A_MIS 0x03001538
#define A_SUP 0x0300153A
#define A_PB 0x0300153B
#define A_BEAMS 0x0300153C
#define A_SUITS 0x0300153E
#define A_STATION 0x03001540
#define A_SUITTYPE 0x03001542
#define A_SELECTOR 0x03001417
#define A_SAMX 0x030013E6
#define A_SAMY 0x030013E8
#define A_MAPDATA 0x02034800
#define A_VISITED 0x02037400
#define A_EVENTS 0x02037E00
#define A_ITEMS 0x03000063

// ---- what the player has set ----------------------------------------------------------------------------------------
enum { TAB_MAP, TAB_ITEMS, TAB_SETUP };
static int g_tab = TAB_MAP;
static int g_showBar[3] = { 1, 1, 1 };
static char g_prefs[512];

static void prefs_load(void) {
    FILE* f = fopen(g_prefs, "r");
    if (!f) return;
    int a, b, c, t;
    if (fscanf(f, "%d %d %d %d", &a, &b, &c, &t) == 4) { g_showBar[0] = a; g_showBar[1] = b; g_showBar[2] = c; if (t >= 0 && t < 3) g_tab = t; }
    fclose(f);
}
static void prefs_save(void) {
    FILE* f = fopen(g_prefs, "w");
    if (!f) return;
    fprintf(f, "%d %d %d %d\n", g_showBar[0], g_showBar[1], g_showBar[2], g_tab);
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
static RectF g_tabRect[3], g_setupRow[3];
static const char* kTabLabel[3] = { "MAP", "ITEMS", "SETUP" };

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

#define COL_LOW COLOR(230, 57, 57)

static RectF g_weaponRect[2];
static float draw_ammo(const Img* im, int count, float x, float midY, float textSize, int selected, float stripH, RectF* tap) {
    float ps = pixelSizeFor(textSize), slotStart = x;
    if (im->px) {
        float iconH = textSize * 0.9f, iconW = iconH * im->w / im->h, pad = stripH * 0.06f;
        if (selected) strokef(x - pad, midY - iconH / 2 - pad, x + iconW + pad, midY + iconH / 2 + pad, fmaxf(2, stripH * 0.03f), COL_ACCENT);
        blit(im, x, midY - iconH / 2, x + iconW, midY + iconH / 2);
        x += iconW + stripH * 0.15f;
    }
    char buf[16];
    snprintf(buf, sizeof buf, "%d", count);
    text(buf, x, midY - textSize / 2, ps, COL_WHITE, 0);
    if (tap) *tap = (RectF){ slotStart, midY - stripH / 2, x + textW(buf, ps), midY + stripH / 2 };
    return x + textW(buf, ps) + stripH * 0.5f;
}

static void draw_strip(RectF strip, float stripH) {
    roundBox(strip, stripH * 0.1f, COL_PANEL_BG, COL_BORDER_DARK, fmaxf(2, stripH * 0.02f));
    int hp = U16(A_HP), maxHp = U16(A_MAXHP);
    float textSize = stripH * 0.32f, pipSize = stripH * 0.28f, pipGap = stripH * 0.07f;
    int maxTanks = maxHp / 100;
    if (maxTanks > 12) maxTanks = 12;
    int filled = hp / 100 > maxTanks ? maxTanks : hp / 100;
    int cols = maxTanks < 6 ? maxTanks : 6, rowsN = maxTanks <= 6 ? 1 : 2;
    float rowStep = pipSize * 1.3f, blockH = rowStep * (rowsN - 1) + pipSize;
    float blockTop = strip.t + (stripH - blockH) * 0.5f, pipLeft = strip.l + stripH * 0.3f;
    uint32_t pipCol = hp < 30 ? COL_LOW : COL_ENERGY_PIP;
    for (int i = 0; i < maxTanks; i++) {
        int row = i / 6, col = i % 6;
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
    int superSel = U8(A_SELECTOR) & 1;
    if (U16(A_MAXMIS) > 0) x = draw_ammo(&g_icon[0], U16(A_MIS), x, midY, textSize, !superSel, stripH, &g_weaponRect[0]);
    if (U8(A_MAXSUP) > 0) x = draw_ammo(&g_icon[1], U8(A_SUP), x, midY, textSize, superSel, stripH, &g_weaponRect[1]);
    if (U8(A_MAXPB) > 0) draw_ammo(&g_icon[2], U8(A_PB), x, midY, textSize, 0, stripH, NULL);
}

static void stat_box(float x, float top, float w, float h, const char* label, const char* value) {
    RectF box = { x, top, x + w, top + h };
    pixelBox(box, COL_SLOT_BG, COL_BORDER_DARK);
    text(label, box.l + w * 0.06f, box.t + h * 0.12f, pixelSizeFor(h * 0.22f), COL_DIM_GRAY, 0);
    text(value, box.l + w * 0.06f, box.b - h * 0.14f - h * 0.36f, pixelSizeFor(h * 0.36f), COL_WHITE, 0);
}

typedef struct { const char* title; int n; int bits[6]; const char* labels[6]; int fromBeams; } Equip;
static const Equip kBeam = { "BEAM", 6, { 1, 2, 4, 8, 0x10, 0x80 }, { "LONG BEAM", "ICE BEAM", "WAVE BEAM", "PLASMA BEAM", "CHARGE BEAM", "BOMBS" }, 1 };
static const Equip kSuit = { "SUIT", 2, { 0x10, 0x20 }, { "VARIA SUIT", "GRAVITY SUIT" }, 0 };
static const Equip kMove = { "MOVEMENT", 4, { 1, 4, 2, 8 }, { "HI-JUMP BOOTS", "SPACE JUMP", "SPEED BOOSTER", "SCREW ATTACK" }, 0 };
static const Equip kMisc = { "MISC.", 2, { 0x40, 0x80 }, { "MORPH BALL", "POWER GRIP" }, 0 };

static void equip_column(float x, float top, float w, float h, int beams, int suits, const Equip* const* groups, int ng) {
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
        int bits = e->fromBeams ? beams : suits;
        float boxH = unitH * e->n;
        RectF box = { x, y, x + w, y + boxH };
        pixelBox(box, COL_SLOT_BG, COL_BORDER_DARK);
        text(e->title, box.l + w * 0.06f, box.t + titleSize * 0.5f, pixelSizeFor(titleSize), COL_ACCENT, 0);
        float rowH = (boxH - titleSize * 1.6f) / e->n, rowY = box.t + titleSize * 1.6f;
        for (int i = 0; i < e->n; i++) {
            int got = (bits & e->bits[i]) != 0;
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
    float rad = rh(area) * 0.02f, sw = fmaxf(2, rh(area) * 0.01f);
    roundBox(area, rad, COL_PANEL_BG, COL_BORDER_DARK, sw);
    int suitless = U8(A_SUITTYPE) == 2;
    int beams = suitless ? 0 : U8(A_BEAMS), suits = suitless ? 0 : U8(A_SUITS);
    float pad = rw(area) * 0.03f, left = area.l + pad, right = area.r - pad, top = area.t + pad, bottom = area.b - pad;
    float colGap = pad * 0.8f, headerH = (bottom - top) * 0.16f;
    char tm[24], it[24];
    snprintf(tm, sizeof tm, "%02d:%02d:%02d", U8(0x03000150), U8(0x03000151), U8(0x03000152));
    int items = 0;
    for (int i = 0; i < 7; i++) items += U8(A_ITEMS + i);
    snprintf(it, sizeof it, "%d/100", items);
    float hgap = pad * 0.4f;
    int boxes = suitless ? 3 : 2;
    float bw = (right - left - hgap * (boxes - 1)) / boxes;
    stat_box(left, top, bw, headerH, "ITEMS", it);
    stat_box(left + bw + hgap, top, bw, headerH, "TIME", tm);
    if (suitless) stat_box(left + 2 * (bw + hgap), top, bw, headerH, "WEAPON", "PISTOL");
    float bodyTop = top + headerH + pad * 0.6f, bodyH = bottom - bodyTop;
    float aspect = 48.0f / 88.0f;
    float suitW = fminf((right - left) * 0.3f, bodyH * aspect);
    float colW = (right - left - colGap * 2 - suitW) / 2;
    const Equip* c1[1] = { &kBeam };
    const Equip* c2[3] = { &kSuit, &kMove, &kMisc };
    equip_column(left, bodyTop, colW, bodyH, beams, suits, c1, 1);
    render_suit(suitless ? 3 : (suits & 0x20) ? 2 : (suits & 0x10) ? 1 : 0);
    float dh = bodyH, dw = dh * aspect;
    if (dw > suitW) { dw = suitW; dh = dw / aspect; }
    float dl = left + colW + colGap + (suitW - dw) / 2, dt = bodyTop + (bodyH - dh) / 2;
    blit(&g_suit, dl, dt, dl + dw, dt + dh);
    equip_column(left + colW + colGap + suitW + colGap, bodyTop, colW, bodyH, beams, suits, c2, 3);
}

static void settings_row(RectF row, RectF* tap, const char* label, const char* value, uint32_t valueCol) {
    *tap = row;
    pixelBox(row, COL_SLOT_BG, COL_BORDER_DARK);
    float h = rh(row), ps = pixelSizeFor(h * 0.38f);
    text(label, row.l + h * 0.25f, rcy(row) - 7 * ps / 2, ps, COL_WHITE, 0);
    text(value, row.r - h * 0.25f, rcy(row) - 7 * ps / 2, ps, valueCol, 2);
}

static RectF g_clearRow;
static long g_clearArmed;

static void draw_setup(RectF area) {
    float rad = rh(area) * 0.02f, sw = fmaxf(2, rh(area) * 0.01f);
    roundBox(area, rad, COL_PANEL_BG, COL_BORDER_DARK, sw);
    float pad = rw(area) * 0.03f, rowH = rh(area) * 0.14f, gap = rowH * 0.2f, y = area.t + pad;
    static const char* labels[3] = { "STATUS BAR MAP", "STATUS BAR ITEMS", "STATUS BAR SETUP" };
    for (int i = 0; i < 3; i++) {
        RectF row = { area.l + pad, y, area.r - pad, y + rowH };
        settings_row(row, &g_setupRow[i], labels[i], g_showBar[i] ? "ON" : "OFF", g_showBar[i] ? COL_ACCENT : COL_DIM_GRAY);
        y = row.b + gap;
    }
    RectF clr = { area.l + pad, y, area.r - pad, y + rowH };
    int armed = (long)time(NULL) <= g_clearArmed;
    settings_row(clr, &g_clearRow, "CLEAR MAP MARKERS", armed ? "TAP AGAIN" : "", COLOR(235, 110, 90));
}

static void draw_dim(void) {
    float cx = g_w / 2.0f, cy = g_h / 2.0f, th = g_w * 0.11f, ps = pixelSizeFor(th);
    uint32_t faded = COLOR(110, 78, 40);
    float tw = textW("METROID", ps), half = tw * 0.55f;
    rectf(cx - half, cy - th * 0.9f, cx + half, cy - th * 0.9f + 3, faded);
    rectf(cx - half, cy + th * 0.7f, cx + half, cy + th * 0.7f + 3, faded);
    text("METROID", cx, cy - th / 2, ps, faded, 1);
}

// ---- the map -----------------------------------------------------------------------------------------------------------
static void set_clip(RectF r) {
    g_clipX0 = (int)floorf(r.l); g_clipY0 = (int)floorf(r.t); g_clipX1 = (int)ceilf(r.r); g_clipY1 = (int)ceilf(r.b);
}
static void clear_clip(void) { g_clipX0 = 0; g_clipY0 = 0; g_clipX1 = g_w; g_clipY1 = g_h; }

static void blit_region(const Img* im, int sx, int sy, int sw, int sh, float dx0, float dy0, float dx1, float dy1) {
    int iy0 = (int)lroundf(dy0), iy1 = (int)lroundf(dy1), ix0 = (int)lroundf(dx0), ix1 = (int)lroundf(dx1);
    if (ix1 <= ix0 || iy1 <= iy0) return;
    for (int y = iy0; y < iy1; y++) {
        if (y < g_clipY0 || y >= g_clipY1) continue;
        int py = sy + (int)((y + 0.5f - dy0) * sh / (dy1 - dy0));
        if (py < 0 || py >= im->h) continue;
        for (int x = ix0; x < ix1; x++) {
            if (x < g_clipX0 || x >= g_clipX1) continue;
            int px = sx + (int)((x + 0.5f - dx0) * sw / (dx1 - dx0));
            if (px < 0 || px >= im->w) continue;
            uint32_t p = im->px[py * im->w + px];
            if (p >> 24) g_back[(size_t)y * g_pitch + x] = p;
        }
    }
}

static void tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t c) {
    int minx = (int)floorf(fminf(x0, fminf(x1, x2))), maxx = (int)ceilf(fmaxf(x0, fmaxf(x1, x2)));
    int miny = (int)floorf(fminf(y0, fminf(y1, y2))), maxy = (int)ceilf(fmaxf(y0, fmaxf(y1, y2)));
    if (minx < g_clipX0) minx = g_clipX0;
    if (miny < g_clipY0) miny = g_clipY0;
    if (maxx > g_clipX1) maxx = g_clipX1;
    if (maxy > g_clipY1) maxy = g_clipY1;
    float d = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
    if (fabsf(d) < 1e-6f) return;
    for (int y = miny; y < maxy; y++)
        for (int x = minx; x < maxx; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float a = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) / d;
            float b = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) / d;
            if (a >= 0 && b >= 0 && 1 - a - b >= 0) g_back[(size_t)y * g_pitch + x] = c;
        }
}

static void shade(float x0, float y0, float x1, float y1, int a) { // black at a/255
    int ix0 = (int)lroundf(x0), iy0 = (int)lroundf(y0), ix1 = (int)lroundf(x1), iy1 = (int)lroundf(y1);
    if (ix0 < g_clipX0) ix0 = g_clipX0;
    if (iy0 < g_clipY0) iy0 = g_clipY0;
    if (ix1 > g_clipX1) ix1 = g_clipX1;
    if (iy1 > g_clipY1) iy1 = g_clipY1;
    for (int y = iy0; y < iy1; y++)
        for (int x = ix0; x < ix1; x++) {
            uint32_t* p = g_back + (size_t)y * g_pitch + x;
            uint32_t v = *p;
            int r = ((v >> 16) & 255) * (255 - a) / 255, g = ((v >> 8) & 255) * (255 - a) / 255, b = (v & 255) * (255 - a) / 255;
            *p = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
}

static float clampf(float v, float lo, float hi) { return hi < lo ? (lo + hi) / 2 : v < lo ? lo : v > hi ? hi : v; }
static int clampi(int v, int lo, int hi) { return hi < lo ? lo : v < lo ? lo : v > hi ? hi : v; }

#define MM 32
#define NAREA 7
static const short kRooms0[] = { 13, 1294, 1551, 2563, 1798, 1542, 2824, 3075, 1554, 1027, 515, 259, 3340, 3336, 4360, 4359, 5384, 4874, 4618, 5640, 7428, 5380, 4866, 3842, 6406, 5894, 5644, 3586, 3332, 2819, 2307, 7174, 7177, 6919, 3337, 2052, 2831, 2066, 6660, 5124, 2828, 2314 };
static const short kRooms1[] = { 2308, 2566, 3841, 3073, 3074, 3075, 2817, 1029, 1031, 2567, 1801, 1803, 2569, 1545, 2571, 2827, 3084, 3082, 3340, 3085, 2829, 1548, 1549, 1292, 1036, 1293, 773, 4105, 3086, 2830, 2316, 2054, 1038, 1806, 2063, 1291, 4361, 2316, 2820, 3586, 3587, 4617 };
static const short kRooms2[] = { 3587, 1541, 1283, 1542, 4100, 5633, 4353, 4097, 4355, 4867, 4866, 4870, 5128, 4872, 3335, 2824, 2311, 1799, 1543, 1544, 1288, 1545, 1546, 2314, 2571, 3339, 2828, 2572, 2573, 3851, 2569, 5129, 3850, 4364, 3596, 4365, 2568, 3334, 2822, 3845, 1033, 3595, 4875, 3596, 3843, 5388, 4875, 5121, 4877, 4360, 4366, 3854, 2318, 5385, 3596, 778, 776 };
static const short kRooms3[] = { 3841, 3587, 3328, 2051, 1795, 1283, 1027, 1285, 2565, 3077, 3332, 2310, 1798, 1287, 4103, 5123, 4098, 3845, 1544, 2304, 2823, 1288, 2308, 2052, 773, 4872, 3335, 2304, 3328, 520, 5638, 5637, 3331 };
static const short kRooms4[] = { 5122, 5382, 7431, 5899, 4874, 4609, 6667, 4876, 4353, 4874, 5899, 5379, 5635, 5891, 5380, 5123, 6923, 7430, 7174, 6662 };
static const short kRooms5[] = { 2049, 1031, 1802, 2314, 780, 2049, 3847, 4358, 3843, 4354, 5891, 3335, 3590, 5895, 4869, 3331, 3843, 3843, 780, 1285, 5891, 1031 };
static const short kRooms6[] = { 7181, 6413, 5647, 5389, 4623, 4110, 4621, 1300, 6155, 3339, 5135, 4879, 6664, 5380, 5127, 5896, 4874, 4872, 3848, 3337, 3594, 4617, 3592, 2576, 2577, 2065, 2057, 1044, 273, 1041, 1806, 1038, 779, 270, 529, 1803, 2821, 1556, 1801, 519, 1799, 1797, 1283, 4871, 3334, 3332, 3333, 3588, 5893, 3844, 6158, 4612, 4357, 3590, 2060, 6152, 1806, 2821, 3590, 4099, 3329, 4354, 2580, 2069, 1813, 1557, 1044, 2065, 533, 4096, 5633, 4865, 5888, 4615, 2838, 268, 788, 5131, 3848, 1290, 528, 528, 270, 519, 524, 2576, 5647, 6413, 7181, 1809, 1797, 524, 5893, 3594, 3080, 3597, 3329, 4098, 3339 };
static const short* const kRoomTab[NAREA] = { kRooms0, kRooms1, kRooms2, kRooms3, kRooms4, kRooms5, kRooms6 };
static const int kRoomCnt[NAREA] = { sizeof kRooms0 / 2, sizeof kRooms1 / 2, sizeof kRooms2 / 2, sizeof kRooms3 / 2, sizeof kRooms4 / 2, sizeof kRooms5 / 2, sizeof kRooms6 / 2 };

enum { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT };
// {area, cellX, cellY, destination area, direction}
static const float kExits[][5] = {
    { 0, 2.60f, 13.13f, 5, DIR_UP }, { 0, 6.60f, 19.40f, 1, DIR_DOWN }, { 0, 22.60f, 13.40f, 2, DIR_DOWN }, { 0, 1.60f, 4.40f, 4, DIR_DOWN },
    { 1, 9.60f, 4.13f, 0, DIR_UP }, { 1, 20.07f, 9.43f, 2, DIR_RIGHT }, { 2, 14.60f, 3.13f, 0, DIR_UP }, { 2, 17.60f, 14.40f, 3, DIR_DOWN },
    { 2, 5.60f, 3.13f, 5, DIR_UP }, { 2, 15.53f, 14.80f, 3, DIR_DOWN }, { 2, 3.07f, 8.43f, 1, DIR_LEFT }, { 3, 15.60f, 1.13f, 2, DIR_UP },
    { 3, 13.53f, 0.07f, 2, DIR_UP }, { 4, 20.60f, 2.13f, 0, DIR_UP }, { 4, 18.60f, 1.13f, 5, DIR_UP }, { 4, 18.60f, 1.13f, 5, DIR_UP },
    { 5, 9.60f, 11.40f, 0, DIR_DOWN }, { 5, 3.60f, 13.40f, 4, DIR_DOWN }, { 5, 15.60f, 8.40f, 2, DIR_DOWN }, { 5, 25.07f, 3.43f, 6, DIR_RIGHT },
    { 5, 25.07f, 7.47f, 6, DIR_RIGHT }, { 5, 25.07f, 3.43f, 6, DIR_RIGHT }, { 5, 3.60f, 13.40f, 4, DIR_DOWN }, { 6, 2.13f, 21.47f, 5, DIR_LEFT },
    { 6, 2.13f, 17.43f, 5, DIR_LEFT },
};
#define NEXIT (int)(sizeof kExits / sizeof *kExits)
static const char* kAreaName[NAREA] = { "BRINSTAR", "KRAID", "NORFAIR", "RIDLEY", "TOURIAN", "CRATERIA", "CHOZODIA" };
// {area, cellX, cellY, event, 0 beams/bombs or 1 suits, bit}: a statue's hint, shown until its item is taken
static const int kStatues[8][6] = {
    { 0, 6, 6, 8, 0, 1 }, { 0, 24, 6, 9, 0, 0x80 }, { 2, 18, 3, 10, 0, 2 }, { 1, 8, 15, 11, 1, 2 },
    { 2, 19, 8, 12, 1, 1 }, { 0, 14, 2, 13, 1, 0x10 }, { 2, 10, 12, 14, 0, 4 }, { 2, 6, 7, 15, 1, 8 },
};

static Img g_mapImg;
static float g_zoom = 0.5f, g_panX, g_panY, g_cellsPerPx;
static int g_lastArea = -1;
static RectF g_mapRect, g_resetBtn, g_zoomOutBtn, g_zoomInBtn, g_backBtn, g_exitRect[40];
static int g_exitRow[40], g_exitCount;
static int g_previewArea = -1, g_previewFrom = -1;
static float g_rmDL, g_rmDT, g_rmCropX, g_rmCropY, g_rmCell;

static void build_map(int area) {
    if (!g_mapImg.px) { g_mapImg.w = g_mapImg.h = MM * 8; g_mapImg.px = calloc((size_t)MM * 8 * MM * 8, 4); }
    memset(g_mapImg.px, 0, (size_t)MM * 8 * MM * 8 * 4);
    uint8_t explored[128];
    memset(explored, 0, sizeof explored);
    int station = 0;
    if (area >= 0 && area < NAREA) {
        memcpy(explored, g_ew + ((A_VISITED + area * 128) & 0x3FFFF), 128);
        station = (U8(A_STATION) >> area) & 1;
    }
    for (int gy = 0; gy < MM; gy++)
        for (int gx = 0; gx < MM; gx++) {
            int tile = rd(A_MAPDATA + (gy * MM + gx) * 2, 2);
            int bit = gy * MM + gx, seen = (explored[bit >> 3] >> (bit & 7)) & 1;
            if (seen) { if ((tile & 0xF000) == 0) tile |= 0x1000; }
            else if (station) { if (tile >= 0x3000) tile = 0x140; else if (tile & 0x3000) tile &= 0xFFF; }
            else if (tile & 0xF000) tile = 0x140;
            int id = tile & 0x3FF, flip = (tile >> 10) & 3, bank = (tile >> 12) & 15;
            if (id >= 0x140 || id >= TILE_COUNT) continue;
            if (bank >= 5) bank = 0;
            for (int row = 0; row < 8; row++) {
                int sr = (flip & 2) ? 7 - row : row;
                for (int col = 0; col < 8; col++) {
                    int sc = (flip & 1) ? 7 - col : col;
                    int b = g_romData[ADDR_TILES_GFX + id * 32 + sr * 4 + (sc >> 1)];
                    int ci = (sc & 1) == 0 ? (b & 15) : (b >> 4);
                    if (ci) g_mapImg.px[(gy * 8 + row) * (MM * 8) + gx * 8 + col] = g_tilePal[bank * 16 + ci];
                }
            }
        }
}

static int explored_cell(const uint8_t* bits, int gx, int gy) {
    int bit = gy * MM + gx;
    return (bits[bit >> 3] >> (bit & 7)) & 1;
}

static void map_message(RectF a, const char* msg) {
    float ps = pixelSizeFor(rh(a) * 0.06f);
    text(msg, rcx(a), rcy(a) - 7 * ps / 2, ps, COL_DIM_GRAY, 1);
}

// ---- map pins: "come back to this" flags, long-press to drop or remove, kept per ROM ---------------------------------------
#define MAX_PINS 64
static int g_pinArea[MAX_PINS], g_pinCount;
static float g_pinX[MAX_PINS], g_pinY[MAX_PINS];
static char g_pinFile[700];

static void pins_load(void) {
    g_pinCount = 0;
    FILE* f = fopen(g_pinFile, "r");
    if (!f) return;
    int a, x, y;
    while (g_pinCount < MAX_PINS && fscanf(f, "%d %d %d", &a, &x, &y) == 3)
        if (a >= 0 && a < NAREA) { g_pinArea[g_pinCount] = a; g_pinX[g_pinCount] = x / 100.0f; g_pinY[g_pinCount] = y / 100.0f; g_pinCount++; }
    fclose(f);
}
static void pins_save(void) {
    if (!g_pinFile[0]) return;
    FILE* f = fopen(g_pinFile, "w");
    if (!f) return;
    for (int i = 0; i < g_pinCount; i++) fprintf(f, "%d %d %d\n", g_pinArea[i], (int)lroundf(g_pinX[i] * 100), (int)lroundf(g_pinY[i] * 100));
    fclose(f);
}
static void pins_follow_rom(void) {
    if (g_pinFile[0]) return;
    char key[512];
    const char* base = strrchr(g_rom, '/');
    snprintf(key, sizeof key, "%s", base ? base + 1 : g_rom);
    for (char* p = key; *p; p++) if (!isalnum((unsigned char)*p)) *p = '_';
    snprintf(g_pinFile, sizeof g_pinFile, "/data/roms/ports/zeromission/pins-%s.txt", key);
    pins_load();
}
static void pin_toggle(int area, float cx, float cy) {
    int nearest = -1;
    float best = 1.1f * 1.1f;
    for (int i = 0; i < g_pinCount; i++) {
        if (g_pinArea[i] != area) continue;
        float dx = g_pinX[i] - cx, dy = g_pinY[i] - cy, d = dx * dx + dy * dy;
        if (d <= best) { best = d; nearest = i; }
    }
    if (nearest != -1) {
        g_pinCount--;
        g_pinArea[nearest] = g_pinArea[g_pinCount]; g_pinX[nearest] = g_pinX[g_pinCount]; g_pinY[nearest] = g_pinY[g_pinCount];
    } else if (g_pinCount < MAX_PINS) {
        g_pinArea[g_pinCount] = area; g_pinX[g_pinCount] = cx; g_pinY[g_pinCount] = cy; g_pinCount++;
    }
    pins_save();
}
static void flag_fill(float cx, float top, float fw, float fh, uint32_t c) {
    for (int y = (int)floorf(top); y < (int)ceilf(top + fh); y++) {
        float t = (y + 0.5f - top) / fh, e = t <= 0.35f ? fw * t / 0.35f : fw * (1 - (t - 0.35f) / 0.65f);
        if (t >= 0 && t <= 1 && e > 0) rectf(cx, (float)y, cx + e, (float)y + 1, c);
    }
}
static void draw_pin_marker(float cx, float cy, float cell) {
    float poleH = cell * 1.8f, fw = cell * 1.3f, fh = cell * 0.9f, ow = fmaxf(2, cell * 0.18f);
    rectf(cx - ow / 2, cy - poleH, cx + ow / 2, cy, COLOR(0, 0, 0));
    float o = fmaxf(1.5f, cell * 0.1f);
    flag_fill(cx - o / 2, cy - poleH - o, fw + o * 2, fh + o * 2, COLOR(0, 0, 0));
    flag_fill(cx, cy - poleH, fw, fh, COL_ACCENT);
    circlef(cx, cy, fmaxf(2, cell * 0.14f), COLOR(0, 0, 0));
}

// ---- exits and statue hints ---------------------------------------------------------------------------------------------
static void draw_exit_arrow(float sx, float sy, float cellPx, int dest, int dir) {
    float half = cellPx * 1.15f * 0.5f, triH = cellPx * 1.15f * 0.85f;
    float vx[3] = { 0, -half, half }, vy[3] = { -triH * 0.6f, triH * 0.4f, triH * 0.4f };
    float ang = dir == DIR_DOWN ? 3.14159265f : dir == DIR_LEFT ? -1.5707963f : dir == DIR_RIGHT ? 1.5707963f : 0;
    float cs = cosf(ang), sn = sinf(ang), ox[3], oy[3], bx[3], by[3];
    float mx = 0, my = (vy[0] + vy[1] + vy[2]) / 3;
    for (int i = 0; i < 3; i++) {
        ox[i] = sx + vx[i] * cs - vy[i] * sn; oy[i] = sy + vx[i] * sn + vy[i] * cs;
        float ex = mx + (vx[i] - mx) * 1.3f, ey = my + (vy[i] - my) * 1.3f;
        bx[i] = sx + ex * cs - ey * sn; by[i] = sy + ex * sn + ey * cs;
    }
    tri(bx[0], by[0], bx[1], by[1], bx[2], by[2], COLOR(0, 0, 0));
    tri(ox[0], oy[0], ox[1], oy[1], ox[2], oy[2], COL_ACCENT);
    const char* label = dest >= 0 && dest < NAREA ? kAreaName[dest] : "?";
    float ps = pixelSizeFor(fmaxf(6, cellPx * 0.5f)), gh = 7 * ps, off = triH * 0.6f + cellPx * 0.15f;
    float lx = sx, ly, tw = textW(label, ps), pad = ps * 1.5f, cl, cr;
    int align = 1;
    if (dir == DIR_DOWN) ly = sy + off;
    else if (dir == DIR_LEFT) { lx = sx - off; ly = sy - gh / 2; align = 2; }
    else if (dir == DIR_RIGHT) { lx = sx + off; ly = sy - gh / 2; align = 0; }
    else ly = sy - off - gh;
    if (align == 0) { cl = lx - pad; cr = lx + tw + pad; }
    else if (align == 2) { cl = lx - tw - pad; cr = lx + pad; }
    else { cl = lx - tw / 2 - pad; cr = lx + tw / 2 + pad; }
    shade(cl, ly - pad * 0.5f, cr, ly + gh + pad * 0.5f, 140);
    text(label, lx, ly, ps, COL_WHITE, align);
}

static void draw_statue_hint(float sx, float sy, float cellPx) {
    float h = cellPx * 0.5f, h2 = h * 1.3f;
    tri(sx, sy - h2, sx + h2, sy, sx, sy + h2, COLOR(0, 0, 0));
    tri(sx, sy - h2, sx - h2, sy, sx, sy + h2, COLOR(0, 0, 0));
    tri(sx, sy - h, sx + h, sy, sx, sy + h, COL_ENERGY_PIP);
    tri(sx, sy - h, sx - h, sy, sx, sy + h, COL_ENERGY_PIP);
}

static void draw_preview(RectF area) {
    int a = g_previewArea;
    const char* name = a >= 0 && a < NAREA ? kAreaName[a] : "?";
    float ts = pixelSizeFor(rh(area) * 0.09f);
    text(name, rcx(area), rcy(area) - 7 * ts * 1.9f, ts, COL_ACCENT, 1);
    if (g_previewFrom >= 0 && g_previewFrom < NAREA) {
        char sub[64];
        snprintf(sub, sizeof sub, "VIA DOOR FROM %s", kAreaName[g_previewFrom]);
        text(sub, rcx(area), rcy(area) - 7 * ts * 0.5f, pixelSizeFor(rh(area) * 0.04f), COL_DIM_GRAY, 1);
    }
    float ms = pixelSizeFor(rh(area) * 0.05f);
    text("WALK THERE TO SEE", rcx(area), rcy(area) + 7 * ms, ms, COL_DIM_GRAY, 1);
    text("THIS AREA'S MAP", rcx(area), rcy(area) + 7 * ms * 2.3f, ms, COL_DIM_GRAY, 1);
    float bh = rh(area) * 0.12f, bw = bh * 2.6f;
    g_backBtn = (RectF){ area.l + rw(area) * 0.03f, area.t + rh(area) * 0.03f, area.l + rw(area) * 0.03f + bw, area.t + rh(area) * 0.03f + bh };
    pixelBox(g_backBtn, COL_PANEL_BG, COL_ACCENT);
    float tsz = rh(g_backBtn) * 0.4f;
    text("< BACK", rcx(g_backBtn), rcy(g_backBtn) - tsz / 2, pixelSizeFor(tsz), COL_ACCENT, 1);
}

static void draw_room_map(RectF area) {
    int areaIndex = U8(A_AREA), room = U8(A_ROOM);
    if (areaIndex != g_lastArea) { g_panX = g_panY = 0; g_lastArea = areaIndex; }
    build_map(areaIndex);
    uint8_t explored[128];
    memset(explored, 0, sizeof explored);
    int station = 0;
    if (areaIndex >= 0 && areaIndex < NAREA) {
        memcpy(explored, g_ew + ((A_VISITED + areaIndex * 128) & 0x3FFFF), 128);
        station = (U8(A_STATION) >> areaIndex) & 1;
    }
    float cellX = -1, cellY = -1;
    int have = 0;
    if (areaIndex >= 0 && areaIndex < NAREA && room >= 0 && room < kRoomCnt[areaIndex]) {
        int pos = kRoomTab[areaIndex][room] & 0xFFFF;
        int rx = (pos >> 8) & 255, ry = pos & 255;
        int sx = U16(A_SAMX), sy = U16(A_SAMY);
        float bx = fmaxf(0, (float)(sx - 128)) / 64.0f, by = fmaxf(0, (float)(sy - 128)) / 64.0f;
        cellX = bx / 15.0f + rx; cellY = by / 10.0f + ry;
        have = cellX >= 0 && cellX < MM && cellY >= 0 && cellY < MM;
    }
    float aspect = rw(area) / rh(area);
    int winW = clampi((int)lroundf(14 * sqrtf(aspect) / g_zoom), 3, MM), winH = clampi((int)lroundf(14 / sqrtf(aspect) / g_zoom), 3, MM);
    float cx = (have ? cellX : MM / 2.0f) + g_panX, cy = (have ? cellY : MM / 2.0f) + g_panY;
    int cropX = clampi((int)lroundf(cx - winW / 2.0f), 0, MM - winW), cropY = clampi((int)lroundf(cy - winH / 2.0f), 0, MM - winH);
    int cropW = winW * 8, cropH = winH * 8;
    float scale = fminf(rw(area) / cropW, rh(area) / cropH), dw = cropW * scale, dh = cropH * scale;
    float dl = rcx(area) - dw / 2, dt = rcy(area) - dh / 2, cell = 8 * scale;
    g_cellsPerPx = 1.0f / cell;
    set_clip(area);
    blit_region(&g_mapImg, cropX * 8, cropY * 8, cropW, cropH, dl, dt, dl + dw, dt + dh);
    g_rmDL = dl; g_rmDT = dt; g_rmCropX = (float)cropX; g_rmCropY = (float)cropY; g_rmCell = cell;
    for (int i = 0; i < g_pinCount; i++)
        if (g_pinArea[i] == areaIndex) draw_pin_marker(dl + (g_pinX[i] - cropX) * cell, dt + (g_pinY[i] - cropY) * cell, cell);
    if (have) {
        float dx = dl + (cellX - cropX) * cell, dy = dt + (cellY - cropY) * cell;
        if (dx >= dl && dx <= dl + dw && dy >= dt && dy <= dt + dh) circlef(dx, dy, fmaxf(3, cell * 0.6f), COLOR(255, 70, 70));
    }
    g_exitCount = 0;
    for (int r = 0; r < NEXIT && g_exitCount < 40; r++) {
        const float* e = kExits[r];
        if ((int)e[0] != areaIndex) continue;
        int seen = explored_cell(explored, clampi((int)lroundf(e[1]), 0, MM - 1), clampi((int)lroundf(e[2]), 0, MM - 1));
        if (!seen && !station) continue;
        float sx = dl + (e[1] - cropX) * cell, sy = dt + (e[2] - cropY) * cell, half = cell * 1.15f * 0.5f;
        if (sx < dl - half || sx > dl + dw + half || sy < dt - half || sy > dt + dh + half) continue;
        int dup = 0;
        for (int i = 0; i < g_exitCount; i++)
            if (fabsf(rcx(g_exitRect[i]) - sx) < 6 && fabsf(rcy(g_exitRect[i]) - sy) < 6) dup = 1;
        if (dup) continue;
        draw_exit_arrow(sx, sy, cell, (int)e[3], (int)e[4]);
        g_exitRect[g_exitCount] = (RectF){ sx - half - 14, sy - half - 14, sx + half + 14, sy + half + 14 };
        g_exitRow[g_exitCount++] = r;
    }
    int beams = U8(A_BEAMS), suits = U8(A_SUITS);
    for (int i = 0; i < 8; i++) {
        const int* t = kStatues[i];
        if (t[0] != areaIndex) continue;
        int ev = t[3];
        if (!((rd(A_EVENTS + (ev / 32) * 4, 4) >> (ev % 32)) & 1)) continue;
        if (((t[4] ? suits : beams) & t[5]) != 0) continue;
        float sx = dl + (t[1] + 0.5f - cropX) * cell, sy = dt + (t[2] + 0.5f - cropY) * cell;
        if (sx < dl - cell || sx > dl + dw + cell || sy < dt - cell || sy > dt + dh + cell) continue;
        draw_statue_hint(sx, sy, cell);
    }
    clear_clip();
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

static void draw_map(RectF area) {
    roundBox(area, rh(area) * 0.03f, COL_PANEL_BG, COL_BORDER_DARK, fmaxf(2, rh(area) * 0.01f));
    g_mapRect = area;
    g_exitCount = 0;
    if (!g_romReady) { map_message(area, "NO ROM"); return; }
    memset(&g_backBtn, 0, sizeof g_backBtn);
    if (g_previewArea >= 0) { g_rmCell = 0; draw_preview(area); return; }
    draw_room_map(area);
}

static void handle_long(float x, float y) {
    if (g_tab != TAB_MAP || g_previewArea >= 0 || !inrect(g_mapRect, x, y) || g_rmCell <= 0) return;
    int area = U8(A_AREA);
    if (area >= 0 && area < NAREA) pin_toggle(area, g_rmCropX + (x - g_rmDL) / g_rmCell, g_rmCropY + (y - g_rmDT) / g_rmCell);
}

static void handle_tap(int ix, int iy) {
    float x = (float)ix, y = (float)iy;
    // the ammo icons choose the weapon: missiles or super missiles (the selector's lowest bit)
    for (int i = 0; i < 2; i++)
        if (inrect(g_weaponRect[i], x, y)) {
            uint8_t* sel = (uint8_t*)g_iw + (A_SELECTOR & 0x7FFF);
            *sel = (*sel & ~1) | i;
            return;
        }
    if (g_tab == TAB_MAP && g_previewArea >= 0) {
        if (inrect(g_backBtn, x, y)) g_previewArea = g_previewFrom = -1;
        return;
    }
    if (g_tab == TAB_MAP)
        for (int i = 0; i < g_exitCount; i++)
            if (inrect(g_exitRect[i], x, y)) {
                g_previewFrom = (int)kExits[g_exitRow[i]][0];
                g_previewArea = (int)kExits[g_exitRow[i]][3];
                return;
            }
    for (int i = 0; i < 3; i++)
        if (inrect(g_tabRect[i], x, y)) { g_tab = i; prefs_save(); return; }
    if (g_tab == TAB_SETUP) {
        for (int i = 0; i < 3; i++)
            if (inrect(g_setupRow[i], x, y)) { g_showBar[i] = !g_showBar[i]; g_clearArmed = 0; prefs_save(); return; }
        if (inrect(g_clearRow, x, y)) {
            long now = (long)time(NULL);
            if (now <= g_clearArmed) { g_pinCount = 0; pins_save(); g_clearArmed = 0; }
            else g_clearArmed = now + 3;
        }
    }
    if (g_tab == TAB_MAP) {
        if (inrect(g_resetBtn, x, y)) { g_panX = g_panY = 0; g_zoom = 0.5f; }
        else if (inrect(g_zoomInBtn, x, y)) g_zoom = fminf(6.0f, g_zoom * 1.2f);
        else if (inrect(g_zoomOutBtn, x, y)) g_zoom = fmaxf(0.5f / 1.44f, g_zoom / 1.2f);
    }
}

// the game beeps when energy is low: the whole panel pulses red, slowly, for as long as it does
static void low_energy_flash(void) {
    int hp = U16(A_HP);
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

static void draw(unsigned frame) {
    (void)frame;
    clear_clip();
    fill(0, 0, g_w, g_h, COL_BG);
    static time_t lastFind;
    if (!g_ew && time(NULL) != lastFind) { lastFind = time(NULL); find_ram(); }
    if (!g_ew) { draw_dim(); return; }
    ensure_rom();
    pins_follow_rom();
    if (U16(A_MODE) != 4) { // not in play: the title screen, menus, cutscenes
        memset(g_tabRect, 0, sizeof g_tabRect);
        g_dragDX = g_dragDY = 0; g_tapX = g_longX = -1;
        draw_dim();
        return;
    }
    if (g_tab == TAB_MAP) {
        int ddx = g_dragDX, ddy = g_dragDY;
        g_dragDX = g_dragDY = 0;
        if ((ddx || ddy) && g_previewArea < 0 && inrect(g_mapRect, (float)g_downX, (float)g_downY)) { g_panX -= ddx * g_cellsPerPx; g_panY -= ddy * g_cellsPerPx; }
    } else g_dragDX = g_dragDY = 0;
    int lx = g_longX, ly = g_longY;
    if (lx >= 0) { g_longX = -1; handle_long((float)lx, (float)ly); }
    int tapx = g_tapX, tapy = g_tapY;
    if (tapx >= 0) { g_tapX = -1; handle_tap(tapx, tapy); }

    float w = (float)g_w, h = (float)g_h, margin = w * 0.025f, stripH = fminf(h * 0.16f, w * 0.11f);
    int bar = g_showBar[g_tab];
    RectF strip = bar ? (RectF){ margin, margin, w - margin, margin + stripH } : (RectF){ margin, margin, w - margin, margin };
    float gapTop = bar ? stripH * 0.15f : 0;
    memset(g_weaponRect, 0, sizeof g_weaponRect);
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
    if (!conn) { fprintf(stderr, "banana-zm: DSI-1 not found\n"); drmModeFreeResources(res); return 0; }
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
    fprintf(stderr, "banana-zm: DSI-1 connector %u %dx%d, crtc %u (possible 0x%x busy 0x%x current %u)\n", conn_id, g_w, g_h,
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
    if (drmModeSetCrtc(g_fd, crtc, fb, 0, 0, &conn_id, 1, &mode)) { perror("banana-zm: setcrtc"); return 0; }
    return 1;
}

static void* thread_main(void* arg) {
    (void)arg;
    sleep(2); // RetroArch's own set-up first
    if (!setup_panel()) { fprintf(stderr, "banana-zm: panel setup failed\n"); return NULL; }
    // the menu keeps the bottom backlight off while a game runs
    system(". /usr/libexec/banana/one-panel; one_panel_dark top; one_panel_restore");
    fprintf(stderr, "banana-zm: running, core %s, rom %s\n", g_core, g_rom);
    prefs_load();
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
    snprintf(g_prefs, sizeof g_prefs, "/data/roms/ports/zeromission/prefs.txt");
    unsetenv("LD_PRELOAD"); // not into what RetroArch starts
}
