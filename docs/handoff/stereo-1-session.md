# Headset session "stereo-1": live single-pass stereo on the M2

The first time live per-eye stereo (DXVK multiview) runs on the Vision Pro. Everything before this was measured on
the M1 and M5 Macs (issue #42). Under an hour of headset time, on AC power if possible: the Nov 15–16 session showed
runs about 7% slower after two hours on battery, and *serious* thermal soon after.

## Build

`main` at or after the merge of `true3d/leftovers` (DXVK twcclegg/dxvk-native, branch `skyrim-apple` plus any bump in
that PR), pinned registers, signed by the M1. The headset needs only main: no side branches.

Record the commit and the DXVK commit in every run's label.

## Settings (launch-env.json)

Base for every stereo arm:

| Key | Value | Why |
|---|---|---|
| `LB_IMMERSIVE_HEAD` | `1` | head tracking, as in normal play |
| `LB_STEREO_MULTIVIEW` | `1` | live single-pass stereo; also turns `LB_DEPTH_COPY` on |
| `LB_STEREO_PRESENT` | `1` | both eyes' final pictures to the compositor |
| `LB_EYE_VIEWS` | `1` | the headset's own eye frusta feed the game |
| `LB_EYE_LATE` | `1` | eyes turned toward the head at present time (max 5°) |
| `LB_STEREO_HUD_DEPTH` | `2` | interface at 2 m |
| `LB_PIPE_LOG` | `1` | each pipeline compile logged, to match hitches |
| `LB_WINDOW` | per arm | render size; checks the 1280×1024 fix on the device |
| `LB_FOV` | per arm | must match the window (65 for 16:9, 93.7 for 1280×1024) |

Leave `LB_MAX_FPS` at the visionOS default (45 fps, every second 90 Hz refresh) unless an arm says otherwise.
Add `LB_HITCH_MS=40` if #18 is in main by then.

The mono arm is the same settings without the four stereo and eye keys (`LB_STEREO_MULTIVIEW`, `LB_STEREO_PRESENT`,
`LB_EYE_VIEWS`, `LB_EYE_LATE`).

## Order (about 50 minutes)

1. **Launch check (2 min).** Stereo base, `LB_WINDOW=1280x720`, `LB_FOV=65`, Riverwood. Read the first
   `immersive: eyes: ...` line (the real eye tangents) and confirm there are no `mvk-error` lines, the app doesn't
   abort on layered attachments, and both eyes show a picture. **Stop the session here if it aborts:** send the log
   to the M1; this is the case the deferred mono fallback is for.
2. **Comfort look (5 min, not timed).** Stand in Riverwood, then cast Flames. Answer in one line each:
   - Do near objects (the hand, the flame, a fence 1–2 m away) fuse with no double image?
   - Does the HUD sit comfortably at 2 m against distant mountains?
   - Is there swimming when turning the head, with `LB_EYE_LATE` on?
3. **Timed A/B (about 30 min), interleaved, `boss-fight-avp` then `whiterun-market-avp`:**
   mono 720 → stereo 720 → stereo 1024 → mono 720 → stereo 720 → stereo 1024.
   720 = `LB_WINDOW=1280x720 LB_FOV=65`; 1024 = `LB_WINDOW=1280x1024 LB_FOV=93.7`.
   Log the thermal state with each run, and drop runs at *serious* from comparisons.
4. **If time and thermal allow:** stereo 1024 with `LB_MAX_FPS=0` once (headroom above 45 fps), and one stereo run with
   `LB_DEPTH_COPY=0` (its cost on the M2).

## What the session has to answer

| Question | From |
|---|---|
| Does live multiview run on the M2 and its MoltenVK slice at all? | step 1 |
| Real eye tangents, so `LB_EYE_VIEWS` uses measured frusta | `immersive: eyes:` |
| Does stereo hold 45 fps inside 22.2 ms, at 720 and at 1024? | step 3: frame mean, p95, game GPU latency |
| GPU cost of stereo on the M2 against mono | step 3: `gpu_busy_ms` uncapped (step 4); capped busy time understates it on Apple GPUs |
| What `LB_EYE_LATE` leaves to correct | compositor's per-5 s mean/max residual lines |
| Do hitches line up with pipeline compiles? | `LB_PIPE_LOG` lines against long frames |
| Is the 1280×1024 render size accepted on the device? | `prefs: render size 1280x1024 (LB_WINDOW)`, no display-mode refusal |
| Comfort: near fusion, HUD depth, head-turn swimming | step 2 |

## Before the session

- The headset scenarios (`*-avp.json`) don't pin the weather yet (the Mac ones do, with `console_later` and
  `benchwx.txt`). Add the same to `boss-fight-avp` and `whiterun-market-avp`, timed for the headset, so stereo and
  mono runs see the same sky.
- The stereo switches above aren't in `docs/perf/IMMERSIVE_OPTIONS.md` yet; add them there.
- Reset the bench `SkyrimPrefs.ini` to 1280×720 afterwards: `LB_WINDOW` writes the render size into it.

Results go on issue #42 under "Headset session stereo-1".
