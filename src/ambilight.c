// ambilight: hybrid capture service for rooted LG webOS TVs.
//
// MVP (phase-2 step 1): keeps the LG histogram open continuously (16 colour zones,
// works for every source including DRM/SVP), captures the graphics/UI plane at the
// same time, blends them, and sends the result to HyperHDR as a small PNG over the
// JSON API (one origin, one priority). The histogram handle is never closed, so DRM
// colours keep flowing; a lost handle (e.g. LG's captureservice grabbing it on an app
// switch, or protected playback) is tolerated by holding the last colours and retrying
// the open while it is allowed again.
//
// No LG headers or libraries are needed at build time: the TV's libraries are loaded
// at runtime with dlopen(). Build with:
//   zig cc -target arm-linux-gnueabi.2.35 -mcpu=cortex_a9 -O2 -rdynamic -o ambilight ambilight.c -ldl
//
// API facts (histogram record layout, halgal surface, vt_devmode gate) were measured
// on the TV and cross-checked against the public-domain webosbrew/tv-native-apis
// headers; see docs/phase-0-1-findings.md. No third-party GPL/unlicensed code is used.

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "profile_b4.h"

// ---------------------------------------------------------------- configuration

// MVP defaults. A config file / settings app comes in a later step.
static char g_host[64] = "192.168.129.249";
static int g_port = 19444;
static int g_priority = 150;
static int g_interval_ms = 50;    // 20 Hz
static int g_duration_ms = 1000;  // image expiry if the service stops feeding
static double g_gain = 1.0;       // overall brightness gain before HyperHDR's own adjustment
static int g_ui_cap_w = 320, g_ui_cap_h = 180;

// Re-acquire pacing after the histogram is lost (e.g. LG's captureservice grabbing it on
// an app switch). For the first FAST window we retry every loop so we catch the brief
// clear gap before protected (SVP) playback engages; after that we probe slowly, which is
// enough to notice when protected content ends, without hammering the resource manager.
#define FAST_REACQUIRE_MS 4000
#define SLOW_PROBE_MS 3000

#define GRID_W PROFILE_COLS
#define GRID_H PROFILE_ROWS

// ---------------------------------------------------------------- utilities

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static void logmsg(const char* fmt, ...)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(stderr, "[%lld.%03d] ", (long long)ts.tv_sec, (int)(ts.tv_nsec / 1000000));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

// libvtcapture calls printLog(ctx, level, id, file, func, line, fmt, ...); interpose a
// quiet stub so its chatter does not flood our log.
__attribute__((visibility("default"))) void printLog(const char* ctx, int level, const char* id,
    const char* file, const char* func, int line, const char* fmt, ...)
{
    (void)ctx; (void)level; (void)id; (void)file; (void)func; (void)line; (void)fmt;
}

// ---------------------------------------------------------------- PNG (dependency-free, stored deflate)

static uint32_t crc32_of(const uint8_t* p, size_t n, uint32_t crc)
{
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (-(int32_t)(crc & 1)));
    }
    return ~crc;
}

static uint32_t adler32_of(const uint8_t* p, size_t n)
{
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + p[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

static void put32(uint8_t* o, uint32_t v)
{
    o[0] = v >> 24; o[1] = v >> 16; o[2] = v >> 8; o[3] = v;
}

// Encode a width*height RGB image (3 bytes/pixel) to a PNG. Returns bytes written.
static size_t png_encode(const uint8_t* rgb, int w, int h, uint8_t* out, size_t out_cap)
{
    // filtered raw = per row: filter byte 0 + RGB
    size_t raw_len = (size_t)h * (1 + (size_t)w * 3);
    uint8_t* raw = malloc(raw_len);
    if (!raw) return 0;
    for (int y = 0; y < h; y++) {
        raw[y * (1 + w * 3)] = 0;
        memcpy(&raw[y * (1 + w * 3) + 1], &rgb[(size_t)y * w * 3], (size_t)w * 3);
    }
    // zlib stream with stored deflate blocks (<=65535 each)
    size_t z_cap = 2 + raw_len + (raw_len / 65535 + 1) * 5 + 4;
    uint8_t* z = malloc(z_cap);
    if (!z) { free(raw); return 0; }
    size_t zn = 0;
    z[zn++] = 0x78; z[zn++] = 0x01;
    size_t off = 0;
    while (off < raw_len) {
        size_t block = raw_len - off;
        if (block > 65535) block = 65535;
        int final = (off + block >= raw_len);
        z[zn++] = final ? 1 : 0;
        z[zn++] = block & 0xff; z[zn++] = (block >> 8) & 0xff;
        uint16_t nlen = ~(uint16_t)block;
        z[zn++] = nlen & 0xff; z[zn++] = (nlen >> 8) & 0xff;
        memcpy(&z[zn], &raw[off], block);
        zn += block;
        off += block;
    }
    put32(&z[zn], 0); // placeholder; adler is big-endian
    uint32_t ad = adler32_of(raw, raw_len);
    z[zn++] = ad >> 24; z[zn++] = ad >> 16; z[zn++] = ad >> 8; z[zn++] = ad;
    free(raw);

    size_t n = 0;
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (out_cap < 8 + 25 + 12 + zn + 12) { free(z); return 0; }
    memcpy(out + n, sig, 8); n += 8;
    // IHDR
    uint8_t ihdr[13];
    put32(ihdr, w); put32(ihdr + 4, h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0; // 8-bit RGB
    put32(out + n, 13); n += 4;
    memcpy(out + n, "IHDR", 4);
    memcpy(out + n + 4, ihdr, 13);
    put32(out + n + 4 + 13, crc32_of(out + n, 4 + 13, 0));
    n += 4 + 13 + 4;
    // IDAT
    put32(out + n, (uint32_t)zn); n += 4;
    memcpy(out + n, "IDAT", 4);
    memcpy(out + n + 4, z, zn);
    put32(out + n + 4 + zn, crc32_of(out + n, 4 + zn, 0));
    n += 4 + zn + 4;
    free(z);
    // IEND
    put32(out + n, 0); n += 4;
    memcpy(out + n, "IEND", 4);
    put32(out + n + 4, crc32_of(out + n, 4, 0));
    n += 4 + 4;
    return n;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static size_t base64_encode(const uint8_t* in, size_t n, char* out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = in[i] << 16;
        if (i + 1 < n) v |= in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? B64[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? B64[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

// ---------------------------------------------------------------- HyperHDR JSON client

static int g_sock = -1;

static void hh_close(void)
{
    if (g_sock >= 0) { close(g_sock); g_sock = -1; }
}

static bool hh_connect(void)
{
    hh_close();
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons(g_port);
    if (inet_pton(AF_INET, g_host, &a.sin_addr) != 1) {
        struct hostent* he = gethostbyname(g_host);
        if (!he) { close(fd); return false; }
        memcpy(&a.sin_addr, he->h_addr_list[0], he->h_length);
    }
    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (connect(fd, (struct sockaddr*)&a, sizeof(a)) != 0) { close(fd); return false; }
    g_sock = fd;
    logmsg("HyperHDR connected %s:%d", g_host, g_port);
    return true;
}

static bool hh_write(const char* buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = send(g_sock, buf + off, n - off, MSG_NOSIGNAL);
        if (w <= 0) return false;
        off += (size_t)w;
    }
    // drain the one-line reply (non-fatal if it times out)
    char tmp[512];
    recv(g_sock, tmp, sizeof(tmp), 0);
    return true;
}

// Send a GRID_W x GRID_H RGB image as a PNG JSON image command.
static bool hh_send_image(const uint8_t* rgb, int w, int h)
{
    static uint8_t png[8192];
    size_t pn = png_encode(rgb, w, h, png, sizeof(png));
    if (!pn) return false;
    static char b64[12000];
    base64_encode(png, pn, b64);
    static char msg[14000];
    int mn = snprintf(msg, sizeof(msg),
        "{\"command\":\"image\",\"imagedata\":\"%s\",\"format\":\"auto\","
        "\"priority\":%d,\"duration\":%d,\"origin\":\"ambilight\"}\n",
        b64, g_priority, g_duration_ms);
    if (mn <= 0 || mn >= (int)sizeof(msg)) return false;
    if (g_sock < 0 && !hh_connect()) return false;
    if (!hh_write(msg, (size_t)mn)) { hh_close(); return false; }
    return true;
}

static void hh_clear(void)
{
    if (g_sock < 0) return;
    char msg[128];
    int n = snprintf(msg, sizeof(msg), "{\"command\":\"clear\",\"priority\":%d}\n", g_priority);
    hh_write(msg, (size_t)n);
}

// ---------------------------------------------------------------- histogram (16 zones)

typedef struct {
    uint32_t id;
    uint16_t bins[3][256];
} hist_rec_t;
_Static_assert(sizeof(hist_rec_t) == 1540, "histogram record size");

static void* (*h_init)(bool (*)(void));
static int (*h_preset)(void*, int);
static int (*h_get)(void*, void*);
static int (*h_fini)(void*);
static void* g_hist;
static volatile int g_policy;

static bool policy_cb(void)
{
    g_policy++;
    return true;
}

static uint8_t mean_bin(const uint16_t* bins)
{
    uint64_t c = 0, w = 0;
    for (int b = 0; b < 256; b++) {
        c += bins[b];
        w += (uint64_t)bins[b] * b;
    }
    return c ? (uint8_t)((w + c / 2) / c) : 0;
}

#define DEVMODE_PATH "/var/luna/preferences/vt_devmode"

// Create the LG developer gate only for the moment of initHistogram; a guard child
// removes it after 10 s even if we die, and an existing marker owned by someone else
// is left untouched. Mirrors LG's own pattern (see findings doc).
static pid_t devmode_enter(int* gfd)
{
    *gfd = -1;
    int fd = open(DEVMODE_PATH, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0) return 0; // already present (or cannot create): leave it alone
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); unlink(DEVMODE_PATH); return 0; }
    close(fd);
    int p[2];
    if (pipe(p) != 0) { unlink(DEVMODE_PATH); return 0; }
    pid_t c = fork();
    if (c == 0) {
        close(p[1]);
        struct pollfd pf = { p[0], POLLIN | POLLHUP, 0 };
        poll(&pf, 1, 10000);
        struct stat cur;
        if (lstat(DEVMODE_PATH, &cur) == 0 && cur.st_ino == st.st_ino && cur.st_dev == st.st_dev)
            unlink(DEVMODE_PATH);
        _exit(0);
    }
    close(p[0]);
    if (c < 0) { close(p[1]); unlink(DEVMODE_PATH); return 0; }
    *gfd = p[1];
    return c;
}

static void devmode_leave(pid_t guard, int gfd)
{
    if (guard <= 0) return;
    close(gfd);
    waitpid(guard, NULL, 0);
}

static bool hist_load(void)
{
    void* lib = dlopen("libvtcapture.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) { logmsg("dlopen libvtcapture failed: %s", dlerror()); return false; }
    h_init = dlsym(lib, "vtCapture_initHistogram");
    h_preset = dlsym(lib, "vtCapture_setPresetHistogram");
    h_get = dlsym(lib, "vtCapture_getHistogram");
    h_fini = dlsym(lib, "vtCapture_finalizeHistogram");
    return h_init && h_preset && h_get && h_fini;
}

static bool hist_open(void)
{
    int gfd;
    pid_t guard = devmode_enter(&gfd);
    g_hist = h_init(policy_cb);
    devmode_leave(guard, gfd);
    if (!g_hist) return false;
    h_preset(g_hist, 0);
    return true;
}

static void hist_close(void)
{
    if (g_hist) { h_fini(g_hist); g_hist = NULL; }
}

// Read 16 zones into zrgb[16][3]; returns true on success.
static bool hist_read(uint8_t zrgb[16][3])
{
    hist_rec_t buf[16];
    if (!g_hist || h_get(g_hist, buf) != 0) return false;
    bool any = false;
    for (int i = 0; i < 16; i++) {
        zrgb[i][0] = mean_bin(buf[i].bins[0]);
        zrgb[i][1] = mean_bin(buf[i].bins[1]);
        zrgb[i][2] = mean_bin(buf[i].bins[2]);
        if (zrgb[i][0] | zrgb[i][1] | zrgb[i][2]) any = true;
    }
    return any || true; // all-black is still a valid reading
}

// ---------------------------------------------------------------- UI plane (halgal)

typedef struct {
    uint32_t offset;
    uint32_t* physicalAddress;
    uint16_t pitch, bpp, width, height, pixelFormat;
    uint32_t paletteInfo[257];
    uint32_t property, vendorData;
} gal_surface_t;

static int (*g_galinit)(void);
static int (*g_create)(uint32_t, uint32_t, uint32_t, gal_surface_t*);
static int (*g_capture)(gal_surface_t*);
static gal_surface_t g_surf;
static uint8_t* g_uimem;
static size_t g_uilen;
static bool g_ui_ok;

static bool ui_open(void)
{
    void* lib = dlopen("libhalgal.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) { logmsg("dlopen libhalgal failed: %s", dlerror()); return false; }
    g_galinit = dlsym(lib, "HAL_GAL_Init");
    g_create = dlsym(lib, "HAL_GAL_CreateSurface");
    g_capture = dlsym(lib, "HAL_GAL_CaptureFrameBuffer");
    if (!g_galinit || !g_create || !g_capture) return false;
    if (g_galinit() != 0) { logmsg("HAL_GAL_Init failed"); return false; }
    if (g_create(g_ui_cap_w, g_ui_cap_h, 0, &g_surf) != 0) { logmsg("CreateSurface failed"); return false; }
    int fd = open("/dev/gfx", O_RDWR);
    if (fd < 0) { logmsg("open /dev/gfx: %s", strerror(errno)); return false; }
    g_uilen = g_surf.property ? g_surf.property : (size_t)g_surf.pitch * g_surf.height;
    g_uimem = mmap(NULL, g_uilen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, g_surf.offset);
    close(fd);
    if (g_uimem == MAP_FAILED) { g_uimem = NULL; logmsg("mmap /dev/gfx failed"); return false; }
    g_ui_ok = true;
    logmsg("UI plane ready %ux%u pitch %u", g_surf.width, g_surf.height, g_surf.pitch);
    return true;
}

// Downsample the UI plane into the grid: per cell an alpha-premultiplied RGB and a mean
// alpha (0..255). Bytes in memory are B,G,R,A. Returns false if capture failed.
static bool ui_read(uint8_t urgb[GRID_H][GRID_W][3], uint8_t ualpha[GRID_H][GRID_W])
{
    if (!g_ui_ok || g_capture(&g_surf) != 0) return false;
    int W = g_surf.width, H = g_surf.height, pitch = g_surf.pitch;
    for (int gy = 0; gy < GRID_H; gy++)
        for (int gx = 0; gx < GRID_W; gx++) {
            uint64_t sr = 0, sg = 0, sb = 0, sa = 0;
            uint32_t n = 0;
            for (int y = gy * H / GRID_H; y < (gy + 1) * H / GRID_H; y++)
                for (int x = gx * W / GRID_W; x < (gx + 1) * W / GRID_W; x++) {
                    const uint8_t* p = g_uimem + (size_t)y * pitch + (size_t)x * 4;
                    uint8_t a = p[3];
                    sr += (uint64_t)p[2] * a;
                    sg += (uint64_t)p[1] * a;
                    sb += (uint64_t)p[0] * a;
                    sa += a;
                    n++;
                }
            urgb[gy][gx][0] = sa ? (uint8_t)(sr / sa) : 0;
            urgb[gy][gx][1] = sa ? (uint8_t)(sg / sa) : 0;
            urgb[gy][gx][2] = sa ? (uint8_t)(sb / sa) : 0;
            ualpha[gy][gx] = n ? (uint8_t)(sa / n) : 0;
        }
    return true;
}

// ---------------------------------------------------------------- compose

static uint8_t clamp8(double v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)(v + 0.5); }

// Build the grid image: each cell = its profile zone colour, with the UI composited on
// top by its alpha. out is GRID_H*GRID_W*3 row-major RGB.
static void compose(const uint8_t zrgb[16][3], const uint8_t urgb[GRID_H][GRID_W][3],
    const uint8_t ualpha[GRID_H][GRID_W], bool have_ui, uint8_t* out)
{
    for (int r = 0; r < GRID_H; r++)
        for (int c = 0; c < GRID_W; c++) {
            int z = PROFILE_CELL_ZONE[r][c];
            double br = zrgb[z][0] * g_gain, bg = zrgb[z][1] * g_gain, bb = zrgb[z][2] * g_gain;
            if (have_ui) {
                double a = ualpha[r][c] / 255.0;
                br = urgb[r][c][0] * a + br * (1 - a);
                bg = urgb[r][c][1] * a + bg * (1 - a);
                bb = urgb[r][c][2] * a + bb * (1 - a);
            }
            uint8_t* o = &out[(r * GRID_W + c) * 3];
            o[0] = clamp8(br); o[1] = clamp8(bg); o[2] = clamp8(bb);
        }
}

// ---------------------------------------------------------------- main loop

#ifndef AMBILIGHT_NO_MAIN
int main(int argc, char** argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host") && i + 1 < argc) snprintf(g_host, sizeof(g_host), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) g_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--priority") && i + 1 < argc) g_priority = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--interval") && i + 1 < argc) g_interval_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gain") && i + 1 < argc) g_gain = atof(argv[++i]);
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (!hist_load()) { logmsg("histogram API unavailable"); return 2; }
    if (!ui_open()) logmsg("UI plane unavailable; continuing with histogram only");

    uint8_t zrgb[16][3] = { { 0 } };     // last good zone colours (held across gaps)
    bool have_colour = false;
    uint64_t down_since = now_ms();      // we start without a handle; 0 once we hold one
    uint64_t next_open_try = 0;
    unsigned frames = 0, holds = 0, reopens = 0;
    uint64_t max_gap = 0, total_gap = 0; // gap = ms between losing and regaining colour
    uint64_t t_log = now_ms();

    logmsg("ambilight starting: host %s:%d priority %d interval %dms", g_host, g_port, g_priority, g_interval_ms);

    while (!g_stop) {
        uint64_t t = now_ms();

        if (!g_hist && t >= next_open_try) {
            bool fast = down_since && (t - down_since) < FAST_REACQUIRE_MS;
            next_open_try = t + (fast ? 0 : SLOW_PROBE_MS);
            if (hist_open()) {
                if (have_colour) {
                    uint64_t gap = down_since ? t - down_since : 0;
                    reopens++;
                    if (gap > max_gap) max_gap = gap;
                    total_gap += gap;
                    logmsg("histogram recovered after %llu ms gap", (unsigned long long)gap);
                } else {
                    logmsg("histogram opened");
                }
                down_since = 0;
            }
        }

        uint8_t z[16][3];
        bool got = g_hist && hist_read(z);
        if (got) {
            memcpy(zrgb, z, sizeof(zrgb));
            have_colour = true;
        } else if (g_hist) {
            // handle was revoked mid-stream: drop it and start re-acquiring at once
            hist_close();
            down_since = t;
            next_open_try = t; // first retry immediately (fast window)
        }
        if (!got) holds++;

        if (have_colour) {
            static uint8_t urgb[GRID_H][GRID_W][3];
            static uint8_t ua[GRID_H][GRID_W];
            bool have_ui = g_ui_ok && ui_read(urgb, ua);
            static uint8_t img[GRID_H * GRID_W * 3];
            compose(zrgb, urgb, ua, have_ui, img);
            if (hh_send_image(img, GRID_W, GRID_H)) frames++;
        }

        if (t - t_log >= 5000) {
            logmsg("frames=%u holds=%u reopens=%u hist=%s policy=%d maxgap=%llums totalgap=%llums", frames, holds,
                reopens, g_hist ? "open" : (down_since && t - down_since < FAST_REACQUIRE_MS ? "lost-fast" : "lost-slow"),
                g_policy, (unsigned long long)max_gap, (unsigned long long)total_gap);
            t_log = t;
        }

        // Pace the loop: normal frame rate while capturing; a short 10 ms step during the
        // fast re-acquire window (catch the clear gap early without a 100% spin); a relaxed
        // 50 ms while slow-probing a prolonged block. Held frames keep going out meanwhile.
        bool fast_phase = down_since && (t - down_since) < FAST_REACQUIRE_MS;
        int budget = g_hist ? g_interval_ms : (fast_phase ? 10 : 50);
        uint64_t spent = now_ms() - t;
        if ((int)spent < budget) usleep((budget - (int)spent) * 1000);
    }

    logmsg("shutting down");
    hh_clear();
    hh_close();
    hist_close();
    return 0;
}

#endif
