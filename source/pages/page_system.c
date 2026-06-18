#include <stdio.h>
#include <switch.h>
#include "render.h"
#include "data.h"
#include "pages.h"

void page_system(int cy, const SysData *d) {
    int r = 0;
    char buf[256];

    const char *mdl = "Unknown";
    switch (d->model) {
        case SetSysProductModel_Nx:     mdl = "Nintendo Switch (HAC-001)"; break;
        case SetSysProductModel_Copper:  mdl = "Nintendo Switch (Dev Unit)"; break;
        case SetSysProductModel_Iowa:    mdl = "Nintendo Switch Lite (HDH-001)"; break;
        case SetSysProductModel_Hoag:    mdl = "Nintendo Switch (HAC-001-01)"; break;
        case SetSysProductModel_Calcio:  mdl = "Nintendo Switch OLED (HEG-001)"; break;
        case SetSysProductModel_Aula:    mdl = "Nintendo Switch OLED (Revised)"; break;
        default: break;
    }
    ROW("Model",      mdl,                   CYAN);
    ROW("Nickname",   d->nick.nickname,       WHITE);
    ROW("Firmware",   d->fw.display_version,  WHITE);
    ROW("Language",   d->langStr,             WHITE);

    const char *reg = "Unknown";
    switch (d->region) {
        case SetRegion_JPN: reg = "Japan"; break;
        case SetRegion_USA: reg = "Americas"; break;
        case SetRegion_EUR: reg = "Europe"; break;
        case SetRegion_AUS: reg = "Australia / NZ"; break;
        case SetRegion_HTK: reg = "HK / TW / Korea"; break;
        case SetRegion_CHN: reg = "China"; break;
        default: break;
    }
    ROW("Region",     reg,                   WHITE);
    ROW("Date / Time", d->timeStr,           GREEN);

    snprintf(buf, sizeof(buf), "%.0f%%", d->bright * 100.f);
    ROW("Brightness", buf, YELLOW);

    ROW("Mode", d->opMode == AppletOperationMode_Handheld ? "Handheld" : "Docked (TV)", WHITE);

    const char *perf = "Unknown";
    switch (d->perfMode) {
        case ApmPerformanceMode_Normal: perf = "Normal"; break;
        case ApmPerformanceMode_Boost:  perf = "Boost (Docked)"; break;
        default: break;
    }
    ROW("Perf Mode", perf, ACCENT);
}
