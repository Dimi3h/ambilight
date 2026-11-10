# Phase 0–1 findings (2026-11-10)

Target: LG OLED55B46LA (board K24_DVB_EU), firmware 33.31.75, webOS `sdkVersion` 10.3.2
(the 10.x line, not the 9.x line that shipped as webOS 24), 32-bit ARM userland
(soft-float, glibc 2.35), 4× Cortex-A73, 2 GB RAM. Receiver: HyperHDR 22.0.0 on a
Raspberry Pi 4 (Debian 13), 210 LEDs.

Raw data: `probe/data/2026-11-10/`. Probe sources: `probe/`.

## Method

`probe/vtprobe.c` dlopen()s the TV's own `libvtcapture.so.1` and `libhalgal.so.1` and
interposes `printLog`, so LG's internal log lines are visible. It ran as a temporarily
installed and Homebrew-Channel-elevated native service (`io.github.dimi3h.ambilight.probe`),
because libvtcapture's resource-manager client must register on the Luna bus
(`LSRegisterPubPriv FAILED` from a plain SSH process). The package was removed afterwards.
`probe/pipewatch.sh` polls `luna://com.webos.media/getActivePipelines`.
`probe/ledlog.py` logs HyperHDR's LED stream (JSON API, ~10 updates/s).

## Answers

### 1. Does the histogram API return spatial zones, and how are they laid out?

Yes. `vtCapture_getHistogram` writes 16 records (verified with a canary buffer) of
`{uint32 id; uint16 bins[3][256]}` for presets 0–2 and 14 records for preset 3.
Valid presets are 0–3 (LG: `preset mode should be 0 ~ 3`). Bin counts per record are
fixed and differ per region, so regions differ in size.

The histogram is taken from the processed output, including the graphics (UI) plane
and the picture-mode white balance: a white UI cell measured as (27,26,22) and a grey
splash as (115,110,99).

Preset 0 footprint, measured with a white cell walking a 16×9 grid on the UI plane
(`#` strong, `+` weak, `?` no data: lost to a revocation):

| Zone | Area |
| --- | --- |
| 4 | left half (cols 0–7, full height) |
| 8 | right half (cols 8–15, full height) |
| 0 | centre band (cols 4–11, full height) |
| 2 | centre (cols 5–10, rows 3–5) |
| 1 | weak everywhere, strong bottom centre (cols 3–12, rows 7–8) |
| 3 | top, left part (rows 0–1, cols ~1–6) |
| 14, 11 | top centre-left (rows 0–1, cols 6–10) |
| 12 | top centre-right (rows 0–1, cols 10–14) |
| 13 | top-right corner (rows 0–3, cols 11–15) |
| 7 | top-left corner (rows 1–3, cols 0–4) |
| 6, 15 | left edge (rows 2–6, cols 0–2 / 0–1) |
| 5 | bottom-left (rows 6–8, cols 0–4) |
| 10 | right edge (rows 2–6, cols 13–15) |
| 9 | bottom-right (rows 6–8, cols 11–15) |

Six cells of the top row (cols 0–5) were not measured. Preset 3 was not mapped.

### 2. Can frame capture and histogram run side by side?

No. Both open the same V4L2 device (`/dev/video60`) and acquire the resource-manager
resource `VT_CAPTURE` (qty 1).

- Histogram init while video capture holds `VT_CAPTURE` (same process or another
  process, e.g. PicCap): `resource acquire failed`, NULL handle.
- Video capture started while the histogram is open: LG sends a policy action, the
  histogram callback fires, `getHistogram` returns 2 from then on. The histogram lowers
  its own priority (`downSubLevel -99`), so it never pre-empts.
- UI capture (halgal) and histogram do run together (no errors, no policy events).
- In one process the switch is fast: histogram init 13–40 ms, video capture start
  ~100–120 ms. No inter-process stop/start of PicCap is needed.
- LG's own `/usr/sbin/captureservice` also takes `VT_CAPTURE` from time to time (seen
  ~15 s after an app launch, held ~1.6 s). Re-acquiring succeeded 1.9 s after revocation.
  Its trigger is not known (assumption: app thumbnails or recognition features).

### 3. Is 10 Hz a limit of the API or a HyperDRM choice?

A choice. `getHistogram` takes 0.2–0.9 ms. With 20 ms polling every call returned new
data; with 5 ms polling ~117 new states/s appeared. Consecutive reads alternate between
two slightly different states (looks double-buffered), so the output needs smoothing.
CPU: 2–5 % of one core at 50 calls/s, 16 % at 200 calls/s. HyperDRM 0.13.4 itself now
defaults to 50 ms.

### 4. Fastest reliable way to see that the video plane is protected?

- `getActivePipelines`: a pipeline holding `SVP_CPB` appeared 0.7 s after the media
  pipeline was created (Disney+). One call takes ~10 ms, so polling every 100–250 ms
  detects it within that interval. Disney+ menu previews were not SVP.
- Kernel: any open of the VT device during SVP fails with EACCES
  (`vt_v4l2_main_open ... unsupport VT capture in SVP video`); libvtcapture returns 17.
  This also hit LG's own captureservice, so it is state-based, not caller-based.
- `com.webos.service.capturepermission` (subscribed by libvtcapture) reported 1 during
  SVP, so it is not a DRM signal.
- Not measured: whether `getActivePipelines` supports push subscriptions (a subscribe
  call returned one reply in 2 s), HDMI/HDCP sources.

Recommended: poll `getActivePipelines` for `SVP_*`, and treat EACCES/17 on open as
"protected" as well.

### 5. Does the UI plane stay capturable while Disney+ plays?

Yes. halgal capture returned rc 0 at ~22 fps (320×180) during SVP playback. The plane is
transparent during playback; subtitles and the pause overlay covered 0.1–0.4 % of the
frame (grey ~150). The edges stay transparent, so the UI plane adds little edge colour.

### 6. Is the `vt_devmode` marker needed?

Yes, for the histogram only. `vtCapture_initHistogram` reads `/proc/<pid>/cmdline` and
calls `stat("/var/luna/preferences/vt_devmode")` (the only `stat` in the library).
Without the file it accepts only a caller whose cmdline contains
`/com.lighting.huesync.service/hue_sync_service` and otherwise logs
`your process has no permissions` and returns NULL (measured). The check happens only
at init; the probe removed the marker 35 ms later. Video capture does not read it.

## Main consequence

On this firmware the histogram cannot be opened while protected (SVP) video plays: the
kernel refuses every new open of the VT device. HyperDRM's approach (start the histogram
once protected playback is detected) therefore cannot work on this B4. Keeping a
histogram handle open from before playback was deliberately not tested: it would work
around an explicit refusal by the TV.

## Other observations

- `luna-send` prints nothing without a TTY (`ssh -tt` needed).
- PicCap 0.5.3 (hyperion-webos `00c932f`): video backend libvtcapture, UI backend
  libhalgal; `libgm` does not exist on this firmware. CPU not yet measured properly.
- HyperHDR `getconfig` requires authorization even on the LAN; `serverinfo` and the LED
  stream do not.
- The Pi was unreachable (no ping) for several minutes during the session; cause unknown.
- `ledlog.py` reports static content as "frozen"; this needs a better criterion.

## Licences

- hyperion-webos, PicCap: MIT. tv-native-apis: public domain (Unlicense).
- HyperDRM (gitea.adnau.de/matze19999/HyperDRM): no licence file or statement, so no code
  is reused; only facts about LG's API were taken from it and verified on the TV.

## Correction (2026-11-10, later): the histogram DOES survive protected playback

The "Main consequence" above was wrong. It was based only on *fresh* opens during
SVP. A follow-up test (user-approved) kept a histogram handle open across the
clear→protected transition and measured what HyperDRM actually relies on.

Run `probe/data/2026-11-10/hold.{log,txt}` + `pw2.log`, aligned on boot-clock seconds:

- Histogram opened during clear content (home/YouTube), kept open.
- One revocation at 7573.5 s (policy event) during the YouTube→switch app change
  (captureservice thumbnail, `svp=0`); re-acquired 1.96 s later, still clear.
- SVP became active at 7588.4 s (Disney+). The handle, open since 7575.5 s, kept
  returning rc 0 at ~20 updates/s for the whole ~36 s SVP window, with **no**
  revocation (`SUMMARY: rc 2 occurred once; re-inits 1`, both before SVP).
- The data was **real content**, not zeros: per-second peak zone value rose to
  203–218 and average brightness to ~160 once the Disney+ scene was up (7591 s on),
  tracking the picture just like the earlier YouTube segment.

### Revised conclusion

An already-open histogram handle keeps delivering real 16-zone colours during SVP
(Disney+) playback on this B4 + firmware 33.31.75. Only *opening* it fresh during SVP
is blocked (the kernel's check is at `open()`). This is exactly how HyperDRM gets DRM
colours: acquire the histogram before protected content starts and never close it.

This stays within the project boundary: the histogram is the TV's own aggregate
colour statistic, delivered through its documented API on a legitimately-acquired
handle. The secure video path is untouched — no frame capture, no decryption; we only
read 16 average colours the TV chooses to keep publishing.

### Remaining constraint

`VT_CAPTURE` is single-owner, so video capture (PicCap-style, per-pixel) and the
histogram cannot be held at the same time. And the histogram cannot be re-opened while
SVP is active. Therefore, to keep DRM working, the histogram handle must stay open
continuously — it cannot be closed to hand `VT_CAPTURE` to a video capturer and then
reopened once protected content is already playing.

Implications for the architecture:
- The histogram (16 zones) is the one capture that works for every source and must
  stay open for the life of the service.
- The graphics/UI plane (halgal) can be captured at the same time as the histogram
  (proven earlier) and does not touch `VT_CAPTURE`, so UI/subtitles/menus can be
  blended on top in all cases.
- Full per-pixel video capture is only possible by giving up the always-open
  histogram, which would break DRM colours. So either accept 16 coarse zones
  everywhere (robust), or only use video capture when we are confident no protected
  content will start before we can re-open the histogram (fragile). Recommendation:
  histogram + UI always on.

Also observed: captureservice grabs `VT_CAPTURE` on app transitions (the 7573 event),
revoking the histogram briefly. During steady SVP playback no grab occurred. A
grab *during* SVP would block re-open until SVP ends; holding the last colour covers
that gap.

## Phase 2 progress (2026-11-10)

### Step 1 — capture core (done)
`src/ambilight.c`: keeps the histogram open, captures the UI plane, composes (zone image
via `src/profile_b4.h` + UI alpha over it), sends a small PNG to HyperHDR over the JSON
API (one origin "ambilight", priority 150). Built with `sh build.sh` (zig, no NDK).
Validated on the LED output: opens on clear content, tracks YouTube (colours change
per frame, directional around the perimeter), recovered from an app-switch grab in ~2 s.

### Step 2 — transition robustness (done, with a caveat)
- `luna-send` prints nothing without a TTY, so SVP cannot be polled cheaply from the
  native service (would need libluna-service2 + a glib loop). Used an adaptive re-acquire
  instead: retry every loop for the first 4 s after a loss (catch the clear gap before
  SVP), then probe every 3 s; gap metrics logged.
- Clean run YouTube → Disney+ (SVP) → home: **zero losses** (`holds=0 reopens=0
  maxgap=0`), ~20 fps throughout, i.e. the open handle survived the whole DRM transition
  and tracked protected content. This is the common case.
- A manual `executeOneShot` (captureservice) could **not** preempt the open histogram
  (the screenshot failed instead), so the intermittent app-switch grab seen earlier is a
  different, non-reproducible path. The fast-re-acquire is the safety net for it;
  recovery on clear content is proven (phase-1), but the worst case (a grab whose clear
  window is fully consumed before SVP) could not be reproduced this session to measure.
- CPU: **21 % of one core** (~5 % of 4 cores), RSS 3.7 MB, 2 threads. UI-plane capture is
  the main cost; optimisation headroom for later.

### Known coexistence issue
PicCap can auto-restart and re-grab VT_CAPTURE, which starves our histogram (observed:
one test run stayed `lost-slow` the whole time because PicCap had come back). Step 6
(autostart) must persistently disable PicCap's autostart while our service runs, keeping
PicCap installed as a fallback.

### Still to do
Calibration refinement (step 3), settings app (step 5), autostart + failsafe + one-command
IPK + README with full rollback (step 6, incl. disabling PicCap autostart), CPU
optimisation. Nothing is committed yet; the probe package and this service are removed/
stopped on the TV, PicCap restored.

### Step 3 — zone calibration (done)

New capture (`probe/data/2026-11-10/cal.txt.gz`, 2.5 min on the TV) with the UI plane
recorded as ground truth at 32x18 (`--calib-dump`) while `patterns/calibrate.html` showed
references, a white cell walking a 16x9 grid, and 16 validation images.

Measured:
- Histogram channels are R,G,B (verified with pure red/green/blue fields; was assumed).
  UI plane bytes are B,G,R,A as assumed.
- The histogram is a **linear** mean: grey 64/128 give gamma 1.01, and every zone's
  footprint fractions sum to ~1.0 (zone 1: 1.21). Black ≈ 0.8.
- The picture mode's white balance is in the histogram: white = (253, 237, 205), i.e.
  blue −20 %. The service now removes it (white → 255,255,255); HyperHDR's own LED
  calibration sets the LED white. One-line change if the screen's tint should be kept.

Reconstruction: instead of assigning each cell the strongest zone, solve
`min ||A c − h||² + λ cᵀLc` (A = measured footprints, L = grid Laplacian), i.e. a
fixed 144×16 operator `PROFILE_R` (src/profile_b4.h), applied in the raw domain and
then level-mapped, upsampled 2× bilinearly to a 32x18 image. Mean absolute error at the
210 HyperHDR LED rectangles on the 15 validation images (0..255):

| method | MAE |
| --- | --- |
| whole-screen average | 38.2 |
| hard assignment (previous) | 22.4 |
| weighted average of zones | 22.0–24.4 |
| **least squares, λ = 0.003, 2× upsample** | **17.7** |

λ was chosen as the lowest-noise operator within 1 % of the best MAE. The operator has
negative weights (worst-case row gain 8.8), but replaying 1562 real YouTube histogram
reads showed **the same LED jitter as before** (0.89 vs 0.93 units read-to-read);
averaging two consecutive reads (now in the service) halves it again (0.38).

Live validation on the TV, HyperHDR LED stream recorded from the Mac
(`tools/ledrec.py`, `tools/validate_live.py`):
- With UI blending on (normal mode), the opaque validation images come straight from
  the UI plane: MAE 3.1 — confirms orientation, edges and HyperHDR mapping, and means
  full-screen menus/launchers get exact per-pixel colours.
- With `--no-ui-blend` (histogram path only, as for video/DRM): references exact
  (white 252, grey 128 → 127.6, primaries pure) and **MAE 17.8** on the 15 images —
  matching the offline prediction (17.7).
- Letterbox (21:9) image: MAE 33.6 — zones span bar and picture, so colour leaks into
  the top/bottom LEDs. Needs black-border handling (open item).
- CPU without the dump: **24 % of one core** (was 21 %), RSS 3.7 MB.
- The C reconstruction equals the Python model to 0.0005/255.

Recalibrating (e.g. after a firmware update): install the IPK, copy
`patterns/calibrate.html` over the app's `index.html`, stop PicCap, run
`ambilight --calib-dump cal.txt` for ~140 s while launching the app, then
`python3 tools/calibrate.py cal.txt leds.json src/profile_b4.h --preview out.png` and
rebuild. `leds.json` is `serverinfo.info.leds` from HyperHDR.

### Black bars (21:9) — done

Capture `probe/data/2026-11-10/lb.txt.gz` (`patterns/letterbox.html`, `--calib-presets 0,1,2,3`,
UI ground truth at 64x36, per-zone dark fractions). Analysis: `tools/letterbox.py`.

What the histogram presets measure (walking-cell footprints):
- preset 1: letterbox crop — top and bottom cell rows excluded (~11 % each);
- preset 2: pillarbox crop (4:3) — left and right ~12 % excluded;
- preset 3: a different 14-zone layout over nearly the whole frame.

Detector (preset 0, continuous): a zone's share of pixels at or below 16 in every
channel must cover the share the bars would take of that zone (from the footprints);
the largest symmetric bar height (1–7 rows of 36) that every zone supports wins. On the
11 test images it was exact at every threshold (4/8/16/24) — bars of 5, 4, 3, 2 rows, a
dark letterboxed scene, and no false alarm on a 16:9 image with a dark top band. In the
service a level is adopted after 2 s; nothing changes while the picture centre is black
(fades), and an opaque UI covering > 50 % (menus) means "no bars".

Strategies (MAE vs the bar-cropped, stretched picture at the LED rectangles, mean over
the 8 letterboxed images):

| strategy | MAE |
| --- | --- |
| no handling (previous) | 53.8 |
| crop and stretch only | 38.8 |
| **preset 0 + bar model** (bars as known black, per-level LS operator) | **22.3** |
| preset 1 (TV letterbox crop) | 25.9 |

The bar model wins and needs no preset switching or probe frames: `src/letterbox_b4.h`
holds one operator per bar level plus each zone's bar share; the service subtracts the
bars' black contribution, reconstructs, composites the UI in screen coordinates, then
crops the bars and stretches the picture (so subtitles inside the bars don't reach the
LEDs). The C output equals the Python model exactly (0/255 on all 11 images).

Live on the TV:
- test images, LED stream recorded: switches 0→5→4→3→2→5(dark scene)→0, each ~2 s after
  the change; letterboxed MAE **22.1** (offline 22.3); 16:9 images unchanged.
- a real 2.39:1 film on Disney+ (protected/SVP): histogram stayed open from the menu into
  the film (`holds=0`), bars detected as 5 rows; top/bottom LEDs now track the picture
  (mean 22 / 27 vs 24 / 17 left/right on a dark scene) instead of the bars' black.
- found and fixed: the service now ignores SIGHUP (it died when the SSH session that
  started it closed). The dark Disney+ menu briefly read as 1-row bars; fixed by the
  opaque-UI rule.

Recalibrating letterbox handling: show `patterns/letterbox.html`, run
`ambilight --calib-presets 0,1,2,3 --calib-dump lb.txt` for ~245 s, then
`python3 tools/letterbox.py lb.txt cal.txt leds.json src/profile_b4.h --emit src/letterbox_b4.h`.
Live check: `patterns/letterbox-validate.html` + `tools/ledrec.py` +
`tools/validate_live.py <dump> <ledrec> leds.json --stretch` (run the service with
`--no-ui-blend`, otherwise the opaque test images come from the UI plane).

### Autostart and emergency brakes — done

Homebrew Channel's boot (`/var/lib/webosbrew/startup.sh`, read on the TV): `run-parts`
runs the init.d hooks one after another, inside a failsafe window (flag set before the
hooks, cleared ~10 s after); a reboot in that window skips all hooks next boot. This
busybox `run-parts` also runs names with dots and only reports a dangling symlink.

Design (`service/`): `70-ambilight` → `autostart.sh` returns at once and starts
`supervise.sh` detached; the supervisor waits 30 s (past the failsafe window), runs the
service with a rotating log, restarts it with back-off. Brakes: package missing; a
`disable` file (`ambilightctl stop`); a boot-loop guard (3 boots without a stable 60 s
run → disable); a crash-loop guard (5 exits within 60 s → disable). PicCap's autostart is
switched off through PicCap's own `setSettings {"autostart": false}` (which also removes
its hook); the previous state is recorded and `tools/uninstall.sh` restores it.

Installed with `tools/install.sh` and verified on the TV:
- PicCap autostart off (config `false`, its hook gone, previous state `true` saved);
  ambilight is the only active HyperHDR source (priority 150).
- Installing while protected content was playing: the service waits (probe every 3 s)
  and opened the histogram as soon as the home screen showed.
- **Standby** (remote power off, ~40 s, on): the service keeps running (same pid); LEDs
  fade to 0 within ~1 s and stay off while the TV is off; on wake the histogram gap was
  45 ms and the LEDs were back within ~1 s of pressing power.
- **Reboot** (`machineReboot`): SSH back after 37 s, the hook started the service at
  uptime 85 s (30 s supervisor delay included), histogram open at once, boot-loop counter
  reset to 0 after 60 s, HyperHDR source active again.

### Settings app — done

App ↔ service: Homebrew Channel's API is in the Luna "public" group and its `exec`
method runs a shell command as root without checking the caller (read in its
`service.js`), so the web app (client permission "public") can call it. The app only
runs `ambilightctl` with fixed sub-commands; values are validated in the app and again
by `ambilightctl` (tested on the TV: `host=x;reboot`, `host=a$(id)b`, out-of-range port,
priority and gain, unknown keys - all rejected, settings file untouched). This also
lets the app start the service when it is not running, which a Luna service could not.

The service reads its settings from `/media/developer/ambilight/args`, reloads them on
SIGUSR1 (new host/port → reconnect; new priority → the old one is cleared on HyperHDR
first), and writes `status.json` every 2 s. Tested with the remote on the TV: brightness
▶▶ reloaded twice without a restart, Ambilight off/on stopped and restarted the service
with the new settings, Back closed the app. The UI (`ui/`) can be previewed on a desktop
browser with an in-memory stand-in for `ambilightctl`.
