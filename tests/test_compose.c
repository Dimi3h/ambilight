// Native unit test: the brightness setting (gain) scales the whole output, the video
// reconstruction and the UI layer alike, and white comes out as white.
// Run with tests/run.sh.
#define AMBILIGHT_NO_MAIN
#include "ambilight.c"

static int fails;

static void expect(const char* what, const uint8_t* img, int want, int tol)
{
    int mid = (GRID_H / 2 * GRID_W + GRID_W / 2) * 3;
    bool ok = true;
    for (int c = 0; c < 3; c++)
        if (abs((int)img[mid + c] - want) > tol) ok = false;
    printf("compose %-34s -> %3d,%3d,%3d (want %d +-%d) %s\n", what, img[mid], img[mid + 1], img[mid + 2], want, tol,
        ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

int main(void)
{
    static double zm[16][3];
    for (int z = 0; z < 16; z++)
        for (int c = 0; c < 3; c++) zm[z][c] = PROFILE_WHITE[c]; // a white picture, as the histogram reports it
    static uint8_t urgb[GRID_H][GRID_W][3], ua[GRID_H][GRID_W], img[GRID_H * GRID_W * 3];
    memset(urgb, 255, sizeof(urgb)); // white UI

    memset(ua, 0, sizeof(ua)); // UI fully transparent
    g_gain = 1.0;
    compose(zm, urgb, ua, true, img);
    expect("white video, gain 1.0", img, 255, 1);
    g_gain = 0.5;
    compose(zm, urgb, ua, true, img);
    expect("white video, gain 0.5", img, 128, 1);

    memset(ua, 255, sizeof(ua)); // UI opaque
    g_gain = 1.0;
    compose(zm, urgb, ua, true, img);
    expect("opaque white UI, gain 1.0", img, 255, 1);
    g_gain = 0.15;
    compose(zm, urgb, ua, true, img);
    expect("opaque white UI, gain 0.15", img, 38, 1);

    memset(ua, 128, sizeof(ua)); // half-transparent white UI over a black picture
    for (int z = 0; z < 16; z++)
        for (int c = 0; c < 3; c++) zm[z][c] = PROFILE_BLACK[c];
    g_gain = 1.0;
    compose(zm, urgb, ua, true, img);
    expect("50% white UI over black, gain 1.0", img, 128, 2);
    g_gain = 3.0;
    compose(zm, urgb, ua, true, img);
    expect("the same, gain 3.0 (clamped)", img, 255, 0);
    return fails != 0;
}
