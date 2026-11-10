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
