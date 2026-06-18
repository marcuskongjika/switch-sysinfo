#include <stdio.h>
#include <switch.h>
#include "render.h"
#include "data.h"
#include "pages.h"

void page_power(int cy, const SysData *d) {
    int r = 0;
    char buf[256];

    snprintf(buf, sizeof(buf), "%u%%", d->batPct);
    Col bc = d->batPct > 50 ? GREEN : (d->batPct > 20 ? YELLOW : RED);
    ROW("Battery", buf, bc);
    BAR(d->batPct / 100.f, bc);

    const char *chrg; Col cc;
    switch (d->charger) {
        case PsmChargerType_Unconnected: chrg = "Not Charging";          cc = GRAY;   break;
        case PsmChargerType_EnoughPower: chrg = "Charging (AC Adapter)"; cc = GREEN;  break;
        case PsmChargerType_LowPower:    chrg = "Charging (Low Power)";  cc = YELLOW; break;
        default:                         chrg = "Charging";              cc = GREEN;  break;
    }
    ROW("Charger", chrg, cc);

    snprintf(buf, sizeof(buf), "%.0f%%", d->bright * 100.f);
    ROW("Brightness", buf, YELLOW);
    BAR(d->bright, YELLOW);

    ROW("Op Mode",
        d->opMode == AppletOperationMode_Handheld ? "Handheld" : "Docked (TV)",
        WHITE);

    const char *perf = "Unknown";
    switch (d->perfMode) {
        case ApmPerformanceMode_Normal: perf = "Normal  (1020 MHz CPU / 307 MHz GPU)"; break;
        case ApmPerformanceMode_Boost:  perf = "Boost   (1785 MHz CPU / 768 MHz GPU)"; break;
        default: break;
    }
    ROW("Perf Profile", perf, ACCENT);
}
