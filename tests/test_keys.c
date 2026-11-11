// Native unit test: remote keys. Only a key going down counts; blue toggles (debounced),
// green and yellow step the brightness by 10 points within 10-300 %.
// Run with tests/run.sh.
#define AMBILIGHT_NO_MAIN
#include "ambilight.c"

static int fails;
static void check(const char* what, bool ok)
{
    printf("keys %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

int main(void)
{
    g_logf = fopen("/dev/null", "w"); // keep the test output clean
    g_toggle_key = 401; g_dim_key = 399; g_bright_key = 400;

    check("green down asks for one step down", key_event(399, 1, 1000) && g_key_steps == -1);
    check("yellow down twice asks for the way back and one up", key_event(400, 1, 1010) && key_event(400, 1, 1020) && g_key_steps == 1);
    check("release and auto-repeat are ignored", !key_event(400, 0, 1030) && !key_event(400, 2, 1040) && g_key_steps == 1);
    check("another key is ignored", !key_event(398, 1, 1050) && g_key_steps == 1 && !g_key_toggle);
    check("blue down toggles", key_event(401, 1, 2000) && g_key_toggle);
    check("blue again within 0.4 s is debounced", !key_event(401, 1, 2200) && g_key_toggle);
    g_toggle_key = g_dim_key = g_bright_key = 0;
    check("no key configured: nothing matches code 0", !key_event(0, 1, 5000));

    g_gain = 1.0; gain_step(-1, 0);
    check("100 % one step down is 90 %", gain_pct() == 90);
    g_gain = 0.15; gain_step(1, 0);
    check("an odd value snaps to tens first (15 % -> 30 %)", gain_pct() == 30);
    g_gain = 0.10; gain_step(-3, 0);
    check("does not go below 10 %", gain_pct() == 10);
    g_gain = 2.95; gain_step(2, 0);
    check("does not go above 300 %", gain_pct() == 300);
    check("a message and a save are scheduled", g_gain_toast_at == 600 && g_gain_save_at == 1200);
    return fails != 0;
}
