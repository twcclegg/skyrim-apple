# Headset session "fresh-eyes-2": the parts that need a wearer

About 35 minutes **worn**, on a 60 W or larger adapter. fresh-eyes-1 ran unattended on 28 Nov, so only its timing
and log steps happened (results on #42). Everything below needs someone looking. Stop at once if anything is
unpleasant.

## Build

The M1's combined build with the fixes from the 27–28 Nov reviews, if it is ready and passed its Mac checks:
#66 (interface depth-stencil), #65 (head steering with Invert Y), #85 (HUD scale). Otherwise fe1 (`fe1-fc678e26`)
as installed, with its known issues: menu models may draw without depth (#66), keep **Invert Y off** (#65).

Base settings: the play preset now on the device (IMMERSIVE_HEAD, EYE_UNITS 2.24, MULTIVIEW, PRESENT, EYE_VIEWS,
EYE_LATE, MAX_FPS 45, WINDOW 1600x1100, REFINE 4, HUD_LAYER 1) plus `LB_FRAMETIMES=1` for the log. Remove
`LB_FRAMETIMES` after the session.

## Order

1. **Launch check (1 min).** Riverwood, stand still. Note: does the world sit still and at the right size?
2. **Compositor looks (8 min, about 90 s each).** Stand by the well; look at fence posts and grass against the
   hills; turn the head slowly left and right. One sentence each:
   - a. Base.
   - c. `LB_IMMERSIVE_REFINE=2 LB_IMMERSIVE_REFINE_EXIT=0.25 LB_IMMERSIVE_SEED=1 LB_IMMERSIVE_GRID=161x91`.
     Look hardest at thin posts, branches and grass edges: any outline wobble or tearing against a?
     (Unattended it cut the compositor's vertex work from 1.18 to 0.31 ms and held 90 Hz.)
   - d. c plus `LB_FRAME_REPEAT=1`. The log must say `each frame shown for 2 refreshes`. Do near objects double
     when you turn? Does the HUD
     swim? Any lag you can feel?
   - e. c plus `LB_IMMERSIVE_MOTION=1`, walking forward and strafing. Smoother or worse than c?
   - Skip arm b (c without the grid): unattended it presented at 45 Hz steadily, so it would judder.
3. **Play probe (20 min, best arm from step 2).** One line each.
   1. HUD: can you read the compass, the health bar and a subtitle without strain? Where do they sit?
   2. Open the inventory, the map, a book and a lock. Readable? Comfortable? Do 3D items look right?
   3. Talk to one NPC and loot one container. Does the view flicker going through a door?
   4. Turn with the stick for 30 seconds, then walk and strafe. Any queasiness?
   5. Toggle third person and turn your head. Stop at once if it is unpleasant.
   6. Shoot a bow at a far target. Is the crosshair doubled?
   7. Sound: stand by running water and turn your head 90°. Where does the sound come from?
   8. Press the Digital Crown once: the game should pause and the launcher show **Return to Immersive**. Press it:
      does the game come back and resume? Then hold the Crown to recenter.
   9. Look down at your hands and the controller. Press each button while looking around. Any rumble?
   10. Take the headset off for 30 seconds and put it back on.
   11. **Shadows (#92, 1 min):** in sunlight, look at the leaf shadows on the ground a few steps ahead. Close one
       eye, then the other. Does a patch move against a stone or tuft next to it? With both eyes open, painted on
       the ground or floating? (Measured on the Mac: with eye views the default path's shadows are right in both
       eyes, so this should look fine.)
4. **One HUD screenshot** in Riverwood (top button + Digital Crown). Keep it private; describe where the compass
   and bars sit, for #85.

## What each step decides

| Step | Decides |
|---|---|
| 2c | Whether the coarse grid can become the default (saves about 0.9 ms of compositor work per refresh) |
| 2d | Whether frame repeat is usable: half the compositor cost per game frame, less power |
| 2e | Whether movement extrapolation is worth finishing |
| 3 | The comfort and menu work (snap turn, cameras, menus, audio mode, recenter) |
| 3.11 | Whether #92 needs anything on the default path (expected: no) |
| 4 | #85's HUD placement |

## Unattended, any time (no wearer)

These need only the device on power, cool and kept awake, with Thomas's OK to use it:
- The boss fight again on a cache that has seen it (the 12 s freeze at frame 8023 was a first sight of about 60
  stereo pipelines).
- A seed state cache from the Mac, once built, against the headset's own (needs Thomas's OK to move caches).
