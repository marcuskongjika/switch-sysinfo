/*
 * SysInfo - MTP USB transport
 * Copyright (c) 2025 marcuskongjika
 *
 * Ported from haze (usb_session.cpp), the USB MTP responder of Atmosphere-NX.
 * Copyright (c) Atmosphere-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * How the USB side works:
 *
 * The Switch exposes itself as a USB device via the usbDs (USB Device Service)
 * kernel module. We register one interface with three endpoints:
 *
 *   Bulk IN  (device → host) : we push command responses / file data here
 *   Bulk OUT (host → device) : we pull command packets / uploaded file data here
 *   Interrupt IN             : required by the PTP spec for async event notifications;
 *                              we never actually send events through it, but Windows
 *                              will refuse to enumerate us without it.
 *
 * The interface class is 0x06 (Still Image / PTP). This is what makes the OS
 * recognise us as a camera/MTP device rather than a generic USB gadget.
 *
 * We advertise both High Speed (USB 2.0, 480 Mbit/s) and SuperSpeed (USB 3.0,
 * 5 Gbit/s) descriptors so the host can pick the fastest link it supports.
 *
 * Only firmware 5.0.0+ is supported because the usbDs API changed significantly
 * at that version (the older path would need a completely different descriptor
 * registration flow and isn't worth implementing for hardware this old).
 */
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include "mtp_usb.h"

/* libnx does not define R_TRY — that's an Atmosphere macro. We define our own
 * local version that returns the failing Result from the current function. */
#define R_TRY(x) do { Result _rc = (x); if (R_FAILED(_rc)) return _rc; } while (0)

/* Max bulk packet sizes per USB generation. High Speed = 512 B, SuperSpeed = 1 KiB.
 * These must match what the host negotiates; using the wrong value causes
 * the transfer to stall. */
#define EP_MAXPKT_HIGH  512
#define EP_MAXPKT_SUPER 1024

/* Global endpoint handles. NULL until mtpUsbInit() succeeds. */
static UsbDsInterface *g_interface = NULL;
static UsbDsEndpoint  *g_epIn      = NULL;  // Bulk IN  (TX: device → host)
static UsbDsEndpoint  *g_epOut     = NULL;  // Bulk OUT (RX: host → device)
static UsbDsEndpoint  *g_epIntr    = NULL;  // Interrupt IN (not actively used)

/* Serial number string. We try to read the actual console serial via setsys;
 * if that fails we fall back to this placeholder. */
static char g_serial[0x20] = "SInfo0000000000";

/* Thin wrapper — usbDsAddUsbStringDescriptor returns the assigned index in *idx. */
static Result addStr(u8 *idx, const char *s) {
    return usbDsAddUsbStringDescriptor(idx, s);
}

/*
 * setupDescriptors() - Register the PTP interface and all three endpoints.
 *
 * Called once during mtpUsbInit(). We build descriptor structs on the stack
 * and pass them to usbDs in two passes: one for High Speed, one for SuperSpeed
 * (SuperSpeed additionally requires an SS Endpoint Companion descriptor after
 * each endpoint descriptor — the host reads these to know burst/stream limits).
 *
 * Endpoint address assignment: usbDs assigns interface numbers sequentially.
 * By convention we add 1 to the interface number for the first pair of
 * endpoints and 2 for the interrupt, which keeps addresses unique across
 * multiple interfaces (relevant if another interface is ever registered first).
 */
static Result setupDescriptors(void) {
    struct usb_interface_descriptor interface_descriptor = {
        .bLength            = USB_DT_INTERFACE_SIZE,
        .bDescriptorType    = USB_DT_INTERFACE,
        .bInterfaceNumber   = 0,                // patched below after registration
        .bNumEndpoints      = 3,
        .bInterfaceClass    = 0x06,             // Still Image (PTP/MTP)
        .bInterfaceSubClass = 0x01,
        .bInterfaceProtocol = 0x01,
    };

    /* Bulk IN: device sends data to the host (responses, file contents). */
    struct usb_endpoint_descriptor endpoint_in = {
        .bLength          = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType  = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_IN,    // direction bit; address patched below
        .bmAttributes     = USB_TRANSFER_TYPE_BULK,
        .wMaxPacketSize   = EP_MAXPKT_HIGH,
    };
    /* Bulk OUT: host sends data to the device (commands, uploaded files). */
    struct usb_endpoint_descriptor endpoint_out = {
        .bLength          = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType  = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_OUT,
        .bmAttributes     = USB_TRANSFER_TYPE_BULK,
        .wMaxPacketSize   = EP_MAXPKT_HIGH,
    };
    /* Interrupt IN: required by PTP spec for async events.
     * We never actually send events, but Windows silently refuses to talk
     * to us if this endpoint is missing. 0x18-byte max, polled every 2^6 = 64 ms. */
    struct usb_endpoint_descriptor endpoint_intr = {
        .bLength          = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType  = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_IN,
        .bmAttributes     = USB_TRANSFER_TYPE_INTERRUPT,
        .wMaxPacketSize   = 0x18,
        .bInterval        = 0x06,   // 2^(6-1) = 32 ms at High Speed
    };

    /* SuperSpeed Endpoint Companion: tells the host burst/streams limits.
     * bMaxBurst=15 for bulk means the device can send 16 packets back-to-back
     * before waiting for an ACK — maximum throughput. Interrupt companion
     * keeps burst=0 (single packet per poll interval). */
    struct usb_ss_endpoint_companion_descriptor ss_companion = {
        .bLength           = sizeof(struct usb_ss_endpoint_companion_descriptor),
        .bDescriptorType   = USB_DT_SS_ENDPOINT_COMPANION,
        .bMaxBurst         = 0x0F,  // max burst size - 1 (15 = 16 packets)
        .bmAttributes      = 0x00,
        .wBytesPerInterval = 0x00,
    };
    struct usb_ss_endpoint_companion_descriptor ss_companion_intr = {
        .bLength           = sizeof(struct usb_ss_endpoint_companion_descriptor),
        .bDescriptorType   = USB_DT_SS_ENDPOINT_COMPANION,
        .bMaxBurst         = 0x00,
        .bmAttributes      = 0x00,
        .wBytesPerInterval = 0x00,
    };

    /* Claim an interface slot. usbDs fills in g_interface and sets
     * g_interface->interface_index to the assigned number. */
    R_TRY(usbDsRegisterInterface(&g_interface));

    /* Add a string descriptor "MTP" and embed its index in the interface. */
    u8 iInterface = 0;
    R_TRY(addStr(&iInterface, "MTP"));

    /* Patch the interface number and endpoint addresses now that we know them. */
    interface_descriptor.bInterfaceNumber = g_interface->interface_index;
    interface_descriptor.iInterface       = iInterface;
    endpoint_in.bEndpointAddress   += interface_descriptor.bInterfaceNumber + 1;
    endpoint_out.bEndpointAddress  += interface_descriptor.bInterfaceNumber + 1;
    endpoint_intr.bEndpointAddress += interface_descriptor.bInterfaceNumber + 2;

    /* High Speed configuration (USB 2.0). */
    R_TRY(usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_High, &interface_descriptor, USB_DT_INTERFACE_SIZE));
    R_TRY(usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_High, &endpoint_in,   USB_DT_ENDPOINT_SIZE));
    R_TRY(usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_High, &endpoint_out,  USB_DT_ENDPOINT_SIZE));
    R_TRY(usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_High, &endpoint_intr, USB_DT_ENDPOINT_SIZE));

    /* SuperSpeed configuration (USB 3.0). Bump bulk packet size to 1024,
     * and append an SS companion after each endpoint descriptor. */
    endpoint_in.wMaxPacketSize  = EP_MAXPKT_SUPER;
    endpoint_out.wMaxPacketSize = EP_MAXPKT_SUPER;
    R_TRY(usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_Super, &interface_descriptor, USB_DT_INTERFACE_SIZE));
    R_TRY(usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_Super, &endpoint_in,    USB_DT_ENDPOINT_SIZE));
    R_TRY(usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_Super, &ss_companion,   USB_DT_SS_ENDPOINT_COMPANION_SIZE));
    R_TRY(usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_Super, &endpoint_out,   USB_DT_ENDPOINT_SIZE));
    R_TRY(usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_Super, &ss_companion,   USB_DT_SS_ENDPOINT_COMPANION_SIZE));
    R_TRY(usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_Super, &endpoint_intr,  USB_DT_ENDPOINT_SIZE));
    R_TRY(usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_Super, &ss_companion_intr, USB_DT_SS_ENDPOINT_COMPANION_SIZE));

    /* Register each endpoint to get the UsbDsEndpoint* handle we'll use later
     * when calling PostBufferAsync. */
    R_TRY(usbDsInterface_RegisterEndpoint(g_interface, &g_epIn,   endpoint_in.bEndpointAddress));
    R_TRY(usbDsInterface_RegisterEndpoint(g_interface, &g_epOut,  endpoint_out.bEndpointAddress));
    R_TRY(usbDsInterface_RegisterEndpoint(g_interface, &g_epIntr, endpoint_intr.bEndpointAddress));

    R_TRY(usbDsInterface_EnableInterface(g_interface));
    return 0;
}

/*
 * setupDeviceDescriptors() - Set the USB device-level descriptors.
 *
 * These are what the host reads when you plug in the cable, before it even
 * knows what the device does. The VID/PID 057E:201D is the standard Nintendo
 * Switch USB identifier — using it means drivers already on the PC will bind.
 *
 * The serial number is filled from the actual console serial if possible,
 * which lets the host distinguish two Switches if you swap cables.
 *
 * The BOS (Binary Object Store) advertises USB 2.0 LPM (Link Power Management)
 * and USB 3.0 capabilities to the host.  The literal bytes are copied straight
 * from haze — they encode exactly the capabilities the Switch hardware supports.
 */
static Result setupDeviceDescriptors(void) {
    /* Language descriptor: we only support US English (0x0409). */
    static const u16 langs[1] = { 0x0409 };
    R_TRY(usbDsAddUsbLanguageStringDescriptor(NULL, langs, 1));

    u8 iMan = 0, iProd = 0, iSer = 0;
    R_TRY(addStr(&iMan,  "marcuskongjika"));
    R_TRY(addStr(&iProd, "SysInfo MTP Responder"));

    /* Attempt to read the real serial number. setsysInitialize() can fail
     * if the service isn't accessible (e.g. in some constrained applet modes),
     * so we silently fall back to the placeholder string in g_serial. */
    if (R_FAILED(setsysInitialize()) == false) {
        SetSysSerialNumber sn = {0};
        if (R_SUCCEEDED(setsysGetSerialNumber(&sn)) && sn.number[0])
            snprintf(g_serial, sizeof(g_serial), "%s", sn.number);
        setsysExit();
    }
    R_TRY(addStr(&iSer, g_serial));

    struct usb_device_descriptor dev = {
        .bLength            = USB_DT_DEVICE_SIZE,
        .bDescriptorType    = USB_DT_DEVICE,
        .bcdUSB             = 0x0200,   // USB 2.0 initially; overridden to 0x0300 below for SS
        .bDeviceClass       = 0x00,     // class defined at interface level
        .bDeviceSubClass    = 0x00,
        .bDeviceProtocol    = 0x00,
        .bMaxPacketSize0    = 0x40,     // 64-byte control endpoint (HS)
        .idVendor           = 0x057E,   // Nintendo
        .idProduct          = 0x201D,   // Switch
        .bcdDevice          = 0x0100,
        .iManufacturer      = iMan,
        .iProduct           = iProd,
        .iSerialNumber      = iSer,
        .bNumConfigurations = 0x01,
    };
    R_TRY(usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &dev));

    /* SuperSpeed: bump bcdUSB and shrink control packet size to 9 (log2(512)). */
    dev.bcdUSB = 0x0300;
    dev.bMaxPacketSize0 = 0x09;
    R_TRY(usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &dev));

    /* BOS descriptor blob: USB2 ext capability + SS device capability.
     * Copied verbatim from haze; encoding matches the Switch hardware. */
    static const u8 bos[0x16] = {
        /* BOS header: total length = 0x16, 2 capabilities */
        0x05, USB_DT_BOS, 0x16, 0x00, 0x02,
        /* USB 2.0 Extension: LPM (Link Power Management) support */
        0x07, USB_DT_DEVICE_CAPABILITY, 0x02, 0x02, 0x00, 0x00, 0x00,
        /* SuperSpeed Device Capability */
        0x0A, USB_DT_DEVICE_CAPABILITY, 0x03, 0x00, 0x0C, 0x00, 0x03, 0x00, 0x00, 0x00,
    };
    R_TRY(usbDsSetBinaryObjectStore(bos, sizeof(bos)));
    return 0;
}

/*
 * mtpUsbInit() - Bring up the USB gadget.
 *
 * Returns true on success. False can mean: firmware too old (< 5.0.0),
 * usbDs service failed, or descriptor registration failed. The caller
 * (mtp_start) marks g_st.initialized = false on failure so the UI can
 * show "Unavailable" on the USB tab.
 */
bool mtpUsbInit(void) {
    if (!hosversionAtLeast(5, 0, 0))
        return false;   // only the 5.x+ usbDs API is implemented

    if (R_FAILED(usbDsInitialize()))
        return false;

    if (R_FAILED(setupDeviceDescriptors()) ||
        R_FAILED(setupDescriptors()) ||
        R_FAILED(usbDsEnable())) {
        usbDsExit();
        return false;
    }
    return true;
}

/* Tear down the USB gadget. Also unblocks any pending epXfer() wait because
 * the endpoint completion event fires when the endpoint is closed. */
void mtpUsbExit(void) {
    usbDsExit();
}

/* Returns true when a host has enumerated and configured the device.
 * Until then, no data can be exchanged. */
bool mtpUsbConfigured(void) {
    UsbState st = UsbState_Detached;
    usbDsGetState(&st);
    return st == UsbState_Configured;
}

/* Block until the USB state changes (cable plugged/unplugged, host
 * configures us, etc.), or until timeout_ns nanoseconds elapse.
 * Used by the MTP thread so it doesn't busy-spin while disconnected. */
void mtpUsbWaitChange(u64 timeout_ns) {
    Event *e = usbDsGetStateChangeEvent();
    if (R_SUCCEEDED(eventWait(e, timeout_ns)))
        eventClear(e);
}

/*
 * epXfer() - Async DMA transfer on a single endpoint.
 *
 * usbDs uses a "post then wait" model:
 *   1. PostBufferAsync queues a DMA request and returns immediately.
 *   2. CompletionEvent fires when the transfer finishes (or errors).
 *   3. GetReportData + ParseReportData tell us how many bytes actually moved.
 *
 * We wait in a 1-second loop so we can check the *run flag between polls —
 * if the MTP thread is being stopped, we bail early rather than hanging
 * forever waiting for a host that may already be disconnected.
 *
 * On a successful wait, *transferred is set to the actual byte count (which
 * may be less than size for short packets).
 */
static Result epXfer(UsbDsEndpoint *ep, void *buf, u32 size, u32 *transferred, volatile bool *run) {
    u32 urbId = 0;
    Result rc = usbDsEndpoint_PostBufferAsync(ep, buf, size, &urbId);
    if (R_FAILED(rc)) return rc;

    /* Wait loop: 1-second timeout so we can check *run each iteration. */
    while (run == NULL || *run) {
        rc = eventWait(&ep->CompletionEvent, 1000000000ULL);
        if (R_SUCCEEDED(rc)) break;
        if (rc == KERNELRESULT(TimedOut)) continue;
        return rc;   // real error
    }
    /* If we exited because *run went false, cancel the pending transfer. */
    if (run && !*run) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    eventClear(&ep->CompletionEvent);

    /* Retrieve the completion report and parse the actual transfer size. */
    UsbDsReportData rep;
    rc = usbDsEndpoint_GetReportData(ep, &rep);
    if (R_FAILED(rc)) return rc;

    u32 reqSize = 0, transSize = 0;
    rc = usbDsParseReportData(&rep, urbId, &reqSize, &transSize);
    if (R_FAILED(rc)) return rc;

    if (transferred) *transferred = transSize;
    return 0;
}

/* Send 'size' bytes from 'buf' to the host via the Bulk IN endpoint.
 * 'run' is the MTP thread's stop flag; the transfer is aborted if it goes false. */
Result mtpUsbWrite(void *buf, u32 size, volatile bool *run) {
    return epXfer(g_epIn, buf, size, NULL, run);
}

/* Receive up to 'size' bytes from the host via the Bulk OUT endpoint.
 * *transferred is set to the number of bytes actually received. */
Result mtpUsbRead(void *buf, u32 size, u32 *transferred, volatile bool *run) {
    return epXfer(g_epOut, buf, size, transferred, run);
}
