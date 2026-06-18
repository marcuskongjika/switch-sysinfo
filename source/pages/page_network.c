#include <stdio.h>
#include <switch.h>
#include "render.h"
#include "data.h"
#include "pages.h"

void page_network(int cy, const SysData *d) {
    int r = 0;
    char buf[256];

    if (!d->nifmOk) {
        ROW("Status", "Network service unavailable", RED);
        return;
    }

    ROW("Internet",
        d->netConnected ? "Connected" : "Disconnected",
        d->netConnected ? GREEN : RED);

    const char *ct = d->connType == NifmInternetConnectionType_WiFi    ? "WiFi"     :
                     d->connType == NifmInternetConnectionType_Ethernet ? "Ethernet" : "None";
    ROW("Type", ct, WHITE);

    if (d->connType == NifmInternetConnectionType_WiFi) {
        snprintf(buf, sizeof(buf), "%u / 3 bars", d->wifiSig);
        Col sc = d->wifiSig >= 2 ? GREEN : (d->wifiSig == 1 ? YELLOW : RED);
        ROW("WiFi Signal", buf, sc);
        BAR(d->wifiSig / 3.f, sc);
    }

    ROW("IP Address", d->netConnected ? d->ipStr : "N/A", CYAN);
}
