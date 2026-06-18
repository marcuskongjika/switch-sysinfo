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

// ---- PTP / MTP constants ----------------------------------------------------

#define PTP_TYPE_CMD   1
#define PTP_TYPE_DATA  2
#define PTP_TYPE_RESP  3

#define OP_GetDeviceInfo            0x1001
#define OP_OpenSession              0x1002
#define OP_CloseSession             0x1003
#define OP_GetStorageIDs            0x1004
#define OP_GetStorageInfo           0x1005
#define OP_GetNumObjects            0x1006
#define OP_GetObjectHandles         0x1007
#define OP_GetObjectInfo            0x1008
#define OP_GetObject                0x1009
#define OP_DeleteObject             0x100B
#define OP_SendObjectInfo           0x100C
#define OP_SendObject               0x100D
#define OP_GetObjectPropsSupported  0x9801
#define OP_GetObjectPropDesc        0x9802
#define OP_GetObjectPropValue       0x9803
#define OP_SetObjectPropValue       0x9804
#define OP_GetObjectPropList        0x9805

#define RC_OK                       0x2001
#define RC_GeneralError             0x2002
#define RC_SessionNotOpen           0x2003
#define RC_OperationNotSupported    0x2005
#define RC_InvalidStorageID         0x2008
#define RC_InvalidObjectHandle      0x2009
#define RC_StoreFull                0x200C
#define RC_InvalidParentObject      0x201A
#define RC_InvalidObjectPropCode    0xA801

#define FMT_Undefined   0x3000
#define FMT_Association 0x3001

#define DT_U16   0x0004
#define DT_U32   0x0006
#define DT_U64   0x0008
#define DT_U128  0x000A
#define DT_STR   0xFFFF

#define OPC_StorageID     0xDC01
#define OPC_ObjectFormat  0xDC02
#define OPC_ObjectSize    0xDC04
#define OPC_FileName      0xDC07
#define OPC_ParentObject  0xDC0B
#define OPC_PersistentUID 0xDC41

#define STORAGE_ID  0x00010001u
#define ROOT_PARENT 0xFFFFFFFFu
#define SD_ROOT     "sdmc:/"

#define PKT (1024 * 1024)

static const u16 SUPPORTED_OPS[] = {
    OP_GetDeviceInfo, OP_OpenSession, OP_CloseSession,
    OP_GetStorageIDs, OP_GetStorageInfo, OP_GetObjectHandles,
    OP_GetObjectInfo, OP_GetObject, OP_SendObjectInfo, OP_SendObject,
    OP_DeleteObject, OP_GetObjectPropsSupported, OP_GetObjectPropDesc,
    OP_GetObjectPropValue, OP_GetObjectPropList,
};
static const u16 SUPPORTED_PROPS[] = {
    OPC_StorageID, OPC_ObjectFormat, OPC_ObjectSize,
    OPC_FileName, OPC_ParentObject, OPC_PersistentUID,
};
static const u16 PLAYBACK_FORMATS[] = { FMT_Undefined, FMT_Association };

// ---- state ------------------------------------------------------------------

static MtpStatus g_st;
static Thread    g_thread;
static volatile bool g_run;

static u8 __attribute__((aligned(0x1000))) g_rx[PKT];
static u8 __attribute__((aligned(0x1000))) g_tx[PKT];

static u32 g_trans;      // current transaction id
static u32 g_code;       // current operation code
static u32 g_sendHandle; // pending SendObject target

// ---- object database (handle <-> path) --------------------------------------

typedef struct { u32 parent; bool isdir; char path[769]; } Obj;
static Obj *g_obj;
static u32  g_objN, g_objCap;

static u32 dbAdd(const char *path, u32 parent) {
    for (u32 i = 0; i < g_objN; i++)
        if (strcmp(g_obj[i].path, path) == 0) return i + 1;
    if (g_objN == g_objCap) {
        u32 nc = g_objCap ? g_objCap * 2 : 256;
        Obj *np = realloc(g_obj, nc * sizeof(Obj));
        if (!np) return 0;
        g_obj = np; g_objCap = nc;
    }
    memset(&g_obj[g_objN], 0, sizeof(Obj));
    snprintf(g_obj[g_objN].path, sizeof(g_obj[g_objN].path), "%s", path);
    g_obj[g_objN].parent = parent;
    return ++g_objN;
}
static Obj *dbGet(u32 h) { return (h == 0 || h > g_objN) ? NULL : &g_obj[h - 1]; }

static void dbReset(void) { g_objN = 0; }

// ---- little-endian buffer builder -------------------------------------------

typedef struct { u8 *p; u32 off; u32 cap; } Buf;
static void p8 (Buf *b, u8  v) { if (b->off < b->cap) b->p[b->off] = v; b->off++; }
static void p16(Buf *b, u16 v) { p8(b, v); p8(b, v >> 8); }
static void p32(Buf *b, u32 v) { p16(b, v); p16(b, v >> 16); }
static void p64(Buf *b, u64 v) { p32(b, v); p32(b, v >> 32); }
static void pstr(Buf *b, const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (!n) { p8(b, 0); return; }
    p8(b, (u8)(n + 1));
    for (size_t i = 0; i < n; i++) p16(b, (u8)s[i]);
    p16(b, 0);
}
static void parr16(Buf *b, const u16 *a, u32 n) { p32(b, n); for (u32 i = 0; i < n; i++) p16(b, a[i]); }

static u16 rd16(const u8 *p) { return p[0] | (p[1] << 8); }
static u32 rd32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

// ---- status helpers ---------------------------------------------------------

static void setOp(const char *op, const char *file) {
    mutexLock(&g_st.lock);
    snprintf(g_st.lastOp, sizeof(g_st.lastOp), "%s", op);
    if (file) snprintf(g_st.curFile, sizeof(g_st.curFile), "%s", file);
    mutexUnlock(&g_st.lock);
}
static void setActive(bool a, u64 done, u64 total) {
    mutexLock(&g_st.lock);
    g_st.active = a; g_st.bytesDone = done; g_st.bytesTotal = total;
    mutexUnlock(&g_st.lock);
}

// ---- transport wrappers -----------------------------------------------------

static Buf dataBuf(void) { Buf b = { g_tx, 12, sizeof(g_tx) }; return b; } // payload after header

static void commitData(Buf *b) {
    u32 plen = b->off - 12;
    Buf h = { g_tx, 0, 12 };
    p32(&h, 12 + plen); p16(&h, PTP_TYPE_DATA); p16(&h, g_code); p32(&h, g_trans);
    mtpUsbWrite(g_tx, 12 + plen, &g_run);
}

static void sendResponse(u16 code, const u32 *params, int np) {
    Buf b = { g_tx, 0, sizeof(g_tx) };
    p32(&b, 12 + np * 4); p16(&b, PTP_TYPE_RESP); p16(&b, code); p32(&b, g_trans);
    for (int i = 0; i < np; i++) p32(&b, params[i]);
    mtpUsbWrite(g_tx, b.off, &g_run);
}
static void respOK(void)        { sendResponse(RC_OK, NULL, 0); }
static void respErr(u16 code)   { sendResponse(code, NULL, 0); }

// ---- filesystem helpers -----------------------------------------------------

static const char *baseName(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}
static void joinPath(char *out, size_t n, const char *dir, const char *name) {
    size_t l = strlen(dir);
    if (l && dir[l - 1] == '/') snprintf(out, n, "%s%s", dir, name);
    else                        snprintf(out, n, "%s/%s", dir, name);
}
static void objMeta(Obj *o, u64 *size, bool *isdir) {
    struct stat s;
    *size = 0; *isdir = o->isdir;
    if (stat(o->path, &s) == 0) {
        *isdir = S_ISDIR(s.st_mode);
        if (!*isdir) *size = (u64)s.st_size;
    }
}
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
                recursiveDelete(child);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        remove(path);
    }
}

// ---- MTP operation handlers -------------------------------------------------

static void opGetDeviceInfo(void) {
    Buf b = dataBuf();
    p16(&b, 100);            // StandardVersion
    p32(&b, 6);              // VendorExtensionID = MTP
    p16(&b, 100);            // VendorExtensionVersion
    pstr(&b, "microsoft.com: 1.0;");
    p16(&b, 0);              // FunctionalMode
    parr16(&b, SUPPORTED_OPS, sizeof(SUPPORTED_OPS) / 2);
    p32(&b, 0);              // EventsSupported (none)
    p32(&b, 0);              // DevicePropertiesSupported (none)
    p32(&b, 0);              // CaptureFormats (none)
    parr16(&b, PLAYBACK_FORMATS, sizeof(PLAYBACK_FORMATS) / 2);
    pstr(&b, "Nintendo");
    pstr(&b, "Nintendo Switch");
    pstr(&b, "1.0");
    pstr(&b, "SInfo");
    commitData(&b);
    respOK();
}

static void opGetStorageIDs(void) {
    Buf b = dataBuf();
    p32(&b, 1); p32(&b, STORAGE_ID);
    commitData(&b);
    respOK();
}

static void opGetStorageInfo(void) {
    u64 total = 0, freeb = 0;
    struct statvfs sv;
    if (statvfs(SD_ROOT, &sv) == 0) {
        total = (u64)sv.f_blocks * sv.f_frsize;
        freeb = (u64)sv.f_bfree  * sv.f_frsize;
    }
    Buf b = dataBuf();
    p16(&b, 0x0004);   // StorageType = RemovableRAM
    p16(&b, 0x0002);   // FilesystemType = GenericHierarchical
    p16(&b, 0x0000);   // AccessCapability = read-write
    p64(&b, total);
    p64(&b, freeb);
    p32(&b, 0xFFFFFFFF); // FreeSpaceInObjects = unknown
    pstr(&b, "SD Card");
    pstr(&b, "");
    commitData(&b);
    respOK();
}

static void opGetObjectHandles(u32 parent) {
    char base[770];
    u32 ph;
    if (parent == ROOT_PARENT || parent == 0) { snprintf(base, sizeof(base), "%s", SD_ROOT); ph = 0; }
    else {
        Obj *o = dbGet(parent);
        if (!o) { respErr(RC_InvalidObjectHandle); return; }
        snprintf(base, sizeof(base), "%s", o->path);
        ph = parent;
    }

    Buf b = dataBuf();
    u32 countOff = b.off;
    p32(&b, 0);            // placeholder count
    u32 cnt = 0;

    DIR *d = opendir(base);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char full[800];
            joinPath(full, sizeof(full), base, e->d_name);
            u32 h = dbAdd(full, ph);
            if (!h) continue;
            Obj *o = dbGet(h);
            struct stat s;
            if (stat(full, &s) == 0) o->isdir = S_ISDIR(s.st_mode);
            p32(&b, h);
            cnt++;
        }
        closedir(d);
    }
    // patch count
    g_tx[countOff + 0] = cnt;       g_tx[countOff + 1] = cnt >> 8;
    g_tx[countOff + 2] = cnt >> 16; g_tx[countOff + 3] = cnt >> 24;

    commitData(&b);
    respOK();
}

static void opGetObjectInfo(u32 handle) {
    Obj *o = dbGet(handle);
    if (!o) { respErr(RC_InvalidObjectHandle); return; }
    u64 size; bool isdir;
    objMeta(o, &size, &isdir);

    Buf b = dataBuf();
    p32(&b, STORAGE_ID);
    p16(&b, isdir ? FMT_Association : FMT_Undefined);
    p16(&b, 0);                                   // ProtectionStatus
    p32(&b, size > 0xFFFFFFFF ? 0xFFFFFFFF : (u32)size);
    p16(&b, 0); p32(&b, 0); p32(&b, 0); p32(&b, 0); // thumb fields
    p32(&b, 0); p32(&b, 0); p32(&b, 0);             // image w/h/depth
    p32(&b, o->parent);
    p16(&b, isdir ? 1 : 0);                        // AssociationType
    p32(&b, 0);                                    // AssociationDesc
    p32(&b, 0);                                    // SequenceNumber
    pstr(&b, baseName(o->path));
    pstr(&b, "");   // capture date
    pstr(&b, "");   // modification date
    pstr(&b, "");   // keywords
    commitData(&b);
    respOK();
}

static void opGetObject(u32 handle) {
    Obj *o = dbGet(handle);
    if (!o) { respErr(RC_InvalidObjectHandle); return; }
    FILE *f = fopen(o->path, "rb");
    if (!f) { respErr(RC_GeneralError); return; }
    setOp("GetObject", baseName(o->path));

    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) sz = 0;

    // First packet: 12-byte data header + as much file as fits.
    Buf h = { g_tx, 0, 12 };
    p32(&h, 12 + (u32)sz); p16(&h, PTP_TYPE_DATA); p16(&h, g_code); p32(&h, g_trans);
    setActive(true, 0, (u64)sz);

    size_t firstCap = sizeof(g_tx) - 12;
    size_t rd = fread(g_tx + 12, 1, firstCap, f);
    if (R_FAILED(mtpUsbWrite(g_tx, 12 + rd, &g_run))) { fclose(f); setActive(false, 0, 0); return; }
    size_t done = rd;
    setActive(true, done, (u64)sz);

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

static void readPtpStr(const u8 *p, char *out, size_t outsz) {
    u8 n = p[0]; size_t j = 0;
    for (u8 i = 0; i < n && j < outsz - 1; i++) {
        u16 c = p[1 + i * 2] | (p[2 + i * 2] << 8);
        if (!c) break;
        out[j++] = (c < 128) ? (char)c : '_';
    }
    out[j] = 0;
}

static void opSendObjectInfo(u32 parentParam) {
    u32 tr = 0;
    if (R_FAILED(mtpUsbRead(g_rx, sizeof(g_rx), &tr, &g_run)) || tr < 12 + 53) {
        respErr(RC_GeneralError); return;
    }
    const u8 *pl = g_rx + 12;
    u16 format = rd16(pl + 4);
    char name[300];
    readPtpStr(pl + 52, name, sizeof(name));
    if (!name[0]) { respErr(RC_GeneralError); return; }

    char base[770], full[800];
    u32 ph;
    if (parentParam == ROOT_PARENT || parentParam == 0) { snprintf(base, sizeof(base), "%s", SD_ROOT); ph = 0; }
    else {
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
    if (o->isdir) { mkdir(full, 0777); g_sendHandle = 0; }
    else { FILE *f = fopen(full, "wb"); if (f) fclose(f); g_sendHandle = handle; }

    u32 rp[3] = { STORAGE_ID, ph, handle };
    sendResponse(RC_OK, rp, 3);
}

static void opSendObject(void) {
    Obj *o = dbGet(g_sendHandle);
    if (!o) { respErr(RC_InvalidObjectHandle); return; }

    u32 tr = 0;
    if (R_FAILED(mtpUsbRead(g_rx, sizeof(g_rx), &tr, &g_run)) || tr < 12) {
        respErr(RC_GeneralError); return;
    }
    u32 len = rd32(g_rx);
    u64 fileBytes = (len >= 12) ? (len - 12) : 0;

    FILE *f = fopen(o->path, "wb");
    setOp("SendObject", baseName(o->path));
    setActive(true, 0, fileBytes);

    u64 written = 0;
    u32 have = (tr > 12) ? tr - 12 : 0;
    if (have) { if (f) fwrite(g_rx + 12, 1, have, f); written += have; }
    setActive(true, written, fileBytes);

    while (written < fileBytes) {
        u32 t = 0;
        if (R_FAILED(mtpUsbRead(g_rx, sizeof(g_rx), &t, &g_run)) || t == 0) break;
        u32 w = t;
        if (written + w > fileBytes) w = (u32)(fileBytes - written);
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

static void opDeleteObject(u32 handle) {
    Obj *o = dbGet(handle);
    if (!o) { respErr(RC_InvalidObjectHandle); return; }
    setOp("DeleteObject", baseName(o->path));
    recursiveDelete(o->path);
    respOK();
}

// ---- object property handlers ----------------------------------------------

static void opPropsSupported(void) {
    Buf b = dataBuf();
    parr16(&b, SUPPORTED_PROPS, sizeof(SUPPORTED_PROPS) / 2);
    commitData(&b);
    respOK();
}

static u16 propType(u16 code) {
    switch (code) {
        case OPC_ObjectFormat: return DT_U16;
        case OPC_ObjectSize:   return DT_U64;
        case OPC_FileName:     return DT_STR;
        case OPC_PersistentUID:return DT_U128;
        default:               return DT_U32; // StorageID, ParentObject
    }
}

static void opPropDesc(u16 code) {
    u16 ty = propType(code);
    Buf b = dataBuf();
    p16(&b, code);
    p16(&b, ty);
    p8(&b, code == OPC_FileName ? 1 : 0);   // get/set
    switch (ty) {                            // default value
        case DT_U16:  p16(&b, 0); break;
        case DT_U64:  p64(&b, 0); break;
        case DT_U128: p64(&b, 0); p64(&b, 0); break;
        case DT_STR:  pstr(&b, ""); break;
        default:      p32(&b, 0); break;
    }
    p32(&b, 0);   // group code
    p8(&b, 0);    // form flag = none
    commitData(&b);
    respOK();
}

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

static void opPropValue(u32 handle, u16 code) {
    Obj *o = dbGet(handle);
    if (!o) { respErr(RC_InvalidObjectHandle); return; }
    Buf b = dataBuf();
    appendPropValue(&b, handle, o, code);
    commitData(&b);
    respOK();
}

static void opPropList(u32 handle) {
    Obj *o = dbGet(handle);
    if (!o) { respErr(RC_InvalidObjectHandle); return; }

    Buf b = dataBuf();
    p32(&b, (u32)(sizeof(SUPPORTED_PROPS) / 2));
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

// ---- command dispatch -------------------------------------------------------

static void handleCommand(const u8 *cmd, u32 len) {
    u16 type = rd16(cmd + 4);
    g_code   = rd16(cmd + 6);
    g_trans  = rd32(cmd + 8);
    if (type != PTP_TYPE_CMD) return;

    u32 p[5] = {0};
    int np = (len >= 12) ? (int)((len - 12) / 4) : 0;
    if (np > 5) np = 5;
    for (int i = 0; i < np; i++) p[i] = rd32(cmd + 12 + i * 4);

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
        case OP_GetObjectHandles:        opGetObjectHandles(p[2]); break;
        case OP_GetObjectInfo:           opGetObjectInfo(p[0]);    break;
        case OP_GetObject:               opGetObject(p[0]);        break;
        case OP_SendObjectInfo:          opSendObjectInfo(p[1]);   break;
        case OP_SendObject:              opSendObject();           break;
        case OP_DeleteObject:            opDeleteObject(p[0]);     break;
        case OP_GetObjectPropsSupported: opPropsSupported();       break;
        case OP_GetObjectPropDesc:       opPropDesc((u16)p[0]);    break;
        case OP_GetObjectPropValue:      opPropValue(p[0], (u16)p[1]); break;
        case OP_GetObjectPropList:       opPropList(p[0]);         break;
        default:                         respErr(RC_OperationNotSupported); break;
    }
}

// ---- worker thread ----------------------------------------------------------

static void mtpThread(void *arg) {
    (void)arg;
    while (g_run) {
        bool cfg = mtpUsbConfigured();
        mutexLock(&g_st.lock); g_st.configured = cfg; mutexUnlock(&g_st.lock);

        if (!cfg) {
            mtpUsbWaitChange(1000000000ULL);
            continue;
        }

        u32 tr = 0;
        Result rc = mtpUsbRead(g_rx, 0x400, &tr, &g_run);
        if (R_FAILED(rc) || tr < 12) {
            if (!g_run) break;
            continue;
        }
        handleCommand(g_rx, tr);
    }
}

// ---- public API -------------------------------------------------------------

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

void mtp_stop(void) {
    if (!g_st.initialized) return;
    g_run = false;
    mtpUsbExit();   // cancels pending transfers so the thread unblocks
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
    free(g_obj); g_obj = NULL; g_objN = g_objCap = 0;
    g_st.initialized = false;
}

void mtp_snapshot(MtpStatus *out) {
    mutexLock(&g_st.lock);
    *out = g_st;
    mutexUnlock(&g_st.lock);
}
