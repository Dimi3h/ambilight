# Changelog

## 0.2.6

- Subtitles stay off the LEDs. Subtitles are drawn over the picture, so the TV's colour
  statistics contain them: a two-line subtitle lifts the zone at the bottom centre by 2 to
  12 of 255, and under a dark scene the bottom LEDs lit up with every line. The text is
  recognised in the UI layer (a small, low box centred at the bottom or the top of an
  otherwise empty layer), its share is taken out of the zones again, it is not laid over
  the picture, and the bars at the top and bottom are not judged while it shows. Measured
  with white test text over a film: +4 to +6 without, under 1 with. Switch: "Ondertitels negeren"
  in the settings app (`ambilightctl set subfilter=on|off`, on by default).
- The white point follows the TV where the picture confirms it. The TV's statistics sit
  behind its white balance, whose white depends on the picture mode's colour temperature
  (measured for ten settings; blue of white runs from 205 at warm 50 to 254 at cool 50).
  One white, warm 50, was built in, so an HDR mode at warm 30 made the LEDs 8 % too blue.
  The service now takes the setting the TV reports, but only once the picture shows that
  white (many frames with their highest blue right at it and hardly any above). The check
  is needed: with an HDR film on Disney+ the TV reported warm 30 while the picture's white
  was that of warm 50. On YouTube in HDR the reported white was confirmed and in use 23 s
  after the video started; pictures at the built-in setting are untouched.
- Automatic updates: "Automatisch bijwerken" in the settings app (`ambilightctl set
  autoupdate=on|off`, off by default). A new release is downloaded and checked against the
  feed's SHA-256 by the daily check, and installed when that cannot cost anyone their
  light: when the TV starts, before the service runs, or after a minute on the home screen
  (the restart takes about 5 s). Never while an app plays: a service that restarts during
  protected video has no colours until that video stops.
- After an update the service keeps its permissions again. Homebrew Channel starts the boot
  hook without `PATH`, and its elevation then failed ("sleep: not found") whenever a
  supervisor that was started at boot handled an update. The scripts now set `PATH`.
- The service follows what the TV reports about itself (foreground app, dynamic range,
  picture mode) through three `luna-send` subscriptions; the status shows it.
- The settings app has the two new switches and still fits one screen.
- Diagnostics: `--snap <prefix>` (raw histogram and UI layer of one moment), `--fixed-white`
  and `--keep-subtitles` (previous behaviour); `--calib-dump` ends when it is taken out of
  the args file.

## 0.2.5

- The colour keys answer at once. Brightness and on/off now show in a small level of our own
  at the top right instead of in the TV's messages: on screen 0.13-0.15 s after the press,
  and the number follows every press of a run. The TV's messages cannot do that: one asked
  for while another is visible only shows 2 s later, and each stays 5 s.
- The level is a second, tiny app (`io.github.dimi3h.ambilight.osd`) in a transparent top
  layer that takes no keys, so whatever is underneath keeps working. The service installs
  and updates it from a copy in the package and removes it with the package. Without it the
  TV's messages remain, now asked for once per run of presses (0.25 s after the last one)
  and never queued behind one another.
- Brightness runs from 5 to 100 % in steps of 5, and 100 % is the maximum. It was 10-300 %
  with 100 % as "as measured"; that level is now called 50 %. Same light, new number: a
  stored setting keeps its light, and one above the new maximum becomes 100 %.

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
