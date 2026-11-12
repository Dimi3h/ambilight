// Native unit test: the edge estimate and the black bars. A black picture gives a black
// image at once, white gives white everywhere, a bright logo on black leaves the edges dark,
// the garbage row at the bottom of the TV's sampling grid is left out, bars are measured to
// about half a sample and followed without toggling, and with bars the image is the picture
// between them. Run with tests/run.sh.
#define AMBILIGHT_NO_MAIN
#include "ambilight.c"

static int fails;

static void check(const char* what, bool ok, const char* detail)
{
    printf("recon %-56s %s%s%s\n", what, ok ? "ok" : "FAIL", detail[0] ? "  " : "", detail);
    if (!ok) fails++;
}

static uint8_t g_urgb[GRID_H][GRID_W][3], g_ua[GRID_H][GRID_W]; // UI layer for the tests (transparent unless set)

static void render(const double zm[16][3], uint8_t* img) { compose(zm, g_urgb, g_ua, true, img); }

static void set_area(double l, double r, double t, double b)
{
    bars_t a = { l, r, t, b };
    area_setup(&a);
}

static int img_max(const uint8_t* img, bool ring_only)
{
    int m = 0;
    for (int y = 0; y < GRID_H; y++)
        for (int x = 0; x < GRID_W; x++) {
            // the part of the image HyperHDR's LEDs look at: the outer two rows and columns
            if (ring_only && y >= 2 && y < GRID_H - 2 && x >= 2 && x < GRID_W - 2) continue;
            for (int c = 0; c < 3; c++)
                if (img[(y * GRID_W + x) * 3 + c] > m) m = img[(y * GRID_W + x) * 3 + c];
        }
    return m;
}

static int img_min(const uint8_t* img)
{
    int m = 255;
    for (int i = 0; i < GRID_H * GRID_W * 3; i++)
        if (img[i] < m) m = img[i];
    return m;
}

// The paused YouTube frame of 2026-11-12: a rainbow logo in the middle of a black picture.
// Zone means (R, G, B) as the TV reported them, garbage row included.
static const double LOGO[16][3] = { { 8.35, 8.36, 7.54 }, { 6.08, 5.82, 5.82 }, { 21.03, 21.53, 19.15 }, { 0, 0, 0 },
    { 5.31, 6.39, 3.46 }, { 3.85, 3.79, 3.85 }, { 0, 0, 0 }, { 0, 0, 0 }, { 4.40, 3.35, 5.42 }, { 3.71, 3.58, 3.49 },
    { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } };

// A picture on the TV's sample grid: grey level per sample.
typedef int (*picture_fn)(int col, int row);
static double g_pl, g_pr, g_pt, g_pb; // bars of the test pictures below, in samples
static int g_inside = 128;
static bool in_picture(int col, int row)
{
    return col + 0.5 >= g_pl && col + 0.5 <= PROFILE_GRID_W - g_pr && row + 0.5 >= g_pt && row + 0.5 <= PROFILE_GRID_H - g_pb;
}
static int pic_bars(int col, int row) { return in_picture(col, row) ? g_inside : 0; }
// the same, but the picture itself is dark along its upper and lower edge (a night scene)
static int pic_dark_edges(int col, int row)
{
    if (!in_picture(col, row)) return 0;
    return row < g_pt + 5 || row > PROFILE_GRID_H - g_pb - 6 ? 3 : g_inside;
}
// ... or only its outermost two rows
static int pic_dark_edge_rows(int col, int row)
{
    if (!in_picture(col, row)) return 0;
    return row < g_pt + 2 || row > PROFILE_GRID_H - g_pb - 3 ? 3 : g_inside;
}
static int pic_logo(int col, int row) { return abs(col - 81) < 22 && abs(row - 45) < 8 ? 200 : 0; }
static int pic_top_band(int col, int row) { (void)col; return row < 15 ? 0 : 128; }

// The histogram records the TV would return for a picture: every zone's samples, with the
// grid's last row replaced by garbage (scattered grey levels), as measured.
static void records(hist_rec_t* buf, picture_fn pic)
{
    memset(buf, 0, sizeof(hist_rec_t) * 16);
    for (int z = 0; z < 16; z++)
        for (int row = PROFILE_ZONE[z][2]; row < PROFILE_ZONE[z][2] + PROFILE_ZONE[z][3]; row++)
            for (int col = PROFILE_ZONE[z][0]; col < PROFILE_ZONE[z][0] + PROFILE_ZONE[z][1]; col++) {
                int v = row == PROFILE_GRID_H - 1 ? 40 + (col * 37) % 200 : pic(col, row);
                for (int c = 0; c < 3; c++) buf[z].bins[c][v]++;
            }
}

// Feed the same picture for `ms` milliseconds of frames, starting at *t.
static void play(picture_fn pic, uint64_t* t, int ms)
{
    static hist_rec_t buf[16];
    uint8_t z8[16][3];
    for (int k = 0; k < ms / 25; k++, *t += 25) {
        records(buf, pic);
        hist_process(buf, z8);
        bars_update(*t);
    }
}

int main(void)
{
    static uint8_t img[GRID_H * GRID_W * 3];
    static double zm[16][3];
    char d[200];
    g_gain = 1.0;
    g_preset = 0;

    // --- black, white, and a cut from white to black
    memset(zm, 0, sizeof(zm));
    render(zm, img);
    snprintf(d, sizeof(d), "max %d", img_max(img, false));
    check("video black (raw 0) gives an all-black image", img_max(img, false) == 0, d);
    for (int z = 0; z < 16; z++)
        for (int c = 0; c < 3; c++) zm[z][c] = PROFILE_WHITE[c];
    render(zm, img);
    snprintf(d, sizeof(d), "min %d max %d", img_min(img), img_max(img, false));
    check("white gives white in every cell", img_min(img) >= 254, d);
    for (int z = 0; z < 16; z++)
        for (int c = 0; c < 3; c++) zm[z][c] = PROFILE_BLACK[c];
    render(zm, img);
    snprintf(d, sizeof(d), "max %d", img_max(img, false));
    check("a cut from white to black is black in the first frame", img_max(img, false) == 0, d);

    // --- the logo on black
    for (int z = 0; z < 16; z++)
        for (int c = 0; c < 3; c++) zm[z][c] = LOGO[z][c];
    render(zm, img);
    int with_row = img_max(img, true);
    for (int c = 0; c < 3; c++) zm[1][c] = zm[5][c] = zm[9][c] = 0; // what the zones read without the garbage row
    g_linear = true;
    render(zm, img);
    int lin_ring = img_max(img, true);
    g_linear = false;
    render(zm, img);
    int ring = img_max(img, true), mid = img[((GRID_H / 2) * GRID_W + GRID_W / 2 - 1) * 3 + 1];
    snprintf(d, sizeof(d), "edge max: with the garbage row %d, without %d (least squares alone %d); centre green %d", with_row,
        ring, lin_ring, mid);
    check("logo on black: the edges stay dark, the logo stays", ring == 0 && mid > 25 && with_row >= 5, d);

    // the solve agrees with the Python model in tools/calibrate.py (settled values for this frame)
    static const struct { int k; double rgb[3]; } want[] = { { 71, { 29.53, 34.63, 29.93 } }, { 53, { 17.02, 23.29, 11.33 } },
        { 90, { 16.60, 12.84, 26.57 } }, { 102, { 22.08, 28.30, 17.11 } }, { 135, { 0, 0, 0 } }, { 64, { 0, 0, 0 } } };
    static const struct { int k; double rgb[3]; } want_lb[] = { { 71, { 28.87, 33.54, 29.74 } }, { 53, { 20.41, 27.18, 14.98 } },
        { 90, { 16.97, 13.67, 26.14 } }, { 102, { 22.65, 28.45, 19.33 } }, { 135, { 0, 0, 0 } }, { 64, { 0, 0, 0 } } };
    static double settled[3][NN_CELLS];
    double worst = 0, one = 0;
    for (int pass = 0; pass < 2; pass++) {
        set_area(0, 0, pass ? 12.5 : 0, pass ? 12.5 : 0);
        render(zm, img); // one frame, as the service does it
        memcpy(settled, g_nn_x, sizeof(settled));
        for (int rep = 0; rep < 200; rep++)
            for (int c = 0; c < 3; c++) {
                double h[16];
                for (int z = 0; z < 16; z++) h[z] = zm[z][c] > PROFILE_BLACK[c] ? zm[z][c] - PROFILE_BLACK[c] : 0;
                nnls_solve(h, g_nn_x[c]);
            }
        for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++)
            for (int c = 0; c < 3; c++) {
                double w = pass ? want_lb[i].rgb[c] : want[i].rgb[c];
                double e = fabs(level_map(g_nn_x[c][want[i].k] + PROFILE_BLACK[c], c) - w);
                if (e > worst) worst = e;
            }
        for (int c = 0; c < 3; c++)
            for (int k = 0; k < NN_CELLS; k++)
                if (fabs(g_nn_x[c][k] - settled[c][k]) > one) one = fabs(g_nn_x[c][k] - settled[c][k]);
    }
    snprintf(d, sizeof(d), "largest difference %.3f of 255 (full frame and with bars)", worst);
    check("the C solve matches the Python model", worst < 0.15, d);
    snprintf(d, sizeof(d), "largest difference %.3f raw levels", one);
    check("one frame gives the settled answer", one < 0.5, d);
    set_area(0, 0, 0, 0);

    // --- the garbage row at the bottom of the sampling grid
    static hist_rec_t buf[16];
    uint8_t z8[16][3];
    g_pl = g_pr = g_pt = g_pb = 0;
    g_inside = 0;
    records(buf, pic_bars); // a black picture; the last grid row holds garbage
    hist_process(buf, z8);
    snprintf(d, sizeof(d), "zone 5 %.3f, zone 1 %.3f, zone 9 %.3f, dark share zone 5 %.3f; left half %.2f", g_zf[5][1], g_zf[1][1],
        g_zf[9][1], g_zlow[5][1][0], g_zf[4][1]);
    check("the garbage row in a black bottom zone reads black", g_zf[5][1] == 0 && g_zf[1][1] == 0 && g_zf[9][1] == 0
        && g_zlow[5][1][0] == 1.0 && g_zf[4][1] > 0.5, d);
    g_keep_last_row = true;
    records(buf, pic_bars);
    hist_process(buf, z8);
    g_keep_last_row = false;
    snprintf(d, sizeof(d), "zone 5 %.2f", g_zf[5][1]);
    check("--keep-last-row leaves it in", g_zf[5][1] > 2.0, d);
    records(buf, pic_bars);
    for (int c = 0; c < 3; c++) buf[5].bins[c][0] += 43; // another sample total: unknown grid, hands off
    hist_process(buf, z8);
    snprintf(d, sizeof(d), "zone 5 %.2f", g_zf[5][1]);
    check("an unknown grid is left alone", g_zf[5][1] > 2.0, d);
    g_inside = 100;
    records(buf, pic_bars);
    for (int c = 0; c < 3; c++) { // a bright, even bottom zone (garbage row made even too)
        memset(buf[5].bins[c], 0, sizeof(buf[5].bins[c]));
        buf[5].bins[c][100] = 1457;
    }
    hist_process(buf, z8);
    snprintf(d, sizeof(d), "zone 5 %.2f", g_zf[5][1]);
    check("a bright bottom zone keeps its level", g_zf[5][1] == 100.0, d);
    g_inside = 128;

    // --- black bars: what one frame says
    double e[4];
    bool ok[4];
    static const struct { const char* what; double l, r, t, b; picture_fn pic; double want_v, want_h; bool decided; } cases[] = {
        { "16:9 picture: no bars", 0, 0, 0, 0, pic_bars, 0, 0, true },
        { "2.39:1 film: 12.8 % top and bottom", 0, 0, 11.65, 11.65, pic_bars, 11.65, 0, true },
        { "2:1 series: 5.5 % top and bottom", 0, 0, 5.0, 5.0, pic_bars, 5.0, 0, true },
        { "4:3 picture: 12.5 % left and right", 20.25, 20.25, 0, 0, pic_bars, 0, 20.25, true },
        { "a dark band at the top only is not a bar", 0, 0, 0, 0, pic_top_band, 0, 0, true },
        { "a logo on black: cannot tell", 0, 0, 0, 0, pic_logo, 0, 0, false },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        g_pl = cases[i].l; g_pr = cases[i].r; g_pt = cases[i].t; g_pb = cases[i].b;
        records(buf, cases[i].pic);
        hist_process(buf, z8);
        bars_estimate(e, ok);
        bool good = cases[i].decided
            ? ok[0] && ok[1] && ok[2] && ok[3] && fabs(e[2] - cases[i].want_v) <= 0.7 && fabs(e[3] - cases[i].want_v) <= 0.7
                && fabs(e[0] - cases[i].want_h) <= 0.7 && fabs(e[1] - cases[i].want_h) <= 0.7
            : !(ok[2] && ok[3]);
        snprintf(d, sizeof(d), "left %.2f right %.2f top %.2f bottom %.2f%s", e[0], e[1], e[2], e[3],
            ok[0] && ok[1] && ok[2] && ok[3] ? "" : " (undecided)");
        check(cases[i].what, good, d);
    }

    // --- black bars: following them over time
    uint64_t t = 1000;
    memset(&g_bt, 0, sizeof(g_bt));
    set_area(0, 0, 0, 0);
    g_pl = g_pr = 0; g_pt = g_pb = 11.65;
    play(pic_bars, &t, 1900);
    bool early = g_bars.t != 0;
    play(pic_bars, &t, 300);
    snprintf(d, sizeof(d), "after 1.9 s top %.1f; after 2.2 s top %.1f bottom %.1f", early ? 1.0 : 0.0, g_bars.t, g_bars.b);
    check("a film's bars are adopted after 2 s", !early && fabs(g_bars.t - 11.65) <= 0.7 && fabs(g_bars.b - 11.65) <= 0.7, d);
    double t0 = g_bars.t, b0 = g_bars.b;
    int changes = 0;
    for (int k = 0; k < 40; k++) { // a night scene: the picture's own edge rows go dark and back
        bars_t before = g_bars;
        play(k % 2 ? pic_bars : pic_dark_edges, &t, 1500);
        if (before.t != g_bars.t || before.b != g_bars.b) changes++;
    }
    snprintf(d, sizeof(d), "%d changes in 60 s; top %.1f bottom %.1f", changes, g_bars.t, g_bars.b);
    check("dark scenes do not make the bars toggle", changes == 0 && g_bars.t == t0 && g_bars.b == b0, d);
    play(pic_dark_edge_rows, &t, 10000); // two black rows next to the bars for 10 s: too little to act on
    snprintf(d, sizeof(d), "top %.1f bottom %.1f", g_bars.t, g_bars.b);
    check("a slightly larger bar is not adopted in 10 s", g_bars.t == t0 && g_bars.b == b0, d);
    play(pic_bars, &t, 100);        // a normal frame in between
    play(pic_dark_edges, &t, 4000); // a long scene with five black rows next to the bars: they grow
    double grown = g_bars.t;
    play(pic_bars, &t, 200); // light where the bar was thought to be
    snprintf(d, sizeof(d), "in a 4 s dark scene top %.1f, 0.2 s after light returns %.1f", grown, g_bars.t);
    check("a bar that grew in a dark scene shrinks back at once", grown > t0 + 2 && fabs(g_bars.t - 11.65) <= 0.7, d);
    g_pt = g_pb = 0;
    play(pic_bars, &t, 75);
    bool still = g_bars.t != 0;
    play(pic_bars, &t, 100);
    snprintf(d, sizeof(d), "after 75 ms %s, after 175 ms top %.1f", still ? "still bars" : "gone", g_bars.t);
    check("a cut to a 16:9 picture drops the bars within 0.2 s", still && g_bars.t == 0 && g_bars.b == 0, d);
    g_pt = g_pb = 11.65;
    play(pic_bars, &t, 2500);
    play(pic_logo, &t, 5000); // fade to a logo on black: nothing to decide, keep the bars
    snprintf(d, sizeof(d), "top %.1f", g_bars.t);
    check("a black picture keeps the bars it had", fabs(g_bars.t - 11.65) <= 0.7, d);
    g_ui_cover = 1.0; // a menu
    play(pic_bars, &t, 25);
    g_ui_cover = 0;
    snprintf(d, sizeof(d), "top %.1f", g_bars.t);
    check("a menu drops the bars at once", g_bars.t == 0 && g_bars.b == 0, d);
    memset(&g_bt, 0, sizeof(g_bt));
    g_pt = g_pb = 0;
    play(pic_logo, &t, 5000);
    snprintf(d, sizeof(d), "top %.1f bottom %.1f left %.1f right %.1f", g_bars.t, g_bars.b, g_bars.l, g_bars.r);
    check("a logo on black never becomes bars", g_bars.t == 0 && g_bars.b == 0 && g_bars.l == 0 && g_bars.r == 0, d);

    // --- with bars the image is the picture between them
    g_pl = g_pr = 20.25; g_pt = g_pb = 0;
    g_inside = 128;
    records(buf, pic_bars); // a 4:3 picture, even grey
    hist_process(buf, z8);
    for (int z = 0; z < 16; z++)
        for (int c = 0; c < 3; c++) zm[z][c] = g_zf[z][c] * (PROFILE_WHITE[c] - PROFILE_BLACK[c]) / 255.0 + PROFILE_BLACK[c];
    set_area(0, 0, 0, 0);
    render(zm, img);
    int l0 = 0, r0 = 0;
    for (int y = 5; y < GRID_H - 5; y++) {
        if (img[(y * GRID_W) * 3 + 1] > l0) l0 = img[(y * GRID_W) * 3 + 1];
        if (img[(y * GRID_W + GRID_W - 1) * 3 + 1] > r0) r0 = img[(y * GRID_W + GRID_W - 1) * 3 + 1];
    }
    set_area(20.25, 20.25, 0, 0);
    render(zm, img);
    snprintf(d, sizeof(d), "side columns without bars known: left %d right %d; with: min %d max %d (picture 128)", l0, r0, img_min(img),
        img_max(img, false));
    check("4:3 picture: with its bars known, every LED gets the picture", l0 < 40 && img_min(img) >= 118 && img_max(img, false) <= 138, d);

    // UI inside a bar does not reach the LEDs, UI over the picture does
    g_pl = g_pr = 0; g_pt = g_pb = 11.65;
    records(buf, pic_bars);
    hist_process(buf, z8);
    for (int z = 0; z < 16; z++)
        for (int c = 0; c < 3; c++) zm[z][c] = g_zf[z][c] * (PROFILE_WHITE[c] - PROFILE_BLACK[c]) / 255.0 + PROFILE_BLACK[c];
    set_area(0, 0, 11.65, 11.65);
    memset(g_urgb, 255, sizeof(g_urgb));
    for (int x = 0; x < GRID_W; x++) g_ua[GRID_H - 1][x] = 255; // white, opaque, in the bottom bar (a subtitle line)
    render(zm, img);
    int in_bar = img_max(img, false);
    memset(g_ua, 0, sizeof(g_ua));
    for (int x = 0; x < GRID_W; x++) g_ua[GRID_H / 2][x] = 255; // the same over the middle of the picture
    render(zm, img);
    int over_picture = img_max(img, false);
    memset(g_ua, 0, sizeof(g_ua));
    snprintf(d, sizeof(d), "brightest pixel: UI in the bar %d, UI over the picture %d (picture 128)", in_bar, over_picture);
    check("UI inside a bar stays off the LEDs", in_bar <= 138 && over_picture >= 200, d);
    set_area(0, 0, 0, 0);

    return fails != 0;
}
