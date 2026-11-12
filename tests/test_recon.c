// Native unit test: the edge estimate. A black picture gives a black image at once, white
// gives white everywhere, a bright logo on black leaves the edges dark, and the garbage row
// at the bottom of the TV's sampling grid is left out of the bottom zones.
// Run with tests/run.sh.
#define AMBILIGHT_NO_MAIN
#include "ambilight.c"

static int fails;

static void check(const char* what, bool ok, const char* detail)
{
    printf("recon %-52s %s%s%s\n", what, ok ? "ok" : "FAIL", detail[0] ? "  " : "", detail);
    if (!ok) fails++;
}

static void render(const double zm[16][3], uint8_t* img)
{
    static uint8_t urgb[GRID_H][GRID_W][3], ua[GRID_H][GRID_W]; // UI fully transparent
    compose(zm, urgb, ua, true, img);
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

// A histogram record set: every zone black, with the real sample counts of the bottom zones.
static void black_records(hist_rec_t* buf)
{
    static const int total[16] = { 7371, 1843, 2405, 931, 7371, 1457, 1584, 1666, 7371, 1519, 1584, 931, 931, 1666, 780, 777 };
    memset(buf, 0, sizeof(hist_rec_t) * 16);
    for (int z = 0; z < 16; z++)
        for (int c = 0; c < 3; c++) buf[z].bins[c][0] = (uint16_t)total[z];
}

// Move n samples of a zone from black to the given levels (one row of garbage).
static void add_row(hist_rec_t* buf, int zone, int n)
{
    static const int level[] = { 5, 9, 41, 58, 83, 106, 129, 174, 225, 243 };
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < n; i++) {
            buf[zone].bins[c][0]--;
            buf[zone].bins[c][level[i % 10]]++;
        }
}

int main(void)
{
    static uint8_t img[GRID_H * GRID_W * 3];
    static double zm[16][3];
    char d[160];
    g_gain = 1.0;
    g_lb_level = 0;

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
    g_linear = true;
    render(zm, img);
    int old_ring = img_max(img, true);
    g_linear = false;
    render(zm, img);
    int with_row = img_max(img, true);
    for (int c = 0; c < 3; c++) zm[1][c] = zm[5][c] = zm[9][c] = 0; // what the zones read without the garbage row
    render(zm, img);
    int ring = img_max(img, true), mid = img[((GRID_H / 2) * GRID_W + GRID_W / 2 - 1) * 3 + 1];
    snprintf(d, sizeof(d), "edge max: old estimate %d, new with garbage row %d, new without %d; centre green %d", old_ring,
        with_row, ring, mid);
    check("logo on black: the edges stay dark, the logo stays", ring == 0 && mid > 25 && old_ring >= 5, d);

    // the solve agrees with the Python model in tools/calibrate.py (values for this frame)
    static const struct { int k; double rgb[3]; } want[] = { { 71, { 26.90, 31.81, 27.12 } }, { 53, { 23.85, 31.92, 17.32 } },
        { 90, { 16.54, 11.21, 28.12 } }, { 102, { 19.17, 24.98, 13.93 } }, { 135, { 0, 0, 0 } }, { 64, { 0, 0, 0 } } };
    double worst = 0;
    for (int rep = 0; rep < 20; rep++) // let the warm-started solve settle completely
        for (int c = 0; c < 3; c++) {
            double h[16];
            for (int z = 0; z < 16; z++) h[z] = zm[z][c] > PROFILE_BLACK[c] ? zm[z][c] - PROFILE_BLACK[c] : 0;
            nnls_solve(h, g_nn_x[c]);
        }
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++)
        for (int c = 0; c < 3; c++) {
            double e = fabs(level_map(g_nn_x[c][want[i].k] + PROFILE_BLACK[c], c) - want[i].rgb[c]);
            if (e > worst) worst = e;
        }
    snprintf(d, sizeof(d), "largest difference %.3f of 255", worst);
    check("the C solve matches the Python model", worst < 0.15, d);

    // a single frame (starting from the linear operator's answer) is already settled
    static double settled[3][NN_CELLS];
    memcpy(settled, g_nn_x, sizeof(settled));
    render(zm, img);
    worst = 0;
    for (int c = 0; c < 3; c++)
        for (int k = 0; k < NN_CELLS; k++)
            if (fabs(g_nn_x[c][k] - settled[c][k]) > worst) worst = fabs(g_nn_x[c][k] - settled[c][k]);
    snprintf(d, sizeof(d), "largest difference %.3f raw levels", worst);
    check("one frame gives the settled answer", worst < 0.5, d);

    // --- a 4:3 picture (white, columns 2-13) with black side bars. The left bar has a zone of
    // its own (columns 0-1) and stays black; the right edge zone is three columns wide, so the
    // right bar cannot be told from a dimmer edge (a limit of the TV's zones, shown, not tested).
    {
        double h[16];
        int left = 0, right = 0;
        for (int z = 0; z < 16; z++) {
            h[z] = 0;
            for (int k = 0; k < NN_CELLS; k++)
                if (k % PROFILE_COLS >= 2 && k % PROFILE_COLS <= 13) h[z] += PROFILE_A[z][k] * 250.0;
        }
        for (int z = 0; z < 16; z++)
            for (int c = 0; c < 3; c++) zm[z][c] = h[z] + PROFILE_BLACK[c];
        render(zm, img);
        for (int y = 5; y < GRID_H - 5; y++) // the side LEDs away from the corners
            for (int c = 0; c < 3; c++) {
                if (img[(y * GRID_W + 0) * 3 + c] > left) left = img[(y * GRID_W + 0) * 3 + c];
                if (img[(y * GRID_W + GRID_W - 1) * 3 + c] > right) right = img[(y * GRID_W + GRID_W - 1) * 3 + c];
            }
        int inside = img[((GRID_H / 2) * GRID_W + GRID_W / 2) * 3 + 1];
        snprintf(d, sizeof(d), "left column max %d, right column max %d, middle %d", left, right, inside);
        check("4:3 picture: the left bar stays black", left == 0 && inside > 200, d);
    }

    // --- the garbage row at the bottom of the sampling grid
    static hist_rec_t buf[16];
    uint8_t z8[16][3];
    g_preset = 0;
    black_records(buf);
    add_row(buf, 5, 47);
    add_row(buf, 1, 97);
    add_row(buf, 9, 49 + 1); // one sample more than a row: it stays
    hist_process(buf, z8);
    snprintf(d, sizeof(d), "zone 5 %.3f, zone 1 %.3f, zone 9 %.3f, dark share zone 5 %.3f", g_zf[5][1], g_zf[1][1],
        g_zf[9][1], g_zlow[5][1][0]);
    check("one row of garbage in a black bottom zone reads black", g_zf[5][1] == 0 && g_zf[1][1] == 0 && g_zf[9][1] < 0.01
        && g_zlow[5][1][0] == 1.0, d);

    black_records(buf);
    add_row(buf, 5, 47);
    g_keep_last_row = true;
    hist_process(buf, z8);
    g_keep_last_row = false;
    snprintf(d, sizeof(d), "zone 5 %.2f", g_zf[5][1]);
    check("--keep-last-row leaves it in", g_zf[5][1] > 2.0, d);

    black_records(buf);
    add_row(buf, 4, 81); // a half: not one of the bottom-only zones
    for (int c = 0; c < 3; c++) { // a zone 5 with another sample count: unknown grid, hands off
        buf[5].bins[c][0] = 1500 - 47;
        buf[5].bins[c][200] = 47;
    }
    hist_process(buf, z8);
    snprintf(d, sizeof(d), "zone 4 %.2f, zone 5 %.2f", g_zf[4][1], g_zf[5][1]);
    check("other zones and an unknown grid are left alone", g_zf[4][1] > 0.5 && g_zf[5][1] > 6.0, d);

    memset(buf, 0, sizeof(buf));
    for (int z = 0; z < 16; z++)
        for (int c = 0; c < 3; c++) buf[z].bins[c][100] = 1457; // a bright, even picture
    hist_process(buf, z8);
    snprintf(d, sizeof(d), "zone 5 %.2f", g_zf[5][1]);
    check("a bright bottom zone keeps its level", g_zf[5][1] == 100.0, d);

    return fails != 0;
}
