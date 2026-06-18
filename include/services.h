#pragma once
#include <stdbool.h>
#include <switch.h>
#include "data.h"

typedef struct {
    bool psmOk, clkOk, tsOk, sysOk;
    bool apmOk, lblOk, nifmOk, timeOk, setOk;
    ClkrstSession          cpuS, gpuS, emcS;
    HidSixAxisSensorHandle gyroH[2];
    int                    gyroN;
} Services;

void services_init(Services *svc);
void services_load_static(Services *svc, SysData *data);
void services_gather_data(Services *svc, SysData *data);
void services_exit(Services *svc);
