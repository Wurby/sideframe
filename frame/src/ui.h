// ui.h — tiny immediate-mode 2D toolkit for the Sideframe tab (software rendered into an RGBA8 buffer; bytes are sRGB-encoded).
// Rounded rectangles / circles with anti-aliasing, gradients, FreeType text (Noto Sans), and simple buttons with hover/click.
#ifndef UI_H
#define UI_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#define RGBA(r, g, b, a) (((uint32_t)(r) << 24) | ((uint32_t)(g) << 16) | ((uint32_t)(b) << 8) | (uint32_t)(a))

/* ---- palette (matches the app icon) ---- */
#define C_BG_TOP     RGBA(0x1b, 0x27, 0x42, 255)
#define C_BG_BOTTOM  RGBA(0x0a, 0x10, 0x20, 255)
#define C_CARD       RGBA(0x1a, 0x25, 0x3d, 255)
#define C_CARD_HOT   RGBA(0x22, 0x31, 0x52, 255)
#define C_LINE       RGBA(0x2b, 0x3a, 0x5c, 255)
#define C_TEXT       RGBA(0xf2, 0xf6, 0xfc, 255)
#define C_TEXT_DIM   RGBA(0x9f, 0xb0, 0xc8, 255)
#define C_TEXT_FAINT RGBA(0x6b, 0x7a, 0x96, 255)
#define C_ACCENT     RGBA(0x46, 0xe6, 0xc8, 255)
#define C_ACCENT_HOT RGBA(0x7a, 0xf0, 0xdb, 255)
#define C_ON_ACCENT  RGBA(0x06, 0x1a, 0x1a, 255)
#define C_BLUE       RGBA(0x3b, 0x82, 0xf6, 255)
#define C_GREEN      RGBA(0x4a, 0xde, 0x80, 255)
#define C_AMBER      RGBA(0xfb, 0xbf, 0x24, 255)
#define C_RED        RGBA(0xf8, 0x71, 0x71, 255)

static bool ui_dry;   /* when set, text functions only measure (used to size boxes before drawing them) */
struct ui {
    uint8_t *px; int w, h;
    float mx, my; bool have_mouse;     /* pointer in canvas pixels, origin top-left */
    bool down;                          /* primary button currently held */
    bool released;                      /* primary button was released this frame (a click if it was over the widget it started on) */
    float down_x, down_y;               /* where the press started */
    bool hover_changed;                 /* a widget's hover state changed since last frame (needs a redraw) */
};
static struct ui UI;

/* ---------------------------------------------------------------- fonts */
static FT_Library ft_lib; static FT_Face ft_reg, ft_bold; static bool ft_ready;
static bool ui_load_fonts(const char *dir_hint) {
    if (ft_ready) return true;
    if (FT_Init_FreeType(&ft_lib)) return false;
    char home_fonts[512] = ""; if (getenv("HOME")) snprintf(home_fonts, sizeof home_fonts, "%s/.local/share/sideframe/fonts", getenv("HOME"));
    const char *dirs[] = { dir_hint, getenv("SIDEFRAME_FONT_DIR"), "assets/fonts", home_fonts, "/usr/share/sideframe/fonts", "/usr/share/fonts/noto", "/usr/share/fonts/TTF", NULL };
    for (int i = 0; dirs[i] && !ft_ready; i++) {
        char a[640], b[640]; snprintf(a, sizeof a, "%s/NotoSans-Regular.ttf", dirs[i]); snprintf(b, sizeof b, "%s/NotoSans-Bold.ttf", dirs[i]);
        if (!FT_New_Face(ft_lib, a, 0, &ft_reg)) { if (FT_New_Face(ft_lib, b, 0, &ft_bold)) ft_bold = ft_reg; ft_ready = true; }
    }
    if (!ft_ready) fprintf(stderr, "ui: no font found (looked for NotoSans-Regular.ttf in assets/fonts, /usr/share/fonts/noto)\n");
    return ft_ready;
}

/* decode one UTF-8 code point, advance *s */
static uint32_t utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s; uint32_t c = *p;
    if (c < 0x80) { *s += 1; return c; }
    if ((c >> 5) == 6 && p[1]) { *s += 2; return ((c & 31) << 6) | (p[1] & 63); }
    if ((c >> 4) == 14 && p[1] && p[2]) { *s += 3; return ((c & 15) << 12) | ((p[1] & 63) << 6) | (p[2] & 63); }
    if ((c >> 3) == 30 && p[1] && p[2] && p[3]) { *s += 4; return ((c & 7) << 18) | ((p[1] & 63) << 12) | ((p[2] & 63) << 6) | (p[3] & 63); }
    *s += 1; return '?';
}

/* ---------------------------------------------------------------- primitives */
static inline void blend_px(int x, int y, uint32_t c, float cov) {
    if (x < 0 || y < 0 || x >= UI.w || y >= UI.h || cov <= 0) return;
    float a = ((c & 255) / 255.0f) * (cov > 1 ? 1 : cov);
    uint8_t *p = UI.px + ((size_t)y * UI.w + x) * 4;
    p[0] = (uint8_t)(p[0] + ((int)((c >> 24) & 255) - (int)p[0]) * a + 0.5f);   /* signed: the new colour may be darker than what is underneath */
    p[1] = (uint8_t)(p[1] + ((int)((c >> 16) & 255) - (int)p[1]) * a + 0.5f);
    p[2] = (uint8_t)(p[2] + ((int)((c >> 8) & 255) - (int)p[2]) * a + 0.5f);
    p[3] = 255;
}
static void ui_clear_gradient(uint32_t top, uint32_t bottom) {
    for (int y = 0; y < UI.h; y++) {
        float t = (float)y / (UI.h - 1);
        uint8_t r = (uint8_t)(((top >> 24) & 255) * (1 - t) + ((bottom >> 24) & 255) * t), g = (uint8_t)(((top >> 16) & 255) * (1 - t) + ((bottom >> 16) & 255) * t),
                b = (uint8_t)(((top >> 8) & 255) * (1 - t) + ((bottom >> 8) & 255) * t);
        uint8_t *row = UI.px + (size_t)y * UI.w * 4;
        for (int x = 0; x < UI.w; x++) { row[x * 4] = r; row[x * 4 + 1] = g; row[x * 4 + 2] = b; row[x * 4 + 3] = 255; }
    }
}
static float sd_rrect(float px, float py, float cx, float cy, float hw, float hh, float r) {   /* signed distance to a rounded rectangle */
    float qx = fabsf(px - cx) - (hw - r), qy = fabsf(py - cy) - (hh - r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    return sqrtf(ox * ox + oy * oy) + (qx > qy ? (qx < 0 ? qx : 0) : (qy < 0 ? qy : 0)) - r;
}
static void ui_rrect(float x, float y, float w, float h, float r, uint32_t c) {
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    int x0 = (int)floorf(x) - 1, y0 = (int)floorf(y) - 1, x1 = (int)ceilf(x + w) + 1, y1 = (int)ceilf(y + h) + 1;
    float cx = x + w / 2, cy = y + h / 2;
    for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) {
        float d = sd_rrect(xx + 0.5f, yy + 0.5f, cx, cy, w / 2, h / 2, r);
        blend_px(xx, yy, c, 0.5f - d);
    }
}
static void ui_rrect_line(float x, float y, float w, float h, float r, float t, uint32_t c) {   /* outline of thickness t */
    int x0 = (int)floorf(x) - 2, y0 = (int)floorf(y) - 2, x1 = (int)ceilf(x + w) + 2, y1 = (int)ceilf(y + h) + 2;
    float cx = x + w / 2, cy = y + h / 2;
    for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) {
        float d = sd_rrect(xx + 0.5f, yy + 0.5f, cx, cy, w / 2, h / 2, r);
        float outer = 0.5f - d, inner = 0.5f - (d + t);
        outer = outer < 0 ? 0 : outer > 1 ? 1 : outer; inner = inner < 0 ? 0 : inner > 1 ? 1 : inner;   /* coverage of the outer / inner edge, each clamped */
        float cov = outer - inner;
        if (cov > 0) blend_px(xx, yy, c, cov);
    }
}
static void ui_circle(float cx, float cy, float r, uint32_t c) {
    for (int yy = (int)(cy - r) - 1; yy <= (int)(cy + r) + 1; yy++) for (int xx = (int)(cx - r) - 1; xx <= (int)(cx + r) + 1; xx++) {
        float d = sqrtf((xx + 0.5f - cx) * (xx + 0.5f - cx) + (yy + 0.5f - cy) * (yy + 0.5f - cy)) - r;
        blend_px(xx, yy, c, 0.5f - d);
    }
}

/* ---------------------------------------------------------------- text */
static void ft_size(FT_Face f, int px) { FT_Set_Pixel_Sizes(f, 0, (FT_UInt)px); }
static float ui_text_w(const char *s, int size, bool bold) {
    if (!ft_ready) return 0;
    FT_Face f = bold ? ft_bold : ft_reg; ft_size(f, size);
    float w = 0; while (*s) { uint32_t cp = utf8_next(&s); if (!FT_Load_Char(f, cp, FT_LOAD_DEFAULT)) w += f->glyph->advance.x / 64.0f; }
    return w;
}
/* y = top of the text line box; returns the width drawn */
static float ui_text(float x, float y, const char *s, int size, bool bold, uint32_t c) {
    if (!ft_ready) return 0;
    FT_Face f = bold ? ft_bold : ft_reg; ft_size(f, size);
    float base = y + size * 0.82f, pen = x;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        if (FT_Load_Char(f, cp, FT_LOAD_RENDER)) continue;
        FT_GlyphSlot g = f->glyph;
        if (ui_dry) { pen += g->advance.x / 64.0f; continue; }
        int gx = (int)floorf(pen) + g->bitmap_left, gy = (int)floorf(base) - g->bitmap_top;
        for (unsigned r = 0; r < g->bitmap.rows; r++) for (unsigned q = 0; q < g->bitmap.width; q++) {
            uint8_t v = g->bitmap.buffer[r * g->bitmap.pitch + q];
            if (v) blend_px(gx + (int)q, gy + (int)r, c, v / 255.0f);
        }
        pen += g->advance.x / 64.0f;
    }
    return pen - x;
}
static float ui_text_center(float cx, float y, const char *s, int size, bool bold, uint32_t c) { float w = ui_text_w(s, size, bold); ui_text(cx - w / 2, y, s, size, bold, c); return w; }
/* word-wrapped paragraph; returns the height used (call with ui_dry set to measure only) */
static float ui_paragraph(float x, float y, float maxw, const char *s, int size, bool bold, uint32_t c) {
    char line[512]; line[0] = 0; float yy = y, lh = size * 1.35f;
    char word[160];
    while (*s) {
        int n = 0; while (*s && *s != ' ' && *s != '\n' && n < 159) word[n++] = *s++;
        word[n] = 0; bool nl = (*s == '\n'); if (*s) s++;
        char trial[700]; snprintf(trial, sizeof trial, "%s%s%s", line, line[0] ? " " : "", word);
        if (line[0] && ui_text_w(trial, size, bold) > maxw) { ui_text(x, yy, line, size, bold, c); yy += lh; snprintf(line, sizeof line, "%s", word); }
        else snprintf(line, sizeof line, "%s", trial);
        if (nl) { ui_text(x, yy, line, size, bold, c); yy += lh; line[0] = 0; }
    }
    if (line[0]) { ui_text(x, yy, line, size, bold, c); yy += lh; }
    return yy - y;
}

/* ---------------------------------------------------------------- widgets */
static bool ui_in(float x, float y, float w, float h, float px, float py) { return px >= x && px < x + w && py >= y && py < y + h; }
/* A button. Returns true when it was clicked (press AND release inside it). style: 0 primary, 1 secondary, 2 danger, 3 ghost. */
static bool ui_button(float x, float y, float w, float h, const char *label, int style, bool enabled, int size) {
    bool hot = enabled && UI.have_mouse && ui_in(x, y, w, h, UI.mx, UI.my);
    bool pressed = hot && UI.down && ui_in(x, y, w, h, UI.down_x, UI.down_y);
    uint32_t fill, text;
    switch (style) {
    case 0: fill = !enabled ? RGBA(0x2a, 0x3a, 0x4a, 255) : pressed ? RGBA(0x2f, 0xc4, 0xa8, 255) : hot ? C_ACCENT_HOT : C_ACCENT; text = enabled ? C_ON_ACCENT : C_TEXT_FAINT; break;
    case 2: fill = pressed ? RGBA(0x6a, 0x2a, 0x2a, 255) : hot ? RGBA(0x5a, 0x24, 0x28, 255) : RGBA(0x3a, 0x1f, 0x26, 255); text = C_RED; break;
    case 3: fill = hot ? RGBA(0xff, 0xff, 0xff, 22) : RGBA(0xff, 0xff, 0xff, 0); text = hot ? C_TEXT : C_TEXT_DIM; break;
    default: fill = !enabled ? RGBA(0x1a, 0x25, 0x3d, 255) : pressed ? RGBA(0x30, 0x42, 0x68, 255) : hot ? RGBA(0x2a, 0x3b, 0x5e, 255) : RGBA(0x22, 0x31, 0x52, 255); text = enabled ? C_TEXT : C_TEXT_FAINT; break;
    }
    ui_rrect(x, y, w, h, h * 0.32f, fill);
    if (style == 1 && enabled) ui_rrect_line(x, y, w, h, h * 0.32f, 2, hot ? C_ACCENT : C_LINE);
    ui_text_center(x + w / 2, y + (h - size) / 2 - size * 0.04f, label, size, style == 0 || style == 2, text);
    return enabled && UI.released && ui_in(x, y, w, h, UI.mx, UI.my) && ui_in(x, y, w, h, UI.down_x, UI.down_y);
}
static bool ui_hot(float x, float y, float w, float h) { return UI.have_mouse && ui_in(x, y, w, h, UI.mx, UI.my); }
static bool ui_clicked(float x, float y, float w, float h) { return UI.released && ui_in(x, y, w, h, UI.mx, UI.my) && ui_in(x, y, w, h, UI.down_x, UI.down_y); }

/* ---------------------------------------------------------------- brand */
/* the Sideframe mark: a frame with the Mac screen docked at its side */
static void ui_logo(float x, float y, float s) {
    ui_rrect(x, y, s, s, s * 0.22f, RGBA(0x22, 0x30, 0x4d, 255));
    ui_rrect_line(x + s * 0.17f, y + s * 0.29f, s * 0.66f, s * 0.42f, s * 0.09f, s * 0.045f, RGBA(0xe8, 0xee, 0xf8, 255));
    ui_rrect(x + s * 0.45f, y + s * 0.33f, s * 0.335f, s * 0.335f, s * 0.05f, C_ACCENT);
    ui_rrect(x + s * 0.49f, y + s * 0.38f, s * 0.15f, s * 0.03f, s * 0.015f, RGBA(255, 255, 255, 210));
    ui_rrect(x + s * 0.49f, y + s * 0.43f, s * 0.24f, s * 0.022f, s * 0.011f, RGBA(255, 255, 255, 120));
}
static void ui_spinner(float cx, float cy, float r, double t) {
    for (int i = 0; i < 12; i++) {
        float a = (float)(i * 6.2831853 / 12.0), fade = fmodf((float)(t * 1.2 * 12.0) - i + 24.0f, 12.0f) / 12.0f;
        ui_circle(cx + cosf(a) * r, cy + sinf(a) * r, r * 0.12f, RGBA(0x46, 0xe6, 0xc8, (uint8_t)(255 * (1.0f - fade) * 0.9f + 25)));
    }
}
#endif
