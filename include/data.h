/*
 * data.h - SysData, the single snapshot of everything the UI displays.
 *
 * services.c fills this in: the "static" fields once at startup, the rest every
 * frame via services_gather_data(). Page code only ever READS a const SysData*,
 * so drawing never has to touch libnx services directly.
 */
#pragma once
#include <stdbool.h>
#include <switch.h>

typedef struct {
    // --- which optional services are available (so pages can show "N/A") ---
    bool nifmOk;   // network info manager came up
    bool gyroOk;   // a six-axis (motion) sensor handle was obtained

    // --- battery / charger ---
    u32 batPct;                 // battery charge 0..100
    PsmChargerType charger;     // none / enough-power / low-power

    // --- clocks, in Hz ---
    u32 cpuHz, gpuHz, emcHz;    // CPU, GPU, and memory (EMC) clock rates

    // --- temperatures, in milli-degrees Celsius (so 52000 == 52.0 C) ---
    s32 tempMC, temp2MC;        // internal (SoC) and external (board)

    // --- memory, in bytes (this process's view) ---
    u64 totalMem, usedMem;

    // --- performance / mode ---
    ApmPerformanceMode  perfMode;  // normal vs boost
    AppletOperationMode opMode;    // handheld vs docked

    // --- display ---
    float bright;               // backlight brightness 0..1

    // --- formatted clock string, e.g. "2026-06-18  14:30:00" ---
    char timeStr[48];

    // --- SD card storage (pre-formatted strings + a 0..1 used fraction) ---
    char sdTot[32], sdFree[32], sdUsed[32];
    float sdPct;

    // --- network ---
    NifmInternetConnectionType connType;     // wifi / ethernet / none
    u32  wifiSig;                            // wifi signal strength 0..3
    bool netConnected;                       // truly connected to the internet
    char ipStr[32];                          // current IPv4 as text

    // --- motion (only the first sensor is sampled for display) ---
    HidSixAxisSensorState gyroState;         // accel + gyro + computed angle

    // --- static info: read ONCE at startup, never changes while running ---
    SetSysFirmwareVersion fw;       // firmware version + build string
    SetSysDeviceNickName  nick;     // user-set console nickname
    SetSysProductModel    model;    // Nx / Iowa / Hoag / Calcio / Aula ...
    char                  langStr[9]; // system language code (e.g. "en-US")
    SetRegion             region;   // JPN / USA / EUR / ...
} SysData;
