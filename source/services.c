#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/statvfs.h>
#include <arpa/inet.h>
#include "services.h"

void services_init(Services *svc) {
    svc->psmOk  = R_SUCCEEDED(psmInitialize());
    svc->clkOk  = R_SUCCEEDED(clkrstInitialize());
    svc->tsOk   = R_SUCCEEDED(tsInitialize());
    svc->sysOk  = R_SUCCEEDED(setsysInitialize());
    svc->apmOk  = R_SUCCEEDED(apmInitialize());
    svc->lblOk  = R_SUCCEEDED(lblInitialize());
    svc->nifmOk = R_SUCCEEDED(nifmInitialize(NifmServiceType_System));
    svc->timeOk = R_SUCCEEDED(timeInitialize());
    svc->setOk  = R_SUCCEEDED(setInitialize());

    if (svc->clkOk) {
        clkrstOpenSession(&svc->cpuS, PcvModuleId_CpuBus, 3);
        clkrstOpenSession(&svc->gpuS, PcvModuleId_GPU, 3);
        clkrstOpenSession(&svc->emcS, PcvModuleId_EMC, 3);
    }

    svc->gyroN = 0;
    if (R_SUCCEEDED(hidGetSixAxisSensorHandles(svc->gyroH, 2,
            HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual))) {
        hidStartSixAxisSensor(svc->gyroH[0]);
        hidStartSixAxisSensor(svc->gyroH[1]);
        svc->gyroN = 2;
    } else if (R_SUCCEEDED(hidGetSixAxisSensorHandles(svc->gyroH, 1,
            HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld))) {
        hidStartSixAxisSensor(svc->gyroH[0]);
        svc->gyroN = 1;
    }
}

void services_load_static(Services *svc, SysData *data) {
    if (svc->sysOk) {
        setsysGetFirmwareVersion(&data->fw);
        setsysGetDeviceNickname(&data->nick);
        setsysGetProductModel(&data->model);
    }
    if (svc->setOk) {
        u64 lc = 0;
        setGetLanguageCode(&lc);
        memcpy(data->langStr, &lc, 8);
        data->langStr[8] = '\0';
        setGetRegionCode(&data->region);
    }
}

void services_gather_data(Services *svc, SysData *data) {
    data->nifmOk = svc->nifmOk;
    data->gyroOk = svc->gyroN > 0;

    // Battery
    data->batPct  = 0;
    data->charger = PsmChargerType_Unconnected;
    if (svc->psmOk) {
        psmGetBatteryChargePercentage(&data->batPct);
        psmGetChargerType(&data->charger);
    }

    // Clocks
    data->cpuHz = data->gpuHz = data->emcHz = 0;
    if (svc->clkOk) {
        clkrstGetClockRate(&svc->cpuS, &data->cpuHz);
        clkrstGetClockRate(&svc->gpuS, &data->gpuHz);
        clkrstGetClockRate(&svc->emcS, &data->emcHz);
    }

    // Temperature
    data->tempMC = data->temp2MC = 0;
    if (svc->tsOk) {
        tsGetTemperatureMilliC(TsLocation_Internal, &data->tempMC);
        tsGetTemperatureMilliC(TsLocation_External, &data->temp2MC);
    }

    // Memory
    svcGetInfo(&data->totalMem, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&data->usedMem,  InfoType_UsedMemorySize,  CUR_PROCESS_HANDLE, 0);

    // APM / op mode
    data->perfMode = ApmPerformanceMode_Invalid;
    if (svc->apmOk) apmGetPerformanceMode(&data->perfMode);
    data->opMode = appletGetOperationMode();

    // Brightness
    data->bright = 0.f;
    if (svc->lblOk) lblGetCurrentBrightnessSetting(&data->bright);

    // Time
    strncpy(data->timeStr, "N/A", sizeof(data->timeStr));
    if (svc->timeOk) {
        u64 posix = 0;
        if (R_SUCCEEDED(timeGetCurrentTime(TimeType_UserSystemClock, &posix))) {
            time_t t = (time_t)posix;
            struct tm *tm = localtime(&t);
            strftime(data->timeStr, sizeof(data->timeStr), "%Y-%m-%d  %H:%M:%S", tm);
        }
    }

    // SD storage
    strncpy(data->sdTot,  "N/A", sizeof(data->sdTot));
    strncpy(data->sdFree, "N/A", sizeof(data->sdFree));
    strncpy(data->sdUsed, "N/A", sizeof(data->sdUsed));
    data->sdPct = 0.f;
    struct statvfs sv;
    if (statvfs("sdmc:/", &sv) == 0) {
        u64 tot = (u64)sv.f_blocks * sv.f_frsize;
        u64 fr  = (u64)sv.f_bfree  * sv.f_frsize;
        u64 us  = tot - fr;
        data->sdPct = tot > 0 ? (float)us / tot : 0.f;
        snprintf(data->sdTot,  sizeof(data->sdTot),  "%.2f GB", tot / 1e9);
        snprintf(data->sdFree, sizeof(data->sdFree), "%.2f GB", fr  / 1e9);
        snprintf(data->sdUsed, sizeof(data->sdUsed), "%.2f GB", us  / 1e9);
    }

    // Network
    data->netConnected = false;
    strncpy(data->ipStr, "N/A", sizeof(data->ipStr));
    data->connType = 0;
    data->wifiSig  = 0;
    if (svc->nifmOk) {
        NifmInternetConnectionStatus cs = 0;
        if (R_SUCCEEDED(nifmGetInternetConnectionStatus(
                &data->connType, &data->wifiSig, &cs))) {
            data->netConnected = (cs == NifmInternetConnectionStatus_Connected);
            if (data->netConnected) {
                u32 ip = 0;
                if (R_SUCCEEDED(nifmGetCurrentIpAddress(&ip))) {
                    struct in_addr a;
                    a.s_addr = ip;
                    strncpy(data->ipStr, inet_ntoa(a), sizeof(data->ipStr) - 1);
                }
            }
        }
    }

    // Gyro
    memset(&data->gyroState, 0, sizeof(data->gyroState));
    if (svc->gyroN > 0)
        hidGetSixAxisSensorStates(svc->gyroH[0], &data->gyroState, 1);
}

void services_exit(Services *svc) {
    for (int i = 0; i < svc->gyroN; i++)
        hidStopSixAxisSensor(svc->gyroH[i]);

    if (svc->clkOk) {
        clkrstCloseSession(&svc->cpuS);
        clkrstCloseSession(&svc->gpuS);
        clkrstCloseSession(&svc->emcS);
        clkrstExit();
    }
    if (svc->psmOk)  psmExit();
    if (svc->tsOk)   tsExit();
    if (svc->sysOk)  setsysExit();
    if (svc->apmOk)  apmExit();
    if (svc->lblOk)  lblExit();
    if (svc->nifmOk) nifmExit();
    if (svc->timeOk) timeExit();
    if (svc->setOk)  setExit();
}
