/*
 * pages.h - the tab list and one draw function per tab.
 *
 * Each page_*() takes the content-area top Y (cy) and a read-only data snapshot,
 * and draws its rows using the ROW/BAR macros from render.h. The Tab enum order
 * must match TAB_NAMES and PAGE_FNS in main.c.
 */
#pragma once
#include "data.h"

typedef enum {
    T_SYSTEM = 0, T_HARDWARE, T_POWER, T_STORAGE,
    T_NETWORK,    T_CTRL,     T_MOTION,  T_USB,
    T_COUNT      // not a real tab; the number of tabs
} Tab;

void page_system(int cy, const SysData *d);
void page_hardware(int cy, const SysData *d);
void page_power(int cy, const SysData *d);
void page_storage(int cy, const SysData *d);
void page_network(int cy, const SysData *d);
void page_controllers(int cy, const SysData *d);
void page_motion(int cy, const SysData *d);
void page_usb(int cy, const SysData *d);
