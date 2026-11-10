# Changelog

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
