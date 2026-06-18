#include <stdio.h>
#include <string.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <switch.h>

#define SCREEN_W 1280
#define SCREEN_H 720
#define PAD      40

typedef SDL_Color Color;

static const Color C_BG     = {15,  15,  20,  255};
static const Color C_CARD   = {28,  28,  38,  255};
static const Color C_ACCENT = {80,  140, 255, 255};
static const Color C_WHITE  = {230, 230, 240, 255};
static const Color C_GRAY   = {110, 110, 130, 255};
static const Color C_GREEN  = {70,  200, 110, 255};
static const Color C_YELLOW = {255, 195, 55,  255};
static const Color C_RED    = {255, 75,  75,  255};

static void fill(SDL_Renderer *r, int x, int y, int w, int h, Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_Rect rect = {x, y, w, h};
    SDL_RenderFillRect(r, &rect);
}

static void text(SDL_Renderer *r, TTF_Font *f, const char *s, int x, int y, Color c) {
    SDL_Surface *surf = TTF_RenderUTF8_Blended(f, s, c);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
    SDL_Rect dst = {x, y, surf->w, surf->h};
    SDL_FreeSurface(surf);
    if (!tex) return;
    SDL_RenderCopy(r, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
}

static void card(SDL_Renderer *r, TTF_Font *lf, TTF_Font *vf,
                 int x, int y, int w, int h,
                 const char *label, const char *value, Color vc) {
    fill(r, x, y, w, h, C_CARD);
    fill(r, x, y, 4, h, C_ACCENT);
    text(r, lf, label, x + 18, y + 14, C_GRAY);
    text(r, vf, value, x + 18, y + 42, vc);
}

int main(void) {
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK);
    TTF_Init();

    SDL_Window   *win = SDL_CreateWindow("sysinfo", 0, 0, SCREEN_W, SCREEN_H, SDL_WINDOW_FULLSCREEN);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    plInitialize(PlServiceType_User);
    PlFontData fd;
    plGetSharedFontByType(&fd, PlSharedFontType_Standard);

    TTF_Font *flabel = TTF_OpenFontRW(SDL_RWFromMem(fd.address, fd.size), 1, 20);
    TTF_Font *fvalue = TTF_OpenFontRW(SDL_RWFromMem(fd.address, fd.size), 1, 30);
    TTF_Font *ftitle = TTF_OpenFontRW(SDL_RWFromMem(fd.address, fd.size), 1, 38);

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    psmInitialize();
    clkrstInitialize();
    tsInitialize();
    setsysInitialize();

    ClkrstSession cpuSes, gpuSes, emcSes;
    clkrstOpenSession(&cpuSes, PcvModuleId_CpuBus, 3);
    clkrstOpenSession(&gpuSes, PcvModuleId_GPU, 3);
    clkrstOpenSession(&emcSes, PcvModuleId_EMC, 3);

    SetSysFirmwareVersion fw;
    setsysGetFirmwareVersion(&fw);

    char fw_str[64];
    snprintf(fw_str, sizeof(fw_str), "FW %s", fw.display_version);

    int cw = (SCREEN_W - PAD * 2 - 20 * 2) / 3;
    int ch = 100;
    int row1 = 100, row2 = 220;
    int col0 = PAD, col1 = PAD + cw + 20, col2 = PAD + (cw + 20) * 2;

    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;

        u32 bat = 0;
        psmGetBatteryChargePercentage(&bat);
        PsmChargerType charger = PsmChargerType_Unconnected;
        psmGetChargerType(&charger);

        u32 cpuHz = 0, gpuHz = 0, emcHz = 0;
        clkrstGetClockRate(&cpuSes, &cpuHz);
        clkrstGetClockRate(&gpuSes, &gpuHz);
        clkrstGetClockRate(&emcSes, &emcHz);

        s32 tempMC = 0;
        tsGetTemperatureMilliC(TsLocation_Internal, &tempMC);

        u64 totalMem = 0, usedMem = 0;
        svcGetInfo(&totalMem, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&usedMem,  InfoType_UsedMemorySize,  CUR_PROCESS_HANDLE, 0);

        char sbat[32], scpu[32], sgpu[32], semc[32], stemp[32], sram[48];
        snprintf(sbat,  sizeof(sbat),  "%u%%%s", bat,
                 charger != PsmChargerType_Unconnected ? "  CHARGING" : "");
        snprintf(scpu,  sizeof(scpu),  "%u MHz", cpuHz / 1000000);
        snprintf(sgpu,  sizeof(sgpu),  "%u MHz", gpuHz / 1000000);
        snprintf(semc,  sizeof(semc),  "%u MHz", emcHz / 1000000);
        snprintf(stemp, sizeof(stemp), "%.1f C",  tempMC / 1000.0f);
        snprintf(sram,  sizeof(sram),  "%llu / %llu MB",
                 (unsigned long long)(usedMem  / 1048576),
                 (unsigned long long)(totalMem / 1048576));

        Color cbat  = bat > 50 ? C_GREEN : (bat > 20 ? C_YELLOW : C_RED);
        Color ctemp = tempMC < 50000 ? C_GREEN : (tempMC < 70000 ? C_YELLOW : C_RED);

        SDL_SetRenderDrawColor(ren, C_BG.r, C_BG.g, C_BG.b, 255);
        SDL_RenderClear(ren);

        // Title bar
        fill(ren, 0, 0, SCREEN_W, 72, C_CARD);
        fill(ren, 0, 68, SCREEN_W, 4, C_ACCENT);
        text(ren, ftitle, "SysInfo", PAD, 16, C_WHITE);
        text(ren, flabel, fw_str, SCREEN_W - PAD - 140, 26, C_GRAY);

        // Row 1: battery, temp, ram
        card(ren, flabel, fvalue, col0, row1, cw, ch, "Battery",     sbat,  cbat);
        card(ren, flabel, fvalue, col1, row1, cw, ch, "Temperature", stemp, ctemp);
        card(ren, flabel, fvalue, col2, row1, cw, ch, "Memory",      sram,  C_WHITE);

        // Row 2: cpu, gpu, emc
        card(ren, flabel, fvalue, col0, row2, cw, ch, "CPU Clock", scpu, C_ACCENT);
        card(ren, flabel, fvalue, col1, row2, cw, ch, "GPU Clock", sgpu, C_ACCENT);
        card(ren, flabel, fvalue, col2, row2, cw, ch, "EMC Clock", semc, C_ACCENT);

        // Footer
        fill(ren, 0, SCREEN_H - 48, SCREEN_W, 48, C_CARD);
        text(ren, flabel, "[+] Exit", PAD, SCREEN_H - 34, C_GRAY);

        SDL_RenderPresent(ren);
        svcSleepThread(33333333ULL);
    }

    clkrstCloseSession(&cpuSes);
    clkrstCloseSession(&gpuSes);
    clkrstCloseSession(&emcSes);
    clkrstExit();
    psmExit();
    tsExit();
    setsysExit();
    plExit();

    TTF_CloseFont(flabel);
    TTF_CloseFont(fvalue);
    TTF_CloseFont(ftitle);
    TTF_Quit();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();

    return 0;
}
