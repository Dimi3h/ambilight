# ambilight

Hybrid ambient-light capture for a rooted LG webOS TV (developed on an OLED B4,
firmware 33.31.75), sending colours to [HyperHDR](https://github.com/awawa-dev/HyperHDR).
Personal-use homebrew.

## What it does

- Keeps the TV's own colour **histogram** (16 zones) open continuously. An open
  histogram keeps delivering colour statistics during protected (DRM/SVP) playback, so
  Netflix/Disney+ get ambient light too. No protected frames are captured or decrypted;
  the secure video path is untouched.
- Reconstructs the colours at the screen edges from the 16 zones with a measured,
  per-model operator (`src/profile_b4.h`), removes the picture mode's white-balance tint.
- Blends the **UI plane** (menus, subtitles, launchers) on top, per pixel.
- Detects **black bars** (21:9 films) and stretches the picture to the full LED frame.
- Sends one small PNG per frame to HyperHDR's JSON API (port 19444): **one origin,
  one priority** (150 by default). HyperHDR's LED layout and smoothing do the rest.

Measurements behind every design decision: `docs/phase-0-1-findings.md`.

## Build (one command)

```sh
sh build.sh
```

Produces `build/io.github.dimi3h.ambilight_<version>_arm.ipk`. Needs
[zig](https://ziglang.org) (cross-compiler, e.g. `brew install zig`) and python3. No
webOS NDK or Docker: the TV's libraries are loaded at runtime.

## Install

Requirements: rooted TV with Homebrew Channel, root SSH access, HyperHDR reachable on the
network.

```sh
AMBI_SSH_KEY=~/.ssh/<your-tv-key> tools/install.sh <tv-ip> --hyperhdr <pi-ip>:19444 --disable-piccap
```

This builds the IPK, installs it with the TV's developer installer, elevates the service
through Homebrew Channel (root + capture permissions), adds the boot hook and starts the
service. `--disable-piccap` turns PicCap's own autostart off (PicCap and ambilight cannot
hold the TV's capture resource at the same time); PicCap stays installed, its config is
backed up to `backups/` and on the TV, and the rollback turns it back on. Re-running the
script upgrades in place.

## Settings app

Open **Ambilight** from the TV's app list. With the remote (▲▼ choose, OK change,
◀▶ brightness, Back close) or the pointer you can:

- switch ambilight on or off (off stays off across reboots);
- set the HyperHDR address, port and priority;
- adjust brightness (10–300 %);
- switch black-bar detection and UI blending (menus, subtitles) on or off;
- see the live state: picture source, HyperHDR connection, detected bars, frames/s,
  autostart, recent log lines, and the reason if an emergency brake switched it off.

Changes apply immediately, without restarting the service. The app runs
`ambilightctl` through Homebrew Channel's public `exec` method; every value is checked
in the app and again by `ambilightctl` (fixed sub-commands, strict character sets), so
nothing else can reach the shell. The texts are in Dutch.

## Control (on the TV)

```sh
C=/media/developer/apps/usr/palm/services/io.github.dimi3h.ambilight.service/ambilightctl
$C status                 # human-readable state
$C stop                   # off, stays off across reboots
$C start                  # on again (also clears the guards below)
$C set gain=1.2 uiblend=off   # change settings live: host, port (1-65535), priority (1-253),
                              # gain (0.1-3.0), letterbox on|off, uiblend on|off
$C json-status            # everything as JSON (used by the settings app)
```

Settings are stored in `/media/developer/ambilight/args` and reloaded by the service on
`SIGUSR1` (`ambilightctl set` does this). Logs: `/media/developer/ambilight/`
(`ambilight.log`, rotated at 512 KB; `supervise.log`); live state:
`status.json` (every 2 s).

## Autostart and emergency brakes

Homebrew Channel runs `/var/lib/webosbrew/init.d/70-ambilight` at boot. That hook returns
immediately and starts a supervisor in the background, 30 s later, after Homebrew
Channel's own startup failsafe window. Any one of these keeps the TV booting as if
ambilight were not installed:

1. **Package missing** – the hook does nothing (a dangling hook link is skipped).
2. **`/media/developer/ambilight/disable` exists** – manual off switch (`ambilightctl stop`).
3. **Boot-loop guard** – three boots in a row without the service running a stable 60 s
   writes the disable file with the reason.
4. **Crash-loop guard** – five service exits within 60 s each writes the disable file.
5. **Homebrew Channel failsafe** – a reboot during its startup window skips all hooks on
   the next boot.

To recover after a guard tripped: read the reason with `ambilightctl status`, then
`ambilightctl start`.

## What it writes on the TV

| Path | What |
| --- | --- |
| `/media/developer/apps/usr/palm/{applications,services,packages}/io.github.dimi3h.ambilight*` | the package (developer installer) |
| `/media/developer/ambilight/` | logs, settings (`args`), live state (`status.json`), guard state, PicCap config backup |
| `/var/lib/webosbrew/init.d/70-ambilight` | boot hook (symlink) |
| `/var/luna-service2-dev/…/io.github.dimi3h.ambilight*` | Luna role/permission files, written by the installer and Homebrew Channel's elevation |
| `/var/luna/preferences/vt_devmode` | LG's developer gate for the histogram API; created only for the moment of opening the histogram and removed right after by a guard process |

## Rollback (complete)

```sh
AMBI_SSH_KEY=~/.ssh/<your-tv-key> tools/uninstall.sh <tv-ip>
```

Stops the service (it clears its HyperHDR priority), removes the boot hook, copies the
logs to `backups/<date>-uninstall/`, removes the package (and with it its Luna files),
turns PicCap's autostart back on if the installer turned it off, and deletes
`/media/developer/ambilight`. Add `--keep-state` to keep the logs on the TV.

Manually, over SSH as root, the same steps are:

```sh
rm -f /var/lib/webosbrew/init.d/70-ambilight
/media/developer/apps/usr/palm/services/io.github.dimi3h.ambilight.service/ambilightctl stop
luna-send -n 1 luna://com.webos.appInstallService/dev/remove '{"id":"io.github.dimi3h.ambilight"}'
luna-send -n 1 luna://org.webosbrew.piccap.service/setSettings '{"autostart":true}'   # if you had it on
rm -rf /media/developer/ambilight
```

(`luna-send` prints its reply only on a terminal: use `ssh -t`.)

## Known limits

- 16 zones are coarser than a per-pixel frame grabber; validation error at the LED
  rectangles is ~18/255 on mixed test images (see the findings).
- Protected content only gets colours if the histogram was open before playback started.
  LG's own capture service briefly takes the capture resource on some app switches; if
  that happens right as protected playback starts, the last colours are held until a
  clear moment (menu, pause, next app).
- Calibration (`src/profile_b4.h`, `src/letterbox_b4.h`) is for the B4 on this firmware;
  `tools/calibrate.py` and `tools/letterbox.py` regenerate it (procedure in the findings).

## Development tools

`tools/calibrate.py`, `tools/letterbox.py` (offline analysis of `--calib-dump`
captures), `tools/ledrec.py` + `tools/validate_live.py` (live check of HyperHDR's LED
output), `probe/` (phase-1 feasibility probes) and `probe/data/` (raw measurements).
