#include <stdio.h>
#include <switch.h>
#include "render.h"
#include "data.h"
#include "pages.h"

void page_hardware(int cy, const SysData *d) {
    int r = 0;
    char buf[256];

    snprintf(buf, sizeof(buf), "%u MHz", d->cpuHz / 1000000);
    ROW("CPU Clock", buf, ACCENT);
    BAR((float)d->cpuHz / 1785600000.f, ACCENT);

    snprintf(buf, sizeof(buf), "%u MHz", d->gpuHz / 1000000);
    ROW("GPU Clock", buf, PURPLE);
    BAR((float)d->gpuHz / 921600000.f, PURPLE);

    snprintf(buf, sizeof(buf), "%u MHz", d->emcHz / 1000000);
    ROW("EMC Clock", buf, CYAN);
    BAR((float)d->emcHz / 1600000000.f, CYAN);

    float mp = d->totalMem > 0 ? (float)d->usedMem / d->totalMem : 0.f;
    snprintf(buf, sizeof(buf), "%llu / %llu MB  (%.0f%%)",
        (unsigned long long)(d->usedMem  / 1048576),
        (unsigned long long)(d->totalMem / 1048576),
        mp * 100.f);
    ROW("RAM", buf, WHITE);
    BAR(mp, GREEN);

    snprintf(buf, sizeof(buf), "%.1f C  (SoC)", d->tempMC / 1000.f);
    Col tc = d->tempMC < 50000 ? GREEN : (d->tempMC < 70000 ? YELLOW : RED);
    ROW("Temp (Internal)", buf, tc);

    snprintf(buf, sizeof(buf), "%.1f C  (Board)", d->temp2MC / 1000.f);
    Col tc2 = d->temp2MC < 50000 ? GREEN : (d->temp2MC < 70000 ? YELLOW : RED);
    ROW("Temp (External)", buf, tc2);

    ROW("Op Mode",
        d->opMode == AppletOperationMode_Handheld ? "Handheld" : "Docked",
        WHITE);
}
