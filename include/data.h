#pragma once
#include <stdbool.h>
#include <switch.h>

typedef struct {
    // Service availability
    bool nifmOk;
    bool gyroOk;

    // Battery
    u32 batPct;
    PsmChargerType charger;

    // Clocks (Hz)
    u32 cpuHz, gpuHz, emcHz;

    // Temperature (milli-Celsius)
    s32 tempMC, temp2MC;

    // Memory
    u64 totalMem, usedMem;

    // Performance
    ApmPerformanceMode  perfMode;
    AppletOperationMode opMode;

    // Display
    float bright;

    // Time string
    char timeStr[48];

    // SD storage
    char sdTot[32], sdFree[32], sdUsed[32];
    float sdPct;

    // Network
    NifmInternetConnectionType   connType;
    u32  wifiSig;
    bool netConnected;
    char ipStr[32];

    // Gyro
    HidSixAxisSensorState gyroState;

    // Static info (loaded once at startup)
    SetSysFirmwareVersion fw;
    SetSysDeviceNickName  nick;
    SetSysProductModel    model;
    char                  langStr[9];
    SetRegion             region;
} SysData;
