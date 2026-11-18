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
| `LB_WINDOW` | per arm | render size at the eyes' aspect: 1280×880 or 1600×1100 |
| `LB_FOV` | unset (stereo) | with eye views the game's field of view follows the eyes (`eye views: game field of view …`) |
| `LB_MAX_FPS` | `45` | the visionOS default, written out: `boss-fight-avp` carries `LB_MAX_FPS=120`, and `launch-env.json` wins |

45 fps is every second 90 Hz refresh. The app applies `launch-env.json` over the environment bench launches it with,
so a key in the file beats bench's `--env` and the scenario's own `env`. Set each per-arm key (`LB_WINDOW`, `LB_FOV`,
the stereo keys) in exactly one place: either edit the file between arms, or leave those keys out of it and pass
them with `--env`.
Add `LB_HITCH_MS=40` if #18 is in main by then.

**Field of view (changed after the first headset contact, 18 Nov).** Each eye sees about 105° × 95° (both together 120° across): measured tangents
L 1.732 / 1.000 / 1.000 / 1.192 and R 1.000 / 1.732 / 1.000 / 1.192 (left, right, top, bottom). `LB_FOV=65` gave the game
77° × 47°, so the eye views were clipped to a window floating in black. With `LB_EYE_VIEWS=1` and `LB_FOV` unset, the
compositor now asks for a game frustum holding both eyes plus its 2° margin (tangents 1.881 × 1.280), and the runtime sets
the game's field of view from that and the window's aspect. At 1280×880 (aspect 1.455, the eyes' own) that is 119.8° at
the 4:3 reference, logged once as `eye views: game field of view 119.8 degrees …`. A number in `LB_FOV` still wins. The
cost is fewer pixels per degree: 1280 pixels across about 124°. The 1600×1100 arm has 56% more pixels for the same view.
The mono arm sets the same field of view by number, since it has no eye views: `LB_FOV=119.8`.

The mono arm is the same settings without the five stereo and eye keys (`LB_STEREO_EYE_UNITS`, `LB_STEREO_MULTIVIEW`,
`LB_STEREO_PRESENT`, `LB_EYE_VIEWS`, `LB_EYE_LATE`). `LB_DEPTH_COPY` is then off by default. Add `LB_DEPTH_COPY=1`
to the mono arm if the A/B should measure stereo alone, not stereo plus the depth copy.

## Order (about 50 minutes)

1. **Launch check (2 min).** Stereo base, `LB_WINDOW=1280x880`, no `LB_FOV`, Riverwood. Confirm
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
   mono 880 → stereo 880 → stereo 1100 → mono 880 → stereo 880 → stereo 1100.
   880 = `LB_WINDOW=1280x880`; 1100 = `LB_WINDOW=1600x1100`; mono adds `LB_FOV=119.8`, stereo leaves `LB_FOV` unset.
   Log the thermal state with each run, and drop runs at *serious* from comparisons.
   At 45 fps a run takes about 3.1 min (whiterun-market-avp, 8400 frames) to 3.3 min (boss-fight-avp, 8800), so the
   twelve runs need about 40 minutes plus launches. Drop the second round of one scenario if time runs short.
4. **If time and thermal allow:** stereo 1100 with `LB_MAX_FPS=0` once (in `launch-env.json`) (headroom above 45 fps), and one stereo run with
   `LB_DEPTH_COPY=0` (its cost on the M2).

## What the session has to answer

| Question | From |
|---|---|
| Does live multiview run on the M2 and its MoltenVK slice at all? | step 1 |
| Real eye tangents, so `LB_EYE_VIEWS` uses measured frusta | `immersive: eyes:` |
| Does stereo hold 45 fps inside 22.2 ms, at 880 and at 1100? | step 3: frame mean, p95, and `game gpu latency mean X ms, max Y ms` in the `immersive cadence:` lines |
| GPU cost of stereo on the M2 against mono | step 3: `gpu_busy_ms` uncapped (step 4); capped busy time understates it on Apple GPUs |
| What `LB_EYE_LATE` leaves to correct | `immersive: eye views: shown X deg from where the picture was rendered looking (mean), max Y`, every 5 s |
| Do hitches line up with pipeline compiles? | `pipe compiled at draw: X ms` (a draw waited) and `pipe compiled ahead: X ms` lines against long frames |
| Are the 1280×880 and 1600×1100 render sizes accepted, and does the view fill both eyes? | `prefs: render size … (LB_WINDOW) in …`, no display-mode refusal; `eye views: game field of view …`; no black border |
| Comfort: near fusion, HUD depth, head-turn swimming | step 2 |

## Before the session

- The headset scenarios (`boss-fight-avp`, `whiterun-market-avp`, `riverwood-walk-avp`) pin the weather to clear with
  `bat benchwx` at frame 3100, after the coc load and before the first phase, with `LB_VPAD=0`. Stereo and mono runs
  see the same sky.
- The switches above, and the lines they log, are in `docs/perf/IMMERSIVE_OPTIONS.md` ("Live stereo switches").
- Reset the bench `SkyrimPrefs.ini` to 1280×720 afterwards: `LB_WINDOW` writes the render size into it.

Results go on issue #42 under "Headset session stereo-1".
