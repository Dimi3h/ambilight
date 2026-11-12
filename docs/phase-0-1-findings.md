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

## Phase 3 (2026-11-10): CPU, remote key, Homebrew Channel distribution

### CPU: 24% → 5% of one core

Measured with the new `--profile` flag (thread CPU time per loop stage, 20 Hz, UI plane
opaque) and cross-checked with `/proc/<pid>/stat`:

| stage | before | soft-float → softfp | + copy-then-scan |
| --- | --- | --- | --- |
| read the UI plane (320×180) | 18.0% | 19.4% | **2.2%** |
| reconstruction + compose | 2.4% | **0.19%** | 0.17% |
| histogram ioctl | 1.4% | 1.4% | 1.4% |
| UI capture blit, PNG/JSON send, rest | ~1.5% | ~1.3% | ~1.3% |
| whole loop | 23.3% | 22.4% | **5.1%** |

- The UI surface is mapped from `/dev/gfx` uncached: reading it byte by byte cost ~150
  cycles per pixel. Copying it out once with 64-bit loads (4× `ldrd` per 32 bytes) and
  scanning the copy gives bit-identical results (native unit test `tests/test_ui_read.c`).
- zig's `arm-linux-gnueabi` target emulated every floating-point operation in software
  (`__aeabi_dmul` etc., no VFP instruction in the binary). `-mfloat-abi=softfp` keeps the
  soft-float calling convention of the TV's libc/libm (ELF flags and loader unchanged,
  doubles to `pow` still in r0–r3) but uses the FPU. NEON intrinsics crash zig 0.17's
  backend for this target ("Do not know how to split this operator's operand"), so the copy
  uses scalar 64-bit loads.
- Paused (LEDs off, histogram kept open, 2 reads/s): 0.2%.

### Remote key

- Key codes (passive evdev reader `probe/keywatch.c` + `lginput2` log lines): `⋯` = 994
  (`KEY_SCREEN_REMOTE`; short press opens "more actions", long press toggles audio
  description through `KEY_AUDIO_DESCRIPTION` → `com.webos.app.tvhotkey`), LG Channels =
  1043 (`KEY_LGCHANNELS`), blue = 401 (`KEY_BLUE`). LG's driver advertises every key code
  on every input device (`EVIOCGBIT`), so the service selects devices by name (`LGE *`).
- **LG Channels cannot be taken over cleanly.** surface-manager's key filter
  (`/usr/lib/qt5/qml/KeyFilters/appLaunch.js`, `handleCPHotkeys`) launches the app from an
  in-memory table. That table is seeded from the system setting `other/mapping_info`
  (read once at start-up) and then **overwritten from LG's server** at every boot
  (`com.webos.service.sdx` `getConfig {"requester":"hot_key"}` → `cb_getHotkeyInfo`, which
  also writes the setting back). Tested: an entry without `app_id` (which would make the
  key pass through) was in place at reboot, LG Channels still launched, and the setting
  read back LG's value afterwards. The original setting was restored and verified identical
  (backup in `backups/2026-11-10/`, not in git).
- **Blue key**: the TV logs it and does nothing on the home screen (no key-filter action,
  no app launch). The service reads it passively and toggles the pause; measured latency
  from `lginput2` to the service: 5 ms. A toast confirms (`luna-send` without a terminal
  still performs the call; it only prints nothing).

### Homebrew Channel (0.7.3 on this TV, read in its `index.html` / `service.js`)

- Extra repositories are plain URLs to a feed `{"packages":[{id, title, iconUri,
  manifest:{version, ipkUrl, ipkHash.sha256, rootRequired, …}}]}` (same format as
  lg-luma's release asset `repo.json`). `rootRequired` is only displayed.
- It has **no version comparison and no automatic updates**: an installed app's button just
  reads "Update". Its `install` method downloads the IPK, checks the SHA-256 and installs
  with the developer installer; it elevates only itself (self-update), **not** other apps.
- So after an install from Homebrew Channel nothing of ours runs until the app is opened:
  the app then runs `ambilightctl setup` (elevate, boot hook, start) through `exec`.
- Update in place, tested twice on the TV through Homebrew Channel's `install` while the
  service ran (0.2.0 → 0.2.1 test build → 0.2.0): the installer gives the binary a new
  inode; the service notices within 5 s and exits 75; the supervisor elevates the new
  version and `exec`s the new `supervise.sh`; LEDs paused 3.8 s. The package files have a
  fixed mtime (deterministic IPK), so the inode is what identifies a new version.
- Not exercised on the TV: removal through Homebrew Channel's Uninstall (the code path
  removes the boot hook and stops) and the GitHub feed itself (no release published yet).

## Review (2026-11-11) and version 0.2.1

A full read-through of the code, scripts, app, build and release, with each suspicion
checked by a test or a read-only measurement on the TV.

### Open: the capture resource is taken every ~122 s

- The service log of one night shows 33 histogram revocations, ~1.4 s each (1.3–1.8 s),
  exactly 122 s apart. The first came 9 s after the app `space.nuvio.webos` (a third-party
  streaming app) was started for the first time; they continued in that app, on the home
  screen and after a standby/wake, and stopped when the power state changed to "Screen
  Saver". In the 38 active minutes before that app start there were none. Extra short
  losses (10–15 ms) appeared at app switches and at standby.
- The caller is unknown. No homebrew package on the TV uses the capture service (only
  PicCap's files mention it, and it was not running). LG components with a capture
  permission in the Luna configuration: `com.webos.service.gibbs` (broadcast),
  `legacybroadcast-*`, `com.webos.service.objectdetection`; a `com.webos.service.acr`
  service exists. Live Plus and the AI picture options were off.
- Consequences: (1) with clear content the LEDs hold their colours for ~1.4 s every two
  minutes; (2) during protected playback the handle cannot be re-opened, so one such grab
  would freeze the colours until playback ends. The longest protected run measured so far
  (Disney+, 2026-11-10) lasted ~100 s, i.e. less than one period: **colours during a whole
  protected film are unproven.**
- To do: watch the Luna bus (`ls-monitor`) for calls to `com.webos.service.capture` while it
  happens; a 10-minute protected run with the log; and measure whether the histogram's
  policy callback can refuse the revocation by returning false (never measured; it returns
  true today).

### Fixed in 0.2.1

- **Brightness did not apply to the UI layer** (unit test: an opaque white UI stayed at 255
  with gain 0.15; now 38). The gain is applied to the composed image.
- **HyperHDR unreachable**: connect waited up to 2 s on every frame and the remote key was
  only read when the loop had time left. Now a 0.3 s connect attempt every 3 s and input is
  always read. Measured with an unused address: the loop keeps its rate (4.7 % CPU as in
  normal operation), one log line for the outage, a pause is applied within the second.
- **Flash writes**: `status.json` every 2 s cost 127 kB/min of dirtied pages (measured in
  `/proc/<pid>/io`, ≈180 MB/day; the eMMC's wear indicator reads 0x01, the lowest level).
  It is now written only while `json-status` asks (a `.want` marker, 15 s): 4 kB per 30 s
  without the app, and `json-status` still returns a fresh status in 0.26 s.
- `ambilightctl stop`'s emergency path matched itself (`pkill -f` on a path prefix); tested
  with a frozen (SIGSTOP) service: now ends it and returns 0. `setup` keeps a pause
  (verified through an upgrade and a stop/setup cycle). The boot counter resets after 20 s
  (verified: 2 → 0 between 12 s and 24 s after start).
- Homebrew Channel's `exec` reports a non-zero exit as `returnValue:false` with
  "Command failed: …" but still returns stdout; the app now shows ambilightctl's own
  message. The app reloads after an update (not yet seen on the TV).
- The copy loop's type punning is now declared (`may_alias`); the generated code was already
  identical with and without strict aliasing. The binary is stripped (951 kB → 111 kB,
  `printLog` still exported). Actions are pinned to commit hashes.

### Not verified

- Brightness end to end on the TV (the screen was black in the screensaver during the
  check; HyperHDR's LED stream read 0 either way).
- The app's new error text and reload, and the timing of the remote key under load (the
  loop rate was measured, the key itself not pressed).

### Cause of the 122 s losses (2026-11-11, later)

Watched the Luna bus (`ls-monitor -f com.webos.service.capture`, 150 s) while it happened:

- `com.webos.service.oledepl` (OLED panel care) calls `executeOneShot` with method `GRAPHIC`
  every 3 s. That captures the UI plane only and does **not** disturb the histogram.
- `com.webos.surfacemanager` calls `executeOneShot` to `/tmp/snapshots/<id>/<app>_tmp_sc…`
  with method `BLENDED` (video + UI). The call at 9388.26 s returned `ERROR_INTERNAL` at
  9390.10 s; the histogram was lost exactly in that window (recovered at 9390.105 s after
  1721 ms). This is the caller.
- Its code (`WebOSCompositor/controllers/StarfishFullscreenController.qml`,
  `fullscreen/StarfishFullscreenCapture.qml`): a screenshot of the foreground app for the
  recents list, `fullscreenFocusCaptureTimeout` (2 s) after the app is ready
  (`fullscreenReadyTimeout` 5 s) and then every `fullscreenCaptureTimeout` (120 s). It is
  skipped when the config `com.webos.surfacemanager.disableCapture` is true (false here; set
  in LG's `hd1080-lite/mini` and `hd2160-lite/mini` configd layers), when
  `compositor.needScreenCapture(item)` says no (C++, not read), or when the app declares
  `imageForRecents` in its appinfo.
- `imageForRecents` on this TV: Netflix, Amazon (Prime Video) and YouTube declare it;
  Disney+, Apple TV, VRT MAX, VTM GO, Play, the browser, Live TV, HDMI, the home screen,
  Homebrew Channel, Nuvio and our own settings app do not. That fits the observations: no
  periodic losses in yesterday's YouTube runs, losses with Nuvio and on the home screen.
- Still unmeasured: whether the screenshot is attempted (and revokes the histogram) while
  Disney+ plays protected content; and whether `setConfigs` can switch `disableCapture` on,
  whether that survives a reboot, and whether it also removes the capture at app switches
  (the controller checks the same flag there).

### LG's `disableCapture` switch, measured (2026-11-11)

- Baseline in the same session (TV active, one third-party player in the foreground the
  whole time): losses every ~121 s, the last two at 4516.8 s and 4638.8 s.
- `luna://com.webos.service.config/setConfigs {"configs":{"com.webos.surfacemanager.disableCapture":true}}`
  at 4659 s returned `returnValue:true`; `getConfigs` read `true`.
- Next 360 s (4671–5031 s) on the Luna bus: 119 `GRAPHIC` calls from
  `com.webos.service.oledepl`, **no** call from `com.webos.surfacemanager`. The service
  logged **no** loss in that window (expected without the switch: ~4760, ~4882, ~5004 s);
  `reopens` stayed at 42, `hist=open`.
- Restored to `false` afterwards (read back). configd keeps such overrides in
  `/var/preferences/configd_factory_db.json` as a "usrDefined" entry, so the value is
  expected to survive a reboot (not tested); after the test that file holds the key with its
  default value `false`. configd also drops a `before_setConfigs` snapshot of its
  configuration in `/tmp` on every set.
- Not measured: a reboot with the switch on; whether the capture at app switches disappears
  too; a long protected run in Disney+ with the switch on.

### `disableCapture` switched on for good on this TV (2026-11-11)

At the owner's request the config was set to `true` at uptime 7030 s (read back `true`; the
override is in `/var/preferences/configd_factory_db.json`). It is a setting of this TV, not
something the package does: installing ambilight elsewhere does not change it, and
`tools/uninstall.sh` does not put it back. To undo it:

```sh
luna-send -n 1 luna://com.webos.service.config/setConfigs '{"configs":{"com.webos.surfacemanager.disableCapture":false}}'
```

Still to observe in normal use (the service log shows it, also while paused): that the
value survives a reboot, whether the short losses at app switches are gone, and a long
protected Disney+ run without a loss (`reopens` not increasing, never `hist=lost-slow`).

### Green and yellow keys for brightness (0.2.2)

- Codes from the TV's own keymap (`/etc/webos-profile/common/profile_keymap.json`): red 398,
  green 399, yellow 400, blue 401 (blue also measured with evdev).
- What LG's key filters do with them on this TV (`KeyFilters/appLaunch.js`, both handlers
  are in the active list): `handleFreeSyncInfo` counts green releases and, at seven within
  5 s while an HDMI input is in the foreground, launches `com.webos.app.tvhotkey` with
  `freesync-info`; `handleHotkeys` starts a 5 s long-press timer with a spinner on yellow
  and then calls `launchAGP` (LG's pattern launcher). Short presses pass through.
- Hence: only key-down events count, no hold-to-repeat; one press is 10 percentage points
  (10–300 %). The message and the write to the settings file follow 0.6 s and 1.2 s after
  the last press, so a burst gives one toast and one write.

### First light on the real strip (2026-11-11)

Strip still on its reel (SK6812 RGBW, 60/m, 300 LEDs; HyperHDR configured for 210), fed at
the start only, ambilight paused, test patterns from `tools/ledtest.py` at level 64 (HyperHDR
gamma 1.5 on top):

- HyperHDR switches its LED device component off while no source is active and on again as
  soon as one is (seen in `serverinfo`: `LEDDEVICE` false → true when the test colour started).
- Dim red: the owner saw dim red on most of the strip, the last part dark. So data, power
  and the red channel are right, and the LEDs beyond the configured 210 stay off.
- Edge pattern: red, green, blue, yellow in that order from the plug, short-long-short-long
  with dark LEDs in between, as predicted from HyperHDR's layout. So the configured byte
  order (GRB) is right for all three colours and the layout runs left, top, right, bottom
  from the data input. No flicker reported.
- Not tested yet: the white channel, brightness above this dim level, the voltage drop along
  the unrolled strip, and real picture content.

### Strip mounted on the TV (2026-11-11)

Stuck about 1 cm from the edge on the thin panel (sides, top) and along the bottom of the
thick part, corners folded on pad rows with tape underneath, no soldering. Start at the
bottom-left as seen from the front (bottom-right from behind), running up. Counted by the
owner, every LED in chain order, fold LEDs counted with the piece that ends in the fold:

| piece | LEDs | chain index |
| --- | --- | --- |
| left side, up (ends in fold 1) | 39 | 1–39 |
| top, left to right (ends in fold 2) | 73 | 40–112 |
| right side, down (ends in fold 3) | 41 | 113–153 |
| bottom, right to left | 75 | 154–228 |

In the bottom piece: 1 LED, 2 on the step from the thin panel onto the thick part
(155–156), 69 on the thick part, 2 on the step back (226–227), 1 next to the start (228).
HyperHDR classic layout for this: top 73, bottom 75, left 39, right 41, input position
189 (= top + right + bottom), direction not reversed. Still to do: confirm with test
patterns, find the LED hidden in each fold (among 38–39, 111–112, 152–153) and disable
those and the step LEDs in HyperHDR.

### Mounted strip verified (2026-11-11)

HyperHDR set to the layout above (228 LEDs; `serverinfo` shows the runs left 39, top 73,
right 41, bottom 75 from the data input).

- Dim red on everything: every LED lit up to the last one next to the start, steady while
  the three folds and the creased spot were moved by hand.
- Edge pattern, close-ups of all four corners and a photo from the front: red left, green
  top, blue right, yellow bottom (front view); the colours change exactly where predicted
  (first red LED is 4, first green 43, first blue 116, first yellow 157, last yellow 225).
  At each fold five of the six dark LEDs are visible. On the wall, 20 cm behind the TV, the
  glow is wide and even, no single LEDs visible.
- The LED hidden in each fold is the last one of its piece: **39, 112, 153**. Confirmed with
  HyperHDR's identify command (`leddevice` / `identify`, `ledDeviceType: "blink"`,
  `params.blinkIndex`, counted from 0), which addresses one LED directly and needs no
  authorisation here: all three light only from under their fold. The image-based
  single-LED pattern cannot show this cleanly, because the corner LEDs' areas overlap.
- HyperHDR's editor counts from 0: the LEDs to disable are 38, 111, 152 (hidden) and
  154, 155, 225, 226 (on the steps onto and off the thick part) in its numbering.
- Yellow (equal red and green) looks greenish on camera only; by eye it is right (see below).

### Disabled LEDs, white channel, load and first real content (2026-11-11)

- **Disabling LEDs in HyperHDR's editor.** Five of the seven lie under another LED's area in
  the preview (corner LEDs overlap; later LEDs are drawn on top, and LEDs 0-2 on top of
  everything). The editor's own way out, from `light_source.js` on 22.0.0: hold Ctrl while
  right-clicking and it takes the LED under the top one. Each toggle is written to the
  configuration at once. `serverinfo` afterwards: exactly 38, 111, 152, 154, 155, 225, 226
  (counted from 0) carry `disabled`, the geometry is unchanged.
- **A solid colour ignores the disabled flag, an image respects it.** HyperHDR's LED stream
  for the `color` command shows all 228 LEDs lit; for an `image` of the same colour exactly
  the seven are black. Seen on the strip as well: the four LEDs on the steps stayed lit with
  `color` and went dark with the image. Ambilight sends images, so it is not affected;
  `ledtest.py color` now sends an image too.
- **White channel.** Neutral white (equal R, G, B) lights only the white die of each LED;
  on the wall it is neutral and the same at both ends of the chain.
- **Load, fed at the start only.** White at level 192 and 255, then cyan, yellow and magenta
  (two colour channels each, the heaviest case when white goes to the white die) at 192 and
  255, 20 s per colour, two to three minutes per step. At every step the end of the chain
  looked the same as the beginning, and the start connector, its thin wires and the supply
  did not get noticeably warm. Current was not measured. Conclusion: with HyperHDR's
  present settings (`scaleOutput` 1, `powerLimit` 1) a second feed at the end is not needed;
  repeat the test if the output is ever scaled up. Full white is as bright as the user wants
  it at most, and the three colours look right by eye.
- **Thick part.** The glow under the TV is as even as on the other sides: no separate trim
  for the bottom LEDs.
- **Real content.** Ambilight resumed: 19.9 fps, colours on the matching sides, follows
  video smoothly. The colour keys work with the LEDs: the log shows the brightness going
  from 100 % to 20 % and back in 10-point steps, then a pause and resume with blue. This
  also covers the 0.2.1 brightness fix end to end. 10 928 frames with `holds=0 reopens=0`;
  `disableCapture` still reads true.
- Still open: a run of ten minutes or more on Disney+, `disableCapture` after a full reboot,
  removal through Homebrew Channel and the first-start setup.

## Delay, and the histogram's three measurement states (2026-11-12)

The user asked for less delay between picture and light.

### Where the delay came from

- The service sampled every 50 ms and averaged each read with the previous one: on
  average 25 ms (age of that mean) + 25 ms (a frame is held until the next one) = about
  50 ms before HyperHDR.
- HyperHDR then smooths. Its settings cannot be read without the admin login; the
  component can be switched at run time through the JSON API (`componentstate`,
  `SMOOTHING`, no login needed, not persistent).
- By eye: smoothing off made the delay "probably shorter" but the light restless; 40
  reads a second on top made the delay "no longer disturbing" but the flicker "disturbing".

### Three measurement states

`getHistogram` does not return one stable measurement of a still picture but cycles
through **three**, e.g. zone 1 blue = 53.72 / 54.87 / 56.30 on a paused YouTube frame
(all channels of a zone move together; at level 15 the spread is about 10 %). Measured
with the new `--hist-probe` mode (microsecond timestamps, a hash of every block returned):

- Reads back to back (0.7 ms apart) alternate between two blocks; about every 8.4 ms
  (one frame of the 120 Hz panel) one of the two is replaced. A read never returns the
  block the previous read returned.
- Reads exactly one frame (8.33 ms) apart returned the three states in strict rotation
  (149 of 149). At 4.17 ms the pattern is `C A B A`, at 6 and 10 ms irregular.
- Bursts started at random moments, error of the burst mean against the mean of the
  three states (levels of 255, 80 bursts each):

| reads per burst | all three states | mean error | max |
| --- | --- | --- | --- |
| 1 | – | 0.84 | 1.34 |
| 2, one frame apart | – | 0.44 | 0.67 |
| **3, 8.33 ms apart** | **91 %** | **0.05** | 0.86 |
| 3, 8.70 ms apart | 94 % | 0.05 | 0.86 |
| 2 back to back + 1 a frame later | 90 % | 0.06 | 0.86 |
| 3, 10 ms apart | 82 % | 0.11 | 0.86 |
| 4, 4.17 ms apart | 99 % (one twice) | 0.27 | 0.62 |

Why there are three states is not known (assumption: three sampling patterns over the
picture). The rule that picks the block for a read is not a simple function of time; the
9 % of bursts that miss a state are probably reads that coincide with the TV's update.
The old average of two reads 50 ms apart could not cancel this, and sampling every 25 ms
(three frames) made it worse: slow beating between the states.

### New sampling (default)

One output frame is the mean of **three reads one panel frame (8.333 ms) apart**, every
25 ms (`--interval 25 --phases 3 --frame-us 8333`, all in the args file for experiments;
`--phases 1` is the old behaviour). The mean is about 8 ms old and held for 25 ms: about
21 ms on average instead of 50. The UI plane is read every other frame (at most every
40 ms). Measured on the TV: 39 frames/s, CPU by `--profile`:

| setting | histogram read | UI plane | whole loop |
| --- | --- | --- | --- |
| 50 ms, 1 read (old) | 1.5 % | 3.3 % | 6.2 % |
| 50 ms, 3 reads | 4.4 % | 3.2 % | 9.3 % |
| 33 ms, 3 reads | 6.7 % | 4.7 % | 13.6 % |
| 25 ms, 3 reads | 8.7 % | 6.4 % | 17.9 % |
| **25 ms, 3 reads, UI every other frame** | | | **14.7 %** (top) |

By eye, with HyperHDR's smoothing on: delay better, light calm, also on a dark picture.
With HyperHDR's smoothing off the light flickers on nearly black pictures (the LEDs
switch between off and their lowest step), so that smoothing stays on.

### Found along the way

- **Do not restart the service while protected video plays.** A restart during Disney+
  left the histogram closed (`hist=lost-slow`) until the video was stopped: a new handle
  is refused while protected content is on screen. Settings can be changed without a
  restart (args file + SIGUSR1); a new binary is only swapped in while YouTube or the home
  screen is in front.
- The black-bar detector switches between 4 and 5 rows on 2.39:1 films (the bars are 4.6
  rows of 36): 101 changes in one evening. Needs hysteresis or fractional bars.
- A small bright logo on a black picture lit the bottom LEDs in the logo's colours (photo
  from the user). Solved the same night, see the next section; the first guess at the cause
  (the linear operator alone) was wrong.
- Panel modes other than 120 Hz (50 Hz broadcasts may run the panel at 100 Hz) are not
  measured; `--frame-us 10000` is the untested guess for that case.

## Black must be black (2026-11-12)

The user: on an OLED, LEDs that glow on a black picture spoil it. Three separate causes.

### 1. HyperHDR's minimum backlight

A fully black image sent to HyperHDR still gave a faint glow. `serverinfo` showed
`backlightThreshold` 0.0039 (1/255, "Minimum backlight level" under Image Processing →
Color Calibration; it applies to images, not to solid colours or effects). The user set it
to 0: LEDs off on black, confirmed by eye and in `serverinfo`.

### 2. A garbage row at the bottom of the TV's sampling grid

Paused YouTube frame: a rainbow logo and two lines of text in the middle, everything else
black. We sent 7 of 255 on average (max 14) to the bottom LEDs, nothing to the others.

- The zone means themselves were the cause: top and side edge zones read exactly 0, but
  bottom-left, bottom centre and bottom-right read 3.2 / 5.9 / 3.2, neutral grey.
- `--hist-dump` (raw bins): a zone's bins add up to a small fixed number of samples that
  factors as columns x rows of one grid: 7371 = 81x91 for the halves and the centre band,
  1457 = 47x31 bottom-left, 1843 = 97x19 bottom centre, 1519 = 49x31 bottom-right,
  2405 = 65x37 centre box. So the TV samples the picture on a **162x91 grid** (which also
  explains the three measurement states: three sampling positions).
- In that frame bottom-left had exactly 47 non-black samples, bottom centre 97 and
  bottom-right 50: **one grid row each**, with scattered grey levels (5-243), the rest of
  the zone at 0. The same count in all three measurement states.
- The user sees nothing on the screen's lower edge, paused or playing. So the grid's last
  row holds values that are not displayed. What they are is not known (assumption: decoder
  padding below the visible picture). Only measured on YouTube.

Fix: for the three zones that only cover the bottom (1, 5, 9) the brightest samples, one
row's worth (97, 47, 49), are left out before the mean and the dark shares are computed.
The sample totals are checked first, so another grid switches this off. On a black bottom
edge the zones now read 0; on a bright one the mean hardly moves. The halves and the centre
band also contain that row (81 of 7371 samples, under 1 level) and are left alone.

### 3. The edge estimate itself

The fixed operator has negative weights and only clamps at the end, so light can appear in
cells whose zone reads black. Replaced, for 16:9 pictures, by a solve per frame:
minimise |A x - h|^2 + lambda x'Lx with x >= 0 (A = measured footprints with entries under
15 % of a zone's peak dropped, lambda = 0.001), by coordinate descent, 30 sweeps, starting
from the old operator's answer. Cells of a zone that reads black (under 0.25 raw) are set
to black and taken out of the smoothness term.

| | old operator | new solve |
| --- | --- | --- |
| validation pictures, MAE at the LED rectangles (of 255) | 17.72 | 17.21 |
| logo frame, garbage row left in: bottom LEDs mean / max | 6.8 / 14.2 | 6.5 / 16.6 |
| logo frame, garbage row dropped: bottom LEDs max | 2.1 | 0.2 |
| one solve per frame against the settled optimum, recorded video | – | mean 0.4, worst 3 |

Row two is why the first diagnosis was wrong: with the garbage row in the data no estimate
can keep the bottom dark. The Python model in `tools/calibrate.py` and the C code agree to
0.04 of 255 (`tests/test_recon.c`). Over-relaxation made convergence worse.

On the TV with both fixes: the LED stream for that frame is 0 on all 228 LEDs; the user
sees the wall dark around the logo and normal pictures unchanged. CPU 14.7 % of one core
(the solve costs 0.7-2 %).

Limits:
- Letterboxed pictures still use the fixed operators (`letterbox_b4.h`).
- A 4:3 picture: the left bar has a zone of its own (columns 0-1) and stays black; the right
  edge zone is three columns wide, so the right bar gets about 40 % of the picture's level
  in the test. Detecting side bars from the dark shares would solve it.
- Whether the garbage row exists on other sources is not measured; dropping it costs the
  bottom zones their brightest 3-5 % of samples there.

## Reading more than one histogram preset (2026-11-12): possible, but little real gain

Idea: presets 1 (letterbox crop) and 2 (pillarbox crop) place the same zones on a smaller
area, so stacking their equations with preset 0's in the non-negative solve should resolve
the edges better. Simulated through the footprints on the 15 validation pictures the error
at the LEDs fell from 16.8 to 7.9; on the letterbox set, with real reads of presets 0 and
1, from 34.6 to 18.6. Measured on the TV with the new `--preset-probe` and `--preset-rec`:

- **Switching is immediate.** `vtCapture_setPresetHistogram` takes 0.14-0.37 ms and the
  very next read (under 1 ms later) already has the new preset's zones; rotating presets
  0, 1, 2 with one read a frame later was right 90 times out of 90.
- Sample totals per zone, hence exact rectangles (columns x rows of the 162x91 grid):
  preset 1 keeps the columns and squeezes the rows into 68 of 91 (rows 11-78), preset 2
  keeps the rows and squeezes the columns into 121 of 162 (columns 20-140); zones 14 and
  15 are the same in every preset. Preset 3 has 14 zones of other sizes.
- **About 1 % of the reads taken right after a switch are wrong** (10 of 1080): all zones
  far off, in two recurring patterns, partly 100 ms apart. Not seen without switching
  (0 of 1760 and 0 of 817 earlier). Cause not known.
- **Real gain on a real screen is small.** Home screen (all UI, so the UI capture is the
  ground truth; 3 s, presets 0-2 in every panel frame). After removing a level factor (see
  below), error at the 221 LEDs: preset 0 alone 18.3; with preset 2 15.2-16.7; with preset 1
  17.8-23.6; all three 16.7-23.5, depending on the model (measured footprints or exact
  rectangles, 16x9 or 32x18 cells). One flat colour for all LEDs gives 23.4.
- Why the simulation was too rosy: the validation pictures are coarse blocks, where extra
  equations are worth most; and each zone mean deviates 4-6 % from what the true picture
  predicts (sparse sampling of fine detail), which the differences between presets amplify.

Conclusion: not built. Whether smooth film pictures would gain more is not measured (no
ground truth for video).

Also seen:
- On the home screen every zone, channel and preset reads **0.79 times** the level the
  calibration predicts from the UI picture (0.75-0.86 per zone). The calibration page
  gave 1.0 by construction. So the histogram's level depends on the context (picture mode
  or the home screen's own dimming - not determined). Irrelevant for menus, whose colours
  come from the UI plane; for video it may put the LEDs off by a factor in other modes.
- With three reads in every panel frame each preset returns the same sampling state every
  time: the three-state rotation needs reads a frame apart.
- libvtcapture exports nothing but init / set preset (0-3) / get / finalize for the
  histogram: no way to place zones freely through the library.

## Black bars from the sample grid (2026-11-12)

Replaces the level detector and the per-level operators of the section "Black bars (21:9)".

**Measuring.** Every zone is a rectangle of the 162x91 sample grid (`PROFILE_ZONE`, fitted
by `tools/calibrate.py` from the sample totals and the measured footprints; a rectangle
within two samples of a screen edge is taken to touch it). For a zone that touches an edge,
black share x zone size is the number of black sample rows (columns) at that edge; the
smallest value among those zones is the bar, zones that are black all over are left out.
For the three bottom zones the grid's last row is not in the statistics but is part of the
bar (+1). The two bars of a pair are equal: the larger value when they agree within 1.5
samples, otherwise the smaller + 1.5.

- Letterbox capture (UI pictures with known bars): 13.00 measured for 12.64, 10.00 for
  10.11, 8.00 for 7.58, 5.00 for 5.06 samples; 0 for the 16:9 pictures, also the one with a
  dark band at the top only. In that capture (taken before the garbage row was removed) the
  bottom zones read one dark row less than the top ones: the garbage row is there for UI
  pictures too, not only on YouTube.
- **The black threshold matters.** With "16 or lower" (the old detector's) a 2.39:1 film on
  Disney+ gave 12.0-15.5 rows at the top, 11.5-16.5 at the bottom and side bars of up to 9
  columns in dark scenes: 10 changes in 16 s when replayed through the tracker. With "4 or
  lower" the same recording gives 12.00 at the top in every one of 576 frames, 10.9-11.7 at
  the bottom (hence the pair rule) and no side bars: one change, the adoption. A zone that
  lies inside the bar has every sample at 4 or lower.

**Following.** A larger bar is adopted after 2 s without interruption (the smallest value
seen), a bar that grows by less than 3 samples only after 15 s, a smaller one after 0.1 s.
Undecided (nothing changes) while the centre zone is mostly black or a side has no zone that
is not black all over; an opaque UI over half the screen means no bars at once. Limits: 19
rows (20.9 %) top and bottom, 22 columns (13.6 %) left and right.

**Solving.** The 16x9 cells are laid over the picture area and the footprints are the exact
overlaps (`area_setup`: also the least-squares start operator, by Cholesky, 2-3 ms on the
TV, only when the bars change). The image is the picture itself, so no crop afterwards; the
UI layer is sampled where it lies over the picture. A cell is blacked out by a black zone
that covers at least half of it.

| | per-level operators | solve over the picture area |
| --- | --- | --- |
| 8 letterboxed pictures, error against the cropped picture (old LED layout) | 22.29 | 20.24 |
| 15 validation pictures (16:9) | 17.21 (measured footprints) | 17.15 (rectangles) |

The C code equals the Python model to 0.05 of 255, full frame and with bars
(`tests/test_recon.c`, which also plays films, dark scenes, cuts, a menu and a logo on black
through the tracker).

On the TV (a film with bars, about 90 s): bars adopted at 13.2 % top and bottom; by eye top
and bottom follow the picture's edge without jumps and dark scenes stay dark. The log showed
two small swings (13.2 <-> 14.8 %) in the first seconds, from a dark scene next to the bars;
the 15 s rule for small growth was added after that and is only tested offline. CPU 14 %.

Not measured: 4:3 content on the TV (only in the unit test), bars that are not black.

## An on-screen level of our own for the colour keys (2026-11-12, 0.2.5)

The messages of the colour keys came late. Everything below was measured on the B4.

### The TV's messages (toasts) cannot be fast

`com.webos.notification/createToast`, timed with the surface-manager log:

- nothing on screen: visible about 0.1 s after the call;
- a toast on screen: the new one shows 2 s after its request, and requests behind it wait
  2 s each;
- every toast stays 5 s. `timeout`, `duration` and `showTimeout` in the request change
  nothing, and `closeToast` does not take a visible toast away.

So a run of presses can never be followed live with toasts. What is left for them (and
still used when the level app is missing): one toast per run, asked for 0.25 s after the
last press (0.6 s after the first if the presses keep coming and nothing of ours is on
screen), and never while an earlier one of ours has not shown yet.

### A window on top that leaves the keys alone

- A web app with `"defaultWindowType":"overlay"` shows on top of the running app but takes
  every key: arrows and OK no longer reached the app underneath, with `setKeyMask([])`, with
  `["KeyMaskRemoteColorRed"]` and with `["KeyMaskDefault"]` alike.
- `"defaultWindowType":"topui"` is the compositor's `StarfishTopUIView`
  (`SurfaceView { layerNumber: 5; enabled: false }` in its QML): a transparent top layer
  that receives no keys at all (0 key events in the page, and the remote kept working
  underneath, checked by hand).
- The app manager registers only the app that a package's `packageinfo.json` names. A second
  app directory inside the main package is ignored ("Invalid appId specified" on launch).
  Hence the level is a package of its own, `io.github.dimi3h.ambilight.osd`, carried as
  `osd.ipk` in the service directory and installed with
  `com.webos.appInstallService/dev/install` by `ambilightctl osd` when the installed
  version differs.
- The service shows a value by launching the app with parameters
  (`com.webos.applicationManager/launch`). With `handlesRelaunch` a loaded app gets them in
  a `webOSRelaunch` event; `webOSSystem.hide()` takes the window off the screen and leaves
  the page loaded.

### When the window reaches the screen

The first version was loaded ahead with empty parameters and hidden at once. On the TV
nothing ever appeared after that. Measured with a page that reports its own state:

| case | window on screen (surface-manager log) |
| --- | --- |
| cold launch with a value | 0.21-0.58 s after the launch call |
| relaunch of a loaded, hidden page with a value | 0.10-0.14 s |
| cold launch that draws nothing (box at opacity 0) | not in the 5 s watched, while `document.visibilityState` says `visible` and animation frames run at 60 per second |
| ... then a value | 1.12 s later (twice the same figure) |
| a 2x2 px, 2 % dot always drawn | not enough: nothing in the 1 s before it was hidden |
| `hide()` before the window was on screen, then any launch | never again (three launches tried); the page gets 1-6 animation frames in 1.7 s instead of about 100 |
| cold launch that draws the box at opacity 0.01, hidden after 1.7 s | on screen at 0.24 s, later values at 0.11 s |

So the TV brings the window to the screen at the first frame with real content, and a window
hidden before that stays away for good, also for later launches. The page therefore never
hides an empty window. Loaded ahead of the first press (the service does that 20 s after its
start) it runs the normal course with the box at 1 % opacity. And as a safeguard it counts
its animation frames while showing: fewer than 20 by the time it leaves means the window was
never drawn, and it closes itself, so that the next press starts a fresh one (tested by
provoking the state: closed, next launch visible after 0.21 s).

A relaunch always puts the window back on screen, also with empty parameters; an empty one
therefore runs the same faint course, unless a value is showing.

### Result

- 40 presses on the real remote: the launch call reaches the app manager 18-27 ms after the
  key event; the two presses that found it hidden had it on screen after 128 and 148 ms,
  and during a run it is already there.
- By eye: immediate, the number follows every press, arrows and OK keep working underneath.
- After a restart of the service its own pre-load showed (faint) at 0.20 s and left after
  1.6 s; the next value was on screen in 0.14 s.

### Brightness scale

Shown brightness = gain x 50, from 5 to 100 % in steps of 5 (gain 0.1-2.0, steps of 0.1).
100 % is the maximum; the level that used to be called 100 % ("as measured", gain 1.0) is
50 %. The settings file and `ambilightctl set gain=` keep the gain; a stored gain above 2.0
is set to 2.0.

### Not measured

The level over protected video, after standby with Quick Start+, what happens when the TV
discards the loaded page for memory (expected: a cold launch, 0.2-0.6 s), and the removal of
the level app together with the main package (written, not run on the TV).

## White point, subtitles and automatic updates (2026-11-12, 0.2.6)

### A way to measure over any picture

The level app's window is a transparent top layer, so a test colour can be laid over
whatever plays: a development copy of its page takes `{"fill":"#rrggbb"}` and fills the
screen. The running service writes the raw bins on request (`--hist-dump`, or `--snap` for
the bins and the UI plane of the same moment) through the args file and a reload, so nothing
is restarted and the method is safe during protected video. A page that only draws a
background does not bring a new window to the screen (first seen after 6 s); showing the
level once first makes every colour a relaunch of a loaded page. The tools are in
`probe/measure/` (`level_dev.py` makes the page; `patch.sh`, `sweep.sh`, `snaprun.sh` and
`subtest.sh` run on the TV), the bins of the white measurements below in
`probe/data/2026-11-12/white/`. Snapshots of the UI plane show what was on the screen and
are not kept.

`luna-send` writes its replies to a pipe as well as to a terminal, one line per reply and at
once, also with `-i` and `"subscribe":true`. What it cannot have is a closed pipe as its
standard input: then it prints nothing (`true | luna-send -n 1 ...` gives no output, the same
call with `< /dev/null` does). That, not the missing terminal, is what an earlier note in
these findings ran into.

### The histogram's white is the colour temperature's white

White, grey steps and the primaries in the top layer, over SDR video (VRT MAX, picture mode
`cinema`, colour temperature -50) and over HDR10 video (YouTube, `hdrCinemaBright`, -30):

| | SDR, -50 | HDR10, -30 |
| --- | --- | --- |
| white | 253.7 236.8 205.1 | 253.9 240.0 221.2 |
| grey 128 / white | 0.500 0.502 0.502 | 0.503 0.503 0.501 |
| pure red | 253.6 0.2 0.2 | 253.7 0.2 0.2 |

- Over real video the SDR white is the profile's white (253.4, 236.6, 204.9): the factor 0.79
  seen on the home screen belongs to the home screen, not to video.
- In HDR the scale is the same and still linear; only the white differs.
- With the user's leave the colour temperature and the picture mode were switched for two
  minutes while the HDR video played (and put back, verified). Nine temperatures in one mode:

  | setting | -50 | -40 | -30 | -20 | -10 | 0 | 10 | 25 | 50 |
  | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
  | R | 253.8 | 254.0 | 253.8 | 253.6 | 253.8 | 253.7 | 252.0 | 249.8 | 246.7 |
  | G | 237.0 | 238.1 | 239.9 | 239.7 | 240.9 | 241.8 | 241.1 | 239.9 | 239.7 |
  | B | 205.3 | 213.3 | 221.2 | 227.7 | 236.0 | 243.9 | 246.0 | 248.8 | 253.6 |

  Six picture modes (`hdrVivid` 50, `hdrStandard` and `hdrEco` 20, `hdrCinemaBright` -30,
  `hdrCinema` -50, `hdrGame` 0) each gave exactly the white of their temperature, with the
  same grey ratios and a pure red, whatever their contrast, colour and dynamic settings. So
  the white depends on the colour temperature alone (`src/whitepoint_b4.h`). `hdrFilmMaker`
  gave the right white but an odd red and `hdrPersonalized` read nonsense: both taken as
  failed measurements, not followed up. White-balance fine tuning (all zero here) is not
  measured.
- Video is limited to the same white: in 60 reads of SDR video no blue sample lay above 207
  (white 205.1), in HDR video none above 222 (white 221.2). So one white serves video and UI.
- A white field's highest blue bin lies 1 above its mean, for all ten settings.

### The TV's settings do not tell which white is in force

`com.webos.settingsservice` reports the dynamic range (`dimensionInfo`) and the picture mode
and colour temperature (`picture`), also as subscriptions, and they followed every switch.
But with an HDR10 film on Disney+ they said `hdrCinemaBright`, -30, while a white field read
(253.7, 236.8, 205.1): the white of -50. Writing -30 again changed nothing (measured before
and after). On YouTube in HDR10, an hour earlier, the same reported mode did read as -30.
Cause not found (protected playback, the app, or how the stream started in SDR and switched).
In 160 s of that film no blue sample lay above 206.

(The user had chosen the film as Dolby Vision; the TV played it as HDR10:
`videooutput/getStatus` `hdrType`, and `is_stream_play_as_dolby_vision: false` in the log.
Real Dolby Vision is not measured.)

The picture alone does not tell either. White is the bluest a neutral picture gets, and
blue of white rises steadily from 205 to 254 along the table, so the highest blue level seen
looked like a measure: per frame the highest blue bin with 8 samples at or above it, over the
ten zones that stay clear of the grid's last row (13,255 samples). On the first recordings it
fitted: SDR video on VRT MAX at most 207 (a white of 205 gives 206), YouTube HDR 219-222 (221
gives 222), the Disney+ film at most 195. A first version built on that alone failed on the
TV within minutes:

- An SDR film on Plex (white at 205, the reported -50 correct) had blue up to 219 in 8 of 40
  reads a second apart, while green never passed its white (237). Saturated blue goes above
  white's blue: the estimate moved to -40 and -35.
- The home screen and Disney+'s menu showed blue up to 222 where the films after them had
  their white at 205. Screens that are mostly UI are not processed like video (the factor
  0.79 above is another sign of that).

What is built: the profile's white (-50) is used until the picture confirms the setting the
TV reports. A white that is in force shows as a ceiling: within the last minute at least 40
frames have their highest blue level at that white (within 1.5 bins of the highest bin of a
white field) and at most one eighth as many lie above it. Blue that passes through that
level on its way up does not look like that. The verdict starts again when the TV reports
another app, dynamic range, picture mode or colour temperature (1.5 s to settle), frames
with more than half the screen under UI do not count, and a confirmed white is given up
only when blue plainly goes above it. The white in use moves at half a step per frame.

On the TV: YouTube in HDR10 (reported `hdrCinemaBright`, -30) was confirmed 23 s after the
video started (40 frames at its white, none above) and held; in the user's SDR apps the
reported setting is the profile's own, so nothing can change there; the Disney+ HDR case
(reported -30, white at 205) was not played again with this version: by construction, and
in the unit test with the recorded levels, it stays at the profile's white.

Weak spots, by design: a picture that never shows white keeps the profile's white (for HDR
on YouTube: blue 8 % too strong, as before); and where the TV reports the profile's setting
while another white is in force, nothing notices. Neither was seen.

### Subtitles

Snapshots of the UI plane (320x180) and the bins of the same moment:

| | VRT MAX | Disney+ |
| --- | --- | --- |
| text | yellow, dark outline | white (about 240), thin |
| share of the plane | 0.64-1.07 % | 0.18-0.45 % |
| box | x 108-212, y 157-174 | x 109-211, y 152-168 |
| lift of the bottom-centre zone (1746 samples) | 7.6-11.9 of 255 in R and G | 1.8-4.4 |

- The text's samples cannot be picked out of the bins. Where the capture promised 70-100
  samples of yellow, 20-25 sat at yellow's level (R 253-254, G 236-238); the rest is spread
  over everything between the picture and the text, because a stroke is a few 4K samples
  wide and most hits land on its soft edge.
- So the text is taken out of the zone's mean: with A = sum of alpha and S = sum of alpha x
  colour over the zone's part of the capture, video = (mean x N - A x black - S x (white -
  black) / 255) / (N - A). Under a picture darker than the text's own lift another 30 % of
  that lift is taken off, so that black stays black; the capture only tells the text's
  share to about a third.
- A subtitle is recognised by where it is: the plane is nearly empty (0.02-3 %), and what it
  shows fits a box at most 85 % wide and 30 % high, centred within 20 % of the middle, that
  lies in the lower half or the upper 40 %. The level of the colour keys (top right) and a
  menu are not subtitles.
- While a subtitle shows it is not laid over the picture, and top and bottom bars are not
  judged (text in a bar used to shrink it).
- A spike in a film's histogram that looked like the subtitle colour turned out to be flat
  video content in zones the text does not reach: only the zones tell.

On the TV, with two lines of subtitle-like text shown and hidden every 1.3 s by a
development copy of the level app over a playing film (bottom-centre zone, mean with the
text minus the mean without it, +- standard error):

| | filter off | predicted from the capture | filter on |
| --- | --- | --- | --- |
| white text, 12 cycles | R +4.3 +-2.0, G +6.4 +-1.4, B +5.4 +-1.3 | +4.8 +4.9 +4.4 | R +0.9 +-2.0, G +0.8 +-1.2, B +0.4 +-1.0 |
| yellow text with outline, 16 cycles | R +7.1 +-1.1, G +5.7 +-1.3, B -2.7 +-0.9 | +5.3 +5.7 -1.3 | not decided: the film moved more than the text added |

So the capture predicts the text's light to within the noise, and with the filter the white
text leaves under 1 of 255. For yellow only the prediction is confirmed; the run with the
filter on fell in a restless part of the film (the control zone at the top moved by 4).
A still, dark picture under real subtitles has not been measured; the unit test covers it.

### Automatic updates

`ambilightctl maintain` (daily) downloads a newer release to the state directory when
automatic updates are on, and checks it against the feed's SHA-256. It is installed with
`appInstallService/dev/install`, as the level app is, at one of two moments: by the
supervisor when the TV starts, before the service runs (not if a streaming app is already
in the foreground), or when the service has seen the home screen in the foreground for a
minute. At most
three attempts per version. The supervisor and `ambilightctl` are replaced by the install
while they run: each does the step inside one compound command, which the shell has read
completely before it starts.

On the TV, with a development build staged as the downloaded update:

- Home screen: 73 s and 89 s after the home screen came up the service asked for the
  install; `ambilightctl` installed it within 1 s, the service noticed its executable was
  replaced 4.5 s later and the new one had the histogram open 4.6 s after that. With a film
  playing in another app the same call does nothing.
- Start of the TV (after a restart): the supervisor installed it before the service's first
  start and handed over to the new supervisor; the service was up 105 s after the restart.
- Switching the TV off for 20 s is not a start: it goes to `Active Standby`
  (`tvpower/power/getPowerState`), every process keeps running (the histogram was away for
  139 ms) and it comes back as it was. A cold start was seen once after a longer time off.
- Found on the way: Homebrew Channel starts the boot hook without `PATH` in its
  environment. The shell finds its own commands, but `elevate-service` then fails in LG's
  `ls-control` ("sleep: not found"), so after any update handled by a supervisor that was
  started at boot the elevation failed. The scripts now set `PATH` themselves.

Everything under `/media/developer` is world-writable on this TV (the installer makes
directories and files 777), the service directory included. Automatic updates add nothing
to that: who can write there could already replace what runs as root.
