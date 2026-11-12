# Changelog

## 0.2.4

- Black bars, redone. The bar height is measured from the TV's sample grid (black share of
  the zones at each edge x their size) instead of picked from seven levels: exact to about
  half a percent of the picture, the same in every frame of a film (it used to jump between
  three levels in dark scenes; one film logged 101 changes in an evening). Bars grow only
  after 2 s (a slightly larger bar after 15 s) and shrink within 0.2 s when light appears.
- Letterboxed films get the same non-negative solve as other pictures, laid over the picture
  between the bars: black stays black there too, and the error on the recorded letterbox
  pictures drops from 22.3 to 20.2 of 255.
- Side bars (4:3 pictures) are handled the same way: both side strips now show the picture's
  edge instead of the right one getting stray light.
- The settings app shows the bars in percent. The package is half the size (the tables per
  bar height are gone).
- New diagnostics `--preset-probe` and `--preset-rec` (see the findings: reading several
  histogram presets was measured and not adopted).

## 0.2.3

- Less delay and less flicker: every output frame is now the mean of three colour readings
  taken one panel frame apart, 40 times a second. The TV's measurement cycles through three
  slightly different states; three readings cancel that, where the old average of two could
  not. Delay before HyperHDR drops from about 50 ms to about 21 ms. CPU load goes from 6 %
  to 15-17 % of one core (of four).
- Black stays black. Two causes of light next to a black picture edge are gone:
  - On YouTube the last row of the TV's sampling grid holds grey values that are not on the
    screen; they lit the bottom LEDs on dark pictures (a logo on black gave 7 of 255 at the
    bottom, up to 14). That row is now left out of the three bottom zones.
  - The edge colours are now solved per frame with no cell allowed below black, instead of
    one fixed operator with negative weights: a zone the TV measures as black keeps its
    LEDs off, also next to a bright area. Slightly more accurate overall (17.2 against 17.7
    of 255 on the validation pictures).
  With both, that logo picture gives exactly 0 on every LED.
- For LEDs that are fully off on black, HyperHDR's "Minimum backlight level" has to be 0
  (README).
- The UI layer is read every other frame.
- New diagnostics: `--hist-probe <file>` (timing of the TV's colour measurement),
  `--hist-dump <file>` (raw bins), `--linear` and `--keep-last-row` (previous behaviour).
- `tools/ledtest.py`: single LEDs (`leds`, and `blink` for an LED hidden in a fold); test
  colours now pass through HyperHDR's LED layout, so LEDs disabled there stay dark.

## 0.2.2

- Colour keys: next to blue (on/off), green now dims and yellow brightens, 10 % per short
  press, with one on-screen message after the last press. The setting "Blauwe knop" became
  "Gekleurde knoppen"; an existing setting is carried over.

## 0.2.1

Fixes from a full review of the project.

- Brightness now scales everything sent to the LEDs. It used to leave the UI layer (menus,
  home screen) at full level.
- HyperHDR not reachable (Pi off): the service no longer stalls up to 2 s per frame; it
  tries to connect for at most 0.3 s every 3 s, logs the outage once, and the remote key and
  the settings app keep responding. A command HyperHDR refuses is logged.
- The remote key is read even when the loop is behind schedule.
- `status.json` is only written while the settings app (or `ambilightctl json-status`) asks
  for it: no more flash write every 2 s around the clock.
- `ambilightctl stop` could kill itself in its emergency path; `setup` (used by upgrades)
  now keeps a pause; black-bar detection no longer runs while paused.
- The boot-loop guard counts a boot as stable after 20 s instead of 60 s, so three short TV
  sessions in a row no longer switch ambilight off.
- Settings app: readable error messages; reloads itself after installing an update.
- Package four times smaller (no debug info). GitHub Actions pinned to commits; CI runs once
  per change; more unit tests (brightness, PNG encoder, update feed).
- New `tools/ledtest.py`: dim test colours and an edge pattern for a real LED strip.

## 0.2.0

- Install and update through Homebrew Channel: add the feed
  `https://github.com/Dimi3h/ambilight/releases/latest/download/repo.json`. Opening the app
  after an install finishes the setup (permissions, autostart).
- Update notice: the service checks the feed daily, shows a toast for a new version, and the
  settings app installs it with one press of OK. After an update the service re-applies its
  permissions and restarts itself.
- Blue remote key (optional, in the app): toggles ambilight on and off, with a short
  on-screen message. The key is read passively; the TV itself ignores it.
- Pause: "off" now keeps the service running with the LEDs off (and the histogram open, so
  protected content keeps its colours when you switch back on).
- CPU load from 24% to about 5% of one core: the UI layer is copied out of uncached device
  memory in one pass instead of read byte by byte, and floating point uses the FPU (softfp).
- New app icon.

## 0.1.0

- Hybrid capture: LG's 16-zone histogram (kept open, so it works for DRM content) with the
  UI layer blended on top, sent to HyperHDR's JSON API.
- Calibrated zone reconstruction for the LG OLED B4, black-bar detection.
- Autostart with emergency brakes, settings app, install and rollback scripts.
