#!/usr/bin/env python3
"""
nxfs.py - PC companion for the sysinfo USB bridge.

Talks to the Switch homebrew over libnx usbComms (vendor 057E:3000).
Requires pyusb:  pip install pyusb
On Windows you may need to bind the WinUSB driver to the device with Zadig.

Usage:
    python nxfs.py ping
    python nxfs.py list  sdmc:/
    python nxfs.py stat  sdmc:/hbmenu.nro
    python nxfs.py pull  sdmc:/hbmenu.nro  ./hbmenu.nro
    python nxfs.py push  ./game.nro        sdmc:/switch/game.nro
    python nxfs.py mkdir sdmc:/switch/mydir
    python nxfs.py rm    sdmc:/switch/old.txt
"""
import sys
import os
import struct
import usb.core
import usb.util

try:
    import libusb_package
    import usb.backend.libusb1
    _BACKEND = usb.backend.libusb1.get_backend(find_library=libusb_package.find_library)
except ImportError:
    _BACKEND = None

VID, PID = 0x057E, 0x3000
MAGIC = 0x5346584E  # "NXFS"
CHUNK = 256 * 1024

CMD_PING, CMD_LIST, CMD_STAT, CMD_PULL, CMD_PUSH, CMD_MKDIR, CMD_DELETE = range(1, 8)
STATUS_NAMES = {0: "OK", 1: "open failed", 2: "I/O error", 3: "not found", 4: "bad command"}

REQ_FMT = "<IIQ"          # magic, cmd, size  (then 768-byte path)
REQ_PATH = 768
RES_FMT = "<IIQ"          # magic, status, size
RES_LEN = struct.calcsize(RES_FMT)


def list_devices():
    print("USB devices visible to pyusb:")
    for d in usb.core.find(find_all=True, backend=_BACKEND):
        try:
            name = usb.util.get_string(d, d.iProduct) if d.iProduct else "?"
        except Exception:
            name = "?"
        print(f"  {d.idVendor:04X}:{d.idProduct:04X}  {name}")


def connect():
    dev = usb.core.find(idVendor=VID, idProduct=PID, backend=_BACKEND)
    if dev is None:
        sys.exit("Switch not found. Is the sysinfo app running with the USB cable connected?\n"
                 "Run 'python nxfs.py devices' to see what pyusb can see, and bind WinUSB with Zadig.")
    dev.set_configuration()
    cfg = dev.get_active_configuration()
    intf = cfg[(0, 0)]
    ep_out = usb.util.find_descriptor(
        intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)
    ep_in = usb.util.find_descriptor(
        intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)
    if ep_out is None or ep_in is None:
        sys.exit("Could not find bulk endpoints.")
    return ep_in, ep_out


def send_req(ep_out, cmd, path="", size=0):
    p = path.encode("utf-8")[:REQ_PATH - 1]
    p = p + b"\x00" * (REQ_PATH - len(p))
    ep_out.write(struct.pack(REQ_FMT, MAGIC, cmd, size) + p)


def read_exact(ep_in, n):
    buf = bytearray()
    while len(buf) < n:
        buf += ep_in.read(min(CHUNK, n - len(buf)), timeout=0).tobytes()
    return bytes(buf)


def read_res(ep_in):
    magic, status, size = struct.unpack(RES_FMT, read_exact(ep_in, RES_LEN))
    if magic != MAGIC:
        sys.exit("Protocol error: bad response magic.")
    return status, size


def write_exact(ep_out, data):
    mv = memoryview(data)
    off = 0
    while off < len(mv):
        off += ep_out.write(mv[off:off + CHUNK], timeout=0)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    op = sys.argv[1]

    if op == "devices":
        list_devices()
        return

    ep_in, ep_out = connect()

    if op == "ping":
        send_req(ep_out, CMD_PING)
        st, _ = read_res(ep_in)
        print("pong" if st == 0 else f"error: {STATUS_NAMES.get(st, st)}")

    elif op == "list":
        path = sys.argv[2]
        send_req(ep_out, CMD_LIST, path)
        st, size = read_res(ep_in)
        if st != 0:
            sys.exit(f"error: {STATUS_NAMES.get(st, st)}")
        data = read_exact(ep_in, size).decode("utf-8", "replace")
        for line in data.splitlines():
            t, sz, name = line.split("\t", 2)
            kind = "<DIR>" if t == "D" else f"{int(sz):>12}"
            print(f"{kind}  {name}")

    elif op == "stat":
        path = sys.argv[2]
        send_req(ep_out, CMD_STAT, path)
        st, size = read_res(ep_in)
        if st != 0:
            sys.exit(f"error: {STATUS_NAMES.get(st, st)}")
        is_dir, fsize = struct.unpack("<QQ", read_exact(ep_in, size))
        print(f"{'directory' if is_dir else 'file'}  size={fsize}")

    elif op == "pull":
        remote, local = sys.argv[2], sys.argv[3]
        send_req(ep_out, CMD_PULL, remote)
        st, size = read_res(ep_in)
        if st != 0:
            sys.exit(f"error: {STATUS_NAMES.get(st, st)}")
        got = 0
        with open(local, "wb") as f:
            while got < size:
                chunk = ep_in.read(min(CHUNK, size - got), timeout=0).tobytes()
                f.write(chunk)
                got += len(chunk)
                print(f"\r  {got/1e6:.1f} / {size/1e6:.1f} MB", end="", flush=True)
        print(f"\nSaved {local}")

    elif op == "push":
        local, remote = sys.argv[2], sys.argv[3]
        size = os.path.getsize(local)
        send_req(ep_out, CMD_PUSH, remote, size)
        with open(local, "rb") as f:
            sent = 0
            while sent < size:
                chunk = f.read(CHUNK)
                if not chunk:
                    break
                write_exact(ep_out, chunk)
                sent += len(chunk)
                print(f"\r  {sent/1e6:.1f} / {size/1e6:.1f} MB", end="", flush=True)
        st, _ = read_res(ep_in)
        print(f"\n{'Done' if st == 0 else 'error: ' + STATUS_NAMES.get(st, str(st))}")

    elif op == "mkdir":
        send_req(ep_out, CMD_MKDIR, sys.argv[2])
        st, _ = read_res(ep_in)
        print("OK" if st == 0 else f"error: {STATUS_NAMES.get(st, st)}")

    elif op == "rm":
        send_req(ep_out, CMD_DELETE, sys.argv[2])
        st, _ = read_res(ep_in)
        print("OK" if st == 0 else f"error: {STATUS_NAMES.get(st, st)}")

    else:
        print(__doc__)


if __name__ == "__main__":
    main()
