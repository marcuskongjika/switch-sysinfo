#include <stdio.h>
#include <switch.h>
#include "render.h"
#include "data.h"
#include "pages.h"

void page_controllers(int cy, const SysData *d) {
    int r = 0;
    char buf[256];

    static const HidNpadIdType ids[] = {
        HidNpadIdType_No1, HidNpadIdType_No2,
        HidNpadIdType_No3, HidNpadIdType_No4,
        HidNpadIdType_Handheld
    };
    static const char *inames[] = {"P1","P2","P3","P4","Handheld"};
    int found = 0;

    for (int i = 0; i < 5 && r < 13; i++) {
        u32 style = hidGetNpadStyleSet(ids[i]);
        if (!style) continue;
        found++;

        const char *type = "Unknown";
        if      (style & HidNpadStyleTag_NpadJoyDual)    type = "Joy-Con Pair";
        else if (style & HidNpadStyleTag_NpadFullKey)     type = "Pro Controller";
        else if (style & HidNpadStyleTag_NpadJoyLeft)     type = "Joy-Con (L)";
        else if (style & HidNpadStyleTag_NpadJoyRight)    type = "Joy-Con (R)";
        else if (style & HidNpadStyleTag_NpadHandheld)    type = "Handheld";
        else if (style & HidNpadStyleTag_NpadSystemExt)   type = "Other Controller";

        char lbl[48];
        snprintf(lbl, sizeof(lbl), "[%s] Type", inames[i]);
        ROW(lbl, type, ACCENT);

        // Battery
        HidPowerInfo pl = {0}, pr = {0};
        if (style & HidNpadStyleTag_NpadJoyDual) {
            hidGetNpadPowerInfoSplit(ids[i], &pl, &pr);
            snprintf(buf, sizeof(buf), "L: %u/4%s   R: %u/4%s",
                pl.battery_level, pl.is_charging ? " (CHG)" : "",
                pr.battery_level, pr.is_charging ? " (CHG)" : "");
        } else {
            hidGetNpadPowerInfoSingle(ids[i], &pl);
            snprintf(buf, sizeof(buf), "%u / 4%s",
                pl.battery_level, pl.is_charging ? "  (Charging)" : "");
        }
        snprintf(lbl, sizeof(lbl), "[%s] Battery", inames[i]);
        ROW(lbl, buf, GREEN);

        // Colors
        if (style & (HidNpadStyleTag_NpadJoyDual |
                     HidNpadStyleTag_NpadJoyLeft  |
                     HidNpadStyleTag_NpadJoyRight)) {
            HidNpadControllerColor cl = {0}, cr = {0};
            if (R_SUCCEEDED(hidGetNpadControllerColorSplit(ids[i], &cl, &cr))) {
                int ry = cy + r * ROW_H;
                snprintf(buf, sizeof(buf), "#%06X                #%06X",
                    cl.main & 0xFFFFFF, cr.main & 0xFFFFFF);
                snprintf(lbl, sizeof(lbl), "[%s] Colors", inames[i]);
                ROW(lbl, buf, WHITE);
                drawSwatch(VALUE_X - 2,   ry, cl.main & 0xFFFFFF);
                drawSwatch(VALUE_X + 118, ry, cr.main & 0xFFFFFF);
            }
        } else if (style & HidNpadStyleTag_NpadFullKey) {
            HidNpadControllerColor cs = {0};
            if (R_SUCCEEDED(hidGetNpadControllerColorSingle(ids[i], &cs))) {
                int ry = cy + r * ROW_H;
                snprintf(buf, sizeof(buf), "#%06X", cs.main & 0xFFFFFF);
                snprintf(lbl, sizeof(lbl), "[%s] Color", inames[i]);
                ROW(lbl, buf, WHITE);
                drawSwatch(VALUE_X - 2, ry, cs.main & 0xFFFFFF);
            }
        }
    }

    if (!found)
        ROW("Controllers", "None connected", GRAY);
}
