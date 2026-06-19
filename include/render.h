/*
 * render.h - drawing toolkit + shared UI constants.
 *
 * Everything visual goes through here. The actual SDL renderer and the loaded
 * fonts live as globals (defined in render.c) so page code can draw without
 * passing them around. Layout is a fixed grid: a title bar on top, a tab bar
 * under it, a list of "rows" in the middle (the content area), and a footer.
 */
#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

// --- Layout constants (all in pixels) ---------------------------------------
#define SCREEN_W  1280              // Switch docked output width
#define SCREEN_H  720               // ...and height
#define PAD       28                // left/right margin for content
#define ROW_H     36                // height of one label/value row
#define TITLE_H   68                // title bar height
#define TABS_H    38                // tab bar height
#define CONTENT_Y (TITLE_H + TABS_H + 2)   // Y where the content area begins
#define FOOTER_Y  (SCREEN_H - 44)          // Y where the footer begins
#define LABEL_X   (PAD + 6)                 // X for a row's label text
#define VALUE_X   (PAD + 340)               // X for a row's value text / bar
#define BAR_W     (SCREEN_W - VALUE_X - PAD - 10)  // width of a progress bar

// A color is just an SDL_Color (r,g,b,a). Aliased for brevity.
typedef SDL_Color Col;

// Theme palette (defined in render.c).
extern const Col BG, TITLEC, TABC, SEPC;                 // backgrounds/separators
extern const Col ACCENT, WHITE, GRAY;                    // text/accent
extern const Col GREEN, YELLOW, RED, CYAN, PURPLE, ORANGE; // status colors

// Global renderer + fonts (defined in render.c, set up in main()).
extern SDL_Renderer *R;
extern TTF_Font *fSm, *fLbl, *fVal, *fTitle, *fTab;

// --- Primitive drawing helpers ----------------------------------------------
void fill(int x, int y, int w, int h, Col c);          // filled rectangle
void hline(int y, Col c);                              // full-width horizontal line
void drawText(TTF_Font *f, const char *s, int x, int y, Col c);
int  textW(TTF_Font *f, const char *s);                // pixel width of a string
void drawRow(int y, const char *lbl, const char *val, Col vc); // label + value row
void drawBar(int y, float pct, Col c);                 // 0..1 progress bar
void drawSwatch(int x, int y, unsigned int rgb);       // small color square (Joy-Con colors)

// --- Composite chrome (title/tabs/footer) -----------------------------------
void render_titlebar(const char *fw_ver);
void render_tabbar(int curTab, const char **names, int count);
void render_footer(void);
void render_tab_dots(int curTab, int count);           // the little tab indicator dots

// Convenience macros used by page code. They assume two locals are in scope:
//   int cy;  // the content-area top Y for this page
//   int r;   // the current row index, starting at 0
// ROW draws "label   value" on row r and advances r. BAR draws a progress bar
// on row r and advances r. Using these keeps page code short and uniform.
#define ROW(l,v,c) drawRow(cy+(r++)*ROW_H, l, v, c)
#define BAR(p,c)   drawBar(cy+(r++)*ROW_H, p, c)
