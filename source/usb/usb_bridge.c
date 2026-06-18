#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <switch.h>
#include "usb_bridge.h"

// Simple bulk request/response protocol over libnx usbComms.
// The PC companion tool (tools/nxfs.py) speaks the same packing.

#define USB_MAGIC 0x5346584EU   // "NXFS"
#define CHUNK     (256 * 1024)

enum {
    CMD_PING   = 1,
    CMD_LIST   = 2,
    CMD_STAT   = 3,
    CMD_PULL   = 4,   // switch -> host (read file off SD)
    CMD_PUSH   = 5,   // host -> switch (write file to SD)
    CMD_MKDIR  = 6,
    CMD_DELETE = 7,
};

enum {
    ST_OK         = 0,
    ST_ERR_OPEN   = 1,
    ST_ERR_IO     = 2,
    ST_ERR_NOTFND = 3,
    ST_ERR_BADCMD = 4,
};

typedef struct {
    u32  magic;
    u32  cmd;
    u64  size;        // PUSH: incoming file size
    char path[768];
} __attribute__((packed)) ReqHeader;

typedef struct {
    u32 magic;
    u32 status;
    u64 size;         // bytes of payload that follow
} __attribute__((packed)) ResHeader;

static UsbBridgeStatus g_st;
static Thread          g_thread;
static bool            g_run;
static u8              g_buf[CHUNK];

// --- low level helpers ---

static bool readExact(void *buf, size_t len) {
    u8 *p = buf;
    size_t got = 0;
    while (got < len) {
        size_t n = usbCommsRead(p + got, len - got);
        if (n == 0) return false;
        got += n;
    }
    return true;
}

static bool writeExact(const void *buf, size_t len) {
    const u8 *p = buf;
    size_t put = 0;
    while (put < len) {
        size_t n = usbCommsWrite(p + put, len - put);
        if (n == 0) return false;
        put += n;
    }
    return true;
}

static void sendStatus(u32 status, u64 size) {
    ResHeader res = { USB_MAGIC, status, size };
    writeExact(&res, sizeof(res));
}

static void setOp(const char *op, const char *file) {
    mutexLock(&g_st.lock);
    snprintf(g_st.lastOp, sizeof(g_st.lastOp), "%s", op);
    if (file) snprintf(g_st.curFile, sizeof(g_st.curFile), "%s", file);
    g_st.hostConnected = true;
    mutexUnlock(&g_st.lock);
}

static void setActive(bool active, u64 done, u64 total) {
    mutexLock(&g_st.lock);
    g_st.active     = active;
    g_st.bytesDone  = done;
    g_st.bytesTotal = total;
    mutexUnlock(&g_st.lock);
}

// --- command handlers ---

static void handleList(const ReqHeader *req) {
    setOp("LIST", req->path);
    DIR *d = opendir(req->path);
    if (!d) { sendStatus(ST_ERR_OPEN, 0); return; }

    size_t off = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", req->path, e->d_name);
        struct stat stt;
        unsigned long long sz = 0;
        char t = 'F';
        if (stat(full, &stt) == 0) {
            if (S_ISDIR(stt.st_mode)) t = 'D';
            else sz = (unsigned long long)stt.st_size;
        }
        int n = snprintf((char *)g_buf + off, CHUNK - off, "%c\t%llu\t%s\n", t, sz, e->d_name);
        if (n < 0 || (size_t)n >= CHUNK - off) break;
        off += n;
    }
    closedir(d);

    ResHeader res = { USB_MAGIC, ST_OK, off };
    if (writeExact(&res, sizeof(res)))
        writeExact(g_buf, off);
}

static void handleStat(const ReqHeader *req) {
    setOp("STAT", req->path);
    struct stat stt;
    if (stat(req->path, &stt) != 0) { sendStatus(ST_ERR_NOTFND, 0); return; }
    u64 info[2];
    info[0] = S_ISDIR(stt.st_mode) ? 1 : 0;
    info[1] = (u64)stt.st_size;
    ResHeader res = { USB_MAGIC, ST_OK, sizeof(info) };
    if (writeExact(&res, sizeof(res)))
        writeExact(info, sizeof(info));
}

static void handlePull(const ReqHeader *req) {
    setOp("PULL", req->path);
    FILE *f = fopen(req->path, "rb");
    if (!f) { sendStatus(ST_ERR_OPEN, 0); return; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) sz = 0;

    ResHeader res = { USB_MAGIC, ST_OK, (u64)sz };
    if (!writeExact(&res, sizeof(res))) { fclose(f); return; }

    setActive(true, 0, (u64)sz);
    size_t total = 0;
    while (total < (size_t)sz) {
        size_t want = (size_t)sz - total < CHUNK ? (size_t)sz - total : CHUNK;
        size_t rd = fread(g_buf, 1, want, f);
        if (rd == 0 || !writeExact(g_buf, rd)) break;
        total += rd;
        setActive(true, total, (u64)sz);
    }
    fclose(f);

    mutexLock(&g_st.lock);
    g_st.active = false;
    g_st.filesOut++;
    mutexUnlock(&g_st.lock);
}

static void handlePush(const ReqHeader *req) {
    setOp("PUSH", req->path);
    FILE *f = fopen(req->path, "wb");

    u64 remaining = req->size;
    bool ioerr = false;
    setActive(true, 0, req->size);
    while (remaining) {
        size_t want = remaining < CHUNK ? (size_t)remaining : CHUNK;
        if (!readExact(g_buf, want)) { ioerr = true; break; }   // keep stream in sync even if file failed
        if (f && fwrite(g_buf, 1, want, f) != want) ioerr = true;
        remaining -= want;
        setActive(true, req->size - remaining, req->size);
    }
    if (f) fclose(f);

    mutexLock(&g_st.lock);
    g_st.active = false;
    if (f && !ioerr) g_st.filesIn++;
    mutexUnlock(&g_st.lock);

    sendStatus(!f ? ST_ERR_OPEN : (ioerr ? ST_ERR_IO : ST_OK), 0);
}

// --- worker thread ---

static void usbThread(void *arg) {
    (void)arg;
    while (g_run) {
        ReqHeader req;
        if (!readExact(&req, sizeof(req))) {
            mutexLock(&g_st.lock);
            g_st.hostConnected = false;
            mutexUnlock(&g_st.lock);
            if (!g_run) break;
            svcSleepThread(100000000ULL);   // 100ms backoff while disconnected
            continue;
        }
        if (req.magic != USB_MAGIC) { sendStatus(ST_ERR_BADCMD, 0); continue; }

        switch (req.cmd) {
            case CMD_PING:   setOp("PING", NULL);     sendStatus(ST_OK, 0); break;
            case CMD_LIST:   handleList(&req);        break;
            case CMD_STAT:   handleStat(&req);        break;
            case CMD_PULL:   handlePull(&req);        break;
            case CMD_PUSH:   handlePush(&req);        break;
            case CMD_MKDIR:  setOp("MKDIR", req.path);
                             sendStatus(mkdir(req.path, 0777) == 0 ? ST_OK : ST_ERR_IO, 0); break;
            case CMD_DELETE: setOp("DELETE", req.path);
                             sendStatus(remove(req.path) == 0 ? ST_OK : ST_ERR_IO, 0); break;
            default:         sendStatus(ST_ERR_BADCMD, 0); break;
        }
    }
}

// --- public API ---

bool usb_bridge_start(void) {
    memset(&g_st, 0, sizeof(g_st));
    mutexInit(&g_st.lock);

    if (R_FAILED(usbCommsInitialize())) return false;
    g_st.initialized = true;

    g_run = true;
    if (R_FAILED(threadCreate(&g_thread, usbThread, NULL, NULL, 0x20000, 0x2C, -2))) {
        usbCommsExit();
        g_st.initialized = false;
        g_run = false;
        return false;
    }
    threadStart(&g_thread);
    return true;
}

void usb_bridge_stop(void) {
    if (!g_st.initialized) return;
    g_run = false;
    usbCommsExit();   // cancels any blocked usbCommsRead so the thread can exit
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
    g_st.initialized = false;
}

void usb_bridge_snapshot(UsbBridgeStatus *out) {
    mutexLock(&g_st.lock);
    *out = g_st;
    mutexUnlock(&g_st.lock);
}
