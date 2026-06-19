/*
 * page_controllers.c - "Controllers" tab: attached input devices, battery,
 *                      and physical color of each Joy-Con / Pro Controller.
 *
 * libnx exposes up to 4 "player" slots (No1-No4) plus a synthetic "Handheld"
 * slot that is active when Joy-Cons are attached to the console body. We poll
 * all five slots and skip any that have a zero style-set (i.e. nothing is
 * attached there).
 *
 * Battery level is reported in the range 0-4 (not a percentage). The "(CHG)"
 * suffix appears when the charging flag is set. For a Joy-Con Pair the left
 * and right sides each have independent battery readings, so we use the Split
 * variant; for everything else the Single variant returns a single reading.
 *
 * Controller color is encoded as 0x00RRGGBB. We render the hex code as text
 * AND call drawSwatch() to paint a small filled square in that color next to
 * the label so you can actually see the color at a glance.
 */
#include <stdio.h>
#include <switch.h>
#include "render.h"
#include "data.h"
#include "pages.h"

void page_controllers(int cy, const SysData *d) {
    int r = 0;        // row counter — advanced by ROW/BAR macros
    char buf[256];

    // The slots we check, in display order. Handheld is last so player slots
    // come first when both are present simultaneously (rare but possible in dev).
    static const HidNpadIdType ids[] = {
        HidNpadIdType_No1, HidNpadIdType_No2,
        HidNpadIdType_No3, HidNpadIdType_No4,
        HidNpadIdType_Handheld
    };
    static const char *inames[] = {"P1","P2","P3","P4","Handheld"};
    int found = 0;   // how many slots had a controller

    // r < 13: rough guard to avoid overflowing the screen at 36px per row.
    for (int i = 0; i < 5 && r < 13; i++) {
        u32 style = hidGetNpadStyleSet(ids[i]);
        if (!style) continue;   // nothing connected in this slot
        found++;

        // Identify the controller type from its style bitfield. We check the
        // most-specific styles first (Dual > FullKey > single Joy-Cons).
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

        // Battery: Joy-Con pairs use the Split API (independent L/R readings),
        // everything else uses Single. Level range is 0-4 (not a percentage).
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

        // Physical color: only Joy-Cons and Pro Controllers carry color data.
        // Joy-Cons use the Split API (L color, R color); Pro Controllers use
        // Single. We also render a small color swatch square for each side.
        // ry = the screen-Y coordinate of this row (ROW() already advanced r,
        // so we capture it before calling ROW to position the swatch correctly).
        if (style & (HidNpadStyleTag_NpadJoyDual |
                     HidNpadStyleTag_NpadJoyLeft  |
                     HidNpadStyleTag_NpadJoyRight)) {
            HidNpadControllerColor cl = {0}, cr = {0};
            if (R_SUCCEEDED(hidGetNpadControllerColorSplit(ids[i], &cl, &cr))) {
                int ry = cy + r * ROW_H;   // screen-Y before ROW() increments r
                snprintf(buf, sizeof(buf), "#%06X                #%06X",
                    cl.main & 0xFFFFFF, cr.main & 0xFFFFFF);
                snprintf(lbl, sizeof(lbl), "[%s] Colors", inames[i]);
                ROW(lbl, buf, WHITE);
                // Swatches sit just left of the value column (L) and offset
                // ~118px right (R), so they visually sit beside the hex codes.
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

    // If nothing was found in any slot, show a single "none" row.
    if (!found)
        ROW("Controllers", "None connected", GRAY);
}
