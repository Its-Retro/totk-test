#ifndef TOTK_HOST_IMAGE_TRACE_H
#define TOTK_HOST_IMAGE_TRACE_H
/* Diagnostics71: opt-in, numeric metadata only. Initialize outside NVN callbacks.
 * Producers only try a fixed ring; the writer owns all file I/O. API entry is
 * observed BEFORE the real driver function, so it does not prove completion. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define BOOT_IMAGE_TRACE_CAP 512
#define BOOT_IMAGE_TRACE_BATCH 64
#define BOOT_IMAGE_TRACE_LIMIT 16384
#define BOOT_IMAGE_TRACE_SECONDS 180
enum { BOOT_IMAGE_API_ENTRY = 1, BOOT_IMAGE_CPU_DRAIN = 2,
       BOOT_IMAGE_EXTERNAL_BEFORE = 3, BOOT_IMAGE_EXTERNAL_AFTER = 4,
       BOOT_IMAGE_ASM_SOURCE = 5, BOOT_IMAGE_REFRESH_SOURCE = 6, BOOT_IMAGE_REFRESH_DEST = 7, BOOT_IMAGE_API_UNRESOLVED = 8 };
typedef struct BootImageTraceRecord {
    uint64_t qpc, serial, thread, kind, v[24];
} BootImageTraceRecord;
static BootImageTraceRecord boot_image_trace_ring[BOOT_IMAGE_TRACE_CAP];
static SRWLOCK boot_image_trace_lock = SRWLOCK_INIT;
static volatile LONG boot_image_trace_enabled;
static unsigned boot_image_trace_head, boot_image_trace_count, boot_image_trace_accepted;
static uint64_t boot_image_trace_base, boot_image_trace_bytes, boot_image_trace_freq, boot_image_trace_start;
static uint64_t boot_image_trace_serial;
_Alignas(8) static volatile LONG64 boot_image_trace_api_epoch;
_Alignas(8) static volatile LONG64 boot_image_trace_lost_lock, boot_image_trace_lost_full;
_Alignas(8) static volatile LONG64 boot_image_trace_lost_limit;
_Alignas(8) static volatile LONG64 boot_image_trace_lost_stop;
static int boot_image_trace_init_done;
static inline int boot_image_trace_overlap(uint64_t va, uint64_t size) {
    if (!boot_image_trace_enabled || !size || !boot_image_trace_bytes) return 0;
    /* Subtraction form cannot wrap at UINT64_MAX. */
    return va >= boot_image_trace_base ? va - boot_image_trace_base < boot_image_trace_bytes :
                                        boot_image_trace_base - va < size;
}
static inline uint64_t boot_image_trace_epoch(void) {
    return (uint64_t)InterlockedCompareExchange64(&boot_image_trace_api_epoch, 0, 0);
}
static inline void boot_image_trace_push(BootImageTraceRecord *e) {
    if (!boot_image_trace_enabled) { InterlockedIncrement64(&boot_image_trace_lost_stop); return; }
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    e->qpc = (uint64_t)now.QuadPart; e->thread = GetCurrentThreadId();
    if (!TryAcquireSRWLockExclusive(&boot_image_trace_lock)) {
        InterlockedIncrement64(&boot_image_trace_lost_lock); return;
    }
    if (!boot_image_trace_enabled) { InterlockedIncrement64(&boot_image_trace_lost_stop); ReleaseSRWLockExclusive(&boot_image_trace_lock); return; }
    e->serial = ++boot_image_trace_serial;
    if (boot_image_trace_accepted >= BOOT_IMAGE_TRACE_LIMIT) InterlockedIncrement64(&boot_image_trace_lost_limit);
    else if (boot_image_trace_count == BOOT_IMAGE_TRACE_CAP) InterlockedIncrement64(&boot_image_trace_lost_full);
    else {
        unsigned i = (boot_image_trace_head + boot_image_trace_count) % BOOT_IMAGE_TRACE_CAP;
        boot_image_trace_ring[i] = *e; ++boot_image_trace_count; ++boot_image_trace_accepted;
    }
    ReleaseSRWLockExclusive(&boot_image_trace_lock);
}
/* Consumer copies under the ring lock, then writes after releasing it. */
static inline unsigned boot_image_trace_take(BootImageTraceRecord *out) {
    unsigned n;
    AcquireSRWLockExclusive(&boot_image_trace_lock);
    n = boot_image_trace_count < BOOT_IMAGE_TRACE_BATCH ? boot_image_trace_count : BOOT_IMAGE_TRACE_BATCH;
    for (unsigned i = 0; i < n; ++i) out[i] = boot_image_trace_ring[(boot_image_trace_head + i) % BOOT_IMAGE_TRACE_CAP];
    boot_image_trace_head = (boot_image_trace_head + n) % BOOT_IMAGE_TRACE_CAP; boot_image_trace_count -= n;
    ReleaseSRWLockExclusive(&boot_image_trace_lock);
    return n;
}
/* A receipt is published only after CSV flush/close and receipt write/close
 * succeeded. An end line alone is not a successful collection receipt. */
static inline int boot_image_trace_commit(const char *path, const char *json) {
    char pending[224], complete[224];
    snprintf(pending, sizeof pending, "%s.complete.tmp", path);
    snprintf(complete, sizeof complete, "%s.complete.json", path);
    HANDLE file = CreateFileA(pending, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    DWORD bytes = (DWORD)strlen(json), wrote = 0;
    int ok = WriteFile(file, json, bytes, &wrote, NULL) && wrote == bytes;
    if (!CloseHandle(file)) ok = 0;
    return ok && MoveFileExA(pending, complete, MOVEFILE_WRITE_THROUGH);
}
static DWORD WINAPI boot_image_trace_writer(void *arg) {
    (void)arg;
    char path[160];
    snprintf(path, sizeof path, "frames/cpu-image-trace-%lu-%llu.csv", (unsigned long)GetCurrentProcessId(),
             (unsigned long long)boot_image_trace_start);
    FILE *f = fopen(path, "wb");
    if (!f) { InterlockedExchange(&boot_image_trace_enabled, 0); fprintf(stderr, "image trace71: cannot open %s (errno %d)\n", path, errno); return 1; }
    fprintf(f, "# image_trace71,version=2,pid=%lu,qpc_frequency=%llu,start_qpc=%llu,watch_base=%llx,watch_bytes=%llx,seconds=%u,record_limit=%u\n",
            (unsigned long)GetCurrentProcessId(), (unsigned long long)boot_image_trace_freq,
            (unsigned long long)boot_image_trace_start, (unsigned long long)boot_image_trace_base,
            (unsigned long long)boot_image_trace_bytes, BOOT_IMAGE_TRACE_SECONDS, BOOT_IMAGE_TRACE_LIMIT);
    fputs("qpc,serial,thread,kind", f);
    for (unsigned j = 0; j < 24; ++j) fprintf(f, ",v%u", j);
    fputc('\n', f);
    uint64_t flushed = boot_image_trace_start, rows = 0; unsigned reason = 0;
    for (;;) {
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        int expired = (uint64_t)now.QuadPart - boot_image_trace_start >= boot_image_trace_freq * BOOT_IMAGE_TRACE_SECONDS;
        if (expired) { reason = 1; InterlockedExchange(&boot_image_trace_enabled, 0); }
        BootImageTraceRecord batch[BOOT_IMAGE_TRACE_BATCH];
        unsigned n = boot_image_trace_take(batch);
        for (unsigned i = 0; i < n; ++i) {
            BootImageTraceRecord *e = &batch[i];
            fprintf(f, "%llu,%llu,%llu,%llu", (unsigned long long)e->qpc, (unsigned long long)e->serial,
                    (unsigned long long)e->thread, (unsigned long long)e->kind);
            for (unsigned j = 0; j < 24; ++j) fprintf(f, ",%llx", (unsigned long long)e->v[j]);
            fputc('\n', f); ++rows;
        }
        if (rows >= BOOT_IMAGE_TRACE_LIMIT) { reason = 2; InterlockedExchange(&boot_image_trace_enabled, 0); }
        if (ferror(f)) goto failed;
        if ((uint64_t)now.QuadPart - flushed >= boot_image_trace_freq) {
            fprintf(f, "# loss,lock=%llu,full=%llu,limit=%llu,stop=%llu,rows=%llu\n",
                    (unsigned long long)InterlockedCompareExchange64(&boot_image_trace_lost_lock, 0, 0),
                    (unsigned long long)InterlockedCompareExchange64(&boot_image_trace_lost_full, 0, 0),
                    (unsigned long long)InterlockedCompareExchange64(&boot_image_trace_lost_limit, 0, 0),
                    (unsigned long long)InterlockedCompareExchange64(&boot_image_trace_lost_stop, 0, 0), (unsigned long long)rows);
            if (fflush(f)) goto failed; flushed = (uint64_t)now.QuadPart;
        }
        if (!n) { if (!boot_image_trace_enabled) break; Sleep(100); }
    }
    {
        LARGE_INTEGER now; QueryPerformanceCounter(&now); if (!reason) reason = 3;
        uint64_t lock = (uint64_t)InterlockedCompareExchange64(&boot_image_trace_lost_lock, 0, 0);
        uint64_t full = (uint64_t)InterlockedCompareExchange64(&boot_image_trace_lost_full, 0, 0);
        uint64_t limit = (uint64_t)InterlockedCompareExchange64(&boot_image_trace_lost_limit, 0, 0);
        uint64_t stop = (uint64_t)InterlockedCompareExchange64(&boot_image_trace_lost_stop, 0, 0);
        if (fflush(f)) goto failed;
        fprintf(f, "# end,qpc=%llu,reason=%u,error=0,lock=%llu,full=%llu,limit=%llu,stop=%llu,rows=%llu\n",
                (unsigned long long)now.QuadPart, reason, (unsigned long long)lock, (unsigned long long)full,
                (unsigned long long)limit, (unsigned long long)stop, (unsigned long long)rows);
        if (ferror(f) || fflush(f)) goto failed;
        int closed = fclose(f); f = NULL; if (closed) goto failed;
        char receipt[768];
        snprintf(receipt, sizeof receipt, "{\"version\":2,\"pid\":%lu,\"start_qpc\":%llu,\"qpc\":%llu,\"reason\":%u,\"error\":0,\"lock\":%llu,\"full\":%llu,\"limit\":%llu,\"stop\":%llu,\"rows\":%llu}\n",
                 (unsigned long)GetCurrentProcessId(), (unsigned long long)boot_image_trace_start,
                 (unsigned long long)now.QuadPart, reason, (unsigned long long)lock, (unsigned long long)full,
                 (unsigned long long)limit, (unsigned long long)stop, (unsigned long long)rows);
        if (!boot_image_trace_commit(path, receipt)) goto failed;
    }
    return 0;
failed:
    InterlockedExchange(&boot_image_trace_enabled, 0);
    fprintf(stderr, "image trace71: incomplete CSV/receipt %s (errno %d, win32 %lu)\n", path, errno, (unsigned long)GetLastError());
    if (f) fclose(f);
    return 1;
}
/* Called once from native NVN registration, before guest callbacks/renderer locks. */
static inline void boot_image_trace_init(void) {
    if (boot_image_trace_init_done) return;
    boot_image_trace_init_done = 1;
    char env[96], tail; unsigned long long base, bytes;
    DWORD n = GetEnvironmentVariableA("TOTK_IMAGE_WRITE_TRACE", env, sizeof env);
    if (!n || n >= sizeof env || sscanf(env, "%llx:%llx%c", &base, &bytes, &tail) != 2 || !base || !bytes ||
        bytes > (UINT64_C(64) << 20) || base > UINT64_MAX - bytes) return;
    LARGE_INTEGER freq, now;
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0 || freq.QuadPart > 10000000000LL || !QueryPerformanceCounter(&now)) return;
    boot_image_trace_base = base; boot_image_trace_bytes = bytes;
    boot_image_trace_freq = (uint64_t)freq.QuadPart; boot_image_trace_start = (uint64_t)now.QuadPart;
    InterlockedExchange(&boot_image_trace_enabled, 1);
    HANDLE worker = CreateThread(NULL, 0, boot_image_trace_writer, NULL, 0, NULL);
    if (worker) CloseHandle(worker); else InterlockedExchange(&boot_image_trace_enabled, 0);
}
#endif
