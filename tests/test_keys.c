// Native unit test: remote keys. Only a key going down counts; blue toggles (debounced),
// green and yellow step the brightness by 5 points within 5-100 % (gain 0.1-2.0), and the
// level is shown with every press (or, without the level app, by toasts that never queue).
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

    g_toast_mute = true; // no calls to the TV from the tests
    g_gain = 1.0; gain_step(-1, 0);
    check("as measured (50 %) one step down is 45 % (gain 0.9)", gain_pct() == 90 && bright_pct() == 45);
    g_gain = 0.15; gain_step(1, 0);
    check("an odd value snaps to a step first (gain 0.15 -> 0.3)", gain_pct() == 30);
    g_gain = 0.10; gain_step(-3, 0);
    check("does not go below 5 % (gain 0.1)", gain_pct() == 10 && bright_pct() == 5);
    g_gain = 1.95; gain_step(2, 0);
    check("does not go above 100 % (gain 2.0)", gain_pct() == 200 && bright_pct() == 100);
    char* a[] = { "x", "--gain", "3.0" };
    apply_args(3, a);
    check("a stored gain above the maximum becomes 100 %", g_gain == 2.0);
    check("the save is scheduled 1.2 s after the press", g_gain_save_at == 1200);

    // --- the on-screen message. The TV shows a toast 0.1 s after the call on an idle screen,
    // but 2 s after the call while another toast is visible; the service must never queue.
    g_self[0] = 0; // no ambilightctl in the tests: the save does nothing
    g_gain = 1.0;
    g_gain_dirty = false;
    g_toast_pending_until = g_toast_visible_until = 0;
    uint64_t t, first = 0, asked[8];
    int n0 = g_toast_count, na = 0;
    bool queued = false;
#define RUN(until) for (; t < (until); t += 25) { int c = g_toast_count; uint64_t pend = g_toast_pending_until; gain_followup(t); \
        if (g_toast_count != c) { if (t < pend) queued = true; if (na < 8) asked[na++] = t; if (!first) first = t; } }
    t = 100000;
    gain_step(1, t); // one press on an idle screen
    RUN(100000 + 3000);
    check("one press: message asked for 0.25 s later, once", g_toast_count == n0 + 1 && first == 100250
        && !strcmp(g_toast_last, "Ambilight helderheid 55%"));

    t = 200000; n0 = g_toast_count; na = 0; first = 0; // four quick presses, 150 ms apart
    for (int k = 0; k < 4; k++) { gain_step(1, t); RUN(200000 + (uint64_t)(k + 1) * 150); }
    RUN(200000 + 4000);
    check("a quick burst: one message, with the final value", g_toast_count == n0 + 1 && !strcmp(g_toast_last, "Ambilight helderheid 75%")
        && first <= 200000 + 450 + 250);

    t = 300000; n0 = g_toast_count; na = 0; first = 0; // presses that keep coming for 2 s, 200 ms apart
    for (int k = 0; k < 10; k++) { gain_step(-1, t); RUN(300000 + (uint64_t)(k + 1) * 200); }
    RUN(300000 + 8000);
    check("a long run of presses: first message within 0.6 s, the last shows the final value", first == 300600
        && g_toast_count >= n0 + 2 && g_toast_count <= n0 + 3 && !strcmp(g_toast_last, "Ambilight helderheid 25%"));

    t = 400000; n0 = g_toast_count; na = 0; first = 0; // three presses 0.8 s apart: each lands while a toast is visible
    for (int k = 0; k < 3; k++) { gain_step(1, t); RUN(400000 + (uint64_t)(k + 1) * 800); }
    RUN(400000 + 9000);
    char note[160];
    snprintf(note, sizeof(note), "slow presses: asked at +%d, +%d, +%d ms; final value shown", (int)(asked[0] - 400000),
        na > 1 ? (int)(asked[1] - 400000) : -1, na > 2 ? (int)(asked[2] - 400000) : -1);
    check(note, g_toast_count == n0 + 3 && asked[0] == 400250 && asked[1] == 401050 && asked[2] >= asked[1] + 2000
        && !strcmp(g_toast_last, "Ambilight helderheid 40%"));
    check("no message is ever asked for while one is still on its way", !queued);

    // --- with the level app every press is shown at once, and no toast is used
    g_osd = true;
    n0 = g_toast_count;
    int o0 = g_osd_count;
    g_gain = 1.5;
    gain_step(1, 500000);
    check("level app: a press shows the new value at once", g_osd_count == o0 + 1 && !strcmp(g_osd_last, "{\"pct\":80}"));
    gain_step(1, 500100);
    gain_step(-1, 500200);
    t = 500200;
    RUN(500200 + 5000);
    check("level app: every press is shown, no toast", g_osd_count == o0 + 3 && !strcmp(g_osd_last, "{\"pct\":80}") && g_toast_count == n0);
    g_paused = false;
    set_paused(true, false, true);
    check("level app: blue shows off", !strcmp(g_osd_last, "{\"state\":\"off\"}") && g_toast_count == n0);
    set_paused(false, false, true);
    check("level app: and on again", !strcmp(g_osd_last, "{\"state\":\"on\"}"));
    g_osd = false;
    return fails != 0;
}
