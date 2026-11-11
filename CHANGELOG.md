# Changelog

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
