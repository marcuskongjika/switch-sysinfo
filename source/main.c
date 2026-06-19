/*
 * main.c - program entry point and the main render/input loop.
 *
 * Big picture: SysInfo is a single-window SDL2 app running fullscreen at
 * 1280x720 on the Switch. Every frame we:
 *     1. read controller input (to switch tabs / quit),
 *     2. refresh all the system data (battery, clocks, network, ...),
 *     3. draw the title bar, the tab bar, the current tab's page, and the footer,
 *     4. present the frame and sleep ~33ms (≈30 FPS).
 *
 * The MTP USB file-transfer server runs on its OWN background thread (see
 * source/usb/mtp.c); main() just starts and stops it.
 */
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <switch.h>
#include "render.h"     // drawing helpers + global renderer/font handles
#include "data.h"       // SysData: the snapshot of everything we display
#include "services.h"   // libnx service init + per-frame data gathering
#include "pages.h"      // the Tab enum + one draw function per tab
#include "mtp.h"        // USB MTP responder (start/stop/status)

// Tab labels shown in the tab bar. Index order MUST match the Tab enum in
// pages.h and the PAGE_FNS array just below.
static const char *TAB_NAMES[T_COUNT] = {
    "System", "Hardware", "Power", "Storage",
    "Network", "Controllers", "Motion", "USB"
};

// A page draw function: given the content's top Y and the data snapshot, it
// renders one tab's rows. We store one per tab and call the active one by index.
typedef void (*PageFn)(int, const SysData *);
static const PageFn PAGE_FNS[T_COUNT] = {
    page_system, page_hardware, page_power, page_storage,
    page_network, page_controllers, page_motion, page_usb
};

int main(void) {
    // --- SDL + font setup ---------------------------------------------------
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK);
    TTF_Init();

    // One fullscreen window. `R` is the global renderer (declared in render.h)
    // that every drawing helper writes to. PRESENTVSYNC caps us to the display
    // refresh so we don't spin needlessly.
    SDL_Window *win = SDL_CreateWindow(
        "sysinfo", 0, 0, SCREEN_W, SCREEN_H, SDL_WINDOW_FULLSCREEN);
    R = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    // Load the console's built-in shared font from RAM (so we don't have to
    // bundle a .ttf). We open it at several point sizes for different UI roles.
    plInitialize(PlServiceType_User);
    PlFontData fd;
    plGetSharedFontByType(&fd, PlSharedFontType_Standard);
    fSm    = TTF_OpenFontRW(SDL_RWFromMem(fd.address, fd.size), 1, 15); // small/footer
    fLbl   = TTF_OpenFontRW(SDL_RWFromMem(fd.address, fd.size), 1, 18); // row labels
    fVal   = TTF_OpenFontRW(SDL_RWFromMem(fd.address, fd.size), 1, 21); // row values
    fTitle = TTF_OpenFontRW(SDL_RWFromMem(fd.address, fd.size), 1, 30); // title bar
    fTab   = TTF_OpenFontRW(SDL_RWFromMem(fd.address, fd.size), 1, 16); // tab labels

    // --- input setup --------------------------------------------------------
    // Accept up to 8 controllers in the standard style set, then grab a handle
    // that reads whichever pad is active (handheld or any attached controller).
    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeAny(&pad);

    // --- services + background MTP server -----------------------------------
    Services svc  = {0};   // owns the libnx service sessions
    SysData  data = {0};   // the data snapshot drawn each frame
    services_init(&svc);              // open psm/clk/ts/setsys/nifm/... sessions
    services_load_static(&svc, &data); // read once: model, firmware, region, ...
    mtp_start();                       // spin up USB MTP responder thread

    int curTab = 0;   // which tab is currently shown

    // --- main loop ----------------------------------------------------------
    // appletMainLoop() returns false when the OS asks us to quit (e.g. user
    // holds Home and closes the app), so this also handles forced exit.
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kd = padGetButtonsDown(&pad);   // buttons pressed THIS frame

        if (kd & HidNpadButton_Plus) break;  // + quits

        // L / R (or D-pad left/right) cycle tabs, wrapping around. The
        // (+ T_COUNT) before % keeps the result non-negative when going left.
        if (kd & (HidNpadButton_R | HidNpadButton_Right)) curTab = (curTab + 1) % T_COUNT;
        if (kd & (HidNpadButton_L | HidNpadButton_Left))  curTab = (curTab - 1 + T_COUNT) % T_COUNT;

        // Refresh all live values for this frame.
        services_gather_data(&svc, &data);

        // Clear to the background color.
        SDL_SetRenderDrawColor(R, BG.r, BG.g, BG.b, 255);
        SDL_RenderClear(R);

        // Draw fixed chrome, then the active page in the middle, then footer.
        render_titlebar(data.fw.display_version);
        render_tabbar(curTab, TAB_NAMES, T_COUNT);
        PAGE_FNS[curTab](CONTENT_Y + 2, &data);   // dispatch to the active tab
        render_footer();
        render_tab_dots(curTab, T_COUNT);

        SDL_RenderPresent(R);
        svcSleepThread(33333333ULL);   // ~33.3ms -> ~30 FPS
    }

    // --- teardown (reverse order of setup) ----------------------------------
    mtp_stop();          // stop USB thread and close usbDs
    services_exit(&svc); // close all service sessions

    TTF_CloseFont(fSm); TTF_CloseFont(fLbl); TTF_CloseFont(fVal);
    TTF_CloseFont(fTitle); TTF_CloseFont(fTab);
    TTF_Quit();
    plExit();
    SDL_DestroyRenderer(R);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
