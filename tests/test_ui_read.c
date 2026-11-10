// Native unit test: the UI-plane reader (copy out of the device mapping, then a word-wise
// scan) must give exactly the per-cell results of the original byte-wise algorithm.
// Run with tests/run.sh.
#define AMBILIGHT_NO_MAIN
#include "ambilight.c"

static int stub_capture(gal_surface_t* s)
{
    (void)s;
    return 0;
}

static int check(int W, int H, int pitch, unsigned seed)
{
    srand(seed);
    g_surf.width = W;
    g_surf.height = H;
    g_surf.pitch = pitch;
    g_uimem = malloc((size_t)pitch * H);
    for (size_t i = 0; i < (size_t)pitch * H; i++) g_uimem[i] = rand() & 255;
    g_uicopy = malloc((size_t)W * H * 4);
    g_capture = stub_capture;
    g_ui_ok = true;
    static uint8_t urgb[GRID_H][GRID_W][3], ua[GRID_H][GRID_W];
    if (!ui_read(urgb, ua)) return 1;
    int bad = 0;
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
            uint8_t r = sa ? sr / sa : 0, g = sa ? sg / sa : 0, b = sa ? sb / sa : 0, a = n ? sa / n : 0;
            if (r != urgb[gy][gx][0] || g != urgb[gy][gx][1] || b != urgb[gy][gx][2] || a != ua[gy][gx]) bad++;
        }
    printf("ui_read W=%d H=%d pitch=%d: %d of %d cells differ\n", W, H, pitch, bad, GRID_W * GRID_H);
    free(g_uimem);
    free(g_uicopy);
    return bad != 0;
}

int main(void)
{
    return check(320, 180, 1280, 1) | check(320, 180, 1344, 2) | check(333, 187, 1340, 3);
}
