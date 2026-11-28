# Plan: trackpad and mouse on Vision Pro

> **Implemented 2026-11-10, not yet run on the headset:** step 1 diagnostics; the `key`
> lock variant (`LB_POINTER_LOCK=key`); the game in its own window, which is the pointer surface
> (`LB_GAME_SCENE=1`: `apple/Sources/App/GameSurface.swift`, plain style, 2400x1300, 0.001-opacity fill with a
> content shape, hover feeding the edge turn, `.pointerStyle(.shape(Circle(), size: 0.01))`; the launcher window
> closes at Play; SDL's view hides during immersive play); indirect-pinch clicks deduplicated against GCMouse.
> Without `LB_GAME_SCENE`, the launcher window turns nearly invisible during immersive play instead. Not done:
> reopen-on-background, the menu-cursor sync (step 8). Headset results from another visionOS port (lock refused, tracking
> areas useless, the window form that works) informed the design.

Plan, 2026-11-10. Planning only; nothing here is built.
"Verified" means read in the XROS27.2 SDK on this Mac, in this repo's code, or in a headset log. Everything else
is marked as a guess. Nothing in this plan has run on the headset.

## Short version

- There is **no verified API that gives unbounded relative deltas** on visionOS. The only candidate is UIKit
  pointer lock, which SDL already requests and the system has never granted. Whether visionOS ever grants it is
  unknown, and there are two cheap things to try before giving up on it (step 2).
- So the plan is: one diagnostic headset run that tries the lock variants through `launch-env.json` (one install,
  no reinstall between variants), then one of two designs depending on the result:
  **L** (lock granted) or **S** (never granted: a pointer surface the pointer can't easily leave, plus the edge
  turn, plus clicks from spatial events).
- Independent of that result: the game gets its own window scene, the launcher window is dismissed during play,
  and menus keep the game cursor under the system dot instead of trying to hide the dot.

## 1. Where relative deltas can come from

| Source | Verified | Not verified |
|---|---|---|
| **GCMouse** `mouseMovedHandler`, buttons, `scroll` | In the SDK without a visionOS exclusion (`GCMouse.h`, `GCMouseInput.h`: raw deltas, "not affected by sensitivity"). Delivers on the headset: `bench-results/avp-immersive-20261110T1258/game-…185145Z.log` shows e.g. "mouse deltas 176 over window / 6 off". So the system gates them on the pointer being over our window. | Whether the gate is "pointer over any surface of the app" (then a tracking area or a second window would also open it) or "over the key window". Whether buttons and scroll are gated the same way (not counted yet). Behaviour in a Full Space with the pointer over nothing: mc-avp keeps a play window as the pointer surface for this reason, but it wasn't measured. |
| **Pointer lock**: `UIViewController.prefersPointerLocked`, `childViewControllerForPointerLock`, `setNeedsUpdateOfPrefersPointerLocked`, `UIScene.pointerLockState`, `UIPointerLockState.didChangeNotification` | All in the XROS27.2 headers, `API_AVAILABLE(ios(14.0))`, not marked unavailable for visionOS. "This preference may or may not be honored." SDL requests it: `SDL_uikitviewcontroller.m:253` returns YES in relative mode and `UIKit_UpdatePointerLock` is compiled for visionOS (`__IPHONE_OS_VERSION_MAX_ALLOWED` is 170000 with the xros SDK; I preprocessed it). Every headset log says "locked 0". | Whether visionOS grants it at all, in the Shared Space or with an ImmersiveSpace open. Which view controller UIKit asks: the game scene holds two UIWindows (SwiftUI's hosting window and SDL's), and if UIKit asks the key window's root controller and that is SwiftUI's, SDL's answer is never read. On iPadOS the lock needs a full-screen, frontmost scene (from memory of Apple's docs, not re-read). |
| SwiftUI **`pointerVisibility`** | **Unavailable on visionOS** in SDK 27.2 (`@available(visionOS, unavailable)`, SwiftUI.swiftinterface line 3695). mc-avp's `docs/review/visionos27.md` item 1d is wrong for this SDK. No SwiftUI pointer-lock modifier exists (no "pointerLock" in the interface). | – |
| **UIPointerInteraction** (and SwiftUI `onContinuousHover`) | Available. Absolute position inside the view only; that is what feeds `SDLXR_PointerMoved`. `hiddenPointerStyle` is in the header, and the headset still shows the dot. | Whether any pointer style changes the visionOS dot at all. |
| **Spatial events** on the compositor layer: `LayerRenderer.onSpatialEvent`, `SpatialEventCollection.Event.Kind.pointer` / `.indirectPinch` | Both in the SDK (visionOS 1.0). Not used in `apple/Sources` today. mc-avp saw trackpad clicks arrive as `indirectPinch` in the full space, rarely. | Events are per interaction (press to release), so they can give clicks and drags, not free movement. Whether `.pointer` events ever arrive on the layer. |
| **Tracking areas** (CompositorServices 26): `Configuration.trackingAreasFormat`, `Drawable.trackingAreasTextures`, `TrackingArea`, `Event.trackingAreaIdentifier` | In the SDK. | Whether a tracking area covering the whole view makes the layer count as "under the pointer" so GCMouse deltas flow in the Full Space with no window. This is the most valuable unknown after pointer lock. |
| `GCEventInteraction` | `handledEventTypes` has only `Gamepad` and `Stylus` (26.2). No mouse type, so no capture through it. | – |

## 2. Clicks, wheel, menu cursor, the hover dot

- **Clicks and wheel today:** GCMouse handlers → `SDL_SendMouseButton` / `SDL_SendMouseWheel` →
  `dinput_handle_event` (wheel × 120). SDL skips its UITouch indirect-pointer path when a GCMouse exists, so
  there are no double clicks. Probably gated like the deltas; step 1 counts them.
- **Clicks with the pointer off every window (immersive):** take `indirectPinch` and `pointer` spatial events from
  `LayerRenderer.onSpatialEvent`, press on `.active`, release on `.ended`/`.cancelled`, and drop one that falls
  within 0.25 s of a GCMouse press (mc-avp's `Mouse.clicked(near:)`). A hand pinch arrives the same way; I'd let
  it be a left click (attack / select) and say so in the PR.
- **Menu cursor:** Skyrim draws its own cursor from relative deltas. With lock (design L) the system hides its
  pointer on iPadOS; on visionOS that is a guess. Without lock, don't try to hide the dot: **keep the game cursor
  under it**. While `skyrim_menu_open()` and the pointer is over the game view, steer the game cursor to the
  pointer's position with injected deltas (home to a corner with one large negative delta, then move by the
  target; or read the cursor position from the menu manager if it's easy to find). Counts-per-pixel in menus has
  to be measured on the Mac. In immersive the menu is a head-following panel, not the window, so there the
  cursor stays relative; mapping the dot onto the panel needs tracking areas and can wait.
- **Releases:** on `UIScene.willDeactivate` / `didEnterBackground` release every held button and clear the edge
  state (mc-avp f681d3ed). The system takes the pointer without sending the release.

## 3. Windows during play

- **Launcher window:** dismiss it while the game runs, reopen it when the game ends. It only covers the game.
- **Game window:** its own `WindowGroup(id: "game")`, with `GameSceneReporter` moved there, so SDL's UIWindow no
  longer shares a scene with the launcher's UI. Risk to check first: `runtime/platform_sdl.c` suspends the game
  while SDL is in the background, and SDL's lifecycle now follows scene notifications from the overlay
  (`SDLUIKitSceneDelegate`). Dismissing or hiding the launcher scene must not look like "app in background"; key
  it on the game scene only.
- **In immersive:** keep the game scene alive (it is SDL's window, the keyboard focus, and the lock holder if
  lock works). Design L: it can be small and anywhere. Design S: make it the pointer surface: plain style, no
  glass, the Metal view hidden but hit-testable, placed in front by the user once; visionOS moves the pointer to
  where the wearer looks when the trackpad is touched, so a surface in front is re-acquired on each touch.
  Don't dismiss the game scene until spatial clicks and (if it works) tracking areas make it unnecessary.

## 4. Steps

Local checks first; headset runs are marked **[headset]** and each needs the wearer's OK.

1. **Diagnostics, no behaviour change.** `ipad/visionos/sdl/SDL_uikit_xros.m` (`skyrim_xr_pointer_edge` log) and
   `simulator/prepare_sdl.py` (button/scroll handlers call a counter like `SDLXR_MouseDelta`). Add to the
   10-second "pointer:" line: `pointerLockState` nil or not; game scene activation state; whether the scene's key
   window is SDL's window, and the class of the key window's root controller; SDL controller's
   `prefersPointerLocked`; `GCMouse.mice.count`; clicks and scroll over/off window; whether the immersive space
   is open. Log `UIPointerLockState.didChangeNotification`, `GCMouseDidConnect/Disconnect/BecomeCurrent`, and
   scene activate/deactivate with the role. In `apple/Sources/App/ImmersiveScreen.swift` set
   `layer.onSpatialEvent` to log kind and phase (counts, not positions).
   *Check:* visionOS simulator build runs and prints the new line (the simulator can send the Mac's pointer to
   the device; whether it honours lock is unknown, don't rely on it).
2. **Lock variants behind `LB_POINTER_LOCK`** (same build as step 1):
   - `1` (default): as now.
   - `key`: when relative mode turns on and whenever the game scene activates, `makeKeyWindow` on SDL's window,
     then `setNeedsUpdateOfPrefersPointerLocked`. File: `prepare_sdl.py` patch to `SDL_uikitwindow.m` /
     `UIKit_UpdatePointerLock`.
   - `scene`: the game in its own scene (step 4) so the only other window is an empty SwiftUI host.
   - `host`: in that scene, the SwiftUI host's root controller forwards the preference: a
     `UIHostingController` subclass used as the scene's root through a UIKit scene delegate
     (`UIApplicationDelegateAdaptor` + `configurationForConnecting`), overriding
     `childViewControllerForPointerLock`/`prefersPointerLocked` to follow `SDL_GetRelativeMouseMode()`.
     Only build this one if `key` and `scene` are cheap and you want all three in one run; otherwise it's run 2.
   *Check:* simulator: the log shows which controller answers and that `prefersPointerLocked` is 1.
3. **[headset] Run 1: diagnostics and lock variants.** One install. The wearer plays windowed and immersive for a
   minute each per variant; variants switch by copying a new `launch-env.json` with `devicectl`, no reinstall.
   Read from the logs: is `locked 1` ever seen (and in which variant, windowed or immersive); are clicks and
   scroll gated like deltas; do deltas arrive in immersive with the pointer off the window; which spatial events
   reach the layer. This run decides L or S.
4. **Game in its own scene; launcher dismissed during play.** `apple/Sources/App/SkyrimLauncherApp.swift`
   (second `WindowGroup(id: "game")`, `GameSceneReporter` moved), `apple/Sources/Views/LibraryView.swift`
   (`openWindow(id: "game")` before `model.play()`, `dismissWindow` for the launcher, reopen on game end),
   `ipad/visionos/sdl/SDL_uikit_xros.m` if `SDLXR_ActiveScene` needs to wait for the scene. Check the
   background-suspend risk in section 3.
   *Check:* simulator: Play opens the game window, the launcher goes away, quitting the game brings it back;
   log shows "game window attached to the registered game scene". Needed by both designs; can ship in run 1.
5. **Button and edge release on deactivate.** `runtime/platform_sdl.c` or the overlay: on scene
   deactivate/background push button-up events for held buttons and call `SDLXR_PointerLeft`.
   *Check:* Mac/simulator: hold a button, deactivate the scene, log shows the release.
6. **Design L (if run 1 shows `locked 1`):** make the granting variant the default; `edge_turn` already turns
   itself off when locked. Menus need nothing more if the dot is hidden while locked; if it isn't, add step 8.
   *Check* **[headset] run 2:** "locked 1" throughout, "edge turn 0 frames", the wearer reports no lost trackpad.
7. **Design S (if never locked):**
   a. Immersive pointer surface as in section 3 (`SkyrimLauncherApp.swift`: `.windowStyle(.plain)` for the game
      scene while immersive; the overlay hides the Metal layer but keeps the view hit-testable). Keep
      `LB_EDGE_TURN`.
   b. Spatial clicks: `ImmersiveScreen.swift` `onSpatialEvent` → a C entry in `runtime/platform_sdl.c` that pushes
      `SDL_MOUSEBUTTONDOWN/UP`, with the 0.25 s dedupe against GCMouse presses.
   c. Tracking-area probe behind `LB_TRACK_AREA=1`: one tracking area covering the whole drawable
      (`ImmersiveScreen.swift`, configuration `trackingAreasFormat`, write one id into
      `trackingAreasTextures`). The log from step 1 then says whether deltas arrive with no window under the
      pointer. If yes, the pointer surface window can go.
   *Check:* simulator for a, b (spatial events can be produced there); **[headset] run 2** for all three,
   `LB_TRACK_AREA` toggled by `launch-env.json`.
8. **Menu cursor follows the dot (windowed, when not locked).** `runtime/platform_sdl.c`: new function next to
   `edge_turn`, using `skyrim_menu_open()`, the pointer position from the overlay (export x and y, today only x)
   and `dinput_inject_mouse`. Calibrate counts per pixel on the Mac build with `LB_SHOT` screenshots.
   *Check:* Mac: scripted pointer positions, cursor lands on them within a few pixels. **[headset]** only as
   part of run 2.

Expected headset cost: two runs. Run 1 = steps 1, 2, 4, 5. Run 2 = step 6, or steps 7 and 8.

## Head tracking: what must stay true

- Every real-motion path, old or new, must arrive as an `SDL_MOUSEMOTION` with `which != SDL_TOUCH_MOUSEID`, so
  `head_note_event` sees it and re-anchors. `edge_turn` already does this; spatial clicks are buttons and don't.
- Head steering and the menu-cursor sync (step 8) inject through `dinput_inject_mouse`, which bypasses SDL
  events, so neither triggers the takeover. Step 8 runs only in menus, where `head_steer` is already off.
- Pointer jumps (the system moving the pointer to the gaze) are absolute moves; in relative mode SDL doesn't
  turn them into motion (`regionForRequest` is guarded), so they can't steal the view from the head. Keep it so.
- Possible refinement, not required: ignore motion of 1 count for the takeover, so a resting finger doesn't
  hold head steering off for 15 frames at a time.
- With a gamepad connected none of this matters for look (the game is in gamepad mode); it matters for
  keyboard-and-trackpad play and for menus.

## Not checked

- Apple's online documentation: I read SDK headers and interfaces only, no web pages.
- Whether `SDLUIKitDelegate`'s `configurationForConnectingSceneSession` from the overlay is the one in effect in
  the SwiftUI-lifecycle launcher; step 2's `host` variant depends on who owns scene configuration.
- SwiftUI `pointerStyle` availability on visionOS (I checked `pointerVisibility` only).
