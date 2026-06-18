#pragma once
#include "data.h"

typedef enum {
    T_SYSTEM = 0, T_HARDWARE, T_POWER, T_STORAGE,
    T_NETWORK,    T_CTRL,     T_MOTION,  T_USB,
    T_COUNT
} Tab;

void page_system(int cy, const SysData *d);
void page_hardware(int cy, const SysData *d);
void page_power(int cy, const SysData *d);
void page_storage(int cy, const SysData *d);
void page_network(int cy, const SysData *d);
void page_controllers(int cy, const SysData *d);
void page_motion(int cy, const SysData *d);
void page_usb(int cy, const SysData *d);
