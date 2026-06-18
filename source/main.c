#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <switch.h>
#include "render.h"
#include "data.h"
#include "services.h"
#include "pages.h"
#include "mtp.h"

static const char *TAB_NAMES[T_COUNT] = {
    "System", "Hardware", "Power", "Storage",
    "Network", "Controllers", "Motion", "USB"
};

typedef void (*PageFn)(int, const SysData *);
static const PageFn PAGE_FNS[T_COUNT] = {
    page_system, page_hardware, page_power, page_storage,
    page_network, page_controllers, page_motion, page_usb
};

int main(void) {
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK);
    TTF_Init();

    SDL_Window *win = SDL_CreateWindow(
        "sysinfo", 0, 0, SCREEN_W, SCREEN_H, SDL_WINDOW_FULLSCREEN);
    R = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    plInitialize(PlServiceType_User);
    PlFontData fd;
    plGetSharedFontByType(&fd, PlSharedFontType_Standard);
    fSm    = TTF_OpenFontRW(SDL_RWFromMem(fd.address, fd.size), 1, 15);
    fLbl   = TTF_OpenFontRW(SDL_RWFromMem(fd.address, fd.size), 1, 18);
    fVal   = TTF_OpenFontRW(SDL_RWFromMem(fd.address, fd.size), 1, 21);
    fTitle = TTF_OpenFontRW(SDL_RWFromMem(fd.address, fd.size), 1, 30);
    fTab   = TTF_OpenFontRW(SDL_RWFromMem(fd.address, fd.size), 1, 16);

    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeAny(&pad);

    Services svc  = {0};
    SysData  data = {0};
    services_init(&svc);
    services_load_static(&svc, &data);
    mtp_start();

    int curTab = 0;

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kd = padGetButtonsDown(&pad);

        if (kd & HidNpadButton_Plus)                      break;
        if (kd & (HidNpadButton_R | HidNpadButton_Right)) curTab = (curTab + 1) % T_COUNT;
        if (kd & (HidNpadButton_L | HidNpadButton_Left))  curTab = (curTab - 1 + T_COUNT) % T_COUNT;

        services_gather_data(&svc, &data);

        SDL_SetRenderDrawColor(R, BG.r, BG.g, BG.b, 255);
        SDL_RenderClear(R);

        render_titlebar(data.fw.display_version);
        render_tabbar(curTab, TAB_NAMES, T_COUNT);
        PAGE_FNS[curTab](CONTENT_Y + 2, &data);
        render_footer();
        render_tab_dots(curTab, T_COUNT);

        SDL_RenderPresent(R);
        svcSleepThread(33333333ULL);
    }

    mtp_stop();
    services_exit(&svc);

    TTF_CloseFont(fSm); TTF_CloseFont(fLbl); TTF_CloseFont(fVal);
    TTF_CloseFont(fTitle); TTF_CloseFont(fTab);
    TTF_Quit();
    plExit();
    SDL_DestroyRenderer(R);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
