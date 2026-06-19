/*
 * services.c - talks to all the libnx system services and packs results into
 * a SysData snapshot for the UI.
 *
 * Pattern used throughout: every service init is guarded by an "Ok" flag. If a
 * service fails to open, we simply leave its data at defaults and the UI shows
 * zeros / "N/A" rather than crashing. gather_data() runs once per frame and is
 * deliberately cheap.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/statvfs.h>     // statvfs() for SD free/total space
#include <arpa/inet.h>       // inet_ntoa() to format the IP address
#include "services.h"

// Open every service session we need. Each call is wrapped so one failure
// doesn't stop the others. clkrst additionally needs a session per clock domain.
void services_init(Services *svc) {
    svc->psmOk  = R_SUCCEEDED(psmInitialize());     // battery / charger
    svc->clkOk  = R_SUCCEEDED(clkrstInitialize());  // clock rates
    svc->tsOk   = R_SUCCEEDED(tsInitialize());      // temperatures
    svc->sysOk  = R_SUCCEEDED(setsysInitialize());  // model/firmware/nickname
    svc->apmOk  = R_SUCCEEDED(apmInitialize());     // performance mode
    svc->lblOk  = R_SUCCEEDED(lblInitialize());     // backlight brightness
    svc->nifmOk = R_SUCCEEDED(nifmInitialize(NifmServiceType_System)); // network
    svc->timeOk = R_SUCCEEDED(timeInitialize());    // clock
    svc->setOk  = R_SUCCEEDED(setInitialize());     // language / region

    if (svc->clkOk) {
        // Open a session for each domain so we can query its current rate.
        clkrstOpenSession(&svc->cpuS, PcvModuleId_CpuBus, 3);
        clkrstOpenSession(&svc->gpuS, PcvModuleId_GPU, 3);
        clkrstOpenSession(&svc->emcS, PcvModuleId_EMC, 3);
    }

    // Motion sensors. Try a dual Joy-Con pair first (2 handles); if that fails,
    // fall back to the handheld console's built-in sensor (1 handle). gyroN
    // records how many we actually started so exit() can stop the right number.
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

// Read the things that never change while the app runs, so we don't re-query
// them every frame.
void services_load_static(Services *svc, SysData *data) {
    if (svc->sysOk) {
        setsysGetFirmwareVersion(&data->fw);
        setsysGetDeviceNickname(&data->nick);
        setsysGetProductModel(&data->model);
    }
    if (svc->setOk) {
        u64 lc = 0;
        setGetLanguageCode(&lc);          // returns up to 8 bytes packed in a u64
        memcpy(data->langStr, &lc, 8);    // copy those bytes out as a string...
        data->langStr[8] = '\0';          // ...and null-terminate
        setGetRegionCode(&data->region);
    }
}

// Refresh all the live values. Called once per frame from main().
void services_gather_data(Services *svc, SysData *data) {
    data->nifmOk = svc->nifmOk;
    data->gyroOk = svc->gyroN > 0;

    // Battery + charger.
    data->batPct  = 0;
    data->charger = PsmChargerType_Unconnected;
    if (svc->psmOk) {
        psmGetBatteryChargePercentage(&data->batPct);
        psmGetChargerType(&data->charger);
    }

    // Clock rates (Hz) per domain.
    data->cpuHz = data->gpuHz = data->emcHz = 0;
    if (svc->clkOk) {
        clkrstGetClockRate(&svc->cpuS, &data->cpuHz);
        clkrstGetClockRate(&svc->gpuS, &data->gpuHz);
        clkrstGetClockRate(&svc->emcS, &data->emcHz);
    }

    // Temperatures (milli-Celsius). Internal = SoC, External = board/ambient.
    data->tempMC = data->temp2MC = 0;
    if (svc->tsOk) {
        tsGetTemperatureMilliC(TsLocation_Internal, &data->tempMC);
        tsGetTemperatureMilliC(TsLocation_External, &data->temp2MC);
    }

    // Memory: ask the kernel for this process's total/used heap+code size.
    svcGetInfo(&data->totalMem, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&data->usedMem,  InfoType_UsedMemorySize,  CUR_PROCESS_HANDLE, 0);

    // Performance mode (normal/boost) and handheld/docked.
    data->perfMode = ApmPerformanceMode_Invalid;
    if (svc->apmOk) apmGetPerformanceMode(&data->perfMode);
    data->opMode = appletGetOperationMode();

    // Backlight brightness (0..1).
    data->bright = 0.f;
    if (svc->lblOk) lblGetCurrentBrightnessSetting(&data->bright);

    // Wall-clock time -> formatted string. timeGetCurrentTime gives POSIX
    // seconds; localtime + strftime turn it into "YYYY-MM-DD  HH:MM:SS".
    strncpy(data->timeStr, "N/A", sizeof(data->timeStr));
    if (svc->timeOk) {
        u64 posix = 0;
        if (R_SUCCEEDED(timeGetCurrentTime(TimeType_UserSystemClock, &posix))) {
            time_t t = (time_t)posix;
            struct tm *tm = localtime(&t);
            strftime(data->timeStr, sizeof(data->timeStr), "%Y-%m-%d  %H:%M:%S", tm);
        }
    }

    // SD card usage via statvfs on the mounted "sdmc:/" device. f_frsize is the
    // block size; multiply by block counts to get bytes. We pre-format the
    // human strings here and also store a 0..1 used fraction for the bar.
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

    // Network: connection status gives type + signal + connected state. If
    // connected, also fetch the current IPv4 and format it.
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

    // Motion: sample the first sensor's latest state for the Motion tab.
    memset(&data->gyroState, 0, sizeof(data->gyroState));
    if (svc->gyroN > 0)
        hidGetSixAxisSensorStates(svc->gyroH[0], &data->gyroState, 1);
}

// Close everything we opened, in a safe order. Stop only the motion sensors we
// actually started (gyroN of them).
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
