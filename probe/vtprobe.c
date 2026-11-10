// vtprobe: Phase 1 feasibility probe for LG webOS capture APIs.
//
// Throwaway measurement tool, not production code. It dlopen()s the TV's own
// libraries at runtime, so no LG headers or libraries are needed at build time.
//
// Modes:
//   hist-init                 init + finalize the histogram API only
//   presets                   try histogram presets 0..7
//   hist   <sec> [preset] [interval_ms]
//   video  <sec> [w] [h]      video-plane capture (vtcapture), luma/colour stats
//   ui     <sec> [w] [h]      graphics-plane capture (halgal), alpha/colour stats
//   both   <sec> [interval_ms]  video capture and histogram in ONE process
//   ui-hist <sec> [interval_ms] graphics capture and histogram in ONE process
//
// Options (before the mode):
//   --devmode      create /var/luna/preferences/vt_devmode only around
//                  vtCapture_initHistogram() and remove it right after
//                  (only if this process created it; a guard child removes it
//                  after 10 s if this process dies)
//   --dump <file>  append per-sample data lines for offline analysis
//   --quiet-lg     do not print libvtcapture log lines
//
// API shapes used here (function signatures, struct layouts) come from the
// public-domain webosbrew/tv-native-apis headers, from the symbol table and
// disassembly of /usr/lib/libvtcapture.so.1 on the TV, and are verified at
// runtime where possible (e.g. the histogram record count via a canary).

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------- utilities

static volatile sig_atomic_t g_stop;
static bool g_lglog = true;
static FILE* g_dump;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts); // same base as /proc/uptime
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}
static uint64_t now_ms(void) { return now_us() / 1000ull; }
static uint64_t g_t0;
// Absolute seconds since boot, comparable with /proc/uptime (pipewatch.sh).
static double rel_s(void) { return (double)now_us() / 1e6; }

static void on_signal(int s)
{
    (void)s;
    g_stop = 1;
}

static double cpu_seconds(void)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (double)ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6
        + (double)ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

// libvtcapture imports printLog(ctx, level, msgid, file, func, line, fmt, ...).
// Exporting our own definition interposes it, so LG's diagnostics (e.g.
// "your process has no permissions") become visible on stderr.
__attribute__((visibility("default"))) void printLog(const char* ctx, int level, const char* msgid,
    const char* file, const char* func, int line, const char* fmt, ...)
{
    (void)file;
    if (!g_lglog)
        return;
    fprintf(stderr, "[%7.3f] LG %s/%s %s:%d: ", rel_s(), ctx ? ctx : "?", msgid ? msgid : "?",
        func ? func : "?", line);
    (void)level;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
    fputc('\n', stderr);
}

static void* sym(void* lib, const char* name)
{
    void* p = dlsym(lib, name);
    if (!p)
        fprintf(stderr, "dlsym(%s) failed: %s\n", name, dlerror());
    return p;
}

// ---------------------------------------------------------------- histogram

typedef struct {
    uint32_t id;
    uint16_t bins[3][256];
} hist_rec_t;
_Static_assert(sizeof(hist_rec_t) == 1540, "histogram record size");

#define HIST_MAX_RECORDS 64

static void* (*h_init)(bool (*)(void));
static int (*h_preset)(void*, int);
static int (*h_get)(void*, void*);
static int (*h_fini)(void*);
static volatile int g_policy_events;
static uint64_t g_policy_at;

static bool policy_cb(void)
{
    g_policy_events++;
    g_policy_at = now_ms();
    fprintf(stderr, "[%7.3f] POLICY callback: histogram resource revoked by LG policy\n", rel_s());
    return true;
}

static bool hist_load(void)
{
    void* lib = dlopen("libvtcapture.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
        fprintf(stderr, "dlopen libvtcapture: %s\n", dlerror());
        return false;
    }
    h_init = sym(lib, "vtCapture_initHistogram");
    h_preset = sym(lib, "vtCapture_setPresetHistogram");
    h_get = sym(lib, "vtCapture_getHistogram");
    h_fini = sym(lib, "vtCapture_finalizeHistogram");
    return h_init && h_preset && h_get && h_fini;
}

#define DEVMODE_PATH "/var/luna/preferences/vt_devmode"
static bool g_use_devmode;
static bool g_reinit; // --reinit: re-acquire the histogram after LG revokes it

// Creates the marker only if absent; returns a guard pid (>0) when we own it.
static pid_t devmode_enter(int* guard_fd)
{
    *guard_fd = -1;
    int fd = open(DEVMODE_PATH, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0) {
        fprintf(stderr, "devmode: marker not created (%s)%s\n", strerror(errno),
            errno == EEXIST ? " - already present, leaving it alone" : "");
        return 0;
    }
    struct stat st;
    fstat(fd, &st);
    close(fd);
    int p[2];
    if (pipe(p) != 0) {
        unlink(DEVMODE_PATH);
        return 0;
    }
    pid_t child = fork();
    if (child == 0) {
        close(p[1]);
        struct pollfd pf = { p[0], POLLIN | POLLHUP, 0 };
        poll(&pf, 1, 10000);
        struct stat cur;
        if (lstat(DEVMODE_PATH, &cur) == 0 && cur.st_ino == st.st_ino && cur.st_dev == st.st_dev)
            unlink(DEVMODE_PATH);
        _exit(0);
    }
    close(p[0]);
    if (child < 0) {
        close(p[1]);
        unlink(DEVMODE_PATH);
        return 0;
    }
    *guard_fd = p[1];
    fprintf(stderr, "[%7.3f] devmode: marker created\n", rel_s());
    return child;
}

static void devmode_leave(pid_t guard, int guard_fd)
{
    if (guard <= 0)
        return;
    close(guard_fd);
    waitpid(guard, NULL, 0);
    fprintf(stderr, "[%7.3f] devmode: marker removed (exists now: %s)\n", rel_s(),
        access(DEVMODE_PATH, F_OK) == 0 ? "YES" : "no");
}

static void* hist_open(void)
{
    int gfd = -1;
    pid_t guard = g_use_devmode ? devmode_enter(&gfd) : 0;
    uint64_t t = now_us();
    void* h = h_init(policy_cb);
    uint64_t dt = now_us() - t;
    devmode_leave(guard, gfd);
    fprintf(stderr, "[%7.3f] initHistogram -> %p (%.1f ms)\n", rel_s(), h, dt / 1000.0);
    return h;
}

static uint8_t mean_bin(const uint16_t* bins, uint32_t* count_out)
{
    uint64_t n = 0, w = 0;
    for (int b = 0; b < 256; b++) {
        n += bins[b];
        w += (uint64_t)bins[b] * (uint64_t)b;
    }
    if (count_out)
        *count_out = (uint32_t)n;
    return n ? (uint8_t)((w + n / 2) / n) : 0;
}

// Returns number of records written (via canary) or -1 on rc != 0.
static int hist_sample(void* h, hist_rec_t* buf, int* rc_out, uint64_t* call_us)
{
    memset(buf, 0xA5, sizeof(hist_rec_t) * HIST_MAX_RECORDS);
    uint64_t t = now_us();
    int rc = h_get(h, buf);
    *call_us = now_us() - t;
    *rc_out = rc;
    const uint8_t* raw = (const uint8_t*)buf;
    size_t total = sizeof(hist_rec_t) * HIST_MAX_RECORDS;
    size_t last = 0;
    for (size_t i = total; i > 0; i--)
        if (raw[i - 1] != 0xA5) {
            last = i;
            break;
        }
    return (int)((last + sizeof(hist_rec_t) - 1) / sizeof(hist_rec_t));
}

static void hist_print(const hist_rec_t* r, int n)
{
    for (int i = 0; i < n && i < 32; i++) {
        uint32_t c0, c1, c2;
        uint8_t m0 = mean_bin(r[i].bins[0], &c0), m1 = mean_bin(r[i].bins[1], &c1), m2 = mean_bin(r[i].bins[2], &c2);
        printf("   rec %2d id=%-3u mean=(%3u,%3u,%3u) count=(%u,%u,%u)\n", i, r[i].id, m0, m1, m2, c0, c1, c2);
    }
}

// ---------------------------------------------------------------- video capture (vtcapture)

typedef struct {
    int32_t dump;
    int16_t x, y;
    int16_t w, h;
    int32_t buf_cnt;
    int32_t frm;
} vt_props_t;
typedef struct {
    int32_t stride;
    int16_t pa, pb, pc, pd;
    int16_t aa, ab, ac, ad;
} vt_plane_t;
typedef struct {
    char* addr0;
    char* addr1;
    int32_t size0;
    int32_t size1;
} vt_buf_t;

static void* (*v_create)(void);
static int (*v_init)(void*, const char*, char*);
static int (*v_preprocess)(void*, char*, vt_props_t*);
static int (*v_planeinfo)(void*, char*, vt_plane_t*);
static int (*v_process)(void*, char*);
static int (*v_curbuf)(void*, vt_buf_t*);
static int (*v_stop)(void*, char*);
static int (*v_postprocess)(void*, char*);
static int (*v_finalize)(void*, char*);
static int (*v_release)(void*);

typedef struct {
    void* drv;
    char client[128];
    int w, h, stride;
    bool running;
    char* last_addr;
    int stage_failed; // which call failed in start
    int stage_rc;
} vcap_t;

static bool video_load(void)
{
    void* lib = dlopen("libvtcapture.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!lib)
        return false;
    v_create = sym(lib, "vtCapture_create");
    v_init = sym(lib, "vtCapture_init");
    v_preprocess = sym(lib, "vtCapture_preprocess");
    v_planeinfo = sym(lib, "vtCapture_planeInfo");
    v_process = sym(lib, "vtCapture_process");
    v_curbuf = sym(lib, "vtCapture_currentCaptureBuffInfo");
    v_stop = sym(lib, "vtCapture_stop");
    v_postprocess = sym(lib, "vtCapture_postprocess");
    v_finalize = sym(lib, "vtCapture_finalize");
    v_release = sym(lib, "vtCapture_release");
    return v_create && v_init && v_preprocess && v_planeinfo && v_process && v_curbuf && v_stop
        && v_postprocess && v_finalize && v_release;
}

// Returns 0 on success; otherwise the failing stage (1 init,2 preprocess,3 planeinfo,4 process).
static int video_start(vcap_t* v, int w, int h, int fps)
{
    if (!v->drv)
        v->drv = v_create();
    if (!v->drv)
        return -1;
    snprintf(v->client, sizeof(v->client), "00");
    int rc = v_init(v->drv, "ambilight-probe", v->client);
    if (rc != 0) {
        v->stage_failed = 1;
        v->stage_rc = rc;
        return 1;
    }
    vt_props_t p = { .dump = 2 /* DISPLAY_OUTPUT */, .x = 0, .y = 0, .w = (int16_t)w, .h = (int16_t)h, .buf_cnt = 3, .frm = fps };
    rc = v_preprocess(v->drv, v->client, &p);
    if (rc != 0) {
        v->stage_failed = 2;
        v->stage_rc = rc;
        v_finalize(v->drv, v->client);
        return 2;
    }
    vt_plane_t pl;
    rc = v_planeinfo(v->drv, v->client, &pl);
    if (rc != 0) {
        v->stage_failed = 3;
        v->stage_rc = rc;
        v_postprocess(v->drv, v->client);
        v_finalize(v->drv, v->client);
        return 3;
    }
    v->w = pl.pc;
    v->h = pl.pd;
    v->stride = pl.stride;
    rc = v_process(v->drv, v->client);
    if (rc != 0) {
        v->stage_failed = 4;
        v->stage_rc = rc;
        v_stop(v->drv, v->client);
        v_postprocess(v->drv, v->client);
        v_finalize(v->drv, v->client);
        return 4;
    }
    v->running = true;
    v->last_addr = NULL;
    fprintf(stderr, "[%7.3f] video: started client=%s plane %dx%d stride %d active (%d,%d %dx%d)\n", rel_s(),
        v->client, v->w, v->h, v->stride, pl.aa, pl.ab, pl.ac, pl.ad);
    return 0;
}

static void video_stop(vcap_t* v)
{
    if (v->running) {
        v_stop(v->drv, v->client);
        v_postprocess(v->drv, v->client);
        v_finalize(v->drv, v->client);
        v->running = false;
    }
}

// BT.709 limited range YUV -> RGB
static void yuv_to_rgb(double y, double u, double v, double* r, double* g, double* b)
{
    double c = (y - 16.0) * 1.164383, d = u - 128.0, e = v - 128.0;
    *r = c + 1.792741 * e;
    *g = c - 0.213249 * d - 0.532909 * e;
    *b = c + 2.112402 * d;
}
static uint8_t clamp8(double x) { return x < 0 ? 0 : x > 255 ? 255 : (uint8_t)(x + 0.5); }

#define BX 16
#define BY 9
// Polls for a new buffer; returns 1 if a new frame was processed into blocks.
static int video_poll(vcap_t* v, uint8_t blocks[BY][BX][3], double* mean_y, int* rc_out)
{
    vt_buf_t b;
    int rc = v_curbuf(v->drv, &b);
    *rc_out = rc;
    if (rc != 0)
        return -1;
    if (b.addr0 == v->last_addr)
        return 0;
    v->last_addr = b.addr0;
    const uint8_t* Y = (const uint8_t*)b.addr0;
    const uint8_t* UV = (const uint8_t*)b.addr1;
    double ysum = 0;
    for (int by = 0; by < BY; by++)
        for (int bx = 0; bx < BX; bx++) {
            int x0 = bx * v->w / BX, x1 = (bx + 1) * v->w / BX, y0 = by * v->h / BY, y1 = (by + 1) * v->h / BY;
            double sy = 0, su = 0, sv = 0;
            int n = 0, nc = 0;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    sy += Y[y * v->stride + x];
                    n++;
                    if (!(y & 1) && !(x & 1)) {
                        su += UV[(y / 2) * v->stride + (x & ~1)];
                        sv += UV[(y / 2) * v->stride + (x & ~1) + 1];
                        nc++;
                    }
                }
            double r, g, bb;
            yuv_to_rgb(n ? sy / n : 16, nc ? su / nc : 128, nc ? sv / nc : 128, &r, &g, &bb);
            blocks[by][bx][0] = clamp8(r);
            blocks[by][bx][1] = clamp8(g);
            blocks[by][bx][2] = clamp8(bb);
            ysum += sy;
        }
    *mean_y = ysum / (double)(v->w * v->h);
    return 1;
}

// ---------------------------------------------------------------- UI capture (halgal)

typedef struct {
    uint32_t offset;
    uint32_t* physicalAddress;
    uint16_t pitch;
    uint16_t bpp;
    uint16_t width;
    uint16_t height;
    uint16_t pixelFormat;
    uint32_t paletteInfo[257];
    uint32_t property;
    uint32_t vendorData;
} gal_surface_t;

static int (*g_galinit)(void);
static int (*g_create)(uint32_t, uint32_t, uint32_t, gal_surface_t*);
static int (*g_destroy)(gal_surface_t*);
static int (*g_capture)(gal_surface_t*);

typedef struct {
    gal_surface_t s;
    int fd;
    uint8_t* mem;
    size_t len;
    int w, h;
} ucap_t;

static int ui_open(ucap_t* u, int w, int h)
{
    void* lib = dlopen("libhalgal.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
        fprintf(stderr, "dlopen libhalgal: %s\n", dlerror());
        return -1;
    }
    g_galinit = sym(lib, "HAL_GAL_Init");
    g_create = sym(lib, "HAL_GAL_CreateSurface");
    g_destroy = sym(lib, "HAL_GAL_DestroySurface");
    g_capture = sym(lib, "HAL_GAL_CaptureFrameBuffer");
    if (!g_galinit || !g_create || !g_destroy || !g_capture)
        return -1;
    int rc = g_galinit();
    if (rc != 0) {
        fprintf(stderr, "HAL_GAL_Init rc=%d\n", rc);
        return -2;
    }
    memset(u, 0, sizeof(*u));
    u->w = w;
    u->h = h;
    rc = g_create(w, h, 0, &u->s);
    if (rc != 0) {
        fprintf(stderr, "HAL_GAL_CreateSurface rc=%d\n", rc);
        return -3;
    }
    u->fd = open("/dev/gfx", O_RDWR);
    if (u->fd < 0) {
        fprintf(stderr, "open /dev/gfx: %s\n", strerror(errno));
        g_destroy(&u->s);
        return -4;
    }
    u->len = u->s.property ? u->s.property : (size_t)u->s.pitch * u->s.height;
    u->mem = mmap(NULL, u->len, PROT_READ | PROT_WRITE, MAP_SHARED, u->fd, u->s.offset);
    if (u->mem == MAP_FAILED) {
        fprintf(stderr, "mmap gfx failed: %s\n", strerror(errno));
        close(u->fd);
        g_destroy(&u->s);
        return -5;
    }
    fprintf(stderr, "[%7.3f] ui: surface %ux%u pitch %u bpp %u fmt %u len %zu\n", rel_s(), u->s.width, u->s.height,
        u->s.pitch, u->s.bpp, u->s.pixelFormat, u->len);
    return 0;
}

static void ui_close(ucap_t* u)
{
    if (u->mem && u->mem != MAP_FAILED)
        munmap(u->mem, u->len);
    if (u->fd > 0)
        close(u->fd);
    g_destroy(&u->s);
}

// Assumption: bytes in memory are B,G,R,A (libyuv "ARGB"), as hyperion-webos treats them.
static int ui_sample(ucap_t* u, double* coverage, uint8_t rgb[3], uint8_t blocks[BY][BX][4])
{
    int rc = g_capture(&u->s);
    if (rc != 0)
        return rc;
    uint64_t cov = 0, sr = 0, sg = 0, sb = 0, sa = 0;
    for (int by = 0; by < BY; by++)
        for (int bx = 0; bx < BX; bx++) {
            uint64_t br = 0, bg = 0, bb = 0, ba = 0, n = 0;
            for (int y = by * u->h / BY; y < (by + 1) * u->h / BY; y++)
                for (int x = bx * u->w / BX; x < (bx + 1) * u->w / BX; x++) {
                    const uint8_t* p = u->mem + (size_t)y * u->s.pitch + (size_t)x * 4;
                    uint8_t B = p[0], G = p[1], R = p[2], A = p[3];
                    if (A > 16)
                        cov++;
                    br += (uint64_t)R * A;
                    bg += (uint64_t)G * A;
                    bb += (uint64_t)B * A;
                    ba += A;
                    n++;
                }
            sr += br;
            sg += bg;
            sb += bb;
            sa += ba;
            if (blocks) {
                blocks[by][bx][0] = ba ? (uint8_t)(br / ba) : 0;
                blocks[by][bx][1] = ba ? (uint8_t)(bg / ba) : 0;
                blocks[by][bx][2] = ba ? (uint8_t)(bb / ba) : 0;
                blocks[by][bx][3] = n ? (uint8_t)(ba / n) : 0;
            }
        }
    *coverage = (double)cov / (double)(u->w * u->h);
    rgb[0] = sa ? (uint8_t)(sr / sa) : 0;
    rgb[1] = sa ? (uint8_t)(sg / sa) : 0;
    rgb[2] = sa ? (uint8_t)(sb / sa) : 0;
    return 0;
}

// ---------------------------------------------------------------- modes

static void dump_hist(uint64_t t, const hist_rec_t* r, int n)
{
    if (!g_dump)
        return;
    fprintf(g_dump, "H %llu", (unsigned long long)t);
    for (int i = 0; i < n; i++)
        fprintf(g_dump, " %u:%u,%u,%u", r[i].id, mean_bin(r[i].bins[0], NULL), mean_bin(r[i].bins[1], NULL),
            mean_bin(r[i].bins[2], NULL));
    fputc('\n', g_dump);
}
static void dump_blocks(char tag, uint64_t t, uint8_t blocks[BY][BX][3])
{
    if (!g_dump)
        return;
    fprintf(g_dump, "%c %llu", tag, (unsigned long long)t);
    for (int y = 0; y < BY; y++)
        for (int x = 0; x < BX; x++)
            fprintf(g_dump, " %u,%u,%u", blocks[y][x][0], blocks[y][x][1], blocks[y][x][2]);
    fputc('\n', g_dump);
}

static int mode_hist_init(void)
{
    if (!hist_load())
        return 2;
    void* h = hist_open();
    if (!h)
        return 3;
    int rc = h_fini(h);
    fprintf(stderr, "finalizeHistogram -> %d\n", rc);
    return 0;
}

static int mode_presets(void)
{
    if (!hist_load())
        return 2;
    void* h = hist_open();
    if (!h)
        return 3;
    static hist_rec_t buf[HIST_MAX_RECORDS];
    for (int p = 0; p <= 7 && !g_stop; p++) {
        int rc = h_preset(h, p);
        usleep(300000);
        int grc;
        uint64_t us;
        int n = hist_sample(h, buf, &grc, &us);
        printf("preset %d -> rc %d; get rc %d records %d (%.2f ms)\n", p, rc, grc, n, us / 1000.0);
        if (grc == 0 && rc == 0)
            hist_print(buf, n > 20 ? 20 : n);
    }
    h_preset(h, 0);
    h_fini(h);
    return 0;
}

static int mode_hist(int secs, int preset, int interval_ms)
{
    if (!hist_load())
        return 2;
    void* h = hist_open();
    if (!h)
        return 3;
    int prc = h_preset(h, preset);
    fprintf(stderr, "[%7.3f] setPresetHistogram(%d) -> %d\n", rel_s(), preset, prc);
    static hist_rec_t buf[HIST_MAX_RECORDS], prev[HIST_MAX_RECORDS];
    int prev_n = 0;
    uint64_t end = now_ms() + (uint64_t)secs * 1000ull;
    unsigned calls = 0, ok = 0, changes = 0, rc_hist[8] = { 0 };
    uint64_t max_call = 0, sum_call = 0, last_change = 0, gap_max = 0;
    double cpu0 = cpu_seconds();
    uint64_t wall0 = now_us();
    int records_seen = -1;
    uint64_t next_print = now_ms();
    int policy_seen = g_policy_events;
    unsigned reinits = 0;
    int last_rc = -1;
    while (!g_stop && now_ms() < end) {
        int rc;
        uint64_t us;
        int n = hist_sample(h, buf, &rc, &us);
        calls++;
        sum_call += us;
        if (us > max_call)
            max_call = us;
        rc_hist[rc >= 0 && rc < 7 ? rc : 7]++;
        if (rc != last_rc) {
            // Every rc transition, timestamped so it can be aligned with pipewatch's
            // SVP_CPB onset. rc 0 = data delivered; rc 2 = revoked by LG.
            printf("[%7.3f] getHistogram rc %d -> %d (policy events so far %d)\n", rel_s(), last_rc, rc,
                g_policy_events);
            last_rc = rc;
        }
        if (rc == 0) {
            ok++;
            if (records_seen != n) {
                fprintf(stderr, "[%7.3f] records written per call: %d (%d bytes)\n", rel_s(), n, n * 1540);
                records_seen = n;
            }
            int cmp_n = n < 16 ? n : 16;
            if (prev_n != n || memcmp(buf, prev, sizeof(hist_rec_t) * cmp_n) != 0) {
                uint64_t t = now_ms();
                if (last_change && t - last_change > gap_max)
                    gap_max = t - last_change;
                last_change = t;
                changes++;
                memcpy(prev, buf, sizeof(hist_rec_t) * (n < HIST_MAX_RECORDS ? n : HIST_MAX_RECORDS));
                prev_n = n;
                dump_hist(t, buf, cmp_n);
            }
            if (now_ms() >= next_print) {
                next_print = now_ms() + 2000;
                printf("[%7.3f] sample (rc 0, %d records):\n", rel_s(), n);
                hist_print(buf, cmp_n);
            }
        } else {
            if (now_ms() >= next_print) {
                next_print = now_ms() + 2000;
                printf("[%7.3f] get rc=%d\n", rel_s(), rc);
            }
            // Revoked (another VT_CAPTURE client took over): release and re-acquire.
            if (g_reinit && g_policy_events > policy_seen) {
                uint64_t lost_at = g_policy_at;
                policy_seen = g_policy_events;
                h_fini(h);
                h = NULL;
                while (!h && !g_stop && now_ms() < end) {
                    usleep(250000);
                    h = hist_open();
                }
                if (!h)
                    break;
                h_preset(h, preset);
                reinits++;
                printf("[%7.3f] re-acquired histogram %llu ms after revocation\n", rel_s(),
                    (unsigned long long)(now_ms() - lost_at));
            }
        }
        if (interval_ms > 0)
            usleep((useconds_t)interval_ms * 1000);
    }
    double wall = (now_us() - wall0) / 1e6;
    double cpu = cpu_seconds() - cpu0;
    printf("SUMMARY hist: preset=%d interval=%dms calls=%u ok=%u distinct=%u (%.1f/s) call avg %.2f ms max %.2f ms; "
           "max gap between changes %llu ms; cpu %.1f%% of one core; policy events %d\n",
        preset, interval_ms, calls, ok, changes, changes / wall, calls ? sum_call / 1000.0 / calls : 0,
        max_call / 1000.0, (unsigned long long)gap_max, 100.0 * cpu / wall, g_policy_events);
    printf("SUMMARY rc histogram: 0:%u 1:%u 2:%u 3:%u 4:%u 5:%u 6:%u other:%u; re-inits %u\n", rc_hist[0], rc_hist[1],
        rc_hist[2], rc_hist[3], rc_hist[4], rc_hist[5], rc_hist[6], rc_hist[7], reinits);
    if (!h)
        return 0;
    int frc = h_fini(h);
    fprintf(stderr, "finalizeHistogram -> %d\n", frc);
    return 0;
}

static int mode_video(int secs, int w, int h)
{
    if (!video_load())
        return 2;
    vcap_t v = { 0 };
    uint64_t end = now_ms() + (uint64_t)secs * 1000ull;
    uint64_t next_try = 0, next_print = 0;
    unsigned frames = 0, frames_sec = 0;
    double cpu0 = cpu_seconds();
    uint64_t wall0 = now_us();
    uint8_t blocks[BY][BX][3];
    int vstate = 0;
    while (!g_stop && now_ms() < end) {
        if (!v.running && now_ms() >= next_try) {
            next_try = now_ms() + 1000;
            int st = video_start(&v, w, h, 30);
            if (st != 0)
                printf("[%7.3f] video start failed at stage %d rc %d\n", rel_s(), v.stage_failed, v.stage_rc);
        }
        if (v.running) {
            double my;
            int rc;
            int r = video_poll(&v, blocks, &my, &rc);
            if (r < 0) {
                printf("[%7.3f] currentCaptureBuffInfo rc=%d -> restarting\n", rel_s(), rc);
                video_stop(&v);
            } else if (r == 1) {
                int st = my < 20.0 ? 1 : 2;
                if (st != vstate) {
                    printf("[%7.3f] video content now %s (mean Y %.1f)\n", rel_s(), st == 1 ? "BLACK" : "non-black", my);
                    vstate = st;
                }
                frames++;
                frames_sec++;
                dump_blocks('V', now_ms(), blocks);
                if (now_ms() >= next_print) {
                    next_print = now_ms() + 1000;
                    printf("[%7.3f] video %u fps, mean Y %.1f, centre rgb (%u,%u,%u)\n", rel_s(), frames_sec, my,
                        blocks[BY / 2][BX / 2][0], blocks[BY / 2][BX / 2][1], blocks[BY / 2][BX / 2][2]);
                    frames_sec = 0;
                }
            }
        }
        usleep(2000);
    }
    double wall = (now_us() - wall0) / 1e6;
    printf("SUMMARY video: frames=%u (%.1f fps) cpu %.1f%%\n", frames, frames / wall, 100.0 * (cpu_seconds() - cpu0) / wall);
    video_stop(&v);
    if (v.drv)
        v_release(v.drv);
    return 0;
}

static int mode_ui(int secs, int w, int h, bool with_hist, int interval_ms)
{
    ucap_t u;
    int rc = ui_open(&u, w, h);
    if (rc != 0)
        return 2;
    void* hh = NULL;
    static hist_rec_t buf[HIST_MAX_RECORDS];
    if (with_hist) {
        if (!hist_load())
            return 2;
        hh = hist_open();
        if (hh)
            fprintf(stderr, "[%7.3f] setPreset(0) -> %d\n", rel_s(), h_preset(hh, 0));
    }
    uint64_t end = now_ms() + (uint64_t)secs * 1000ull, next_print = 0, next_hist = 0;
    unsigned frames = 0, fails = 0, hist_ok = 0, hist_fail = 0;
    double cpu0 = cpu_seconds();
    uint64_t wall0 = now_us();
    uint8_t ublocks[BY][BX][4];
    while (!g_stop && now_ms() < end) {
        double cov;
        uint8_t rgb[3];
        int r = ui_sample(&u, &cov, rgb, ublocks);
        if (r != 0)
            fails++;
        else
            frames++;
        if (hh && now_ms() >= next_hist) {
            next_hist = now_ms() + (uint64_t)interval_ms;
            int grc;
            uint64_t us;
            int n = hist_sample(hh, buf, &grc, &us);
            if (grc == 0) {
                hist_ok++;
                dump_hist(now_ms(), buf, n < 16 ? n : 16);
            } else
                hist_fail++;
        }
        if (now_ms() >= next_print) {
            next_print = now_ms() + 1000;
            printf("[%7.3f] ui rc=%d coverage %.1f%% mean rgb (%u,%u,%u)%s", rel_s(), r, 100 * cov, rgb[0], rgb[1], rgb[2],
                hh ? "" : "\n");
            if (hh)
                printf(" | hist ok %u fail %u\n", hist_ok, hist_fail);
        }
        usleep(33000);
    }
    double wall = (now_us() - wall0) / 1e6;
    printf("SUMMARY ui: frames=%u fails=%u (%.1f fps) hist ok=%u fail=%u cpu %.1f%% policy events %d\n", frames, fails,
        frames / wall, hist_ok, hist_fail, 100.0 * (cpu_seconds() - cpu0) / wall, g_policy_events);
    if (hh)
        h_fini(hh);
    ui_close(&u);
    return 0;
}

static int mode_both(int secs, int interval_ms)
{
    if (!video_load() || !hist_load())
        return 2;
    vcap_t v = { 0 };
    void* h = NULL;
    static hist_rec_t buf[HIST_MAX_RECORDS];
    uint64_t end = now_ms() + (uint64_t)secs * 1000ull, next_v = 0, next_h = 0, next_hist = 0, next_print = 0;
    unsigned vframes = 0, hok = 0, hfail = 0, vfail_start = 0;
    uint8_t blocks[BY][BX][3];
    double cpu0 = cpu_seconds();
    uint64_t wall0 = now_us();
    double last_y = -1;
    while (!g_stop && now_ms() < end) {
        if (!v.running && now_ms() >= next_v) {
            next_v = now_ms() + 1000;
            if (video_start(&v, 160, 90, 30) != 0) {
                vfail_start++;
                printf("[%7.3f] video start failed stage %d rc %d\n", rel_s(), v.stage_failed, v.stage_rc);
            }
        }
        if (!h && now_ms() >= next_h) {
            next_h = now_ms() + 1000;
            h = hist_open();
            if (h)
                fprintf(stderr, "[%7.3f] setPreset(0) -> %d\n", rel_s(), h_preset(h, 0));
        }
        if (v.running) {
            int rc;
            int r = video_poll(&v, blocks, &last_y, &rc);
            if (r < 0) {
                printf("[%7.3f] video currentCaptureBuffInfo rc=%d -> restart\n", rel_s(), rc);
                video_stop(&v);
            } else if (r == 1) {
                vframes++;
                dump_blocks('V', now_ms(), blocks);
            }
        }
        if (h && now_ms() >= next_hist) {
            next_hist = now_ms() + (uint64_t)interval_ms;
            int grc;
            uint64_t us;
            int n = hist_sample(h, buf, &grc, &us);
            if (grc == 0) {
                hok++;
                dump_hist(now_ms(), buf, n < 16 ? n : 16);
            } else
                hfail++;
        }
        if (now_ms() >= next_print) {
            next_print = now_ms() + 1000;
            printf("[%7.3f] video running=%d frames=%u meanY=%.1f | hist handle=%s ok=%u fail=%u policy=%d\n", rel_s(),
                v.running, vframes, last_y, h ? "yes" : "no", hok, hfail, g_policy_events);
        }
        usleep(2000);
    }
    double wall = (now_us() - wall0) / 1e6;
    printf("SUMMARY both: video frames=%u (%.1f fps) start failures=%u | hist ok=%u fail=%u | policy events=%d | cpu %.1f%%\n",
        vframes, vframes / wall, vfail_start, hok, hfail, g_policy_events, 100.0 * (cpu_seconds() - cpu0) / wall);
    if (h)
        h_fini(h);
    video_stop(&v);
    if (v.drv)
        v_release(v.drv);
    return 0;
}

int main(int argc, char** argv)
{
    g_t0 = now_us();
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGALRM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    int i = 1;
    for (; i < argc && strncmp(argv[i], "--", 2) == 0; i++) {
        if (!strcmp(argv[i], "--devmode"))
            g_use_devmode = true;
        else if (!strcmp(argv[i], "--quiet-lg"))
            g_lglog = false;
        else if (!strcmp(argv[i], "--reinit"))
            g_reinit = true;
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) {
            g_dump = fopen(argv[++i], "a");
            if (g_dump)
                setvbuf(g_dump, NULL, _IOFBF, 1 << 16);
        }
    }
    if (i >= argc) {
        fprintf(stderr, "usage: see source header\n");
        return 1;
    }
    const char* mode = argv[i];
    int a1 = i + 1 < argc ? atoi(argv[i + 1]) : 0;
    int a2 = i + 2 < argc ? atoi(argv[i + 2]) : -1;
    int a3 = i + 3 < argc ? atoi(argv[i + 3]) : -1;
    // Hard safety stop: every mode ends within its duration + 10 s.
    alarm((unsigned)(a1 > 0 ? a1 + 10 : 30));
    int rc = 1;
    if (!strcmp(mode, "hist-init"))
        rc = mode_hist_init();
    else if (!strcmp(mode, "presets"))
        rc = mode_presets();
    else if (!strcmp(mode, "hist"))
        rc = mode_hist(a1 > 0 ? a1 : 10, a2 >= 0 ? a2 : 0, a3 >= 0 ? a3 : 0);
    else if (!strcmp(mode, "video"))
        rc = mode_video(a1 > 0 ? a1 : 10, a2 > 0 ? a2 : 160, a3 > 0 ? a3 : 90);
    else if (!strcmp(mode, "ui"))
        rc = mode_ui(a1 > 0 ? a1 : 10, a2 > 0 ? a2 : 320, a3 > 0 ? a3 : 180, false, 0);
    else if (!strcmp(mode, "ui-hist"))
        rc = mode_ui(a1 > 0 ? a1 : 10, 320, 180, true, a2 > 0 ? a2 : 50);
    else if (!strcmp(mode, "both"))
        rc = mode_both(a1 > 0 ? a1 : 10, a2 > 0 ? a2 : 50);
    else
        fprintf(stderr, "unknown mode %s\n", mode);
    if (g_dump)
        fclose(g_dump);
    return rc;
}
