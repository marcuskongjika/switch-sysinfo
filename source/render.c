/*
 * render.c - implementation of the drawing toolkit declared in render.h.
 *
 * Nothing here knows about "system info"; it only knows rectangles, text, rows,
 * and bars. Page code (source/pages/*) composes these into actual screens.
 */
#include <stdio.h>
#include "render.h"

// The single renderer + fonts. main() creates these at startup; everything
// here draws into `R`.
SDL_Renderer *R     = NULL;
TTF_Font     *fSm   = NULL;
TTF_Font     *fLbl  = NULL;
TTF_Font     *fVal  = NULL;
TTF_Font     *fTitle = NULL;
TTF_Font     *fTab  = NULL;

// Theme palette. {r, g, b, a}.
const Col BG     = {12,  12,  18,  255};  // app background (near-black)
const Col TITLEC = {20,  20,  30,  255};  // title/footer bar fill
const Col TABC   = {16,  16,  24,  255};  // tab bar fill
const Col SEPC   = {38,  38,  54,  255};  // row separators / empty bar track
const Col ACCENT = {80,  140, 255, 255};  // blue accent (active tab, highlights)
const Col WHITE  = {228, 228, 238, 255};  // primary text
const Col GRAY   = {108, 108, 128, 255};  // secondary/label text
const Col GREEN  = {68,  198, 108, 255};  // "good" status
const Col YELLOW = {252, 192, 52,  255};  // "warning" status
const Col RED    = {252, 72,  72,  255};  // "bad" status
const Col CYAN   = {58,  208, 208, 255};
const Col PURPLE = {178, 98,  252, 255};
const Col ORANGE = {252, 158, 48,  255};

// Filled rectangle in color c.
void fill(int x, int y, int w, int h, Col c) {
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, 255);
    SDL_Rect r = {x, y, w, h};
    SDL_RenderFillRect(R, &r);
}

// Horizontal line spanning the whole screen width at height y.
void hline(int y, Col c) {
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, 255);
    SDL_RenderDrawLine(R, 0, y, SCREEN_W, y);
}

// Draw a UTF-8 string. We render to a surface, upload it as a texture, blit it,
// then free both. Not the fastest approach (a glyph cache would be faster) but
// fine for a ~30 FPS info screen.
void drawText(TTF_Font *f, const char *s, int x, int y, Col c) {
    if (!s || !s[0]) return;                          // skip empty strings
    SDL_Surface *sf = TTF_RenderUTF8_Blended(f, s, c);
    if (!sf) return;
    SDL_Texture *t = SDL_CreateTextureFromSurface(R, sf);
    SDL_Rect d = {x, y, sf->w, sf->h};
    SDL_FreeSurface(sf);
    if (!t) return;
    SDL_RenderCopy(R, t, NULL, &d);
    SDL_DestroyTexture(t);
}

// Measure the rendered pixel width of a string (used to right-align / center).
int textW(TTF_Font *f, const char *s) {
    int w = 0;
    TTF_SizeUTF8(f, s, &w, NULL);
    return w;
}

// One content row: a gray label on the left, a colored value on the right,
// with a thin separator line along the bottom. y is the row's top.
void drawRow(int y, const char *lbl, const char *val, Col vc) {
    SDL_SetRenderDrawColor(R, SEPC.r, SEPC.g, SEPC.b, 255);
    SDL_RenderDrawLine(R, PAD, y + ROW_H - 1, SCREEN_W - PAD, y + ROW_H - 1);
    drawText(fLbl, lbl, LABEL_X, y + (ROW_H - 19) / 2, GRAY);   // vertically centered-ish
    drawText(fVal, val, VALUE_X, y + (ROW_H - 22) / 2, vc);
}

// A horizontal progress bar at row position y. pct is clamped to [0,1].
// Drawn as: filled track (SEPC) -> filled portion (c) -> outline.
void drawBar(int y, float pct, Col c) {
    int by = y + (ROW_H - 14) / 2;       // bar is 14px tall, vertically centered
    if (pct < 0.f) pct = 0.f;
    if (pct > 1.f) pct = 1.f;
    fill(VALUE_X, by, BAR_W, 14, SEPC);  // empty track
    int fw = (int)(pct * BAR_W);
    if (fw > 0) fill(VALUE_X, by, fw, 14, c);   // filled portion
    SDL_SetRenderDrawColor(R, 55, 55, 75, 255); // subtle outline
    SDL_Rect o = {VALUE_X, by, BAR_W, 14};
    SDL_RenderDrawRect(R, &o);
}

// A 24x24 color square with a border, given a packed 0xRRGGBB value. Used on
// the Controllers tab to show Joy-Con body colors.
void drawSwatch(int x, int y, unsigned int rgb) {
    Col c = {(rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255};
    fill(x, y + 5, 24, 24, c);
    SDL_SetRenderDrawColor(R, 80, 80, 100, 255);
    SDL_Rect r = {x, y + 5, 24, 24};
    SDL_RenderDrawRect(R, &r);
}

// Title bar: app name on the left, firmware version on the right, an accent
// stripe down the left edge.
void render_titlebar(const char *fw_ver) {
    char buf[64];
    fill(0, 0, SCREEN_W, TITLE_H, TITLEC);
    fill(0, 0, 4, TITLE_H, ACCENT);                 // accent stripe
    drawText(fTitle, "SysInfo", PAD + 8, 16, WHITE);
    snprintf(buf, sizeof(buf), "FW %s", fw_ver);
    drawText(fSm, buf, SCREEN_W - PAD - textW(fSm, buf) - 4, 26, GRAY); // right-aligned
}

// Tab bar: `count` equal-width tabs. The active one gets an accent fill and
// white text; the rest are gray. A two-tone underline separates it from content.
void render_tabbar(int curTab, const char **names, int count) {
    int tw = SCREEN_W / count;          // per-tab width
    fill(0, TITLE_H, SCREEN_W, TABS_H, TABC);
    for (int i = 0; i < count; i++) {
        int tx = i * tw;
        if (i == curTab) fill(tx, TITLE_H, tw, TABS_H, ACCENT);
        Col tc = (i == curTab) ? WHITE : GRAY;
        int sw = textW(fTab, names[i]);
        drawText(fTab, names[i], tx + (tw - sw) / 2, TITLE_H + (TABS_H - 16) / 2, tc); // centered
    }
    hline(TITLE_H + TABS_H, ACCENT);
    hline(TITLE_H + TABS_H + 1, SEPC);
}

// Footer: a bar with the control hints.
void render_footer(void) {
    fill(0, FOOTER_Y, SCREEN_W, SCREEN_H - FOOTER_Y, TITLEC);
    hline(FOOTER_Y, ACCENT);
    drawText(fSm, "[L][R] or [<][>] Switch Tab     [+] Exit",
             PAD, FOOTER_Y + (44 - 15) / 2, GRAY);
}

// Row of little squares centered in the footer, one per tab; the active tab's
// dot is the accent color. A quick "you are here" indicator.
void render_tab_dots(int curTab, int count) {
    int dotX = SCREEN_W / 2 - count * 12;   // center the row of dots (24px each)
    for (int i = 0; i < count; i++) {
        Col dc = (i == curTab) ? ACCENT : SEPC;
        fill(dotX + i * 24, FOOTER_Y + 15, 12, 12, dc);
    }
}
