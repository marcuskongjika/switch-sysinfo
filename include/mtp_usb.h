#pragma once
#include <stdbool.h>
#include <switch.h>

// Low-level USB transport for the MTP responder.
bool   mtpUsbInit(void);
void   mtpUsbExit(void);
bool   mtpUsbConfigured(void);
void   mtpUsbWaitChange(u64 timeout_ns);

// Bulk transfers. `run` is polled so a blocked transfer can be cancelled on
// shutdown; pass the responder's run flag. Buffers must be 0x1000-aligned.
Result mtpUsbWrite(void *buf, u32 size, volatile bool *run);
Result mtpUsbRead(void *buf, u32 size, u32 *transferred, volatile bool *run);
