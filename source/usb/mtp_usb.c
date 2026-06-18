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
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include "mtp_usb.h"

// libnx has no R_TRY (that's an Atmosphere macro) — define a local one.
#define R_TRY(x) do { Result _rc = (x); if (R_FAILED(_rc)) return _rc; } while (0)

// USB device setup for an MTP/PTP interface (class 6 / subclass 1 / protocol 1),
// ported from haze's usb_session.cpp. Three endpoints: bulk IN, bulk OUT,
// interrupt IN. Only the 5.x+ usbDs path is implemented (modern firmware).

#define EP_MAXPKT_HIGH  512
#define EP_MAXPKT_SUPER 1024

static UsbDsInterface *g_interface = NULL;
static UsbDsEndpoint  *g_epIn      = NULL;  // device -> host
static UsbDsEndpoint  *g_epOut     = NULL;  // host -> device
static UsbDsEndpoint  *g_epIntr    = NULL;

static char g_serial[0x20] = "SInfo0000000000";

static Result addStr(u8 *idx, const char *s) {
    return usbDsAddUsbStringDescriptor(idx, s);
}

static Result setupDescriptors(void) {
    struct usb_interface_descriptor interface_descriptor = {
        .bLength            = USB_DT_INTERFACE_SIZE,
        .bDescriptorType    = USB_DT_INTERFACE,
        .bInterfaceNumber   = 0,
        .bNumEndpoints      = 3,
        .bInterfaceClass    = 0x06,   // Still Image (PTP)
        .bInterfaceSubClass = 0x01,
        .bInterfaceProtocol = 0x01,
    };

    struct usb_endpoint_descriptor endpoint_in = {
        .bLength          = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType  = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_IN,
        .bmAttributes     = USB_TRANSFER_TYPE_BULK,
        .wMaxPacketSize   = EP_MAXPKT_HIGH,
    };
    struct usb_endpoint_descriptor endpoint_out = {
        .bLength          = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType  = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_OUT,
        .bmAttributes     = USB_TRANSFER_TYPE_BULK,
        .wMaxPacketSize   = EP_MAXPKT_HIGH,
    };
    struct usb_endpoint_descriptor endpoint_intr = {
        .bLength          = USB_DT_ENDPOINT_SIZE,
        .bDescriptorType  = USB_DT_ENDPOINT,
        .bEndpointAddress = USB_ENDPOINT_IN,
        .bmAttributes     = USB_TRANSFER_TYPE_INTERRUPT,
        .wMaxPacketSize   = 0x18,
        .bInterval        = 0x06,
    };

    struct usb_ss_endpoint_companion_descriptor ss_companion = {
        .bLength           = sizeof(struct usb_ss_endpoint_companion_descriptor),
        .bDescriptorType   = USB_DT_SS_ENDPOINT_COMPANION,
        .bMaxBurst         = 0x0F,
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

    R_TRY(usbDsRegisterInterface(&g_interface));

    u8 iInterface = 0;
    R_TRY(addStr(&iInterface, "MTP"));

    interface_descriptor.bInterfaceNumber = g_interface->interface_index;
    interface_descriptor.iInterface       = iInterface;
    endpoint_in.bEndpointAddress   += interface_descriptor.bInterfaceNumber + 1;
    endpoint_out.bEndpointAddress  += interface_descriptor.bInterfaceNumber + 1;
    endpoint_intr.bEndpointAddress += interface_descriptor.bInterfaceNumber + 2;

    // High speed
    R_TRY(usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_High, &interface_descriptor, USB_DT_INTERFACE_SIZE));
    R_TRY(usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_High, &endpoint_in,   USB_DT_ENDPOINT_SIZE));
    R_TRY(usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_High, &endpoint_out,  USB_DT_ENDPOINT_SIZE));
    R_TRY(usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_High, &endpoint_intr, USB_DT_ENDPOINT_SIZE));

    // Super speed
    endpoint_in.wMaxPacketSize  = EP_MAXPKT_SUPER;
    endpoint_out.wMaxPacketSize = EP_MAXPKT_SUPER;
    R_TRY(usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_Super, &interface_descriptor, USB_DT_INTERFACE_SIZE));
    R_TRY(usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_Super, &endpoint_in,    USB_DT_ENDPOINT_SIZE));
    R_TRY(usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_Super, &ss_companion,   USB_DT_SS_ENDPOINT_COMPANION_SIZE));
    R_TRY(usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_Super, &endpoint_out,   USB_DT_ENDPOINT_SIZE));
    R_TRY(usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_Super, &ss_companion,   USB_DT_SS_ENDPOINT_COMPANION_SIZE));
    R_TRY(usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_Super, &endpoint_intr,  USB_DT_ENDPOINT_SIZE));
    R_TRY(usbDsInterface_AppendConfigurationData(g_interface, UsbDeviceSpeed_Super, &ss_companion_intr, USB_DT_SS_ENDPOINT_COMPANION_SIZE));

    R_TRY(usbDsInterface_RegisterEndpoint(g_interface, &g_epIn,   endpoint_in.bEndpointAddress));
    R_TRY(usbDsInterface_RegisterEndpoint(g_interface, &g_epOut,  endpoint_out.bEndpointAddress));
    R_TRY(usbDsInterface_RegisterEndpoint(g_interface, &g_epIntr, endpoint_intr.bEndpointAddress));

    R_TRY(usbDsInterface_EnableInterface(g_interface));
    return 0;
}

static Result setupDeviceDescriptors(void) {
    // Language: US English
    static const u16 langs[1] = { 0x0409 };
    R_TRY(usbDsAddUsbLanguageStringDescriptor(NULL, langs, 1));

    u8 iMan = 0, iProd = 0, iSer = 0;
    R_TRY(addStr(&iMan,  "marcuskongjika"));
    R_TRY(addStr(&iProd, "SysInfo MTP Responder"));

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
        .bcdUSB             = 0x0200,
        .bDeviceClass       = 0x00,
        .bDeviceSubClass    = 0x00,
        .bDeviceProtocol    = 0x00,
        .bMaxPacketSize0    = 0x40,
        .idVendor           = 0x057E,
        .idProduct          = 0x201D,
        .bcdDevice          = 0x0100,
        .iManufacturer      = iMan,
        .iProduct           = iProd,
        .iSerialNumber      = iSer,
        .bNumConfigurations = 0x01,
    };
    R_TRY(usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_High, &dev));

    dev.bcdUSB = 0x0300;
    dev.bMaxPacketSize0 = 0x09;
    R_TRY(usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed_Super, &dev));

    static const u8 bos[0x16] = {
        0x05, USB_DT_BOS, 0x16, 0x00, 0x02,
        0x07, USB_DT_DEVICE_CAPABILITY, 0x02, 0x02, 0x00, 0x00, 0x00,
        0x0A, USB_DT_DEVICE_CAPABILITY, 0x03, 0x00, 0x0C, 0x00, 0x03, 0x00, 0x00, 0x00,
    };
    R_TRY(usbDsSetBinaryObjectStore(bos, sizeof(bos)));
    return 0;
}

bool mtpUsbInit(void) {
    if (!hosversionAtLeast(5, 0, 0))
        return false;   // only the 5.x+ path is supported

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

void mtpUsbExit(void) {
    usbDsExit();
}

bool mtpUsbConfigured(void) {
    UsbState st = UsbState_Detached;
    usbDsGetState(&st);
    return st == UsbState_Configured;
}

void mtpUsbWaitChange(u64 timeout_ns) {
    Event *e = usbDsGetStateChangeEvent();
    if (R_SUCCEEDED(eventWait(e, timeout_ns)))
        eventClear(e);
}

static Result epXfer(UsbDsEndpoint *ep, void *buf, u32 size, u32 *transferred, volatile bool *run) {
    u32 urbId = 0;
    Result rc = usbDsEndpoint_PostBufferAsync(ep, buf, size, &urbId);
    if (R_FAILED(rc)) return rc;

    while (run == NULL || *run) {
        rc = eventWait(&ep->CompletionEvent, 1000000000ULL);
        if (R_SUCCEEDED(rc)) break;
        if (rc == KERNELRESULT(TimedOut)) continue;
        return rc;
    }
    if (run && !*run) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    eventClear(&ep->CompletionEvent);

    UsbDsReportData rep;
    rc = usbDsEndpoint_GetReportData(ep, &rep);
    if (R_FAILED(rc)) return rc;

    u32 reqSize = 0, transSize = 0;
    rc = usbDsParseReportData(&rep, urbId, &reqSize, &transSize);
    if (R_FAILED(rc)) return rc;

    if (transferred) *transferred = transSize;
    return 0;
}

Result mtpUsbWrite(void *buf, u32 size, volatile bool *run) {
    return epXfer(g_epIn, buf, size, NULL, run);
}

Result mtpUsbRead(void *buf, u32 size, u32 *transferred, volatile bool *run) {
    return epXfer(g_epOut, buf, size, transferred, run);
}
