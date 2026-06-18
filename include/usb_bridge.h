#pragma once
#include <stdbool.h>
#include <switch.h>

// Live status shared with the GUI (USB tab). Snapshot it under the lock
// via usb_bridge_snapshot() — never read the global directly from the GUI.
typedef struct {
    Mutex lock;
    bool  initialized;    // usbComms gadget is up
    bool  hostConnected;  // a valid command was seen recently
    bool  active;         // a file transfer is in progress
    char  lastOp[64];
    char  curFile[160];
    u64   bytesDone;
    u64   bytesTotal;
    u32   filesIn;        // files written to the SD card (PUSH)
    u32   filesOut;       // files sent to the host (PULL)
} UsbBridgeStatus;

bool usb_bridge_start(void);
void usb_bridge_stop(void);
void usb_bridge_snapshot(UsbBridgeStatus *out);
