// Native unit test: the white point read from the picture, subtitles taken out of the
// zones, the TV's own reports (luna-send replies) and the moment an update may install.
// Run with tests/run.sh.
#define AMBILIGHT_NO_MAIN
#include "ambilight.c"

static int fails;
static void check(const char* what, bool ok, const char* detail)
{
    printf("tv %-58s %s  %s\n", what, ok ? "ok" : "FAIL", detail ? detail : "");
    if (!ok) fails++;
}

// A frame's histogram: `n` samples at blue level `blue` in zone `zone`, the rest black.
static void records(hist_rec_t* buf, int zone, int blue, int n)
{
    memset(buf, 0, 16 * sizeof(*buf));
    for (int z = 0; z < 16; z++)
        for (int c = 0; c < 3; c++) buf[z].bins[c][0] = (uint16_t)(PROFILE_ZONE[z][1] * PROFILE_ZONE[z][3]);
    buf[zone].bins[2][0] = (uint16_t)(buf[zone].bins[2][0] - n);
    buf[zone].bins[2][blue] = (uint16_t)n;
}

// `frames` output frames, 25 ms apart, with highest blue level `top`.
static uint64_t run_wb(uint64_t t, int frames, int top)
{
    for (int i = 0; i < frames; i++, t += 25) wb_update(t, top);
    return t;
}

// Subtitle-like text in the capture: every other pixel of two lines, colour rgb, opaque.
static void draw_text(int x0, int x1, int y0, int y1, uint32_t rgb)
{
    int W = g_surf.width;
    for (int y = y0; y <= y1; y++)
        for (int x = x0 + (y & 1); x <= x1; x += 2) g_uicopy[(size_t)y * W + x] = 0xff000000u | rgb;
}

static void clear_ui(void) { memset(g_uicopy, 0, (size_t)g_surf.width * g_surf.height * 4); }

int main(void)
{
    char d[200];
    g_logf = fopen("/dev/null", "w");

    // ---------------------------------------------------------------- white point
    static hist_rec_t buf[16];
    records(buf, 2, 222, WB_TOP_K);
    int top = wb_top_of(buf);
    records(buf, 2, 222, WB_TOP_K - 1);
    int few = wb_top_of(buf);
    records(buf, 1, 240, 200); // the bottom-centre zone holds the grid's last row
    int bottom = wb_top_of(buf);
    snprintf(d, sizeof(d), "%d samples: %d, %d samples: %d, bottom zone only: %d", WB_TOP_K, top, WB_TOP_K - 1, few, bottom);
    check("highest blue level: needs a few samples, not from the last row", top == 222 && few == 0 && bottom == 0, d);

    uint64_t t = 100000;
    wb_reset(t, "-50"); // SDR at the profile's own setting, a film with saturated blue (as measured on Plex)
    static const int plex[] = { 209, 214, 219, 215, 212, 212, 167, 160, 204, 203, 154, 168, 155, 187, 199, 211, 200, 200, 200, 208 };
    for (int i = 0; i < 4800; i++, t += 25) wb_update(t, plex[(i / 40) % 20]);
    snprintf(d, sizeof(d), "setting %.1f, blue gain %.3f", g_wb_applied, g_wb_gain[2]);
    check("reported = the profile's setting: nothing changes, whatever the picture", g_wb_applied == -50 && g_wb_gain[2] == 1, d);

    wb_reset(t, "-30"); // HDR on YouTube: the mode's white is in force, highlights clip at it
    uint64_t t1 = run_wb(t, 60, 222); // 1.5 s: the settling time
    bool settling = g_wb_confirmed;
    t = run_wb(t1, 39, 222);
    bool almost = g_wb_confirmed;
    t = run_wb(t, 1, 221);
    bool yes = g_wb_confirmed;
    t = run_wb(t, 60, 180);
    snprintf(d, sizeof(d), "settling %d, 39 frames %d, 40 frames %d; then setting %.1f, white B %.1f (221.2 x 204.9 / 205.3 = 220.8)", settling,
        almost, yes, g_wb_applied, white_of(2));
    check("reported warm 30 and a ceiling at its white: taken over", !settling && !almost && yes && g_wb_applied == -30
        && fabs(white_of(2) - 220.8) < 0.3, d);
    snprintf(d, sizeof(d), "%.1f %.1f", level_map(white_of(2), 2), level_map(white_of(1), 1));
    check("... and that white maps to 255", level_map(white_of(2), 2) == 255 && level_map(white_of(1), 1) == 255, d);
    t = run_wb(t, 4800, 120); // two minutes of a dark scene
    check("... and kept through a dark scene", g_wb_confirmed && g_wb_applied == -30, "");
    t = run_wb(t, 400, 232); // then ten seconds of blue well above that white
    snprintf(d, sizeof(d), "setting %.1f", g_wb_applied);
    check("... until blue plainly goes above it", !g_wb_confirmed && g_wb_applied == -50, d);

    wb_reset(t, "-30"); // the HDR film on Disney+: reported warm 30, the picture's white is at 205
    for (int i = 0; i < 4800; i++, t += 25) wb_update(t, i % 300 < 60 ? 206 : 190);
    snprintf(d, sizeof(d), "setting %.1f (%d frames at the reported white)", g_wb_applied, g_wb_at);
    check("reported warm 30, picture never above 206: the profile's white stays", !g_wb_confirmed && g_wb_applied == -50, d);

    wb_reset(t, "-30"); // saturated blue passing through 221-223 on its way up is not a ceiling
    static const int blue[] = { 206, 212, 218, 222, 226, 231, 224, 221, 214, 207 };
    for (int i = 0; i < 4800; i++, t += 25) wb_update(t, blue[(i / 20) % 10]);
    snprintf(d, sizeof(d), "%d frames at 221-223, %d above", g_wb_at, g_wb_above);
    check("blue that passes through the reported white is not taken for it", !g_wb_confirmed && g_wb_applied == -50, d);

    wb_reset(t, "-30");
    t = run_wb(t, 2400, -1); // a menu: the frames do not count
    check("frames of a UI screen do not count", !g_wb_confirmed && g_wb_at == 0 && g_wb_above == 0, "");
    t = run_wb(t, 120, 222);
    check("... the video after it does", g_wb_confirmed, "");
    wb_reset(t, "-50");
    t = run_wb(t, 60, 150);
    snprintf(d, sizeof(d), "setting %.1f, blue gain %.3f", g_wb_applied, g_wb_gain[2]);
    check("another app or mode: back to the profile's white", g_wb_applied == -50 && fabs(g_wb_gain[2] - 1) < 1e-9, d);

    wb_reset(t, "50");
    t = run_wb(t, 400, 255);
    snprintf(d, sizeof(d), "%.1f: white %.1f %.1f %.1f", g_wb_applied, white_of(0), white_of(1), white_of(2));
    check("cool 50 confirmed: red is the channel that gives way", g_wb_applied == 50 && white_of(0) < 247.5 && white_of(2) > 252, d);
    g_wb_fixed = true;
    t = run_wb(t, 3, 255);
    check("--fixed-white keeps the profile's white", g_wb_gain[0] == 1 && g_wb_gain[1] == 1 && g_wb_gain[2] == 1, "");
    g_wb_fixed = false;
    wb_reset(t, "");
    double none = g_wb_target;
    wb_reset(t, "80");
    double far = g_wb_target;
    wb_reset(t, "warm");
    check("nothing or nonsense reported: the profile's setting", none == -50 && far == -50 && g_wb_target == -50, "");
    t = run_wb(t, 300, 100);

    // ---------------------------------------------------------------- subtitles
    g_surf.width = 320;
    g_surf.height = 180;
    g_uicopy = malloc(320 * 180 * 4);
    clear_ui();
    draw_text(108, 212, 157, 164, 0xffff00); // yellow, as on VRT MAX
    draw_text(120, 200, 167, 174, 0xffff00);
    sub_scan();
    snprintf(d, sizeof(d), "active %d, box x %d-%d y %d-%d, bottom-centre zone: %.0f of %.0f samples", g_sub.active, g_sub.x0, g_sub.x1,
        g_sub.y0, g_sub.y1, g_sub.a[1], zone_samples(1));
    check("two yellow lines at the bottom are a subtitle", g_sub.active && g_sub.a[1] > 80 && g_sub.a[1] < 300 && g_sub.a[5] == 0
        && g_sub.a[9] == 0 && g_sub.a[3] == 0 && g_sub.s[1][2] == 0 && g_sub.s[1][0] == g_sub.a[1] * 255, d);

    // a black picture with that text: what the histogram reports, then what is left of it
    double zf[16][3];
    double with_text = 0;
    for (int pass = 0; pass < 2; pass++) {
        double video = pass ? 100.0 : PROFILE_BLACK[0];
        for (int z = 0; z < 16; z++)
            for (int c = 0; c < 3; c++) {
                double n = zone_samples(z), a = g_sub.a[z];
                zf[z][c] = (a * PROFILE_BLACK[c] + g_sub.s[z][c] * (white_of(c) - PROFILE_BLACK[c]) / 255 + (n - a) * video) / n;
            }
        if (!pass) with_text = zf[1][0];
        sub_remove(zf);
        if (!pass) {
            snprintf(d, sizeof(d), "bottom-centre red %.1f with the text, %.2f after; left half %.2f", with_text, zf[1][0], zf[4][0]);
            check("black under a subtitle stays black", with_text > 10 && zf[1][0] <= PROFILE_BLACK[0] && zf[4][0] <= PROFILE_BLACK[0]
                && zf[1][1] <= PROFILE_BLACK[1], d);
        } else {
            snprintf(d, sizeof(d), "red %.1f green %.1f blue %.1f (picture 100)", zf[1][0], zf[1][1], zf[1][2]);
            check("a grey picture keeps its level", fabs(zf[1][0] - 100) < 0.01 && fabs(zf[1][1] - 100) < 0.01
                && fabs(zf[1][2] - 100) < 0.01 && fabs(zf[5][0] - 100) < 1e-9, d);
        }
    }

    // bars: a subtitle in the bar must not end it
    g_lb_enabled = true;
    g_ui_cover = 0;
    for (int z = 0; z < 16; z++)
        for (int c = 0; c < 3; c++)
            for (int k = 0; k < NLOW; k++) g_zlow[z][c][k] = 0; // light everywhere: no bars
    for (int i = 0; i < 4; i++) g_bt.cur[i] = i >= 2 ? 12 : 0;
    for (uint64_t u = 0; u < 1000; u += 25) bars_update(t + u);
    bool held = g_bt.cur[2] == 12 && g_bt.cur[3] == 12;
    g_sub.active = false;
    for (uint64_t u = 1000; u < 2000; u += 25) bars_update(t + u);
    snprintf(d, sizeof(d), "with the subtitle %s, after it top %.1f bottom %.1f", held ? "kept" : "dropped", g_bt.cur[2], g_bt.cur[3]);
    check("top and bottom bars wait while a subtitle shows", held && g_bt.cur[2] == 0 && g_bt.cur[3] == 0, d);

    static const struct { const char* what; int x0, x1, y0, y1; bool want; } boxes[] = {
        { "white text at the top is a subtitle too", 110, 210, 10, 26, true },
        { "our own level at the top right is not", 231, 309, 11, 28, false },
        { "a banner across the whole width is not", 4, 316, 150, 170, false },
        { "something in the middle of the screen is not", 110, 210, 80, 100, false },
        { "a tall box is not", 110, 210, 100, 176, false },
    };
    for (size_t i = 0; i < sizeof(boxes) / sizeof(boxes[0]); i++) {
        clear_ui();
        draw_text(boxes[i].x0, boxes[i].x1, boxes[i].y0, boxes[i].y1, 0xf0f0f0);
        sub_scan();
        check(boxes[i].what, g_sub.active == boxes[i].want, "");
    }
    clear_ui();
    for (int y = 120; y < 176; y++)
        for (int x = 60; x < 260; x++) g_uicopy[y * 320 + x] = 0xc0202020u; // a menu: 19 % of the plane
    sub_scan();
    check("a menu is not", !g_sub.active, "");
    clear_ui();
    sub_scan();
    check("an empty plane is not", !g_sub.active, "");
    draw_text(108, 212, 157, 174, 0xffff00);
    g_sub_filter = false;
    sub_scan();
    check("--keep-subtitles: left alone", !g_sub.active, "");
    g_sub_filter = true;

    // ---------------------------------------------------------------- the TV's reports (lines as the B4 sends them)
    char v[64];
    check("a reply with spaces around the colon", json_str("{ \"state\": \"Active\", \"subscribed\": true }", "state", v, sizeof(v))
        && !strcmp(v, "Active"), v);
    check("a missing key", !json_str("{\"returnValue\":true,\"settings\":{}}", "dynamicRange", v, sizeof(v)), "");
    tv_line(TVW_APP, "{\"subscribed\":true,\"returnValue\":true,\"appId\":\"com.webos.app.home\",\"appCategory\":[\"home\"],\"processId\":\"\",\"windowId\":\"\"}", 1000);
    tv_line(TVW_RANGE, "{\"subscribed\":true,\"category\":\"dimensionInfo\",\"method\":\"getSystemSettings\",\"returnValue\":true,\"settings\":{\"twinMode\":\"off\",\"input\":\"default\",\"dynamicRange\":\"sdr\",\"resolution\":\"sd\"}}", 1000);
    tv_line(TVW_PICTURE, "{\"subscribed\":true,\"category\":\"picture\",\"method\":\"getSystemSettings\",\"dimension\":{\"dynamicRange\":\"sdr\",\"input\":\"default\"},\"settings\":{\"colorTemperature\":\"-50\",\"pictureMode\":\"cinema\"},\"returnValue\":true}", 1000);
    snprintf(d, sizeof(d), "%s, %s, %s, %s", g_tv_app, g_tv_range, g_tv_mode, g_tv_temp);
    check("first replies: app, range, mode, colour temperature", g_tv_changed && !strcmp(g_tv_app, "com.webos.app.home")
        && !strcmp(g_tv_range, "sdr") && !strcmp(g_tv_mode, "cinema") && !strcmp(g_tv_temp, "-50") && g_tv_app_since == 1000, d);
    g_tv_changed = false;
    tv_line(TVW_RANGE, "{\"returnValue\":true,\"method\":\"getSystemSettings\",\"app_id\":\"\",\"category\":\"dimensionInfo\",\"settings\":{\"resolution\":\"fhd\",\"input\":\"default\"}}", 2000);
    tv_line(TVW_PICTURE, "{\"returnValue\":true,\"dimension\":{\"pictureMode\":\"cinema\",\"dynamicRange\":\"sdr\",\"input\":\"default\"},\"settings\":{\"pictureMode\":\"cinema\",\"colorTemperature\":\"-50\"}}", 2000);
    tv_line(TVW_APP, "{\"subscribed\":true,\"returnValue\":true,\"appId\":\"com.webos.app.home\",\"appCategory\":[\"home\"]}", 2000);
    check("replies that repeat what is known are no change", !g_tv_changed && g_tv_app_since == 1000, "");
    tv_line(TVW_RANGE, "{\"returnValue\":true,\"settings\":{\"dynamicRange\":\"hdr\"}}", 3000);
    tv_line(TVW_PICTURE, "{\"returnValue\":true,\"dimension\":{\"pictureMode\":\"hdrCinemaBright\",\"dynamicRange\":\"hdr\",\"input\":\"default\"},\"settings\":{\"pictureMode\":\"hdrCinemaBright\",\"colorTemperature\":\"-30\"}}", 3000);
    snprintf(d, sizeof(d), "%s, %s, %s", g_tv_range, g_tv_mode, g_tv_temp);
    check("HDR starts: range, mode and temperature follow", g_tv_changed && !strcmp(g_tv_range, "hdr")
        && !strcmp(g_tv_mode, "hdrCinemaBright") && !strcmp(g_tv_temp, "-30") && g_tv_app_since == 1000, d);
    g_tv_changed = false;
    tv_line(TVW_APP, "{\"subscribed\":true,\"returnValue\":true,\"appId\":\"\",\"appCategory\":[]}", 4000);
    check("between two apps: no app, and that is a change", g_tv_changed && !g_tv_app[0] && g_tv_app_since == 4000, "");

    // ---------------------------------------------------------------- the subscriptions themselves
    // A stand-in for luna-send: a first reply at once, a second one in two pieces, then it
    // ends (as luna-send does when the bus goes away).
    char script[] = "/tmp/ambilight-luna-XXXXXX";
    int sfd = mkstemp(script);
    const char* body = "#!/bin/sh\n"
        "case \"$2\" in *getForegroundAppInfo) ;; *) sleep 5; exit 0 ;; esac\n"
        "echo '{\"subscribed\":true,\"appId\":\"youtube.leanback.v4\"}'\n"
        "sleep 0.3\nprintf '{\"appId\":\"com.disney.'\nsleep 0.2\necho 'disneyplus-prod\"}'\nsleep 0.3\n";
    if (sfd < 0 || write(sfd, body, strlen(body)) < 0) { perror("stand-in"); return 1; }
    fchmod(sfd, 0700);
    close(sfd);
    g_luna_send = script;
    g_tv_app[0] = 0;
    g_tv_changed = false;
    char seen[3][64] = { "", "", "" };
    int nseen = 0;
    uint64_t t0 = now_ms(), ended = 0;
    while (now_ms() - t0 < 3000 && !ended) {
        tv_poll(now_ms());
        if (g_tv_changed) {
            g_tv_changed = false;
            if (nseen < 3) snprintf(seen[nseen++], 64, "%s", g_tv_app);
        }
        if (nseen && g_tvw[TVW_APP].fd < 0) ended = now_ms() - t0;
        usleep(20000);
    }
    snprintf(d, sizeof(d), "%s, then %s; ended after %llu ms, retry in %llu s", seen[0], seen[1], (unsigned long long)ended,
        (unsigned long long)((g_tvw[TVW_APP].retry_at - now_ms() + 500) / 1000));
    check("replies arrive as lines, also in pieces; the end is noticed", nseen == 2 && !strcmp(seen[0], "youtube.leanback.v4")
        && !strcmp(seen[1], "com.disney.disneyplus-prod") && ended > 0 && g_tvw[TVW_APP].pid == 0
        && g_tvw[TVW_APP].retry_at > now_ms() + TV_RETRY_MS - 2000, d);
    check("the other two subscriptions are still running", g_tvw[TVW_RANGE].fd >= 0 && g_tvw[TVW_PICTURE].pid > 0, "");
    tv_close_all();
    check("... and are stopped with the service", g_tvw[TVW_RANGE].fd < 0 && g_tvw[TVW_RANGE].pid == 0 && g_tvw[TVW_PICTURE].pid == 0, "");
    unlink(script);

    // ---------------------------------------------------------------- when an update may install
    char dir[] = "/tmp/ambilight-test-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    char ready[80];
    snprintf(g_args_path, sizeof(g_args_path), "%s/args", dir);
    snprintf(ready, sizeof(ready), "%s/update-ready", dir);
    g_self[0] = 0; // no ambilightctl in the tests: asking does nothing
    FILE* f = fopen(ready, "w");
    fputs("9.9.9\n", f);
    fclose(f);
    t = 10 * 3600 * 1000;
    tv_line(TVW_APP, "{\"appId\":\"netflix\"}", t);
    int asked = g_update_asks;
    g_auto_update = false;
    for (uint64_t u = 0; u < 300000; u += 1000) update_poll(t + u);
    check("automatic updates off: never", g_update_asks == asked, "");
    g_auto_update = true;
    t += 3600 * 1000;
    for (uint64_t u = 0; u < 300000; u += 1000) update_poll(t + u);
    check("a streaming app in the foreground: never", g_update_asks == asked, "");
    t += 3600 * 1000;
    tv_line(TVW_APP, "{\"appId\":\"com.webos.app.home\"}", t);
    for (uint64_t u = 0; u < 59000; u += 1000) update_poll(t + u);
    bool early = g_update_asks != asked;
    for (uint64_t u = 59000; u < 125000; u += 1000) update_poll(t + u);
    snprintf(d, sizeof(d), "asked %d time(s)", g_update_asks - asked);
    check("the home screen for a minute: asked once", !early && g_update_asks == asked + 1, d);
    for (uint64_t u = 125000; u < 500000; u += 1000) update_poll(t + u);
    check("... and not again for ten minutes", g_update_asks == asked + 1, "");
    unlink(ready);
    t += 3600 * 1000;
    for (uint64_t u = 0; u < 300000; u += 1000) update_poll(t + u);
    check("nothing downloaded: never", g_update_asks == asked + 1, "");
    rmdir(dir);

    printf(fails ? "tv: %d FAILED\n" : "tv: all ok\n", fails);
    return fails != 0;
}
