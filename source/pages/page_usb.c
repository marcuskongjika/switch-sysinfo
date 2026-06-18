#include <stdio.h>
#include <switch.h>
#include "render.h"
#include "data.h"
#include "pages.h"
#include "mtp.h"

void page_usb(int cy, const SysData *d) {
    (void)d;
    int r = 0;
    char buf[256];

    MtpStatus s;
    mtp_snapshot(&s);

    ROW("MTP Service", s.initialized ? "Running" : "Unavailable",
        s.initialized ? GREEN : RED);
    ROW("Host", s.configured ? "Connected" : "Waiting for PC...",
        s.configured ? GREEN : GRAY);
    ROW("Session", s.sessionOpen ? "Open" : "Closed",
        s.sessionOpen ? GREEN : GRAY);
    ROW("Last Command", s.lastOp[0] ? s.lastOp : "(none)", WHITE);
    ROW("Current File", s.curFile[0] ? s.curFile : "-", CYAN);

    if (s.active && s.bytesTotal > 0) {
        float p = (float)s.bytesDone / s.bytesTotal;
        snprintf(buf, sizeof(buf), "%.1f / %.1f MB  (%.0f%%)",
                 s.bytesDone / 1e6, s.bytesTotal / 1e6, p * 100.f);
        ROW("Transfer", buf, YELLOW);
        BAR(p, YELLOW);
    } else {
        ROW("Transfer", "Idle", GRAY);
    }

    snprintf(buf, sizeof(buf), "%u received   /   %u sent", s.filesIn, s.filesOut);
    ROW("Files", buf, ACCENT);

    ROW("Protocol", "MTP (PTP transport)", GRAY);
    ROW("Mount", "Plug into PC — appears as a drive", GRAY);
}
