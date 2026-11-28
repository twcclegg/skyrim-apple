# Headset session "fresh-eyes-1": combat timing, compositor switches, first real play

About 40 minutes of headset time, **on a 60 W or larger adapter** (on battery the headset slowed and twice powered
off). Everything here was prepared and checked off the headset; this session answers what only the headset can.
Results go on issue #42, one comment, with the commits in the first line.

## Build

- skyrim-apple `headset/fresh-eyes-1`: main (with #83 re-entry and #84 comfort settings) plus #66 + #85 (HUD layer
  and its scale), #87 (compositor cost switches), #69 + #74 (shader bytecode cache, pipeline misses), and the
  eye-view fixes the last staged build carried: #71 + #72 (per-eye shadow mask, bloom and distance blur) and
  #76 with #75 (screen blood and the image-space blur quad). Without those the play probe would show artefacts
  already fixed and colour the comfort answers.
- dxvk-native `headset/fresh-eyes-1` (8ab830c7): #8, #10, #11, #7 and #9, the DXVK halves of the above.

Every new switch is off unless set below, except two DXVK defaults from #69 and #74: the bytecode cache and
pipeline normalization (`DXVK_SHADER_BYTECODE_CACHE=0` and `DXVK_PIPELINE_NORMALIZE=0` turn them off).

## Base settings (launch-env.json)

The stereo-1 base (`docs/handoff/stereo-1-session.md`: `LB_IMMERSIVE_HEAD`, `LB_STEREO_EYE_UNITS=2.24`,
`LB_STEREO_MULTIVIEW`, `LB_STEREO_PRESENT`, `LB_EYE_VIEWS`, `LB_EYE_LATE`, `LB_MAX_FPS=45`, no `LB_FOV`), plus:

| Key | Value | Why |
|---|---|---|
| `LB_WINDOW` | `1600x1100` | the best headset config so far |
| `LB_IMMERSIVE_REFINE` | `4` | today's per-pixel depth search, the baseline the switches are measured against |
| `LB_FRAMETIMES` | `1` | `ft <frame> <ms> <Present ms> <work ms> <draws>` |
| `LB_IMMERSIVE_PASS_TIME` | `1` | compositor vertex and fragment time per frame in the 5 s log |
| `LB_PIPE_LOG` | `1` | every pipeline compiled at draw, with its ms |
| `LB_HUD_LAYER` | `1` | the interface as a flat layer; at the default scale 0.28 (about 56° × 40°) |

Play Immersive adds the comfort settings itself (`ini: comfort defaults for immersive play: 3 of 3 added`).

## Order

1. **Launch check (2 min).** Riverwood, stand still. From the log, copy these lines into the results:
   `drawable:`, `rate maps:`, `hud layer:`, `ini: comfort defaults`, `per-pixel depth search:`, any line from the
   bytecode cache at startup (`dxso`), and the first two `compositor gpu` / `compositor passes` lines.
2. **Timed boss fight (8 min).** Base settings, the `boss-fight-avp` scenario through bench as in stereo-1. Keep
   the full log. This one run replaces the whole combat estimate (is the frame over 22.2 ms, and by how much).
3. **Compositor A/B in Riverwood (6 min, about a minute each).** Stand by the well, look at fence posts and the
   grass against the hills, turn the head slowly left and right. After each arm, note the two `compositor passes`
   lines and one sentence on how it looks.
   - a. Base.
   - b. `LB_IMMERSIVE_REFINE=2 LB_IMMERSIVE_REFINE_EXIT=0.25 LB_IMMERSIVE_SEED=1`. Should look the same as a.
   - c. b plus `LB_IMMERSIVE_GRID=161x91`. Look hardest at grass, branches and thin posts: any outline wobble?
   - d. b plus `LB_FRAME_REPEAT=1`. The log must say `frame repeat: … 2.00x the display's`. Do near objects double
     when you turn? Does the HUD swim? Any lag you can feel?
   - e. b plus `LB_IMMERSIVE_MOTION=1`, walking forward and strafing. Smoother or worse than b?
4. **Play probe (20 min, best arm from step 3).** One line of notes each. Stop at once if anything is unpleasant.
   1. HUD: can you read the compass, the health bar and a subtitle without strain?
   2. Open the inventory, the map, a book and a lock. Readable? Comfortable?
   3. Talk to one NPC and loot one container. Does the view flicker going through a door?
   4. Turn with the stick for 30 seconds, then walk and strafe. Any queasiness?
   5. Toggle third person and turn your head. Stop at once if it is unpleasant.
   6. Shoot a bow at a far target. Is the crosshair doubled?
   7. Sound: stand by running water and turn your head 90°. Where does the sound come from? (Today's build uses
      stereo; if time allows, relaunch with `LB_AUDIO_CHANNELS=6`, then add `LB_AUDIO_SPATIAL=fixed`.)
   8. Press the Digital Crown once. The game should pause and the launcher show **Return to Immersive**. Press it:
      does the game come back in the space and resume? Then hold the Crown to recenter.
   9. Look down at your hands and the controller. Press each button while looking around. Is there rumble?
   10. Take the headset off for 30 seconds and put it back on.
   11. Shadows (#92, 1 min): in sunlight in Riverwood, look at the leaf shadows on the ground a few steps ahead.
       Close one eye, then the other. Does a shadow patch move against a stone or a grass tuft next to it, or
       stay put on the ground? With both eyes open: do the shadows look painted on the ground, or do they float or
       shimmer?

Known in this build, not device faults:
- #66 as built drops the interface pass's depth-stencil, so 3D models in menus and on the loading screen
  (inventory item, map) may draw without a depth test. Note it if seen; don't stop.
- Keep **Invert Y off**. With it on, head steering can pin the camera to the floor or ceiling (#65). Main's head
  steering can also flip falsely after mouse use; note it if it happens.
5. **Warm relaunch (3 min).** Quit and start again from the same save in Riverwood. Note the bytecode cache's
   startup line and the number of `pipe compiled at draw` lines in the first two minutes against the first launch.
   This tells whether Metal's shader cache survives a relaunch on the headset (hitch plan, step 4).
6. **One screenshot of the HUD** in Riverwood (top button + Digital Crown). Keep it private; describe in the
   results where the compass and bars sit.

After the session, put a plain play preset on the device as launch-env.json (no `LB_HITCH_MS`, `LB_PIPE_LOG`,
`LB_FRAMETIMES` or `LB_PASS_TIME`). The device's previous file is kept, but it had the 18–19 Nov diagnostic keys on
for normal play, so don't restore it as it was.

With Thomas's OK only: after the session, pull `Skyrim.dxvk-cache` and `Skyrim.dxso-cache` from the app's
`Application Support/Skyrim Jitless/Saves` for the shader index tests (#86). They are derived from the game files:
private, never in the repo.

## What each step decides

| Step | Decides |
|---|---|
| 2 | Whether combat fits 45 fps on the M2, and how much the compositor work in #87 matters |
| 3b | Whether the search exit and seeded mesh give back the predicted 1–2 ms per refresh with no visible change |
| 3c | Whether the coarse grid can become a default (the reference check says it changes thin outlines) |
| 3d | Whether frame repeat is usable: about half the remaining compositor cost, and less power |
| 3e | Whether movement extrapolation is worth finishing |
| 4 | The comfort and menu work in the plan (snap turn, cameras, menus, audio mode, recenter) |
| 4.11 | How urgent the per-eye shadow fix (#92) is on the default path |
| 5 | Whether a seed pipeline cache shipped in the app is needed |
