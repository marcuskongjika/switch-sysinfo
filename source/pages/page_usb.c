/*
 * page_usb.c - "USB / MTP" tab: live status of the background MTP responder.
 *
 * The MTP worker runs on a separate thread (mtp.c). This page calls
 * mtp_snapshot() to get a mutex-protected copy of the worker's status struct
 * so we can read it safely from the render thread without races.
 *
 * Row breakdown:
 *   MTP Service  - whether mtpUsbInit() succeeded (USB gadget registered)
 *   Host         - whether a PC has enumerated the USB device (UsbState_Configured)
 *   Session      - whether the host has sent OpenSession (MTP session active)
 *   Last Command - name of the most recent MTP operation ("GetObject", "Rename" …)
 *   Current File - basename of the file being transferred / last transferred
 *   Transfer     - progress bar + MB counts; shown as "Idle" when nothing active
 *   Files        - cumulative received / sent file count since app launch
 */
#include <stdio.h>
#include <switch.h>
#include "render.h"
#include "data.h"
#include "pages.h"
#include "mtp.h"

void page_usb(int cy, const SysData *d) {
    (void)d;    // SysData unused; all state comes from the MTP thread directly
    int r = 0;  // row counter — advanced by ROW/BAR macros
    char buf[256];

    // Take a snapshot of the MTP thread's state under its mutex. After this
    // call, 's' is a plain struct copy — no synchronization needed to read it.
    MtpStatus s;
    mtp_snapshot(&s);

    // "Running" = the USB gadget was registered successfully at startup.
    // If this is RED the USB interface failed to init (wrong FW version, etc.).
    ROW("MTP Service", s.initialized ? "Running" : "Unavailable",
        s.initialized ? GREEN : RED);

    // "Connected" = the host PC has enumerated and configured the USB device.
    // The switch stays gray / "Waiting" until Windows/macOS detects the gadget.
    ROW("Host", s.configured ? "Connected" : "Waiting for PC...",
        s.configured ? GREEN : GRAY);

    // "Open" = the host has sent OP_OpenSession. File operations are only
    // valid inside an open session; the MTP thread enforces this too.
    ROW("Session", s.sessionOpen ? "Open" : "Closed",
        s.sessionOpen ? GREEN : GRAY);

    // Last MTP command name set by setOp() inside mtp.c (e.g. "GetObject").
    // Blank at startup.
    ROW("Last Command", s.lastOp[0] ? s.lastOp : "(none)", WHITE);

    // Filename currently being transferred (or the last one that was).
    ROW("Current File", s.curFile[0] ? s.curFile : "-", CYAN);

    // Show a live progress bar while a transfer is in progress (active == true
    // and bytesTotal > 0). Otherwise fall back to a static "Idle" label.
    if (s.active && s.bytesTotal > 0) {
        float p = (float)s.bytesDone / s.bytesTotal;
        snprintf(buf, sizeof(buf), "%.1f / %.1f MB  (%.0f%%)",
                 s.bytesDone / 1e6, s.bytesTotal / 1e6, p * 100.f);
        ROW("Transfer", buf, YELLOW);
        BAR(p, YELLOW);
    } else {
        ROW("Transfer", "Idle", GRAY);
    }

    // Cumulative counters since the app started. filesIn = files received from
    // the PC (writes to the SD); filesOut = files sent to the PC (reads).
    snprintf(buf, sizeof(buf), "%u received   /   %u sent", s.filesIn, s.filesOut);
    ROW("Files", buf, ACCENT);
}
