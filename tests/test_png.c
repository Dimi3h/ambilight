// Native unit test helper: encode a known image with the service's own PNG encoder and
// write it to argv[1]; tests/check_png.py decodes it independently and compares pixels.
#define AMBILIGHT_NO_MAIN
#include "ambilight.c"

int main(int argc, char** argv)
{
    if (argc < 2) return 2;
    static uint8_t rgb[GRID_H * GRID_W * 3], png[8192];
    for (int i = 0; i < GRID_H * GRID_W * 3; i++) rgb[i] = (uint8_t)(i * 7 + (i >> 5));
    size_t n = png_encode(rgb, GRID_W, GRID_H, png, sizeof(png));
    if (!n) return 1;
    FILE* f = fopen(argv[1], "wb");
    if (!f) return 1;
    fwrite(png, 1, n, f);
    fclose(f);
    // and base64 of a short known string
    char b64[16];
    base64_encode((const uint8_t*)"ambi", 4, b64);
    if (strcmp(b64, "YW1iaQ==") != 0) { printf("base64 FAIL: %s\n", b64); return 1; }
    printf("png %dx%d: %zu bytes written, base64 ok\n", GRID_W, GRID_H, n);
    return 0;
}
