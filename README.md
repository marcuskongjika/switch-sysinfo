# SysInfo

A homebrew system-information dashboard for the Nintendo Switch, built with
devkitPro (devkitA64 + libnx) and rendered with SDL2. It shows live hardware,
power, network, controller, and motion data in a tabbed GUI, and doubles as a
**native MTP responder** so the SD card mounts as a drive over USB.

## Features

### Tabbed dashboard
Navigate tabs with **L / R** (or the D-pad), exit with **+**.

| Tab | Shows |
|-----|-------|
| **System** | Model (HAC/HDH/HEG…), nickname, firmware, language, region, date/time, brightness, handheld/docked mode, performance mode |
| **Hardware** | CPU / GPU / EMC clocks (with fill bars), RAM usage, internal + board temperature sensors |
| **Power** | Battery % and charger type, brightness, operation mode, performance profile with clock targets |
| **Storage** | SD card used / free / total with usage bar, RAM usage |
| **Network** | Internet status, Wi-Fi / Ethernet type, signal strength, IP address |
| **Controllers** | Per-pad (P1–P4 + handheld) type, battery level, and Joy-Con body color swatches |
| **Motion** | Live accelerometer (XYZ), angular velocity (XYZ), and roll/pitch/yaw, with bars |
| **USB** | MTP service / host / session state, current file, live transfer bar, file counters |

The UI uses the console's built-in shared font (nothing is bundled), drawn via
SDL2 + SDL2_ttf at 1280×720.

### Native USB MTP file transfer
While the app is running, plugging into a PC exposes the SD card as a standard
MTP drive — browse, copy, and delete files with no host-side tools. It's a C
port of the PTP/MTP core from Atmosphère's *haze*, talking directly to `usbDs`
on a background thread. MTP recognition is achieved by reporting
`VendorExtensionID = 6` ("microsoft.com: 1.0;") in `GetDeviceInfo`, so no
Microsoft OS descriptors are needed.

Supported operations: `GetDeviceInfo`, `OpenSession`/`CloseSession`,
`GetStorageIDs`/`GetStorageInfo`, `GetObjectHandles`/`GetObjectInfo`,
`GetObject`, `SendObjectInfo`/`SendObject`, `DeleteObject`, and the object
property ops (`GetObjectPropsSupported`/`PropDesc`/`PropValue`/`PropList`) for
fast host enumeration.

## Project layout

```
include/
  render.h     colors, fonts, layout constants, ROW/BAR macros
  data.h       SysData snapshot struct
  services.h   service init / data gathering
  pages.h      tab enum + page declarations
  mtp.h        MTP responder public API + status struct
  mtp_usb.h    USB transport API
source/
  main.c       SDL init, input loop, tab dispatch
  render.c     drawing primitives + title/tab/footer
  services.c   all libnx service init, per-frame data gather, cleanup
  pages/       one file per tab (page_system.c, page_hardware.c, …, page_usb.c)
  usb/
    mtp_usb.c  usbDs descriptors + bulk read/write
    mtp.c      PTP protocol, object database, operations, worker thread
```

## Building

Requires devkitPro with the `switch-dev` group plus SDL2 portlibs:

```bash
pacman -S switch-dev switch-sdl2 switch-sdl2_ttf switch-mesa switch-harfbuzz switch-libpng
make
```

Output lands in `build/sysinfo.nro`. Copy it to `sdmc:/switch/` and launch from
the homebrew menu.

## Credits

Huge thanks to **[Atmosphère-NX](https://github.com/Atmosphere-NX/Atmosphere)**
and the authors of **haze**, its USB MTP responder. The MTP support here is
essentially a C port of haze's PTP/MTP core — the protocol logic, USB interface
setup, and object handling all follow their implementation. None of this would
exist without their work. (haze is GPLv2; see the Atmosphère repository.)

## License

This project is licensed under the **GNU General Public License v2** — see
[LICENSE](LICENSE). The MTP responder is derived from haze (Atmosphère-NX),
which is GPLv2, so SysInfo is distributed under the same terms.

## Notes & limitations
- USB MTP requires firmware **≥ 5.0.0**, and any other USB MTP provider
  (e.g. CFW's built-in haze sysmodule) must be disabled — only one USB gadget
  can be active at a time.
- The MTP responder is a from-scratch port; the core protocol is faithful to
  haze but has been exercised far less, so unusual host behavior may surface
  edge cases.
- Tested target is the standard Switch; Lite/OLED report correctly on the
  System tab.
