/*
 * services.h - ownership of libnx service sessions + the data-gathering API.
 *
 * The Services struct holds every service handle we open and tracks which ones
 * succeeded (so we can skip the ones that failed). The four functions below are
 * the whole lifecycle: init -> load static info -> gather (every frame) -> exit.
 */
#pragma once
#include <stdbool.h>
#include <switch.h>
#include "data.h"

typedef struct {
    // One "Ok" flag per service; false means init failed and we must not use it.
    bool psmOk, clkOk, tsOk, sysOk;       // power, clocks, temp sensor, set:sys
    bool apmOk, lblOk, nifmOk, timeOk, setOk; // perf mode, backlight, net, time, settings

    // clkrst needs an open session per clock domain to read its rate.
    ClkrstSession          cpuS, gpuS, emcS;

    // Motion sensor handles (one for handheld, two for a dual Joy-Con pair) and
    // how many we actually started.
    HidSixAxisSensorHandle gyroH[2];
    int                    gyroN;
} Services;

void services_init(Services *svc);                       // open all sessions
void services_load_static(Services *svc, SysData *data); // read once-only info
void services_gather_data(Services *svc, SysData *data); // refresh live values (per frame)
void services_exit(Services *svc);                       // close everything
