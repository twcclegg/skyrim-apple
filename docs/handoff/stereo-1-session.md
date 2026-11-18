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
| `LB_STEREO_EYE_UNITS` | `2.24` | half the eye separation (32 mm at 70 units/m, the compositor's default). Required: without it the runtime never turns live stereo on, and both eyes get the same picture |
| `LB_STEREO_MULTIVIEW` | `1` | live single-pass stereo; also turns `LB_DEPTH_COPY` on |
| `LB_STEREO_PRESENT` | `1` | both eyes' final pictures to the compositor |
| `LB_EYE_VIEWS` | `1` | the headset's own eye frusta feed the game |
| `LB_EYE_LATE` | `1` | eyes turned toward the head at each frame's first main-scene draw (max 2° each way, `LB_EYE_LATE_MAX`) |
| `LB_STEREO_HUD_DEPTH` | `2` | interface at 2 m |
| `LB_PIPE_LOG` | `1` | each pipeline compile logged, to match hitches |
| `LB_WINDOW` | per arm | render size; checks the 1280×1024 fix on the device |
| `LB_FOV` | per arm | 65 at 1280×720, 84.4 at 1280×1024: the same 80.7° across and pixels per degree, 51° → 68° tall |
| `LB_MAX_FPS` | `45` | the visionOS default, written out: `boss-fight-avp` carries `LB_MAX_FPS=120`, and `launch-env.json` wins |

45 fps is every second 90 Hz refresh. The app applies `launch-env.json` over the environment bench launches it with,
so a key in the file beats bench's `--env` and the scenario's own `env`. Set each per-arm key (`LB_WINDOW`, `LB_FOV`,
the stereo keys) in exactly one place: either edit the file between arms, or leave those keys out of it and pass
them with `--env`.
Add `LB_HITCH_MS=40` if #18 is in main by then.

`LB_FOV` is the field of view at the game's 4:3 reference. The vertical field follows from it at any aspect, and the
horizontal grows with the window's width. At 1280×1024, 84.4 keeps the 720 arm's horizontal field and pixel density
and adds height, which is what the headset's eyes are short of. The earlier 93.7 gave 90° × 77° at 1024: wider, with
about 15% fewer pixels per degree across, so the arms would have differed in more than height.

The mono arm is the same settings without the five stereo and eye keys (`LB_STEREO_EYE_UNITS`, `LB_STEREO_MULTIVIEW`,
`LB_STEREO_PRESENT`, `LB_EYE_VIEWS`, `LB_EYE_LATE`). `LB_DEPTH_COPY` is then off by default. Add `LB_DEPTH_COPY=1`
to the mono arm if the A/B should measure stereo alone, not stereo plus the depth copy.

## Order (about 50 minutes)

1. **Launch check (2 min).** Stereo base, `LB_WINDOW=1280x720`, `LB_FOV=65`, Riverwood. Confirm
   `stereo: live single-pass stereo, d 2.240 units` and `immersive: stereo frames: each eye draws its own picture`.
   Read the first `immersive: eyes: …` line: its `raw L … R …` values are the real eye tangents (the first pair is
   after clipping to the game's frustum). Confirm there are no `mvk-error` lines, the app doesn't
   abort on layered attachments, and both eyes show a picture. **Stop the session here if it aborts:** send the log
   to the M1; this is the case the deferred mono fallback is for.
2. **Comfort look (5 min, not timed).** Stand in Riverwood, then cast Flames. Answer in one line each:
   - Do near objects (the hand, the flame, a fence 1–2 m away) fuse with no double image?
   - Does the HUD sit comfortably at 2 m against distant mountains?
   - Is there swimming when turning the head, with `LB_EYE_LATE` on?
3. **Timed A/B (about 30 min), interleaved, `boss-fight-avp` then `whiterun-market-avp`:**
   mono 720 → stereo 720 → stereo 1024 → mono 720 → stereo 720 → stereo 1024.
   720 = `LB_WINDOW=1280x720 LB_FOV=65`; 1024 = `LB_WINDOW=1280x1024 LB_FOV=84.4`.
   Log the thermal state with each run, and drop runs at *serious* from comparisons.
   At 45 fps a run takes about 3.1 min (whiterun-market-avp, 8400 frames) to 3.3 min (boss-fight-avp, 8800), so the
   twelve runs need about 40 minutes plus launches. Drop the second round of one scenario if time runs short.
4. **If time and thermal allow:** stereo 1024 with `LB_MAX_FPS=0` once (in `launch-env.json`) (headroom above 45 fps), and one stereo run with
   `LB_DEPTH_COPY=0` (its cost on the M2).

## What the session has to answer

| Question | From |
|---|---|
| Does live multiview run on the M2 and its MoltenVK slice at all? | step 1 |
| Real eye tangents, so `LB_EYE_VIEWS` uses measured frusta | `immersive: eyes:` |
| Does stereo hold 45 fps inside 22.2 ms, at 720 and at 1024? | step 3: frame mean, p95, and `game gpu latency mean X ms, max Y ms` in the `immersive cadence:` lines |
| GPU cost of stereo on the M2 against mono | step 3: `gpu_busy_ms` uncapped (step 4); capped busy time understates it on Apple GPUs |
| What `LB_EYE_LATE` leaves to correct | `immersive: eye views: shown X deg from where the picture was rendered looking (mean), max Y`, every 5 s |
| Do hitches line up with pipeline compiles? | `pipe compiled at draw: X ms` (a draw waited) and `pipe compiled ahead: X ms` lines against long frames |
| Is the 1280×1024 render size accepted on the device? | `prefs: render size 1280x1024 (LB_WINDOW) in …`, no display-mode refusal |
| Comfort: near fusion, HUD depth, head-turn swimming | step 2 |

## Before the session

- The headset scenarios (`boss-fight-avp`, `whiterun-market-avp`, `riverwood-walk-avp`) pin the weather to clear with
  `bat benchwx` at frame 3100, after the coc load and before the first phase, with `LB_VPAD=0`. Stereo and mono runs
  see the same sky.
- The switches above, and the lines they log, are in `docs/perf/IMMERSIVE_OPTIONS.md` ("Live stereo switches").
- Reset the bench `SkyrimPrefs.ini` to 1280×720 afterwards: `LB_WINDOW` writes the render size into it.

Results go on issue #42 under "Headset session stereo-1".
