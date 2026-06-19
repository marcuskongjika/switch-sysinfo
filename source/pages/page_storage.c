/*
 * page_storage.c - "Storage" tab: SD card usage and RAM usage.
 *
 * The SD strings/fraction were computed in services_gather_data via statvfs.
 */
#include <stdio.h>
#include <switch.h>
#include "render.h"
#include "data.h"
#include "pages.h"

void page_storage(int cy, const SysData *d) {
    int r = 0;
    char buf[256];

    snprintf(buf, sizeof(buf), "%s used  /  %s free  /  %s total",
             d->sdUsed, d->sdFree, d->sdTot);
    ROW("SD Card", buf, WHITE);
    BAR(d->sdPct, ACCENT);

    snprintf(buf, sizeof(buf), "%.1f%%", d->sdPct * 100.f);
    ROW("SD Used", buf, ACCENT);

    // Process RAM usage, mirrored from the Hardware tab for convenience.
    float mp = d->totalMem > 0 ? (float)d->usedMem / d->totalMem : 0.f;
    snprintf(buf, sizeof(buf), "%llu MB / %llu MB",
        (unsigned long long)(d->usedMem  / 1048576),
        (unsigned long long)(d->totalMem / 1048576));
    ROW("RAM", buf, CYAN);
    BAR(mp, CYAN);
}
