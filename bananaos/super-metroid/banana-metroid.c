// Super Metroid second screen for bananaOS (Anbernic RG DS): stock RetroArch + snes9x on the top panel, this on the bottom one.
// Preloaded into RetroArch (LD_PRELOAD). RetroArch holds DRM master and draws on the top panel through its KMS context; this
// takes its DRM fd the first time RetroArch flips a page or sets a mode, gives the bottom panel (DSI-1) a CRTC and a dumb
// buffer of its own, and draws there from a thread. The game's memory is read straight from the loaded libretro core
// (retro_get_memory_data), the core's path and the ROM taken from RetroArch's command line. Touch is read from evdev.
// The UI (layout, colours, 5x7 font, suit art, map decoding and world-map layout) is a port of the MetroidArch Android second screen.
// Writes to the game: the selected ammo (tap an ammo icon). Hiding the HUD edits the finished picture on its way to RetroArch.
// Build: see build.sh
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
#include "suit_data.h"

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

// ---- the ROM: the HUD icons and the item percentage's tables ----------------------------------------------------
static uint8_t* g_romData;
static long g_romSize, g_romHdr;
static Img g_icon[3];
static int g_iconsLoaded;

static long romOff(uint32_t a) { return ((((a >> 16) << 15) | (a & 0x7fff)) & 0x3fffff) + g_romHdr; }
static int romU16(uint32_t a) {
    long o = romOff(a);
    return o + 1 < g_romSize ? g_romData[o] | (g_romData[o + 1] << 8) : 0;
}

static void load_rom(void) {
    if (g_romData || !g_rom[0]) return;
    FILE* f = fopen(g_rom, "rb");
    if (!f) { fprintf(stderr, "banana-metroid: cannot open the ROM %s\n", g_rom); return; }
    fseek(f, 0, SEEK_END);
    g_romSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_romHdr = (g_romSize % 1024) == 512 ? 512 : 0;
    g_romData = malloc(g_romSize);
    if (!g_romData || fread(g_romData, 1, g_romSize, f) != (size_t)g_romSize) { free(g_romData); g_romData = NULL; }
    fclose(f);
}

static uint32_t snes15(int c) {
    int r5 = c & 31, g5 = (c >> 5) & 31, b5 = (c >> 10) & 31;
    return COLOR(r5 * 255 / 31, g5 * 255 / 31, b5 * 255 / 31);
}

static const int kHudTiles[22] = { 0x344b, 0x3449, 0x744b, 0x344c, 0x344a, 0x744c, 0x3434, 0x7434, 0x3435, 0x7435, 0x3436,
                                   0x7436, 0x3437, 0x7437, 0x3438, 0x7438, 0x3439, 0x7439, 0x343a, 0x743a, 0x343b, 0x743b };

static void decode_icon(Img* im, int tw, int entryOff) {
    long gfx = romOff(0x9ab200), pal = romOff(0x9a8000);
    im->w = tw * 8; im->h = 16;
    im->px = calloc((size_t)im->w * im->h, 4);
    for (int ty = 0; ty < 2; ty++)
        for (int tx = 0; tx < tw; tx++) {
            int entry = kHudTiles[entryOff + ty * tw + tx];
            int tile = entry & 0x3ff, prow = (entry >> 10) & 7, fx = entry & 0x4000, fy = entry & 0x8000;
            if (tile >= 512) continue;
            for (int py = 0; py < 8; py++)
                for (int px = 0; px < 8; px++) {
                    int sx = fx ? 7 - px : px, sy = fy ? 7 - py : py;
                    const uint8_t* t = g_romData + gfx + tile * 16;
                    int bit = 7 - sx, ci = ((t[sy * 2] >> bit) & 1) | (((t[sy * 2 + 1] >> bit) & 1) << 1);
                    if (!ci) continue;
                    long po = pal + (prow * 4 + ci) * 2;
                    im->px[(ty * 8 + py) * im->w + tx * 8 + px] = snes15(g_romData[po] | (g_romData[po + 1] << 8));
                }
        }
}

static void ensure_icons(void) {
    if (g_iconsLoaded) return;
    load_rom();
    if (!g_romData) return;
    decode_icon(&g_icon[0], 3, 0);
    decode_icon(&g_icon[1], 2, 6);
    decode_icon(&g_icon[2], 2, 10);
    g_iconsLoaded = 1;
}

// ---- the Samus on the equipment screen (a Redux suit's wireframe-less graphic: tiles and tilemaps from suit_data.h)
static Img g_suit;
static int g_suitFor = -1;

static void render_suit(int equipped) {
    if (g_suit.px && g_suitFor == equipped) return;
    g_suitFor = equipped;
    if (!g_suit.px) { g_suit.w = 64; g_suit.h = 136; g_suit.px = calloc(64 * 136, 4); }
    memset(g_suit.px, 0, 64 * 136 * 4);
    static const int keys[4] = { 0x0, 0x100, 0x1, 0x101 };
    int key = equipped & 0x101, variant = 0;
    while (variant < 4 && keys[variant] != key) variant++;
    if (variant == 4) variant = 0;
    const int16_t* body = SUIT_PALETTE_POWER;
    if (equipped & 0x20) body = SUIT_PALETTE_GRAVITY;
    else if (equipped & 0x01) body = SUIT_PALETTE_VARIA;
    int idx = 0;
    for (int row = 0; row < 17; row++)
        for (int col = 0; col < 8; col++) {
            int entry = SUIT_TILEMAPS[variant][idx++] & 0xFFFF;
            int tile = entry & 0x3ff, prow = (entry >> 10) & 7, fx = entry & 0x4000, fy = entry & 0x8000;
            if (tile >= 116 || prow >= 4) continue;
            const int8_t* t = SUIT_TILES + tile * 32;
            for (int py = 0; py < 8; py++)
                for (int px = 0; px < 8; px++) {
                    int sx = fx ? 7 - px : px, sy = fy ? 7 - py : py, bit = 7 - sx;
                    int p0 = t[sy * 2] & 255, p1 = t[sy * 2 + 1] & 255, p2 = t[16 + sy * 2] & 255, p3 = t[16 + sy * 2 + 1] & 255;
                    int ci = ((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1) | (((p2 >> bit) & 1) << 2) | (((p3 >> bit) & 1) << 3);
                    if (!ci) continue;
                    int c15 = (prow == 1 ? body[ci] : SUIT_PALETTE[prow * 16 + ci]) & 0xFFFF;
                    g_suit.px[(row * 8 + py) * 64 + col * 8 + px] = snes15(c15);
                }
        }
}

// ---- the game --------------------------------------------------------------------------------------------------
typedef void* (*get_data_t)(unsigned);
typedef size_t (*get_size_t)(unsigned);
static const uint8_t* g_ram;
static uint8_t* g_vram;

static int find_ram(void) {
    void* h = dlopen(g_core, RTLD_NOLOAD | RTLD_LAZY);
    if (!h) return 0;
    get_data_t d = (get_data_t)dlsym(h, "retro_get_memory_data");
    get_size_t s = (get_size_t)dlsym(h, "retro_get_memory_size");
    if (!d || !s) return 0;
    if (s(RETRO_MEMORY_SYSTEM_RAM) < 0x20000) return 0;
    g_ram = (const uint8_t*)d(RETRO_MEMORY_SYSTEM_RAM);
    if (s(3) >= 0x10000) g_vram = (uint8_t*)d(3);
    for (unsigned id = 0; id < 6; id++) fprintf(stderr, "banana-metroid: memory %u: %zu bytes\n", id, s(id));
    return g_ram != NULL;
}
static void put16(int off, int v) { uint8_t* w = (uint8_t*)g_ram; w[off] = v & 255; w[off + 1] = (v >> 8) & 255; }
static int u16(int off) { return g_ram[off] | (g_ram[off + 1] << 8); }

// ---- what the player has set ----------------------------------------------------------------------------------------
enum { TAB_MAP, TAB_ITEMS, TAB_SETUP };
static int g_tab = TAB_ITEMS;
static int g_showBar[3] = { 1, 1, 1 };
static char g_prefs[512];
static volatile int g_hideHud;

static void prefs_load(void) {
    FILE* f = fopen(g_prefs, "r");
    if (!f) return;
    int a, b, c, t, hh = 0;
    if (fscanf(f, "%d %d %d %d %d", &a, &b, &c, &t, &hh) >= 4) { g_showBar[0] = a; g_showBar[1] = b; g_showBar[2] = c; if (t >= 0 && t < 3) g_tab = t; g_hideHud = hh != 0; }
    fclose(f);
}
static void prefs_save(void) {
    FILE* f = fopen(g_prefs, "w");
    if (!f) return;
    fprintf(f, "%d %d %d %d %d\n", g_showBar[0], g_showBar[1], g_showBar[2], g_tab, g_hideHud);
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

static RectF g_weaponRect[3];
static void draw_strip(RectF strip, float stripH) {
    roundBox(strip, stripH * 0.1f, COL_PANEL_BG, COL_BORDER_DARK, fmaxf(2, stripH * 0.02f));
    int health = u16(0x9C2), maxHealth = u16(0x9C4);
    int counts[3] = { u16(0x9C6), u16(0x9CA), u16(0x9CE) }, maxes[3] = { u16(0x9C8), u16(0x9CC), u16(0x9D0) };
    float textSize = stripH * 0.32f, pipSize = stripH * 0.28f;
    int maxTanks = maxHealth / 100 > 14 ? 14 : maxHealth / 100;
    int filled = health / 100 > maxTanks ? maxTanks : health / 100;
    int cols = maxTanks < 7 ? maxTanks : 7, rowsN = maxTanks <= 7 ? 1 : 2;
    float rowStep = pipSize * 1.3f, blockH = rowStep * (rowsN - 1) + pipSize;
    float blockTop = strip.t + (stripH - blockH) * 0.5f, pipLeft = strip.l + stripH * 0.3f;
    for (int i = 0; i < maxTanks; i++) {
        int row = i / 7, col = i % 7;
        float midY = blockTop + rowStep * row + pipSize * 0.5f, px = pipLeft + col * (pipSize + stripH * 0.07f);
        rectf(px, midY - pipSize / 2, px + pipSize, midY + pipSize / 2, i < filled ? COL_ENERGY_PIP : COL_DIM_GRAY);
        if (i < filled) {
            float th = pipSize * 0.16f;
            rectf(px, midY - pipSize / 2, px + pipSize, midY - pipSize / 2 + th, COL_WHITE);
            rectf(px, midY - pipSize / 2, px + th, midY + pipSize / 2, COL_WHITE);
        }
    }
    float midY = strip.t + stripH * 0.5f, x = pipLeft + cols * (pipSize + stripH * 0.07f) + stripH * 0.35f;
    float ps = pixelSizeFor(textSize);
    char buf[16];
    snprintf(buf, sizeof buf, "%d", maxTanks > 0 ? health % 100 : health);
    text(buf, x, midY - textSize / 2, ps, COL_WHITE, 0);
    x += textW(buf, ps) + stripH * 0.5f;
    float iconH = stripH * 0.34f;
    int sel = u16(0x9D2);
    for (int i = 0; i < 3; i++) {
        if (maxes[i] <= 0) continue;
        float slotStart = x;
        if (g_icon[i].px) {
            float iconW = iconH * g_icon[i].w / g_icon[i].h;
            if (sel == i + 1) strokef(x - stripH * 0.05f, midY - iconH / 2 - stripH * 0.05f, x + iconW + stripH * 0.05f, midY + iconH / 2 + stripH * 0.05f, stripH * 0.04f, COL_ACCENT);
            blit(&g_icon[i], x, midY - iconH / 2, x + iconW, midY + iconH / 2);
            x += iconW + stripH * 0.15f;
        }
        snprintf(buf, sizeof buf, "%d", counts[i]);
        text(buf, x, midY - textSize / 2, ps, COL_WHITE, 0);
        x += textW(buf, ps);
        g_weaponRect[i] = (RectF){ slotStart, strip.t, x, strip.b };
        x += stripH * 0.45f;
    }
}

static void stat_box(float x, float top, float w, float h, const char* label, const char* value) {
    RectF box = { x, top, x + w, top + h };
    pixelBox(box, COL_SLOT_BG, COL_BORDER_DARK);
    text(label, box.l + w * 0.08f, box.t + h * 0.12f, pixelSizeFor(h * 0.22f), COL_DIM_GRAY, 0);
    text(value, box.l + w * 0.08f, box.b - h * 0.14f - h * 0.36f, pixelSizeFor(h * 0.36f), COL_WHITE, 0);
}

typedef struct { const char* title; int n; int bits[5]; const char* labels[5]; int beam; } Equip;
static const Equip kSuit = { "SUIT", 2, { 0x0001, 0x0020 }, { "VARIA SUIT", "GRAVITY SUIT" }, 0 };
static const Equip kMisc = { "MISC.", 4, { 0x0004, 0x1000, 0x0002, 0x0008 }, { "MORPHING BALL", "BOMB", "SPRING BALL", "SCREW ATTACK" }, 0 };
static const Equip kBoots = { "BOOTS", 3, { 0x0100, 0x0200, 0x2000 }, { "HI-JUMP BOOTS", "SPACE JUMP", "SPEED BOOSTER" }, 0 };
static const Equip kBeam = { "BEAM", 5, { 0x1000, 0x0002, 0x0001, 0x0004, 0x0008 }, { "CHARGE", "ICE", "WAVE", "SPAZER", "PLASMA" }, 1 };

static void equip_column(float x, float top, float w, float h, int items, int beams, const Equip* a, const Equip* b) {
    const Equip* groups[2] = { a, b };
    int ng = b ? 2 : 1, total = a->n + (b ? b->n : 0);
    float gap = h * 0.04f, unitH = (h - gap * (ng - 1)) / total;
    float titleSize = unitH * 0.34f, entrySize = unitH * 0.40f, maxLabelW = w * 0.80f;
    float eps = pixelSizeFor(entrySize), widest = 0;
    for (int g = 0; g < ng; g++)
        for (int i = 0; i < groups[g]->n; i++) widest = fmaxf(widest, textW(groups[g]->labels[i], eps));
    if (widest > maxLabelW) { float k = maxLabelW / widest; eps *= k; entrySize *= k; }
    float y = top;
    for (int g = 0; g < ng; g++) {
        const Equip* e = groups[g];
        int bits = e->beam ? beams : items;
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

// the item percentage the game's own tables give (SuperMetroidItemPercent): tanks per 1%, then items and beams
static int item_percent(int items, int beams) {
    if (!g_romData) return -1;
    int total = 0;
    for (int i = 4; i >= 0; i--) {
        int ramAddr = romU16(0x8be70d + i * 2), divisor = romU16(0x8be717 + i * 2) & 0xFF;
        if (ramAddr < 0 || ramAddr >= 0x2000) continue;
        total += divisor == 0 ? 0xFFFF : u16(ramAddr) / divisor;
    }
    for (int i = 0; i < 11; i++) if (romU16(0x8be721 + i * 2) & items) total++;
    for (int i = 0; i < 5; i++) if (romU16(0x8be737 + i * 2) & beams) total++;
    return total;
}

static void draw_items(RectF area) {
    float rad = rh(area) * 0.02f, sw = fmaxf(2, rh(area) * 0.01f);
    roundBox(area, rad, COL_PANEL_BG, COL_BORDER_DARK, sw);
    int items = u16(0x9A4), beams = u16(0x9A8), equipped = u16(0x9A2);
    float pad = rw(area) * 0.03f, left = area.l + pad, right = area.r - pad, top = area.t + pad, bottom = area.b - pad;
    float colGap = pad * 0.8f, headerH = (bottom - top) * 0.16f;
    float aspect = 64.0f / 136.0f;
    float wireW = fminf((right - left) * 0.26f, (bottom - top - headerH) * aspect);
    float colW = (right - left - colGap * 2 - wireW) / 2;
    char pct[24], tm[24];
    int p = item_percent(items, beams);
    if (p < 0) snprintf(pct, sizeof pct, "--.-%%"); else snprintf(pct, sizeof pct, "%d.0%%", p);
    snprintf(tm, sizeof tm, "%02d:%02d:%02d", u16(0x9E0), u16(0x9DE), u16(0x9DC));
    stat_box(left, top, colW, headerH, "ITEMS", pct);
    stat_box(left + colW + colGap + wireW + colGap, top, colW, headerH, "TIME", tm);
    float bodyTop = top + headerH + pad * 0.6f, bodyH = bottom - bodyTop;
    equip_column(left, bodyTop, colW, bodyH, items, beams, &kSuit, &kMisc);
    render_suit(equipped);
    float dh = bodyH, dw = dh * aspect;
    if (dw > wireW) { dw = wireW; dh = dw / aspect; }
    float dl = left + colW + colGap + (wireW - dw) / 2, dt = bodyTop + (bodyH - dh) / 2;
    blit(&g_suit, dl, dt, dl + dw, dt + dh);
    equip_column(left + colW + colGap + wireW + colGap, bodyTop, colW, bodyH, items, beams, &kBoots, &kBeam);
}

static void settings_row(RectF row, RectF* tap, const char* label, const char* value, uint32_t valueCol) {
    *tap = row;
    pixelBox(row, COL_SLOT_BG, COL_BORDER_DARK);
    float h = rh(row), ps = pixelSizeFor(h * 0.38f);
    text(label, row.l + h * 0.25f, rcy(row) - 7 * ps / 2, ps, COL_WHITE, 0);
    text(value, row.r - h * 0.25f, rcy(row) - 7 * ps / 2, ps, valueCol, 2);
}

static RectF g_hideRow, g_clearRow;
static long g_clearArmed;

static void draw_setup(RectF area) {
    float rad = rh(area) * 0.02f, sw = fmaxf(2, rh(area) * 0.01f);
    roundBox(area, rad, COL_PANEL_BG, COL_BORDER_DARK, sw);
    float pad = rw(area) * 0.03f, rowH = rh(area) * 0.14f, gap = rowH * 0.2f, y = area.t + pad;
    static const char* labels[3] = { "STATUS BAR (MAP)", "STATUS BAR (ITEMS)", "STATUS BAR (SETUP)" };
    for (int i = 0; i < 3; i++) {
        RectF row = { area.l + pad, y, area.r - pad, y + rowH };
        settings_row(row, &g_setupRow[i], labels[i], g_showBar[i] ? "ON" : "OFF", g_showBar[i] ? COL_ACCENT : COL_DIM_GRAY);
        y = row.b + gap;
    }
    RectF hide = { area.l + pad, y, area.r - pad, y + rowH };
    settings_row(hide, &g_hideRow, "HIDE MAIN HUD", g_hideHud == 2 ? "CUT" : g_hideHud ? "BLANK" : "OFF", g_hideHud ? COL_ACCENT : COL_DIM_GRAY);
    y = hide.b + gap;
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

// ---- the maps: the pause screen's own map art, decoded from the ROM, on top of what Samus has explored ------------------
#define GRID_W 64
#define GRID_H 32
#define LABEL_TILES 12
#define UNEXPLORED 0xFF14141Eu
#define NAREA 6
#define CANVAS_TW 70
#define CANVAS_TH 57
#define CANVAS_W (CANVAS_TW * 8)
#define CANVAS_H (CANVAS_TH * 8)

// {declMinX, declMinY, declMaxX, declMaxY, destX, destY} in shared-canvas tile units
static const float kAreaLayout[NAREA][6] = {
    { 6, 0, 57, 19, 6.07f, 4.00f },   { 5, 0, 58, 20, 2.07f, 12.00f },  { 2, 0, 38, 18, 30.07f, 30.00f },
    { 10, 10, 22, 20, 43.07f, 4.00f }, { 10, 0, 43, 20, 28.07f, 14.00f }, { 11, 9, 22, 22, 8.07f, 13.00f },
};
static const uint32_t kAreaColor[NAREA] = { COLOR(150, 165, 210), COLOR(110, 210, 110), COLOR(235, 110, 90),
                                            COLOR(210, 180, 110), COLOR(255, 100, 100), COLOR(220, 110, 190) };
// {areaA, ax, ay, areaB, bx, by}: every door between two areas
static const float kConnectors[13][6] = {
    { 0, 6.07f, 12.00f, 1, 6.07f, 12.00f },   { 1, 23.07f, 20.00f, 0, 23.07f, 21.00f }, { 0, 18.07f, 21.00f, 5, 17.07f, 21.00f },
    { 5, 17.07f, 13.00f, 0, 17.07f, 13.00f }, { 1, 34.07f, 16.00f, 0, 34.07f, 11.00f }, { 1, 36.07f, 19.00f, 4, 30.07f, 21.00f },
    { 0, 45.07f, 8.00f, 3, 45.07f, 8.00f },   { 0, 45.07f, 4.00f, 3, 45.07f, 4.00f },   { 0, 45.07f, 5.00f, 3, 45.07f, 5.00f },
    { 0, 43.07f, 7.00f, 3, 43.07f, 7.00f },   { 3, 54.07f, 8.00f, 0, 49.07f, 8.00f },   { 0, 52.07f, 14.00f, 4, 52.07f, 14.00f },
    { 4, 28.07f, 32.00f, 1, 34.07f, 30.00f },
};

static int romByte(uint32_t a) {
    long o = romOff(a);
    return o >= 0 && o < g_romSize ? g_romData[o] : 0;
}

static void decode_explored(const uint8_t* bits, uint8_t* out) {
    for (int y = 0; y < GRID_H; y++)
        for (int x = 0; x < GRID_W; x++) {
            int idx = ((x >> 3) & 3) + (x >> 5) * 128 + 4 * y;
            out[y * GRID_W + x] = (bits[idx] & (0x80 >> (x & 7))) != 0;
        }
}

static void tile_pixels(uint32_t* out, int tx, int ty, uint16_t entry, int dim) {
    int tile = entry & 0x3ff, prow = (entry >> 10) & 7, fx = entry & 0x4000, fy = entry & 0x8000;
    long gfx = romOff(0xb68000) + tile * 32L, pal = romOff(0xb6f000) + prow * 32L;
    for (int py = 0; py < 8; py++) {
        int sy = fy ? 7 - py : py;
        for (int px = 0; px < 8; px++) {
            int sx = fx ? 7 - px : px, bit = 7 - sx;
            const uint8_t* t = g_romData + gfx;
            int ci = ((t[sy * 2] >> bit) & 1) | (((t[sy * 2 + 1] >> bit) & 1) << 1) | (((t[16 + sy * 2] >> bit) & 1) << 2) |
                     (((t[16 + sy * 2 + 1] >> bit) & 1) << 3);
            uint32_t c = snes15(g_romData[pal + ci * 2] | (g_romData[pal + ci * 2 + 1] << 8));
            if (dim) c = 0xFF000000u | ((((c >> 16) & 255) / 2) << 16) | ((((c >> 8) & 255) / 2) << 8) | ((c & 255) / 2);
            out[(ty * 8 + py) * (GRID_W * 8) + tx * 8 + px] = c;
        }
    }
}

// the area's whole map as 512x256 pixels; unexplored tiles flat dark, a Map Station's reveal dimmed
static void render_area_map(int area, const uint8_t* exploredBits, int haveStation, uint32_t* out) {
    int remapped = area >= 7 ? 0 : area;
    uint32_t tmAddr = romU16(0x82964a + remapped * 3) | ((uint32_t)romByte(0x82964a + remapped * 3 + 2) << 16);
    long tmOff = romOff(tmAddr);
    uint8_t explored[GRID_W * GRID_H], station[GRID_W * GRID_H];
    decode_explored(exploredBits, explored);
    memset(station, 0, sizeof station);
    if (haveStation) {
        long so = romOff(0x820000 | romU16(0x829717 + remapped * 2));
        if (so >= 0 && so + 256 <= g_romSize) decode_explored(g_romData + so, station);
    }
    for (int ty = 0; ty < GRID_H; ty++)
        for (int tx = 0; tx < GRID_W; tx++) {
            int idx = ty * GRID_W + tx, ex = explored[idx], st = !ex && station[idx];
            long eo = tmOff + ((tx >> 5) * 1024 + ty * 32 + (tx & 31)) * 2L;
            uint16_t entry = 0;
            int ok = (ex || st) && eo >= 0 && eo + 1 < g_romSize;
            if (ok) entry = g_romData[eo] | (g_romData[eo + 1] << 8);
            if (!ok || (entry & 0x3ff) >= 768) {
                for (int py = 0; py < 8; py++)
                    for (int px = 0; px < 8; px++) out[(ty * 8 + py) * (GRID_W * 8) + tx * 8 + px] = UNEXPLORED;
                continue;
            }
            tile_pixels(out, tx, ty, entry, st);
        }
}

static void tint_pixels(uint32_t* px, size_t n, int colorIndex) {
    uint32_t a = kAreaColor[colorIndex];
    float tr = ((a >> 16) & 255) / 255.0f, tg = ((a >> 8) & 255) / 255.0f, tb = (a & 255) / 255.0f;
    for (size_t i = 0; i < n; i++) {
        uint32_t p = px[i];
        if (p == UNEXPLORED) continue;
        px[i] = (p & 0xFF000000u) | ((uint32_t)(((p >> 16) & 255) * tr) << 16) | ((uint32_t)(((p >> 8) & 255) * tg) << 8) | (uint32_t)((p & 255) * tb);
    }
}
static int color_index(int area) { return area < 0 || area >= NAREA ? 0 : area; }

static uint32_t hash_bytes(const uint8_t* b, size_t n, uint32_t h) {
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

// ---- the area-name labels ("BRINSTAR" …), 96x8 each
static Img g_label[NAREA];
static void ensure_labels(void) {
    if (g_label[0].px || !g_romData) return;
    for (int a = 0; a < NAREA; a++) {
        Img* im = &g_label[a];
        im->w = LABEL_TILES * 8; im->h = 8;
        im->px = calloc((size_t)im->w * im->h, 4);
        long tmOff = romOff(0x820000 | romU16(0x82965f + a * 2));
        for (int t = 0; t < LABEL_TILES; t++) {
            long eo = tmOff + t * 2L;
            if (eo < 0 || eo + 1 >= g_romSize) continue;
            uint16_t entry = (g_romData[eo] | (g_romData[eo + 1] << 8)) & 0xEFFF;
            int tile = entry & 0x3ff, prow = (entry >> 10) & 7, fx = entry & 0x4000, fy = entry & 0x8000;
            if (tile >= 768) continue;
            long gfx = romOff(0xb68000) + tile * 32L, pal = romOff(0xb6f000) + prow * 32L;
            for (int py = 0; py < 8; py++) {
                int sy = fy ? 7 - py : py;
                for (int px = 0; px < 8; px++) {
                    int sx = fx ? 7 - px : px, bit = 7 - sx;
                    const uint8_t* g = g_romData + gfx;
                    int ci = ((g[sy * 2] >> bit) & 1) | (((g[sy * 2 + 1] >> bit) & 1) << 1) | (((g[16 + sy * 2] >> bit) & 1) << 2) |
                             (((g[16 + sy * 2 + 1] >> bit) & 1) << 3);
                    if (!ci) continue;
                    im->px[py * im->w + t * 8 + px] = snes15(g_romData[pal + ci * 2] | (g_romData[pal + ci * 2 + 1] << 8));
                }
            }
        }
    }
}

// ---- the current area's own map
static uint32_t g_areaPx[GRID_W * 8 * GRID_H * 8];
static Img g_areaImg = { GRID_W * 8, GRID_H * 8, g_areaPx };
static int g_areaFor = -1;
static uint32_t g_areaHash;

static void update_area_map(int area) {
    uint8_t station = g_ram[0xD908 + (area >= 0 && area < 8 ? area : 0)] != 0;
    uint32_t h = hash_bytes(g_ram + 0x7F7, 256, 2166136261u) * 31 + station;
    if (g_areaFor == area && g_areaHash == h) return;
    render_area_map(area, g_ram + 0x7F7, station, g_areaPx);
    tint_pixels(g_areaPx, GRID_W * 8 * GRID_H * 8, color_index(area));
    g_areaFor = area;
    g_areaHash = h;
}

// ---- the world map: every area at its true place, joined where the doors are
static uint32_t g_world[CANVAS_W * CANVAS_H];
static Img g_worldImg = { CANVAS_W, CANVAS_H, g_world };
static int8_t g_owner[CANVAS_W * CANVAS_H];
static int g_areaDrawn[NAREA];
static float g_labelCx[NAREA], g_labelCy[NAREA];
static uint32_t g_worldHash = 1;
static uint32_t g_tmpArea[GRID_W * 8 * GRID_H * 8];

static void thick_line(float ax, float ay, float bx, float by, float hw, uint32_t color) {
    int minX = (int)floorf(fminf(ax, bx) - hw), maxX = (int)ceilf(fmaxf(ax, bx) + hw);
    int minY = (int)floorf(fminf(ay, by) - hw), maxY = (int)ceilf(fmaxf(ay, by) + hw);
    if (minX < 0) minX = 0;
    if (minY < 0) minY = 0;
    if (maxX > CANVAS_W - 1) maxX = CANVAS_W - 1;
    if (maxY > CANVAS_H - 1) maxY = CANVAS_H - 1;
    float dx = bx - ax, dy = by - ay, lenSq = dx * dx + dy * dy;
    for (int y = minY; y <= maxY; y++)
        for (int x = minX; x <= maxX; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float t = lenSq > 0 ? ((px - ax) * dx + (py - ay) * dy) / lenSq : 0;
            t = fmaxf(0, fminf(1, t));
            float ddx = px - (ax + t * dx), ddy = py - (ay + t * dy);
            if (ddx * ddx + ddy * ddy <= hw * hw) g_world[y * CANVAS_W + x] = color;
        }
}
static uint32_t darken(uint32_t c, float f) {
    return 0xFF000000u | ((uint32_t)(((c >> 16) & 255) * f) << 16) | ((uint32_t)(((c >> 8) & 255) * f) << 8) | (uint32_t)((c & 255) * f);
}

static void update_world_map(int currentArea) {
    uint8_t bits[NAREA][256];
    uint32_t h = 2166136261u;
    int have[NAREA], station[NAREA];
    for (int a = 0; a < NAREA; a++) {
        memcpy(bits[a], a == currentArea ? g_ram + 0x7F7 : g_ram + 0xCD52 + a * 256, 256);
        station[a] = g_ram[0xD908 + a] != 0;
        h = hash_bytes(bits[a], 256, h) * 31 + station[a];
        int any = 0;
        for (int i = 0; i < 256; i++) if (bits[a][i]) { any = 1; break; }
        have[a] = any;
    }
    if (h == g_worldHash) return;
    g_worldHash = h;
    memset(g_world, 0, sizeof g_world);
    memset(g_owner, -1, sizeof g_owner);
    float bbox[NAREA];
    for (int a = 0; a < NAREA; a++) bbox[a] = (kAreaLayout[a][2] - kAreaLayout[a][0]) * (kAreaLayout[a][3] - kAreaLayout[a][1]);
    for (int a = 0; a < NAREA; a++) {
        g_areaDrawn[a] = have[a];
        if (!have[a]) continue;
        render_area_map(a, bits[a], station[a], g_tmpArea);
        tint_pixels(g_tmpArea, GRID_W * 8 * GRID_H * 8, color_index(a));
        const float* l = kAreaLayout[a];
        int sx0 = (int)l[0] * 8, sy0 = (int)l[1] * 8, dx0 = (int)(l[4] * 8), dy0 = (int)(l[5] * 8);
        int w = ((int)l[2] - (int)l[0]) * 8, hh = ((int)l[3] - (int)l[1]) * 8;
        for (int y = 0; y < hh; y++)
            for (int x = 0; x < w; x++) {
                if (dx0 + x >= CANVAS_W || dy0 + y >= CANVAS_H) continue;
                uint32_t p = g_tmpArea[(sy0 + y) * (GRID_W * 8) + sx0 + x];
                if (p == UNEXPLORED) continue;
                int di = (dy0 + y) * CANVAS_W + dx0 + x;
                int8_t o = g_owner[di];
                if (o != -1 && o != a && bbox[a] >= bbox[(int)o]) continue;
                g_world[di] = p;
                g_owner[di] = (int8_t)a;
            }
    }
    for (int i = 0; i < 13; i++) {
        const float* c = kConnectors[i];
        int A = (int)c[0], B = (int)c[3];
        if (!g_areaDrawn[A] || !g_areaDrawn[B]) continue;
        if (hypotf(c[1] - c[4], c[2] - c[5]) > 3.0f) continue;
        uint32_t ca = kAreaColor[A], cb = kAreaColor[B];
        uint32_t blend = COLOR((((ca >> 16) & 255) + ((cb >> 16) & 255)) / 2, (((ca >> 8) & 255) + ((cb >> 8) & 255)) / 2, ((ca & 255) + (cb & 255)) / 2);
        thick_line(c[1] * 8, c[2] * 8, c[4] * 8, c[5] * 8, 5.0f, darken(blend, 0.35f));
        thick_line(c[1] * 8, c[2] * 8, c[4] * 8, c[5] * 8, 4.3f, darken(blend, 0.6f));
    }
    double sx[NAREA] = { 0 }, sy[NAREA] = { 0 };
    long cnt[NAREA] = { 0 };
    for (int y = 0; y < CANVAS_H; y++)
        for (int x = 0; x < CANVAS_W; x++) {
            int o = g_owner[y * CANVAS_W + x];
            if (o < 0) continue;
            sx[o] += x; sy[o] += y; cnt[o]++;
        }
    for (int a = 0; a < NAREA; a++)
        if (cnt[a]) { g_labelCx[a] = (float)(sx[a] / cnt[a]) / 8; g_labelCy[a] = (float)(sy[a] / cnt[a]) / 8; }
}

// ---- drawing the maps
static float g_roomZoom = 0.6f, g_worldZoom = 1.0f, g_roomPanX, g_roomPanY, g_worldPanX, g_worldPanY;
static int g_worldView, g_lastRoomArea = -1;
static float g_roomTilesPerPx = 1, g_worldTilesPerPx = 1;
static RectF g_mapRect, g_toggleBtn, g_zoomOutBtn, g_zoomInBtn;

static void set_clip(RectF r) {
    g_clipX0 = (int)floorf(r.l); g_clipY0 = (int)floorf(r.t); g_clipX1 = (int)ceilf(r.r); g_clipY1 = (int)ceilf(r.b);
}
static void clear_clip(void) { g_clipX0 = 0; g_clipY0 = 0; g_clipX1 = g_w; g_clipY1 = g_h; }

// nearest-neighbour copy of a source rectangle onto dst, clipped
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

static void map_message(RectF a, const char* msg) {
    float ps = pixelSizeFor(rh(a) * 0.06f);
    text(msg, rcx(a), rcy(a) - 7 * ps / 2, ps, COL_DIM_GRAY, 1);
}

static float clampf(float v, float lo, float hi) { return hi < lo ? (lo + hi) / 2 : v < lo ? lo : v > hi ? hi : v; }
static int clampi(int v, int lo, int hi) { return hi < lo ? lo : v < lo ? lo : v > hi ? hi : v; }

static void samus_tile(float* fx, float* fy) {
    int roomX = u16(0x7A1), roomY = u16(0x7A3), sx = u16(0xAF6), sy = u16(0xAFA);
    *fx = roomX + sx / 256.0f;
    *fy = roomY + sy / 256.0f + 1.0f;
}

// ---- map pins: "come back to this" flags, long-press to drop or remove, kept per ROM and save slot ----------------------
#define MAX_PINS 64
static int g_pinArea[MAX_PINS], g_pinCount;
static float g_pinX[MAX_PINS], g_pinY[MAX_PINS];
static char g_pinFile[700];
static float g_rmDL, g_rmDT, g_rmCropTx, g_rmCropTy, g_rmScale, g_wOriginX, g_wOriginY, g_wScale;

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
// the pins belong to one ROM and one of the game's three save slots
static void pins_follow_save(void) {
    static time_t last;
    time_t now = time(NULL);
    if (g_pinFile[0] && now == last) return;
    last = now;
    char key[512];
    const char* base = strrchr(g_rom, '/');
    snprintf(key, sizeof key, "%s", base ? base + 1 : g_rom);
    for (char* p = key; *p; p++) if (!isalnum((unsigned char)*p)) *p = '_';
    char path[700];
    snprintf(path, sizeof path, "/data/roms/ports/metroid2s/pins-%s-slot%d.txt", key, u16(0x952));
    if (strcmp(path, g_pinFile)) { snprintf(g_pinFile, sizeof g_pinFile, "%s", path); pins_load(); }
}
static void pin_toggle(int area, float tx, float ty) {
    int nearest = -1;
    float best = 1.1f * 1.1f;
    for (int i = 0; i < g_pinCount; i++) {
        if (g_pinArea[i] != area) continue;
        float dx = g_pinX[i] - tx, dy = g_pinY[i] - ty, d = dx * dx + dy * dy;
        if (d <= best) { best = d; nearest = i; }
    }
    if (nearest != -1) {
        g_pinCount--;
        g_pinArea[nearest] = g_pinArea[g_pinCount]; g_pinX[nearest] = g_pinX[g_pinCount]; g_pinY[nearest] = g_pinY[g_pinCount];
    } else if (g_pinCount < MAX_PINS) {
        g_pinArea[g_pinCount] = area; g_pinX[g_pinCount] = tx; g_pinY[g_pinCount] = ty; g_pinCount++;
    }
    pins_save();
}
// a flag whose pole stands on (cx, cy)
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

static void draw_room_map(RectF area) {
    int areaIndex = u16(0x79F);
    if (areaIndex != g_lastRoomArea) { g_roomPanX = g_roomPanY = 0; g_lastRoomArea = areaIndex; }
    update_area_map(areaIndex);
    int roomX = u16(0x7A1), roomY = u16(0x7A3);
    int tileX = roomX + (u16(0xAF6) >> 8), tileY = roomY + (u16(0xAFA) >> 8) + 1;
    int winW = clampi((int)lroundf(20 / g_roomZoom), 3, GRID_W), winH = clampi((int)lroundf(12 / g_roomZoom), 2, GRID_H);
    float cx = tileX + g_roomPanX, cy = tileY + g_roomPanY;
    int cropTx = clampi((int)lroundf(cx - winW / 2.0f), 0, GRID_W - winW), cropTy = clampi((int)lroundf(cy - winH / 2.0f), 0, GRID_H - winH);
    int cropW = winW * 8, cropH = winH * 8;
    float scale = fminf(rw(area) / cropW, rh(area) / cropH), dw = cropW * scale, dh = cropH * scale;
    float dl = rcx(area) - dw / 2, dt = rcy(area) - dh / 2;
    g_roomTilesPerPx = 1.0f / (scale * 8);
    set_clip(area);
    blit_region(&g_areaImg, cropTx * 8, cropTy * 8, cropW, cropH, dl, dt, dl + dw, dt + dh);
    g_rmDL = dl; g_rmDT = dt; g_rmCropTx = (float)cropTx; g_rmCropTy = (float)cropTy; g_rmScale = scale;
    for (int i = 0; i < g_pinCount; i++)
        if (g_pinArea[i] == areaIndex) draw_pin_marker(dl + (g_pinX[i] - cropTx) * 8 * scale, dt + (g_pinY[i] - cropTy) * 8 * scale, 8 * scale);
    float dotX = dl + (tileX - cropTx + 0.5f) * 8 * scale, dotY = dt + (tileY - cropTy + 0.5f) * 8 * scale;
    if (dotX >= dl && dotX <= dl + dw && dotY >= dt && dotY <= dt + dh) circlef(dotX, dotY, fmaxf(3, 8 * scale * 0.6f), COLOR(255, 70, 70));
    clear_clip();
}

static void draw_world_label(const Img* label, float cx, float cy, float labelW) {
    float labelH = labelW * (8.0f / (LABEL_TILES * 8)), padX = labelW * 0.10f, padY = labelH * 0.35f;
    RectF plate = { cx - labelW / 2 - padX, cy - labelH / 2 - padY, cx + labelW / 2 + padX, cy + labelH / 2 + padY };
    roundBox(plate, rh(plate) * 0.15f, COL_PANEL_BG, COL_BORDER_DARK, fmaxf(1.5f, rh(plate) * 0.06f));
    blit_region(label, 0, 0, label->w, label->h, cx - labelW / 2, cy - labelH / 2, cx + labelW / 2, cy + labelH / 2);
}

static void draw_world_map(RectF area) {
    int currentArea = u16(0x79F);
    update_world_map(currentArea);
    ensure_labels();
    float visMinX = 1e9f, visMinY = 1e9f, visMaxX = -1e9f, visMaxY = -1e9f;
    for (int a = 0; a < NAREA; a++) {
        if (!g_areaDrawn[a]) continue;
        const float* l = kAreaLayout[a];
        visMinX = fminf(visMinX, l[4]); visMinY = fminf(visMinY, l[5]);
        visMaxX = fmaxf(visMaxX, l[4] + (l[2] - l[0])); visMaxY = fmaxf(visMaxY, l[5] + (l[3] - l[1]));
    }
    if (visMinX > visMaxX) { map_message(area, "NOTHING EXPLORED YET"); return; }
    float margin = fmaxf(visMaxX - visMinX, visMaxY - visMinY) * 0.06f;
    visMinX -= margin; visMinY -= margin; visMaxX += margin; visMaxY += margin;
    float fitCx = (visMinX + visMaxX) / 2, fitCy = (visMinY + visMaxY) / 2, fitW = visMaxX - visMinX, fitH = visMaxY - visMinY;
    float canvasW = fitW / g_worldZoom, canvasH = fitH / g_worldZoom, halfW = canvasW / 2, halfH = canvasH / 2;
    float centerX = clampf(fitCx + g_worldPanX, visMinX + halfW, visMaxX - halfW);
    float centerY = clampf(fitCy + g_worldPanY, visMinY + halfH, visMaxY - halfH);
    float viewMinX = centerX - halfW, viewMinY = centerY - halfH;
    float scale = fminf(rw(area) / canvasW, rh(area) / canvasH);
    float originX = area.l + (rw(area) - canvasW * scale) / 2 - viewMinX * scale;
    float originY = area.t + (rh(area) - canvasH * scale) / 2 - viewMinY * scale;
    g_worldTilesPerPx = 1.0f / scale;
    set_clip(area);
    blit_region(&g_worldImg, 0, 0, CANVAS_W, CANVAS_H, originX, originY, originX + CANVAS_TW * scale, originY + CANVAS_TH * scale);
    g_wOriginX = originX; g_wOriginY = originY; g_wScale = scale;
    for (int i = 0; i < g_pinCount; i++) {
        if (!g_areaDrawn[g_pinArea[i]]) continue;
        const float* l = kAreaLayout[g_pinArea[i]];
        draw_pin_marker(originX + (l[4] + (g_pinX[i] - l[0])) * scale, originY + (l[5] + (g_pinY[i] - l[1])) * scale, scale);
    }
    float labelW = rw(area) * 0.16f;
    for (int a = 0; a < NAREA; a++)
        if (g_areaDrawn[a] && g_label[a].px) draw_world_label(&g_label[a], originX + g_labelCx[a] * scale, originY + g_labelCy[a] * scale, labelW);
    if (currentArea >= 0 && currentArea < NAREA) {
        float sx, sy;
        samus_tile(&sx, &sy);
        const float* l = kAreaLayout[currentArea];
        if (sx >= l[0] && sx < l[2] && sy >= l[1] && sy < l[3]) {
            float mx = originX + (l[4] + (sx - l[0])) * scale, my = originY + (l[5] + (sy - l[1])) * scale, rad = fmaxf(4, scale * 0.55f);
            circlef(mx, my, rad, COL_WHITE);
            circlef(mx, my, rad - fmaxf(2, rad * 0.25f), COLOR(255, 70, 70));
        }
    }
    clear_clip();
}

static void layout_map_buttons(RectF bar) {
    float gap = rw(bar) * 0.02f, bw = (rw(bar) - gap * 2) / 3;
    g_toggleBtn = (RectF){ bar.l, bar.t, bar.l + bw, bar.b };
    g_zoomOutBtn = (RectF){ bar.l + bw + gap, bar.t, bar.l + 2 * bw + gap, bar.b };
    g_zoomInBtn = (RectF){ bar.l + 2 * (bw + gap), bar.t, bar.r, bar.b };
}

static void draw_map_buttons(void) {
    pixelBox(g_toggleBtn, COL_PANEL_BG, COL_BORDER_DARK);
    float pad = fminf(rw(g_toggleBtn), rh(g_toggleBtn)) * 0.24f, cx = rcx(g_toggleBtn), cy = rcy(g_toggleBtn), lw = fmaxf(2, pad * 0.16f);
    strokef(cx - pad, cy - pad, cx + pad, cy + pad, lw, g_worldView ? COL_WHITE : COL_ACCENT);
    float in = pad * 0.45f;
    strokef(cx - in, cy - in, cx + in, cy + in, lw, g_worldView ? COL_ACCENT : COL_WHITE);
    pixelBox(g_zoomInBtn, COL_PANEL_BG, COL_BORDER_DARK);
    pixelBox(g_zoomOutBtn, COL_PANEL_BG, COL_BORDER_DARK);
    float half = fminf(rw(g_zoomInBtn), rh(g_zoomInBtn)) * 0.28f, t = fmaxf(2, fminf(rw(g_zoomInBtn), rh(g_zoomInBtn)) * 0.08f);
    float x1 = rcx(g_zoomInBtn), y1 = rcy(g_zoomInBtn), x2 = rcx(g_zoomOutBtn), y2 = rcy(g_zoomOutBtn);
    rectf(x1 - half, y1 - t / 2, x1 + half, y1 + t / 2, COL_ACCENT);
    rectf(x1 - t / 2, y1 - half, x1 + t / 2, y1 + half, COL_ACCENT);
    rectf(x2 - half, y2 - t / 2, x2 + half, y2 + t / 2, COL_ACCENT);
}

static void draw_map(RectF area) {
    roundBox(area, rh(area) * 0.03f, COL_PANEL_BG, COL_BORDER_DARK, fmaxf(2, rh(area) * 0.01f));
    g_mapRect = area;
    if (!g_romData) { map_message(area, "NO ROM"); return; }
    if (g_worldView) draw_world_map(area); else draw_room_map(area);
}

static void map_tap(float x, float y) {
    if (inrect(g_toggleBtn, x, y)) {
        g_worldView = !g_worldView;
        if (g_worldView) { g_worldZoom = 1.0f; g_worldPanX = g_worldPanY = 0; }
        else { g_roomZoom = 0.6f; g_roomPanX = g_roomPanY = 0; }
    } else if (inrect(g_zoomInBtn, x, y) || inrect(g_zoomOutBtn, x, y)) {
        int in = inrect(g_zoomInBtn, x, y);
        if (g_worldView) g_worldZoom = in ? fminf(4.0f, g_worldZoom * 1.4f) : fmaxf(1.0f, g_worldZoom / 1.4f);
        else g_roomZoom = in ? fminf(6.0f, g_roomZoom * 1.4f) : fmaxf(0.6f, g_roomZoom / 1.4f);
    }
}

static void map_drag(int dx, int dy) {
    if (g_worldView) { g_worldPanX -= dx * g_worldTilesPerPx; g_worldPanY -= dy * g_worldTilesPerPx; }
    else { g_roomPanX -= dx * g_roomTilesPerPx; g_roomPanY -= dy * g_roomTilesPerPx; }
}


static void handle_long(float x, float y) {
    if (g_tab != TAB_MAP || !inrect(g_mapRect, x, y)) return;
    if (g_worldView) {
        if (g_wScale <= 0) return;
        float cx = (x - g_wOriginX) / g_wScale, cy = (y - g_wOriginY) / g_wScale;
        for (int a = 0; a < NAREA; a++) {
            if (!g_areaDrawn[a]) continue;
            const float* l = kAreaLayout[a];
            float lx = cx - l[4] + l[0], ly = cy - l[5] + l[1];
            if (lx >= l[0] && lx < l[2] && ly >= l[1] && ly < l[3]) { pin_toggle(a, lx, ly); break; }
        }
    } else {
        if (g_rmScale <= 0) return;
        int area = u16(0x79F);
        if (area >= 0 && area < NAREA) pin_toggle(area, g_rmCropTx + (x - g_rmDL) / (g_rmScale * 8), g_rmCropTy + (y - g_rmDT) / (g_rmScale * 8));
    }
}

static void handle_tap(int x, int y) {
    for (int i = 0; i < 3; i++)
        if (inrect(g_weaponRect[i], (float)x, (float)y)) {
            put16(0x9D2, u16(0x9D2) == i + 1 ? 0 : i + 1);
            put16(0xA04, 0);
            put16(0x9EA, 0);
            return;
        }
    for (int i = 0; i < 3; i++)
        if (inrect(g_tabRect[i], (float)x, (float)y)) { g_tab = i; prefs_save(); return; }
    if (g_tab == TAB_MAP) { map_tap((float)x, (float)y); return; }
    if (g_tab == TAB_SETUP) {
        for (int i = 0; i < 3; i++)
            if (inrect(g_setupRow[i], (float)x, (float)y)) { g_showBar[i] = !g_showBar[i]; g_clearArmed = 0; prefs_save(); return; }
        if (inrect(g_hideRow, (float)x, (float)y)) { g_hideHud = (g_hideHud + 1) % 3; g_clearArmed = 0; prefs_save(); return; }
        if (inrect(g_clearRow, (float)x, (float)y)) {
            long now = (long)time(NULL);
            if (now <= g_clearArmed) { g_pinCount = 0; pins_save(); g_clearArmed = 0; }
            else g_clearArmed = now + 3;
        }
    }
}

// the game beeps when energy is below 30: the whole panel pulses red, slowly, for as long as it does
static void low_energy_flash(void) {
    int health = u16(0x9C2);
    if (health <= 0 || health >= 30) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    float phase = (ts.tv_sec % 2 + ts.tv_nsec / 1e9f) / 2.0f; // one pulse every two seconds
    float k = 0.5f - 0.5f * cosf(phase * 6.2831853f);
    int a = (int)(k * 90); // 0..90 of 255
    for (int y = 0; y < g_h; y++) {
        uint32_t* row = g_back + (size_t)y * g_pitch;
        for (int x = 0; x < g_w; x++) {
            uint32_t p = row[x];
            int r = (((p >> 16) & 255) * (255 - a) + 255 * a) / 255, gr = (((p >> 8) & 255) * (255 - a)) / 255, b = ((p & 255) * (255 - a)) / 255;
            row[x] = 0xFF000000u | (r << 16) | (gr << 8) | b;
        }
    }
}

// ---- hiding the game's own HUD: the finished picture is touched on its way to RetroArch ---------------------------------
// The core hands every frame to RetroArch through a function pointer it keeps in its own data. That pointer is found by
// setting it to ours and seeing which word changed, and ours then blanks or cuts away the top 32 lines (the HUD) before
// passing the picture on, so nothing can flicker. g_hideHud: 0 shown, 1 blanked, 2 cut away (the picture then fills the panel).
typedef void (*video_cb_t)(const void*, unsigned, unsigned, size_t);
static video_cb_t g_origVideo;

static int playing_live(void) {
    int st = u16(0x998);
    return st >= 7 && st <= 0xB;
}

static void video_wrapper(const void* data, unsigned w, unsigned h, size_t pitch) {
    video_cb_t o = g_origVideo;
    if (!o) return;
    int mode = g_hideHud;
    if (mode && data && data != (const void*)-1 && g_ram && playing_live() && h >= 224) {
        unsigned rows = 32 * (h / 224);
        if (mode == 1) memset((void*)data, 0, pitch * rows);
        else { data = (const uint8_t*)data + pitch * rows; h -= rows; }
    }
    o(data, w, h, pitch);
}

static void install_video_hook(void) {
    void* h = dlopen(g_core, RTLD_NOLOAD | RTLD_LAZY);
    void (*setv)(video_cb_t) = h ? (void (*)(video_cb_t))dlsym(h, "retro_set_video_refresh") : NULL;
    if (!setv) return;
    // the core's writable mappings (its data, and the anonymous zero-fill right after it)
    struct { uintptr_t a, b; uint8_t* copy; } seg[8];
    int n = 0;
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[1024];
    uintptr_t prevEnd = 0;
    int inCore = 0;
    while (fgets(line, sizeof line, f) && n < 8) {
        unsigned long a, b;
        char perms[8], path[900] = "";
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %899[^\n]", &a, &b, perms, path) < 3) continue;
        int named = path[0] != 0 && path[0] != ' ';
        const char* p = path;
        while (*p == ' ') p++;
        if (*p) inCore = strstr(p, "snes9x_libretro") != NULL;
        else if (!(inCore && a == prevEnd)) inCore = 0;
        prevEnd = b;
        (void)named;
        if (inCore && perms[0] == 'r' && perms[1] == 'w') { seg[n].a = a; seg[n].b = b; seg[n].copy = malloc(b - a); memcpy(seg[n].copy, (void*)a, b - a); n++; }
    }
    fclose(f);
    setv(video_wrapper);
    for (int i = 0; i < n; i++)
        for (uintptr_t q = seg[i].a; q + 8 <= seg[i].b; q += 8)
            if (*(uintptr_t*)q == (uintptr_t)video_wrapper) {
                uintptr_t was = *(uintptr_t*)(seg[i].copy + (q - seg[i].a));
                if (was && was != (uintptr_t)video_wrapper) { g_origVideo = (video_cb_t)was; fprintf(stderr, "banana-metroid: video hook at %p\n", (void*)q); return; }
            }
    fprintf(stderr, "banana-metroid: video hook not found\n");
}

static void draw(unsigned frame) {
    (void)frame;
    clear_clip();
    fill(0, 0, g_w, g_h, COL_BG);
    if (!g_ram && !find_ram()) { draw_dim(); return; }
    ensure_icons();
    int state = u16(0x998);
    if (state < 7 || state > 0xB) { // not in play: the title screen, menus, cutscenes
        memset(g_tabRect, 0, sizeof g_tabRect);
        draw_dim();
        return;
    }
    if (g_tab == TAB_MAP) {
        int ddx = g_dragDX, ddy = g_dragDY;
        g_dragDX = g_dragDY = 0;
        if ((ddx || ddy) && inrect(g_mapRect, (float)g_downX, (float)g_downY)) map_drag(ddx, ddy);
    } else g_dragDX = g_dragDY = 0;
    pins_follow_save();
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
    if (!conn) { fprintf(stderr, "banana-metroid: DSI-1 not found\n"); drmModeFreeResources(res); return 0; }
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
    fprintf(stderr, "banana-metroid: DSI-1 connector %u %dx%d, crtc %u (possible 0x%x busy 0x%x current %u)\n", conn_id, g_w, g_h,
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
    if (drmModeSetCrtc(g_fd, crtc, fb, 0, 0, &conn_id, 1, &mode)) { perror("banana-metroid: setcrtc"); return 0; }
    return 1;
}

static void* thread_main(void* arg) {
    (void)arg;
    sleep(2); // RetroArch's own set-up first
    if (!setup_panel()) { fprintf(stderr, "banana-metroid: panel setup failed\n"); return NULL; }
    // the menu keeps the bottom backlight off while a game runs
    system(". /usr/libexec/banana/one-panel; one_panel_dark top; one_panel_restore");
    fprintf(stderr, "banana-metroid: running, core %s, rom %s\n", g_core, g_rom);
    prefs_load();
    pthread_t tt;
    pthread_create(&tt, NULL, touch_thread, NULL);
    install_video_hook();
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
    snprintf(g_prefs, sizeof g_prefs, "/data/roms/ports/metroid2s/prefs.txt");
    unsetenv("LD_PRELOAD"); // not into what RetroArch starts
}
