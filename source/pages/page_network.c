/*
 * page_network.c - "Network" tab: WiFi/Ethernet connection status and IP.
 *
 * All network state is collected once per frame by services_gather_data()
 * via nifm (Network Interface Manager). If nifm failed to open (nifmOk ==
 * false, e.g. running without the right permission), we bail early with a
 * single error row rather than showing garbage values.
 *
 * WiFi signal strength comes back from libnx as a value 0-3 (0 = no signal,
 * 3 = full bars). We display it as "X / 3 bars" and map it to a bar chart
 * spanning 0..1 by dividing by 3.
 */
#include <stdio.h>
#include <switch.h>
#include "render.h"
#include "data.h"
#include "pages.h"

void page_network(int cy, const SysData *d) {
    int r = 0;        // row counter — advanced by the ROW/BAR macros
    char buf[256];

    // If the nifm service didn't open (e.g. permission denied), there's
    // nothing useful to show — print one row and return early.
    if (!d->nifmOk) {
        ROW("Status", "Network service unavailable", RED);
        return;
    }

    // Top-level connection state: green = online, red = offline.
    ROW("Internet",
        d->netConnected ? "Connected" : "Disconnected",
        d->netConnected ? GREEN : RED);

    // Connection medium. The Switch only exposes WiFi and Ethernet; anything
    // else (shouldn't happen in practice) falls through to "None".
    const char *ct = d->connType == NifmInternetConnectionType_WiFi    ? "WiFi"     :
                     d->connType == NifmInternetConnectionType_Ethernet ? "Ethernet" : "None";
    ROW("Type", ct, WHITE);

    // WiFi-only rows: signal strength bar and label.
    // Signal level: 0 = weak/none, 1 = marginal (yellow), 2-3 = good (green).
    if (d->connType == NifmInternetConnectionType_WiFi) {
        snprintf(buf, sizeof(buf), "%u / 3 bars", d->wifiSig);
        Col sc = d->wifiSig >= 2 ? GREEN : (d->wifiSig == 1 ? YELLOW : RED);
        ROW("WiFi Signal", buf, sc);
        BAR(d->wifiSig / 3.f, sc);  // 3 is the libnx max signal level
    }

    // The Switch IP is a u32 in host byte order; services.c formats it as
    // a dotted-decimal string in ipStr. Show N/A when offline.
    ROW("IP Address", d->netConnected ? d->ipStr : "N/A", CYAN);
}
