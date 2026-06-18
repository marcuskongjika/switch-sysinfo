#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

// Layout
#define SCREEN_W  1280
#define SCREEN_H  720
#define PAD       28
#define ROW_H     36
#define TITLE_H   68
#define TABS_H    38
#define CONTENT_Y (TITLE_H + TABS_H + 2)
#define FOOTER_Y  (SCREEN_H - 44)
#define LABEL_X   (PAD + 6)
#define VALUE_X   (PAD + 340)
#define BAR_W     (SCREEN_W - VALUE_X - PAD - 10)

typedef SDL_Color Col;

extern const Col BG, TITLEC, TABC, SEPC;
extern const Col ACCENT, WHITE, GRAY;
extern const Col GREEN, YELLOW, RED, CYAN, PURPLE, ORANGE;

extern SDL_Renderer *R;
extern TTF_Font *fSm, *fLbl, *fVal, *fTitle, *fTab;

void fill(int x, int y, int w, int h, Col c);
void hline(int y, Col c);
void drawText(TTF_Font *f, const char *s, int x, int y, Col c);
int  textW(TTF_Font *f, const char *s);
void drawRow(int y, const char *lbl, const char *val, Col vc);
void drawBar(int y, float pct, Col c);
void drawSwatch(int x, int y, unsigned int rgb);

void render_titlebar(const char *fw_ver);
void render_tabbar(int curTab, const char **names, int count);
void render_footer(void);
void render_tab_dots(int curTab, int count);

// Page-local row/bar helpers — require local `int cy` and `int r` in scope
#define ROW(l,v,c) drawRow(cy+(r++)*ROW_H, l, v, c)
#define BAR(p,c)   drawBar(cy+(r++)*ROW_H, p, c)
