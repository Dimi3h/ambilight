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
- Light on the TV: about 5% of one CPU core and 4 MB of memory at 20 frames/s; 0.2% while
  switched off.

Measurements behind every design decision: `docs/phase-0-1-findings.md`.

## Install through Homebrew Channel (recommended)

Requirements: rooted TV with Homebrew Channel, HyperHDR reachable on the network.

1. In Homebrew Channel open **Settings**, choose **Add repository** under **Repositories**
   and enter
   ```text
   https://github.com/Dimi3h/ambilight/releases/latest/download/repo.json
   ```
2. Install **Ambilight** from the list.
3. Open **Ambilight** from the TV's app list once. On this first start the app finishes the
   setup through Homebrew Channel: it grants the service its permissions (elevation), adds the
   boot hook and starts the service. Set the HyperHDR address there.

If PicCap is installed with its autostart on, turn that off in PicCap: PicCap and ambilight
cannot hold the TV's capture resource at the same time.

**Updates.** The service reads the same feed once a day. When a newer version is out, the TV
shows a short message and the settings app offers it: select **Updates → Installeren** and
press OK. The app installs it through Homebrew Channel's own installer (download, SHA-256
check); the running service notices its new files, re-applies its permissions and restarts
itself as the new version (the LEDs pause for about 4 s). Homebrew Channel itself (0.7.3) does
not update apps automatically; its **Update** button on the Ambilight page works too.

**Releases** are built by GitHub Actions (`.github/workflows/release.yml`) when a tag `vX.Y.Z`
matching `VERSION` is pushed: it runs the tests, builds the IPK and publishes it with the
feed (`repo.json`), description, icon and checksums (`tools/make_feed.py`).

## Build (one command)

```sh
sh build.sh
```

Produces `build/io.github.dimi3h.ambilight_<version>_arm.ipk`. Needs
[zig](https://ziglang.org) (cross-compiler, e.g. `brew install zig`) and python3. No
webOS NDK or Docker: the TV's libraries are loaded at runtime.

## Install from a computer (development)

Requirements: rooted TV with Homebrew Channel, root SSH access, HyperHDR reachable on the
network.

```sh
AMBI_SSH_KEY=~/.ssh/<your-tv-key> tools/install.sh <tv-ip> --hyperhdr <pi-ip>:19444 --disable-piccap
```

This builds the IPK, installs it with the TV's developer installer, elevates the service
through Homebrew Channel (root + capture permissions), adds the boot hook and starts the
service (`ambilightctl setup`). `--disable-piccap` turns PicCap's own autostart off (PicCap
and ambilight cannot hold the TV's capture resource at the same time); PicCap stays
installed, its config is backed up to `backups/` and on the TV, and the rollback turns it
back on. Re-running the script upgrades in place and keeps the settings.

## Settings app

Open **Ambilight** from the TV's app list. With the remote (▲▼ choose, OK change,
◀▶ brightness, Back close) or the pointer you can:

- switch ambilight on or off (off stays off across reboots; the service keeps running with
  the LEDs off and the histogram open, so protected content has colours again at once when
  you switch back on);
- let the remote's **blue key** switch ambilight on and off (a short on-screen message
  confirms it);
- set the HyperHDR address, port and priority;
- adjust brightness (10–300 %; it scales everything sent to the LEDs, picture and menus);
- switch black-bar detection and UI blending (menus, subtitles) on or off;
- check for and install updates;
- see the live state: picture source, HyperHDR connection, detected bars, frames/s,
  autostart, version, recent log lines, and the reason if an emergency brake switched it
  off.

The blue key is read passively from the remote's input devices (never grabbed): the TV
receives it as always. On the home screen it does nothing (measured: no app launch, no key
filter action); apps that use colour keys (teletext, HbbTV) still get it as well. Why the blue key and not a
dedicated button such as LG Channels: see the findings (LG re-applies its button table from
its server at every boot).

Changes apply immediately, without restarting the service. The app runs
`ambilightctl` through Homebrew Channel's public `exec` method; every value is checked
in the app and again by `ambilightctl` (fixed sub-commands, strict character sets), so
nothing else can reach the shell. The texts are in Dutch.

## Control (on the TV)

```sh
C=/media/developer/apps/usr/palm/services/io.github.dimi3h.ambilight.service/ambilightctl
$C status                 # human-readable state
$C pause | resume         # LEDs off / on, the service keeps running (what the app does)
$C stop                   # service off, stays off across reboots
$C start                  # on again (also clears the guards below and a pause)
$C set gain=1.2 uiblend=off   # change settings live: host, port (1-65535), priority (1-253),
                              # gain (0.1-3.0), letterbox|uiblend|bluekey on|off
$C json-status            # everything as JSON (used by the settings app)
$C check-update           # read the release feed now
$C setup                  # elevate + boot hook + start (the app runs this on first start)
```

Settings are stored in `/media/developer/ambilight/args` and reloaded by the service on
`SIGUSR1` (`ambilightctl set` does this); a pause is the file `paused` next to it. Logs:
`/media/developer/ambilight/` (`ambilight.log`, rotated at 512 KB; `supervise.log`); live
state: `status.json` (written every 2 s, but only while the app or `json-status` asks for
it, so nothing is written to the TV's flash around the clock); last update check:
`update.json`.

## Autostart and emergency brakes

Homebrew Channel runs `/var/lib/webosbrew/init.d/70-ambilight` at boot. That hook returns
immediately and starts a supervisor in the background, 30 s later, after Homebrew
Channel's own startup failsafe window. Any one of these keeps the TV booting as if
ambilight were not installed:

1. **Package missing** – the hook does nothing (a dangling hook link is skipped).
2. **`/media/developer/ambilight/disable` exists** – manual off switch (`ambilightctl stop`).
3. **Boot-loop guard** – three boots in a row without the service running a stable 20 s
   writes the disable file with the reason.
4. **Crash-loop guard** – five service exits within 60 s each writes the disable file.
5. **Homebrew Channel failsafe** – a reboot during its startup window skips all hooks on
   the next boot.

To recover after a guard tripped: read the reason with `ambilightctl status`, then
`ambilightctl start`.

When the package is replaced on disk (an update) the service exits with code 75; the
supervisor waits for the new files, re-applies the permissions and hands over to the new
version's supervisor. Replaced again within 30 s of a start counts as a quick exit for the
crash-loop guard, so a fault there cannot loop. If the package was removed instead, the
supervisor removes the boot hook and stops.

## What it writes on the TV

| Path | What |
| --- | --- |
| `/media/developer/apps/usr/palm/{applications,services,packages}/io.github.dimi3h.ambilight*` | the package (developer installer) |
| `/media/developer/ambilight/` | logs, settings (`args`), live state (`status.json`), guard state, PicCap config backup |
| `/var/lib/webosbrew/init.d/70-ambilight` | boot hook (symlink) |
| `/var/luna-service2-dev/…/io.github.dimi3h.ambilight*` | Luna role/permission files, written by the installer and Homebrew Channel's elevation |
| `/var/luna/preferences/vt_devmode` | LG's developer gate for the histogram API; created only for the moment of opening the histogram and removed right after by a guard process |

## Rollback (complete)

From Homebrew Channel: **Uninstall** on the Ambilight page removes the package. If the
service is running, its supervisor notices, clears the HyperHDR priority, removes the boot
hook and stops (this path has not been exercised on the TV yet); otherwise the boot hook is
left as a dangling link, which is skipped at boot. Settings and logs stay in
`/media/developer/ambilight` (delete that folder to remove them too).

From a computer:

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
- **Periodic 1.5 s hold (cause found 2026-11-11):** LG's surface-manager takes a
  screenshot of the foreground app for its "recent apps" list shortly after the app starts
  and then every 120 s. That screenshot takes the capture resource for ~1.5 s, during which
  the LEDs hold their colours. Apps that ship their own recents image (`imageForRecents`:
  Netflix, Prime Video, YouTube) are not captured; others are (measured: the home screen and
  a third-party player; Disney+, Apple TV and the Belgian broadcasters' apps do not declare
  it either). During protected playback such a loss cannot be repaired, and the longest
  protected run measured so far was ~100 s: colours during a whole film in those apps are
  unproven. LG has a switch for these screenshots (`com.webos.surfacemanager.disableCapture`).
  Measured: with it on, the screenshots and the losses stop at once; it is not built into
  the service yet. See the findings.
- The white point in the profile was measured in the picture mode "cinema"; other modes,
  HDR and Dolby Vision are not calibrated and may tint the LEDs.
- Calibration (`src/profile_b4.h`, `src/letterbox_b4.h`) is for the B4 on this firmware;
  `tools/calibrate.py` and `tools/letterbox.py` regenerate it (procedure in the findings).

## Development tools

`sh tests/run.sh` (native unit tests: UI reader, compose/brightness, PNG encoder, update
feed), `tools/ledtest.py` (dim test colours and an edge pattern straight to HyperHDR, to
check a real strip's colour order and layout), `tools/calibrate.py`, `tools/letterbox.py` (offline
analysis of `--calib-dump` captures), `tools/ledrec.py` + `tools/validate_live.py` (live
check of HyperHDR's LED output), the service's `--profile` flag (CPU time per loop stage,
logged every 10 s), `probe/keywatch.c` (prints remote key codes, passively), `probe/`
(phase-1 feasibility probes) and `probe/data/` (raw measurements).
