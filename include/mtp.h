#pragma once
#include <stdbool.h>
#include <switch.h>

// Native MTP responder (PTP transport over USB), modeled on Atmosphere's haze.
// Exposes the SD card as a real MTP drive on the host PC.

typedef struct {
    Mutex lock;
    bool  initialized;    // usbDs gadget registered
    bool  configured;     // host has enumerated us
    bool  sessionOpen;    // an MTP session is open
    bool  active;         // a file transfer is in progress
    char  lastOp[48];
    char  curFile[160];
    u64   bytesDone;
    u64   bytesTotal;
    u32   filesIn;        // files written to SD (host -> switch)
    u32   filesOut;       // files read from SD (switch -> host)
} MtpStatus;

bool mtp_start(void);
void mtp_stop(void);
void mtp_snapshot(MtpStatus *out);
