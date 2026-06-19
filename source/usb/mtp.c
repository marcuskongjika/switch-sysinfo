/*
 * SysInfo - MTP responder
 * Copyright (c) 2025 marcuskongjika
 *
 * Ported from haze, the USB MTP responder of Atmosphere-NX.
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
 * Overview — how MTP/PTP works over USB
 * ======================================
 *
 * MTP (Media Transfer Protocol) is a thin layer on top of PTP (Picture Transfer
 * Protocol, ISO 15740). Every exchange follows a strict 3-phase request/response
 * cycle:
 *
 *   1. Command phase  (host → device, PTP_TYPE_CMD = 1)
 *        12-byte header  [ total_length u32 | type u16 | op_code u16 | trans_id u32 ]
 *        optional params [ up to 5 × u32 ]
 *
 *   2. Data phase     (either direction, PTP_TYPE_DATA = 2) — present for ops
 *        that transfer a payload (GetObject sends device→host; SendObject
 *        receives host→device).  Same 12-byte header; body follows immediately.
 *
 *   3. Response phase (device → host, PTP_TYPE_RESP = 3)
 *        12-byte header + optional response params.  RC_OK (0x2001) = success.
 *
 * Every packet carries the same trans_id as the command that started it.
 * Sessions gate most operations: the host must send OP_OpenSession before
 * asking for files, and we enforce this check in handleCommand().
 *
 * Object model
 * ------------
 * The host identifies files/directories by "object handle" — a u32 assigned by
 * us. We maintain a flat array (g_obj) that maps handle → {path, parent, isdir}.
 * Handle = array_index + 1 (handle 0 is reserved for "none").
 *
 * The database is populated lazily: OP_GetObjectHandles reads a directory and
 * calls dbAdd() for each entry. dbAdd() deduplicates by path, so browsing the
 * same directory twice doesn't create duplicate handles.
 *
 * MTP recognition trick (VendorExtensionID + string)
 * --------------------------------------------------
 * Returning VendorExtensionID=6 and the extension string "microsoft.com: 1.0;"
 * in GetDeviceInfo is what makes Windows treat us as an MTP device (rather than
 * a generic PTP camera). Without it, Windows Media Player would try to talk to
 * us instead of Explorer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <switch.h>
#include "mtp.h"
#include "mtp_usb.h"

/* ===========================================================================
 * PTP / MTP protocol constants
 * =========================================================================*/

/* Container types (field at byte 4 of every container header). */
#define PTP_TYPE_CMD   1
#define PTP_TYPE_DATA  2
#define PTP_TYPE_RESP  3

/* Operation codes — what the host wants us to do. */
#define OP_GetDeviceInfo            0x1001  // enumerate our capabilities
#define OP_OpenSession              0x1002  // begin a session (must come before file ops)
#define OP_CloseSession             0x1003
#define OP_GetStorageIDs            0x1004  // list available storage volumes
#define OP_GetStorageInfo           0x1005  // capacity/free space for a storage
#define OP_GetNumObjects            0x1006  // count objects (we don't implement this)
#define OP_GetObjectHandles         0x1007  // list handles in a directory
#define OP_GetObjectInfo            0x1008  // metadata for one object
#define OP_GetObject                0x1009  // download a file
#define OP_DeleteObject             0x100B  // delete a file or directory
#define OP_SendObjectInfo           0x100C  // first half of upload: send metadata
#define OP_SendObject               0x100D  // second half of upload: send data
#define OP_GetObjectPropsSupported  0x9801  // MTP extension: list supported properties
#define OP_GetObjectPropDesc        0x9802  // describe a property (type, writable?)
#define OP_GetObjectPropValue       0x9803  // read a property from an object
#define OP_SetObjectPropValue       0x9804  // write a property (we support rename via this)
#define OP_GetObjectPropList        0x9805  // bulk-read all properties for one object

/* Response codes returned in the response container. */
#define RC_OK                       0x2001
#define RC_GeneralError             0x2002
#define RC_SessionNotOpen           0x2003
#define RC_OperationNotSupported    0x2005
#define RC_InvalidStorageID         0x2008
#define RC_InvalidObjectHandle      0x2009
#define RC_StoreFull                0x200C
#define RC_InvalidParentObject      0x201A
#define RC_InvalidObjectPropCode    0xA801  // MTP extension response code

/* Object format codes. We only care about two: regular file and directory. */
#define FMT_Undefined   0x3000  // any file format
#define FMT_Association 0x3001  // directory / folder

/* MTP property data types (used in GetObjectPropDesc). */
#define DT_U16   0x0004
#define DT_U32   0x0006
#define DT_U64   0x0008
#define DT_U128  0x000A  // 4× u32, used for PersistentUID
#define DT_STR   0xFFFF

/* Object property codes. */
#define OPC_StorageID     0xDC01  // which storage this object lives on
#define OPC_ObjectFormat  0xDC02  // file type (FMT_*)
#define OPC_ObjectSize    0xDC04  // size in bytes (u64)
#define OPC_FileName      0xDC07  // basename of the file (PTP string, writable)
#define OPC_ParentObject  0xDC0B  // parent handle (0 for root items)
#define OPC_PersistentUID 0xDC41  // stable ID that survives sessions (we fake it)

/* We expose exactly one storage: the SD card. STORAGE_ID is arbitrary but
 * must be non-zero and must match what we return from GetStorageIDs. */
#define STORAGE_ID  0x00010001u

/* The handle value that means "no parent / root level". */
#define ROOT_PARENT 0xFFFFFFFFu

/* Filesystem root on the Switch. devkitPro maps the SD card here. */
#define SD_ROOT     "sdmc:/"

/* Size of our DMA-aligned RX and TX buffers. 1 MiB is large enough for a
 * single MTP container in one shot for all but the largest files. For files
 * bigger than the buffer, we loop in opGetObject / opSendObject. */
#define PKT (1024 * 1024)

/* Advertised capability lists — these tell the host exactly what we support.
 * Only list things we actually handle in the dispatch switch below; advertising
 * something you don't implement causes Windows to throw confusing errors. */
static const u16 SUPPORTED_OPS[] = {
    OP_GetDeviceInfo, OP_OpenSession, OP_CloseSession,
    OP_GetStorageIDs, OP_GetStorageInfo, OP_GetObjectHandles,
    OP_GetObjectInfo, OP_GetObject, OP_SendObjectInfo, OP_SendObject,
    OP_DeleteObject, OP_GetObjectPropsSupported, OP_GetObjectPropDesc,
    OP_GetObjectPropValue, OP_SetObjectPropValue, OP_GetObjectPropList,
};
static const u16 SUPPORTED_PROPS[] = {
    OPC_StorageID, OPC_ObjectFormat, OPC_ObjectSize,
    OPC_FileName, OPC_ParentObject, OPC_PersistentUID,
};
/* Playback formats: file types the device can "play back". We claim both
 * Undefined (files) and Association (directories) so folders are browsable. */
static const u16 PLAYBACK_FORMATS[] = { FMT_Undefined, FMT_Association };

/* ===========================================================================
 * Thread-shared state
 * =========================================================================*/

/* g_st is the public status struct, updated under g_st.lock. The render thread
 * calls mtp_snapshot() to take a mutex-protected copy for the UI. */
static MtpStatus g_st;
static Thread    g_thread;    // the MTP worker thread handle
static volatile bool g_run;   // false = tell the thread to stop cleanly

/* DMA buffers. They MUST be 0x1000-aligned for usbDs PostBufferAsync.
 * __attribute__((aligned)) on a static puts them in BSS at the right alignment. */
static u8 __attribute__((aligned(0x1000))) g_rx[PKT];  // host → us
static u8 __attribute__((aligned(0x1000))) g_tx[PKT];  // us → host

/* Current transaction context (set at the top of handleCommand). */
static u32 g_trans;      // transaction ID echoed in every response
static u32 g_code;       // current operation code (echoed in data containers)
static u32 g_sendHandle; // object handle waiting for the SendObject data phase

/* ===========================================================================
 * Object database: handle ↔ path mapping
 * =========================================================================*/

/*
 * Each entry maps one MTP object handle to a filesystem path plus metadata.
 * The handle is (index + 1) — handle 0 is the reserved "none" value.
 *
 * path[769] is sized to hold "sdmc:/" (6) + 255 path components of up to
 * 255 bytes each, separated by '/' — in practice Switch paths are much shorter.
 */
typedef struct { u32 parent; bool isdir; char path[769]; } Obj;

static Obj *g_obj;          // heap-allocated, grows on demand
static u32  g_objN;         // number of entries currently in use
static u32  g_objCap;       // allocated capacity (slots)

/*
 * dbAdd() - Insert or find a path in the database and return its handle.
 *
 * If the path is already present we return the existing handle (dedup).
 * Otherwise we append a new entry, growing the array if needed.
 * Returns 0 on allocation failure (caller should respond with StoreFull).
 */
static u32 dbAdd(const char *path, u32 parent) {
    /* Check for an existing entry first to avoid duplicate handles. */
    for (u32 i = 0; i < g_objN; i++)
        if (strcmp(g_obj[i].path, path) == 0) return i + 1;

    /* Grow the array if we're at capacity (doubles each time). */
    if (g_objN == g_objCap) {
        u32 nc = g_objCap ? g_objCap * 2 : 256;
        Obj *np = realloc(g_obj, nc * sizeof(Obj));
        if (!np) return 0;
        g_obj = np; g_objCap = nc;
    }

    memset(&g_obj[g_objN], 0, sizeof(Obj));
    snprintf(g_obj[g_objN].path, sizeof(g_obj[g_objN].path), "%s", path);
    g_obj[g_objN].parent = parent;
    return ++g_objN;   // return new handle (pre-increments objN, so handle = objN)
}

/* dbGet() - Return a pointer into g_obj for a given handle, or NULL if invalid. */
static Obj *dbGet(u32 h) { return (h == 0 || h > g_objN) ? NULL : &g_obj[h - 1]; }

/* dbReset() - Wipe all handles. Called on OpenSession so stale handles from a
 * previous session don't confuse a new host connection. */
static void dbReset(void) { g_objN = 0; }

/* ===========================================================================
 * Little-endian buffer builder
 * =========================================================================*/

/*
 * MTP is entirely little-endian. These helpers write typed values at the
 * current offset into an output buffer, advancing the offset automatically.
 * If writing would exceed cap, the offset still advances (so the caller can
 * detect overflow via b.off > b.cap), but nothing is written.
 *
 * We start every response buffer at offset 12 (just past the 12-byte header),
 * then write the header last in commitData() once we know the total length.
 */
typedef struct { u8 *p; u32 off; u32 cap; } Buf;
static void p8 (Buf *b, u8  v) { if (b->off < b->cap) b->p[b->off] = v; b->off++; }
static void p16(Buf *b, u16 v) { p8(b, v); p8(b, v >> 8); }
static void p32(Buf *b, u32 v) { p16(b, v); p16(b, v >> 16); }
static void p64(Buf *b, u64 v) { p32(b, v); p32(b, v >> 32); }

/*
 * pstr() - Write a PTP string: 1-byte count, then UTF-16LE chars, then NUL.
 *
 * PTP strings are NOT null-terminated C strings. The leading byte is the
 * character count (including the trailing NUL), so an empty string is just 0x00.
 * We only encode ASCII (passthrough); non-ASCII becomes '_' since filenames on
 * the Switch SD card are expected to be ASCII-compatible anyway.
 */
static void pstr(Buf *b, const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (!n) { p8(b, 0); return; }
    p8(b, (u8)(n + 1));                       // count includes trailing NUL
    for (size_t i = 0; i < n; i++) p16(b, (u8)s[i]);  // ASCII → UTF-16LE
    p16(b, 0);                                 // UTF-16 NUL terminator
}

/*
 * parr16() - Write a PTP array of u16s: 4-byte count, then elements.
 *
 * Used for capability arrays (supported ops, props, formats). The count is
 * passed as 'n' (number of elements), not byte count.
 */
static void parr16(Buf *b, const u16 *a, u32 n) { p32(b, n); for (u32 i = 0; i < n; i++) p16(b, a[i]); }

/* Little-endian readers for parsing incoming packets. */
static u16 rd16(const u8 *p) { return p[0] | (p[1] << 8); }
static u32 rd32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

/* ===========================================================================
 * Status helpers (thread-safe updates to g_st)
 * =========================================================================*/

/*
 * setOp() - Update the "last command" and optionally "current file" display strings.
 * Called at the start of each handler so the UI always shows what's happening.
 */
static void setOp(const char *op, const char *file) {
    mutexLock(&g_st.lock);
    snprintf(g_st.lastOp, sizeof(g_st.lastOp), "%s", op);
    if (file) snprintf(g_st.curFile, sizeof(g_st.curFile), "%s", file);
    mutexUnlock(&g_st.lock);
}

/* setActive() - Update the transfer progress counters seen by the USB tab. */
static void setActive(bool a, u64 done, u64 total) {
    mutexLock(&g_st.lock);
    g_st.active = a; g_st.bytesDone = done; g_st.bytesTotal = total;
    mutexUnlock(&g_st.lock);
}

/* ===========================================================================
 * Transport wrappers (TX side)
 * =========================================================================*/

/*
 * dataBuf() - Return a Buf starting at byte 12 of g_tx (leaving room for the header).
 * Use with commitData() to finalize and send the full container.
 */
static Buf dataBuf(void) { Buf b = { g_tx, 12, sizeof(g_tx) }; return b; }

/*
 * commitData() - Fill in the 12-byte header and send the complete data container.
 *
 * Header layout (little-endian):
 *   [0..3]  total container length (header + payload)
 *   [4..5]  container type (PTP_TYPE_DATA = 2)
 *   [6..7]  operation code (echoed from the command that requested this data)
 *   [8..11] transaction ID (echoed)
 */
static void commitData(Buf *b) {
    u32 plen = b->off - 12;   // payload bytes (everything after the header)
    Buf h = { g_tx, 0, 12 };
    p32(&h, 12 + plen); p16(&h, PTP_TYPE_DATA); p16(&h, g_code); p32(&h, g_trans);
    mtpUsbWrite(g_tx, 12 + plen, &g_run);
}

/*
 * sendResponse() - Send a response container with up to 5 u32 params.
 * np = number of valid entries in params (may be 0 for a plain status response).
 */
static void sendResponse(u16 code, const u32 *params, int np) {
    Buf b = { g_tx, 0, sizeof(g_tx) };
    p32(&b, 12 + np * 4); p16(&b, PTP_TYPE_RESP); p16(&b, code); p32(&b, g_trans);
    for (int i = 0; i < np; i++) p32(&b, params[i]);
    mtpUsbWrite(g_tx, b.off, &g_run);
}
static void respOK(void)        { sendResponse(RC_OK, NULL, 0); }
static void respErr(u16 code)   { sendResponse(code, NULL, 0); }

/* ===========================================================================
 * Filesystem helpers
 * =========================================================================*/

/* baseName() - Return a pointer to the last path component (after the final '/'). */
static const char *baseName(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/*
 * joinPath() - Concatenate dir + '/' + name into out, handling trailing slashes.
 * "sdmc:/" already ends with '/', so we don't double it.
 */
static void joinPath(char *out, size_t n, const char *dir, const char *name) {
    size_t l = strlen(dir);
    if (l && dir[l - 1] == '/') snprintf(out, n, "%s%s", dir, name);
    else                        snprintf(out, n, "%s/%s", dir, name);
}

/*
 * objMeta() - Stat an object and return its size and directory flag.
 * Falls back to the database's isdir hint if stat() fails (e.g. file deleted).
 */
static void objMeta(Obj *o, u64 *size, bool *isdir) {
    struct stat s;
    *size = 0; *isdir = o->isdir;
    if (stat(o->path, &s) == 0) {
        *isdir = S_ISDIR(s.st_mode);
        if (!*isdir) *size = (u64)s.st_size;
    }
}

/*
 * recursiveDelete() - Delete a file or an entire directory tree.
 * Used by opDeleteObject(); MTP requires that deleting a folder deletes all
 * of its contents too.
 */
static void recursiveDelete(const char *path) {
    struct stat s;
    if (stat(path, &s) != 0) return;
    if (S_ISDIR(s.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                char child[800];
                joinPath(child, sizeof(child), path, e->d_name);
                recursiveDelete(child);   // depth-first
            }
            closedir(d);
        }
        rmdir(path);    // now empty, safe to remove
    } else {
        remove(path);
    }
}

/* ===========================================================================
 * MTP operation handlers
 * =========================================================================*/

/*
 * opGetDeviceInfo() - Advertise our identity and capabilities.
 *
 * Key fields the host cares about:
 *   VendorExtensionID = 6, VendorExtensionDesc = "microsoft.com: 1.0;"
 *     → Windows treats us as MTP rather than a plain PTP camera.
 *   SupportedOperations  → what op codes we handle (must match SUPPORTED_OPS)
 *   PlaybackFormats      → object types we serve
 */
static void opGetDeviceInfo(void) {
    Buf b = dataBuf();
    p16(&b, 100);                                             // StandardVersion = 1.00
    p32(&b, 6);                                               // VendorExtensionID = MTP
    p16(&b, 100);                                             // VendorExtensionVersion
    pstr(&b, "microsoft.com: 1.0;");                          // makes Windows use MTP
    p16(&b, 0);                                               // FunctionalMode = standard
    parr16(&b, SUPPORTED_OPS, sizeof(SUPPORTED_OPS) / 2);
    p32(&b, 0);                                               // EventsSupported (none)
    p32(&b, 0);                                               // DevicePropertiesSupported (none)
    p32(&b, 0);                                               // CaptureFormats (none)
    parr16(&b, PLAYBACK_FORMATS, sizeof(PLAYBACK_FORMATS) / 2);
    pstr(&b, "marcuskongjika");                               // manufacturer
    pstr(&b, "SysInfo MTP Responder");                        // model
    pstr(&b, "1.2");                                          // device version
    pstr(&b, "SInfo");                                        // serial
    commitData(&b);
    respOK();
}

/* opGetStorageIDs() - Tell the host there is exactly one storage: the SD card. */
static void opGetStorageIDs(void) {
    Buf b = dataBuf();
    p32(&b, 1); p32(&b, STORAGE_ID);   // count=1, then the single ID
    commitData(&b);
    respOK();
}

/*
 * opGetStorageInfo() - Capacity and free space for the SD card.
 *
 * StorageType   0x0004 = RemovableRAM (closest match for an SD card)
 * FilesystemType 0x0002 = GenericHierarchical (supports folders)
 * AccessCapability 0x0000 = read-write
 * FreeSpaceInObjects 0xFFFFFFFF = unknown (we don't count inodes)
 */
static void opGetStorageInfo(void) {
    u64 total = 0, freeb = 0;
    struct statvfs sv;
    if (statvfs(SD_ROOT, &sv) == 0) {
        total = (u64)sv.f_blocks * sv.f_frsize;
        freeb = (u64)sv.f_bfree  * sv.f_frsize;
    }
    Buf b = dataBuf();
    p16(&b, 0x0004);        // StorageType = RemovableRAM
    p16(&b, 0x0002);        // FilesystemType = GenericHierarchical
    p16(&b, 0x0000);        // AccessCapability = read-write
    p64(&b, total);
    p64(&b, freeb);
    p32(&b, 0xFFFFFFFF);    // FreeSpaceInObjects = unknown
    pstr(&b, "SD Card");
    pstr(&b, "");           // volume identifier (empty)
    commitData(&b);
    respOK();
}

/*
 * opGetObjectHandles() - List child handles of a directory (or the SD root).
 *
 * MTP params p[0]=storage, p[1]=format filter (ignored), p[2]=parent handle.
 *
 * We open the directory, call dbAdd() for every entry (dedup keeps handles
 * stable across multiple listings), and emit the handles as a u32 array.
 * The total count is written as a placeholder first, then patched after we
 * know how many entries we found (we can't know ahead of time without a
 * separate opendir pass).
 */
static void opGetObjectHandles(u32 parent) {
    char base[770];
    u32 ph;

    /* Resolve parent: ROOT_PARENT (0xFFFFFFFF) or 0 means the SD root. */
    if (parent == ROOT_PARENT || parent == 0) {
        snprintf(base, sizeof(base), "%s", SD_ROOT);
        ph = 0;
    } else {
        Obj *o = dbGet(parent);
        if (!o) { respErr(RC_InvalidObjectHandle); return; }
        snprintf(base, sizeof(base), "%s", o->path);
        ph = parent;
    }

    Buf b = dataBuf();
    u32 countOff = b.off;   // remember where we put the placeholder count
    p32(&b, 0);             // placeholder — patched below
    u32 cnt = 0;

    DIR *d = opendir(base);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char full[800];
            joinPath(full, sizeof(full), base, e->d_name);
            u32 h = dbAdd(full, ph);
            if (!h) continue;   // allocation failure — skip entry
            /* Cache the isdir flag while we have the stat result handy. */
            Obj *o = dbGet(h);
            struct stat s;
            if (stat(full, &s) == 0) o->isdir = S_ISDIR(s.st_mode);
            p32(&b, h);
            cnt++;
        }
        closedir(d);
    }
    /* Patch the count field in-place now that we know how many handles we wrote. */
    g_tx[countOff + 0] = cnt;       g_tx[countOff + 1] = cnt >> 8;
    g_tx[countOff + 2] = cnt >> 16; g_tx[countOff + 3] = cnt >> 24;

    commitData(&b);
    respOK();
}

/*
 * opGetObjectInfo() - Return metadata for a single object.
 *
 * The full ObjectInfo dataset is described in the PTP spec. Many fields
 * (thumbnail dimensions, image depth, etc.) are irrelevant for file transfer
 * and we send zeros. The ones that matter to Explorer/Finder are:
 *   ObjectFormat (file vs. folder), ObjectCompressedSize, Filename.
 * Sizes > 4 GB are capped at 0xFFFFFFFF in the 32-bit size field; the real
 * size is always available via the 64-bit OPC_ObjectSize property.
 */
static void opGetObjectInfo(u32 handle) {
    Obj *o = dbGet(handle);
    if (!o) { respErr(RC_InvalidObjectHandle); return; }
    u64 size; bool isdir;
    objMeta(o, &size, &isdir);

    Buf b = dataBuf();
    p32(&b, STORAGE_ID);
    p16(&b, isdir ? FMT_Association : FMT_Undefined);
    p16(&b, 0);                                            // ProtectionStatus = none
    p32(&b, size > 0xFFFFFFFF ? 0xFFFFFFFF : (u32)size);  // capped 32-bit size
    p16(&b, 0); p32(&b, 0); p32(&b, 0); p32(&b, 0);       // thumbnail type/size/w/h
    p32(&b, 0); p32(&b, 0); p32(&b, 0);                   // image w/h/bit-depth
    p32(&b, o->parent);
    p16(&b, isdir ? 1 : 0);    // AssociationType: 1 = generic folder
    p32(&b, 0);                 // AssociationDesc
    p32(&b, 0);                 // SequenceNumber
    pstr(&b, baseName(o->path));
    pstr(&b, "");   // capture date (empty)
    pstr(&b, "");   // modification date (empty)
    pstr(&b, "");   // keywords (empty)
    commitData(&b);
    respOK();
}

/*
 * opGetObject() - Stream a file to the host.
 *
 * The data phase header must declare the total file size upfront, then the
 * file content follows immediately in the same packet (if it fits), and
 * continues in subsequent bulk writes until all bytes are sent.
 *
 * We use a 1 MiB transfer buffer (g_tx).  The first write contains the
 * 12-byte header + as much file data as fits.  Subsequent writes are pure
 * file data with no header.
 */
static void opGetObject(u32 handle) {
    Obj *o = dbGet(handle);
    if (!o) { respErr(RC_InvalidObjectHandle); return; }
    FILE *f = fopen(o->path, "rb");
    if (!f) { respErr(RC_GeneralError); return; }
    setOp("GetObject", baseName(o->path));

    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) sz = 0;

    /* Build the data container header in the first 12 bytes of g_tx. */
    Buf h = { g_tx, 0, 12 };
    p32(&h, 12 + (u32)sz); p16(&h, PTP_TYPE_DATA); p16(&h, g_code); p32(&h, g_trans);
    setActive(true, 0, (u64)sz);

    /* Fill the remainder of g_tx with the start of the file and send it. */
    size_t firstCap = sizeof(g_tx) - 12;
    size_t rd = fread(g_tx + 12, 1, firstCap, f);
    if (R_FAILED(mtpUsbWrite(g_tx, 12 + rd, &g_run))) { fclose(f); setActive(false, 0, 0); return; }
    size_t done = rd;
    setActive(true, done, (u64)sz);

    /* Stream the rest of the file in buffer-sized chunks. */
    while (done < (size_t)sz) {
        size_t want = (size_t)sz - done;
        if (want > sizeof(g_tx)) want = sizeof(g_tx);
        size_t r = fread(g_tx, 1, want, f);
        if (r == 0) break;
        if (R_FAILED(mtpUsbWrite(g_tx, r, &g_run))) break;
        done += r;
        setActive(true, done, (u64)sz);
    }
    fclose(f);
    setActive(false, 0, 0);
    mutexLock(&g_st.lock); g_st.filesOut++; mutexUnlock(&g_st.lock);
    respOK();
}

/*
 * readPtpStr() - Decode a PTP string from a raw byte pointer into a C string.
 *
 * PTP string format: [count u8][char0 u16le][char1 u16le]...[NUL u16le]
 * count includes the trailing NUL. We only handle ASCII (< 128); anything
 * wider gets replaced with '_'.
 */
static void readPtpStr(const u8 *p, char *out, size_t outsz) {
    u8 n = p[0]; size_t j = 0;
    for (u8 i = 0; i < n && j < outsz - 1; i++) {
        u16 c = p[1 + i * 2] | (p[2 + i * 2] << 8);
        if (!c) break;                              // stop at the UTF-16 NUL
        out[j++] = (c < 128) ? (char)c : '_';
    }
    out[j] = 0;
}

/*
 * opSendObjectInfo() - First half of a file upload: receive metadata.
 *
 * The host sends an ObjectInfo dataset describing the incoming object.
 * We extract the name and format (file vs. directory) from it, register the
 * target path in the database, and reply with [storage, parent, new_handle].
 *
 * If it's a directory, we create it immediately (mkdir) and set g_sendHandle=0
 * so the following OP_SendObject (if any) knows there's no file to receive.
 * If it's a file, we create an empty placeholder and set g_sendHandle so
 * opSendObject() knows which path to write to.
 */
static void opSendObjectInfo(u32 parentParam) {
    u32 tr = 0;
    /* The ObjectInfo dataset has a minimum of 65 bytes (12 header + 53 fixed fields). */
    if (R_FAILED(mtpUsbRead(g_rx, sizeof(g_rx), &tr, &g_run)) || tr < 12 + 53) {
        respErr(RC_GeneralError); return;
    }
    const u8 *pl = g_rx + 12;          // skip the 12-byte container header
    u16 format = rd16(pl + 4);         // ObjectFormat is at byte 4 of the dataset
    char name[300];
    readPtpStr(pl + 52, name, sizeof(name));   // Filename PTP string at byte 52
    if (!name[0]) { respErr(RC_GeneralError); return; }

    /* Resolve parent directory (same logic as GetObjectHandles). */
    char base[770], full[800];
    u32 ph;
    if (parentParam == ROOT_PARENT || parentParam == 0) {
        snprintf(base, sizeof(base), "%s", SD_ROOT);
        ph = 0;
    } else {
        Obj *po = dbGet(parentParam);
        if (!po) { respErr(RC_InvalidParentObject); return; }
        snprintf(base, sizeof(base), "%s", po->path);
        ph = parentParam;
    }
    joinPath(full, sizeof(full), base, name);

    u32 handle = dbAdd(full, ph);
    if (!handle) { respErr(RC_StoreFull); return; }
    Obj *o = dbGet(handle);
    o->isdir = (format == FMT_Association);

    setOp("SendObjectInfo", name);
    if (o->isdir) {
        mkdir(full, 0777);
        g_sendHandle = 0;   // no data phase needed for directories
    } else {
        /* Touch the file so it exists (overwritten in opSendObject). */
        FILE *f = fopen(full, "wb"); if (f) fclose(f);
        g_sendHandle = handle;
    }

    /* MTP requires the response to echo the storage, parent handle, and new handle. */
    u32 rp[3] = { STORAGE_ID, ph, handle };
    sendResponse(RC_OK, rp, 3);
}

/*
 * opSendObject() - Second half of a file upload: receive the file data.
 *
 * The host sends a data container whose payload is the file contents.
 * The 4-byte total length at the start of the container includes the 12-byte
 * header, so the file byte count is (container_length - 12).
 *
 * We write the portion of data that arrived in the first read directly, then
 * loop reading more from the bulk-out endpoint until all bytes are written.
 */
static void opSendObject(void) {
    Obj *o = dbGet(g_sendHandle);
    if (!o) { respErr(RC_InvalidObjectHandle); return; }

    u32 tr = 0;
    if (R_FAILED(mtpUsbRead(g_rx, sizeof(g_rx), &tr, &g_run)) || tr < 12) {
        respErr(RC_GeneralError); return;
    }
    u32 len = rd32(g_rx);                            // total container length
    u64 fileBytes = (len >= 12) ? (len - 12) : 0;   // subtract the 12-byte header

    FILE *f = fopen(o->path, "wb");
    setOp("SendObject", baseName(o->path));
    setActive(true, 0, fileBytes);

    /* Write the data that came in with the first read (after the 12-byte header). */
    u64 written = 0;
    u32 have = (tr > 12) ? tr - 12 : 0;
    if (have) { if (f) fwrite(g_rx + 12, 1, have, f); written += have; }
    setActive(true, written, fileBytes);

    /* Keep reading until we have all the bytes the container promised. */
    while (written < fileBytes) {
        u32 t = 0;
        if (R_FAILED(mtpUsbRead(g_rx, sizeof(g_rx), &t, &g_run)) || t == 0) break;
        u32 w = t;
        if (written + w > fileBytes) w = (u32)(fileBytes - written);  // don't overshoot
        if (f) fwrite(g_rx, 1, w, f);
        written += w;
        setActive(true, written, fileBytes);
    }
    if (f) fclose(f);
    setActive(false, 0, 0);
    g_sendHandle = 0;
    if (f) { mutexLock(&g_st.lock); g_st.filesIn++; mutexUnlock(&g_st.lock); }
    sendResponse(f ? RC_OK : RC_GeneralError, NULL, 0);
}

/* opDeleteObject() - Delete a file or directory (recursively for directories). */
static void opDeleteObject(u32 handle) {
    Obj *o = dbGet(handle);
    if (!o) { respErr(RC_InvalidObjectHandle); return; }
    setOp("DeleteObject", baseName(o->path));
    recursiveDelete(o->path);
    respOK();
}

/* ===========================================================================
 * Object property handlers
 * =========================================================================*/

/* opPropsSupported() - List which property codes we handle for any object. */
static void opPropsSupported(void) {
    Buf b = dataBuf();
    parr16(&b, SUPPORTED_PROPS, sizeof(SUPPORTED_PROPS) / 2);
    commitData(&b);
    respOK();
}

/* propType() - Return the MTP data type for a property code. */
static u16 propType(u16 code) {
    switch (code) {
        case OPC_ObjectFormat: return DT_U16;
        case OPC_ObjectSize:   return DT_U64;
        case OPC_FileName:     return DT_STR;
        case OPC_PersistentUID:return DT_U128;
        default:               return DT_U32;   // StorageID, ParentObject
    }
}

/*
 * opPropDesc() - Describe one property: type, read/write flag, default value.
 *
 * The get/set byte controls whether the host can write this property back via
 * SetObjectPropValue. We advertise ObjectFileName (OPC_FileName) as writable
 * (1) because that's how MTP rename works; everything else is read-only (0).
 *
 * The default value is just a zero / empty string; we don't bother with
 * meaningful defaults since the host typically doesn't use them for files.
 */
static void opPropDesc(u16 code) {
    u16 ty = propType(code);
    Buf b = dataBuf();
    p16(&b, code);
    p16(&b, ty);
    p8(&b, code == OPC_FileName ? 1 : 0);  // 1 = get/set, 0 = read-only
    switch (ty) {                           // default value (type-specific encoding)
        case DT_U16:  p16(&b, 0); break;
        case DT_U64:  p64(&b, 0); break;
        case DT_U128: p64(&b, 0); p64(&b, 0); break;
        case DT_STR:  pstr(&b, ""); break;
        default:      p32(&b, 0); break;
    }
    p32(&b, 0);   // GroupCode (not used)
    p8(&b, 0);    // FormFlag = none (no range/enum constraint)
    commitData(&b);
    respOK();
}

/*
 * appendPropValue() - Write one property value into a Buf (shared by opPropValue
 * and opPropList, which needs to batch all props in a single response).
 *
 * PersistentUID is a u128 (4× u32). We use the handle as the low word and pad
 * with zeros; it's stable within a session, which is good enough for the spec.
 */
static void appendPropValue(Buf *b, u32 handle, Obj *o, u16 code) {
    u64 size; bool isdir; objMeta(o, &size, &isdir);
    switch (code) {
        case OPC_StorageID:     p32(b, STORAGE_ID); break;
        case OPC_ObjectFormat:  p16(b, isdir ? FMT_Association : FMT_Undefined); break;
        case OPC_ObjectSize:    p64(b, size); break;
        case OPC_ParentObject:  p32(b, o->parent); break;
        case OPC_FileName:      pstr(b, baseName(o->path)); break;
        case OPC_PersistentUID: p32(b, handle); p32(b, 0); p32(b, 0); p32(b, 0); break;
        default: break;
    }
}

/* opPropValue() - Return the current value of a single property for one object. */
static void opPropValue(u32 handle, u16 code) {
    Obj *o = dbGet(handle);
    if (!o) { respErr(RC_InvalidObjectHandle); return; }
    Buf b = dataBuf();
    appendPropValue(&b, handle, o, code);
    commitData(&b);
    respOK();
}

/*
 * opPropList() - Return all supported properties for one object in one response.
 *
 * The format is: count u32, then for each property:
 *   handle u32 | prop_code u16 | data_type u16 | value (variable)
 *
 * This is the bulk variant that Windows Explorer uses to enumerate a folder
 * efficiently (one round-trip per object instead of six).
 */
static void opPropList(u32 handle) {
    Obj *o = dbGet(handle);
    if (!o) { respErr(RC_InvalidObjectHandle); return; }

    Buf b = dataBuf();
    p32(&b, (u32)(sizeof(SUPPORTED_PROPS) / 2));   // total property count
    for (u32 i = 0; i < sizeof(SUPPORTED_PROPS) / 2; i++) {
        u16 code = SUPPORTED_PROPS[i];
        p32(&b, handle);
        p16(&b, code);
        p16(&b, propType(code));
        appendPropValue(&b, handle, o, code);
    }
    commitData(&b);
    respOK();
}

/*
 * opSetPropValue() - Handle a property write from the host.
 *
 * The only writable property we expose is OPC_FileName (ObjectFileName), which
 * is how MTP implements rename: the host sends SetObjectPropValue with the
 * object's handle, property code 0xDC07, and a new name in the data phase.
 *
 * Steps:
 *   1. Read the data container carrying the new name.
 *   2. Decode the PTP string from the payload.
 *   3. Build the new path by keeping the parent directory and swapping the name.
 *   4. Call POSIX rename() to atomically move the file on the filesystem.
 *   5. Patch every entry in the object database that was under the old path
 *      (handles must keep working after the rename, especially for directories
 *      whose children were already enumerated).
 */
static void opSetPropValue(u32 handle, u16 code) {
    /* Read the data container carrying the new value. */
    u32 tr = 0;
    if (R_FAILED(mtpUsbRead(g_rx, sizeof(g_rx), &tr, &g_run)) || tr < 12) {
        respErr(RC_GeneralError); return;
    }
    Obj *o = dbGet(handle);
    if (!o) { respErr(RC_InvalidObjectHandle); return; }

    /* We only allow renaming (ObjectFileName). Everything else is read-only. */
    if (code != OPC_FileName) { respErr(RC_InvalidObjectPropCode); return; }

    /* The new name is a PTP string starting at byte 12 of the data container. */
    char name[300];
    readPtpStr(g_rx + 12, name, sizeof(name));
    if (!name[0]) { respErr(RC_GeneralError); return; }

    /* Copy the old path before we start mutating db entries (o points into g_obj). */
    char oldPath[800];
    snprintf(oldPath, sizeof(oldPath), "%s", o->path);

    /* Isolate the parent directory portion by truncating at the last '/'. */
    char dir[800];
    snprintf(dir, sizeof(dir), "%s", oldPath);
    char *slash = strrchr(dir, '/');
    if (!slash) { respErr(RC_GeneralError); return; }
    *slash = '\0';          // dir = parent portion  (e.g. "sdmc:/folder")

    char newPath[820];
    joinPath(newPath, sizeof(newPath), dir, name);

    setOp("Rename", name);
    if (rename(oldPath, newPath) != 0) { respErr(RC_GeneralError); return; }

    /* Update the database: patch the renamed entry itself, and if it was a
     * directory, rewrite every child path that started with oldPath + '/'.
     * Without this, children of a renamed directory would have stale paths
     * and any subsequent file access would fail. */
    size_t oldLen = strlen(oldPath);
    for (u32 i = 0; i < g_objN; i++) {
        if (strcmp(g_obj[i].path, oldPath) == 0) {
            /* This is the entry that was renamed. */
            snprintf(g_obj[i].path, sizeof(g_obj[i].path), "%s", newPath);
        } else if (strncmp(g_obj[i].path, oldPath, oldLen) == 0 &&
                   g_obj[i].path[oldLen] == '/') {
            /* This is a descendant: replace the leading oldPath with newPath. */
            char tmp[820];
            snprintf(tmp, sizeof(tmp), "%s%s", newPath, g_obj[i].path + oldLen);
            snprintf(g_obj[i].path, sizeof(g_obj[i].path), "%s", tmp);
        }
    }
    respOK();
}

/* ===========================================================================
 * Command dispatch
 * =========================================================================*/

/*
 * handleCommand() - Parse a received command container and call the handler.
 *
 * Container layout (all little-endian):
 *   [0..3]   total length (header + params)
 *   [4..5]   container type (must be PTP_TYPE_CMD = 1)
 *   [6..7]   operation code
 *   [8..11]  transaction ID
 *   [12+]    up to 5 × u32 parameters (presence depends on the operation)
 *
 * Most operations require an open session. The two exceptions are GetDeviceInfo
 * (needed by Windows before opening a session to check compatibility) and
 * OpenSession itself.
 */
static void handleCommand(const u8 *cmd, u32 len) {
    u16 type = rd16(cmd + 4);
    g_code   = rd16(cmd + 6);
    g_trans  = rd32(cmd + 8);
    if (type != PTP_TYPE_CMD) return;   // ignore non-command containers

    /* Parse up to 5 parameters from the tail of the command container. */
    u32 p[5] = {0};
    int np = (len >= 12) ? (int)((len - 12) / 4) : 0;
    if (np > 5) np = 5;
    for (int i = 0; i < np; i++) p[i] = rd32(cmd + 12 + i * 4);

    /* Session gate. */
    if (!g_st.sessionOpen && g_code != OP_OpenSession && g_code != OP_GetDeviceInfo) {
        respErr(RC_SessionNotOpen);
        return;
    }

    switch (g_code) {
        case OP_GetDeviceInfo:           opGetDeviceInfo();        break;
        case OP_OpenSession:             g_st.sessionOpen = true; dbReset(); respOK(); break;
        case OP_CloseSession:            g_st.sessionOpen = false; respOK(); break;
        case OP_GetStorageIDs:           opGetStorageIDs();        break;
        case OP_GetStorageInfo:          opGetStorageInfo();       break;
        /* GetObjectHandles: p[0]=storage, p[1]=format filter, p[2]=parent */
        case OP_GetObjectHandles:        opGetObjectHandles(p[2]); break;
        case OP_GetObjectInfo:           opGetObjectInfo(p[0]);    break;
        case OP_GetObject:               opGetObject(p[0]);        break;
        /* SendObjectInfo: p[0]=storage, p[1]=parent handle */
        case OP_SendObjectInfo:          opSendObjectInfo(p[1]);   break;
        case OP_SendObject:              opSendObject();           break;
        case OP_DeleteObject:            opDeleteObject(p[0]);     break;
        case OP_GetObjectPropsSupported: opPropsSupported();       break;
        /* GetObjectPropDesc: p[0]=prop code, p[1]=format (ignored) */
        case OP_GetObjectPropDesc:       opPropDesc((u16)p[0]);    break;
        /* GetObjectPropValue: p[0]=handle, p[1]=prop code */
        case OP_GetObjectPropValue:      opPropValue(p[0], (u16)p[1]); break;
        /* SetObjectPropValue: p[0]=handle, p[1]=prop code (new value in data phase) */
        case OP_SetObjectPropValue:      opSetPropValue(p[0], (u16)p[1]); break;
        case OP_GetObjectPropList:       opPropList(p[0]);         break;
        default:                         respErr(RC_OperationNotSupported); break;
    }
}

/* ===========================================================================
 * Worker thread
 * =========================================================================*/

/*
 * mtpThread() - The MTP responder's main loop. Runs on its own thread so it
 * doesn't block the render loop.
 *
 * Structure:
 *   1. If USB not configured (no host), wait for a state-change event.
 *   2. Otherwise, block on a 0x400-byte read waiting for a command container.
 *      (0x400 = 1024 bytes covers any command — max params = 5 × u32 + 12 header = 32 bytes.)
 *   3. On receipt, dispatch to handleCommand().
 *   4. Loop until g_run is set to false.
 *
 * The configured flag is refreshed each iteration so the UI shows "Connected"
 * as soon as a host appears and "Waiting" immediately when it disconnects.
 */
static void mtpThread(void *arg) {
    (void)arg;
    while (g_run) {
        bool cfg = mtpUsbConfigured();
        mutexLock(&g_st.lock); g_st.configured = cfg; mutexUnlock(&g_st.lock);

        if (!cfg) {
            /* Sleep up to 1 second waiting for a USB state change (plug/unplug). */
            mtpUsbWaitChange(1000000000ULL);
            continue;
        }

        /* Block until we receive a command container (at most 0x400 bytes). */
        u32 tr = 0;
        Result rc = mtpUsbRead(g_rx, 0x400, &tr, &g_run);
        if (R_FAILED(rc) || tr < 12) {
            if (!g_run) break;   // stop flag set while waiting — clean exit
            continue;
        }
        handleCommand(g_rx, tr);
    }
}

/* ===========================================================================
 * Public API
 * =========================================================================*/

/*
 * mtp_start() - Initialize USB and launch the MTP worker thread.
 *
 * Thread stack: 256 KiB (0x40000). Priority 0x2C, core -2 (any core).
 * Returns false if USB init failed or thread creation failed; the UI will
 * show "MTP Service: Unavailable" in that case.
 */
bool mtp_start(void) {
    memset(&g_st, 0, sizeof(g_st));
    mutexInit(&g_st.lock);

    if (!mtpUsbInit()) return false;
    g_st.initialized = true;

    g_run = true;
    if (R_FAILED(threadCreate(&g_thread, mtpThread, NULL, NULL, 0x40000, 0x2C, -2))) {
        mtpUsbExit();
        g_st.initialized = false;
        g_run = false;
        return false;
    }
    threadStart(&g_thread);
    return true;
}

/*
 * mtp_stop() - Signal the thread to exit and tear down USB.
 *
 * Setting g_run=false causes the thread's eventWait / mtpUsbRead loop to bail.
 * Calling mtpUsbExit() additionally cancels any pending DMA transfer so the
 * thread doesn't block indefinitely inside epXfer().
 */
void mtp_stop(void) {
    if (!g_st.initialized) return;
    g_run = false;
    mtpUsbExit();               // unblocks any pending epXfer wait
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
    free(g_obj); g_obj = NULL; g_objN = g_objCap = 0;
    g_st.initialized = false;
}

/*
 * mtp_snapshot() - Copy the current MtpStatus into *out under the mutex.
 * Called from the render thread; safe to call at any time.
 */
void mtp_snapshot(MtpStatus *out) {
    mutexLock(&g_st.lock);
    *out = g_st;
    mutexUnlock(&g_st.lock);
}
