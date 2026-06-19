/*
 * page_system.c - "System" tab: identity and OS-level info.
 *
 * Most of this comes from the static fields read once in services_load_static
 * (model, firmware, language, region), plus a couple of live ones (time, mode).
 */
#include <stdio.h>
#include <switch.h>
#include "render.h"
#include "data.h"
#include "pages.h"

void page_system(int cy, const SysData *d) {
    int r = 0;          // current row index (ROW/BAR macros advance this)
    char buf[256];      // scratch for formatted values

    // Map the product model enum to a friendly hardware name. These cover the
    // retail/dev variants: Nx=original, Iowa=Lite, Hoag=revised V2,
    // Calcio/Aula=OLED.
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

    // Region enum -> readable string.
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

    snprintf(buf, sizeof(buf), "%.0f%%", d->bright * 100.f);   // 0..1 -> percent
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
