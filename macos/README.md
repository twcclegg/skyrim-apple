> **Historical development reference.** The current public build targets iOS/iPadOS and has physical iPad Pro acceptance. See the [root README](../README.md) and the [current build guide](../docs/BUILDING.md). Status and launcher holds below describe the earlier prototype, not current iPad support.

# JITless Windows Skyrim on Apple Silicon

This private prototype ports the original 32-bit Windows Skyrim runtime to
native ARM64 macOS. The Windows executable and three required libraries are
translated to C on the build computer and compiled into the app. Runtime calls
resolve to compiled native functions; an unknown code address aborts instead of
falling back to a CPU interpreter or JIT. Graphics drivers compile GPU shaders
normally, without requiring a JIT entitlement for guest CPU code.

The starting source is `ttom795/Skyrim_Android` at
`0819237bac51dc6f68c1a299c30359376ba0331d`. Its code matches the audited
`30b6b87d7ac0aefcd5daddb8dd3852b1962420d4` version; the intervening upstream
change only added an image. Upstream tip `ceb35d4` adds no core source change
since the pin (`macos/evidence/upstream-audit-ceb35d4.md`).

## Game status (2026-11-01)

The original Steam Skyrim 1.9.32.0.8, prepared from the owner's own Steam
depots, boots natively on this Apple Silicon Mac and in the dedicated iPad
Simulator, with no guest CPU JIT or interpreter. The prototype objective (the
original game natively and JITless on this Mac, then in the dedicated iPad
Simulator, with world rendering, live input, save/reload and audio output) is
**verified**: the coordinator's review of the final artifacts on 2026-11-01.
The items under [Not yet verified](#not-yet-verified) are follow-ups outside
that objective.

Evidence is private and git-ignored. The timeline, milestones N1–N22 with UTC
times, is `macos/evidence/native-game-handoff.md`. Logs, receipts and proofs
named below are in `macos/evidence/` unless another folder is given. Frame
captures of the owned game stay private in `macos/evidence/shots/`. "CUA"
below means real OS input events sent by the coordinator agent's ("root")
computer-use session, not by a person; no human play is recorded.

### Verified on the Mac

| Result | When (UTC) | Evidence |
|---|---|---|
| Native JITless boot in a 1280×720 window | first boot 11:29:43Z; 1280×720 after the INI fix | `boot-*.log`, `shots/boot*/` (N5) |
| Main menu, new game, opening cart scene and world rendering | boot 8, 11:56:21Z | `boot-20261101T045621-depthfix.log` (N5) |
| Console `save`, then `load` in a fresh process: Riverwood at the saved viewpoint, mean frame difference 4.39/255 | save 12:08:33Z, reload 12:17:26Z | `boot-20261101T050833-coc-save.log`, `boot-20261101T051726-load-kb.log` (N7) |
| Real OS keyboard and mouse on the `d025944` dev .app, sent by the coordinator's computer-use (CUA) session: menus, W/S movement, mouse look, F5 quicksave, D strafe, F9 restore to the quicksave viewpoint. The report records no executable hash (the dev .app then held `be47ad4b…`, N8); it predates the `5c52657` input changes and is superseded by the final check below | 12:37Z | `cua-host-input-20261101.md`, `cua-input-20261101.log` (N9) |
| Final check of the `5c52657` build before the audio fix (dev .app, `skyrim_macos` SHA-256 `832f8277…`, matching the sealed receipt), real OS input from the CUA session with only `LB_LOG=1` and `LB_MOUSE_CAPTURE=0` (pointer capture off): CONTINUE and the "Enter Yes / Tab No" legend, held-key movement and mouse look (proof flags `live_held_key_movement_verified`, `live_mouse_look_verified`), F5 wrote a new `quicksave.ess` (3,156,672 bytes, SHA-256 `8a549994…`), A held 1.6 s moved into branches and F9 restored the saved bridge/rock view. Log: `input: virtual pad off; 0 controller(s) attached at launch`. Its audio went silent after root's vmmap snapshot (log line 1058), the defect fixed below; the fixed build's final check is further down this table. Fresh guest memory proof: 4.00 GiB in 16 regions, `---` or `rw-` only. `mactest.ess` unchanged; the previous quicksave is kept in `cua-mac-final-20261101-save-backups/` | PID 61677, 14:35:40Z–14:38:11Z | `cua-mac-final-20261101-proof.json`, `cua-mac-final-20261101.log`, `cua-mac-final-20261101-vmmap.txt` (N15) |
| Audio: a non-silent mix reaches an open SDL output device (menu music, Riverwood ambience); not heard by a person. Measured on a working-tree build before `0c646bb` (binary hash not recorded). In the `5c52657` run (PID 61677) the output device was closed for good after about 100 s (`cua-mac-final-20261101.log` line 1058), fixed in the next row | boot 15, 12:20Z | `boot-20261101T052021-audio.log` with `LB_AUDIO_METER=1` (N7) |
| Audio output-device recovery. Cause: any process suspension over 200 ms (vmmap, a debugger, `SIGSTOP`) looked like a dead device, which was closed and never reopened. Reproduced on the pre-fix binary `832f8277…` with a 1 s `SIGSTOP` and with vmmap alone: every later meter window read `(no device)`. Fixed in the shared `runtime/xaudio2.c`: stall time counts only the engine's own polling (each gap capped at 20 ms), and a real stall closes the device, keeps mixing on the 10 ms timer and reopens after 500 ms (doubling to 8 s, 2 s on iOS). Live on the fixed game: a 1 s `SIGSTOP`, vmmap and a 3 s `SIGSTOP` left the device open with non-zero levels in every window; a forced real stall through CoreAudio (interim build `54a3432c…`) logged stalled, then reopened 500 ms later. `audio_device_recovery` fails 3 of 3 runs on old-equivalent logic and passes on the fix, with the dummy driver and on the host's real CoreAudio | 14:54–15:35Z | N18; `boot-20261101T145439Z-audio-stall-repro-prefix.log`, `boot-20261101T145851Z-audio-stall-repro-prefix-vmmap.log`, `boot-20261101T150436Z-audio-stall-postfix.log`, `boot-20261101T151939Z-audio-recovery-live-mac.log`, `audio-recovery-harness-coreaudio-20261101T1530Z.txt` |
| Final check of the fixed build (`b44c19b` dev .app, `skyrim_macos` SHA-256 `78e0ac17…`), real OS input from the CUA session with only `LB_LOG=1`, `LB_AUDIO_METER=1` and `LB_SHOT` frames, default pointer capture on (`mouse: captured for the game window`), copied saves: the "Enter Yes / Tab No" legend, held-key movement, F5 wrote `quicksave.ess` (3,164,687 bytes, SHA-256 `bd4e77b7…`), A held 0.8 s moved to an open-road/bridge view and F9 restored the saved bridge/rock view. Audio: 78 meter windows all `(device open)`, 77 non-zero, peak up to 0.279, still open and non-zero after root's vmmap, no stall. Guest memory 4.00 GiB in 16 regions, `---` or `rw-`. 16 runtime frame captures. User saves unchanged. Captured mouse look was not shown: 80 px and 700 px CUA drags moved the visible pointer without turning the camera; the next row explains why | PID 92316, 15:32:25Z–15:39:11Z | `cua-final-post-audio-20261101/mac-proof.json`, `mac-game.log`, `mac-vmmap.txt`, `mac-saves/shot-*.bmp` (N20) |
| Mouse diagnosis and mouse look on the fixed build. A scratch SDL probe on the game's own SDL 2.30.9 (`69b93815…`), mirroring `capture_mouse()`, received the same CUA drags: every raw AppKit move carried a zero native delta, so with capture on SDL delivered no motion (sum of |xrel| 0) and with capture off it derived motion from the absolute positions (sum of |xrel| 1240). This is a limitation of the computer-use tool, not the game. Then, on the same `78e0ac17…` build with `LB_MOUSE_CAPTURE=0` and no scripted input, a 700 px CUA drag panned the camera toward the rock and moved the compass (east marker about 625 → 567 px); the save file was unchanged. Captured-pointer mouse look with a physical mouse is untested | probe logs closed 15:54:03Z (captured) and 15:54:47Z (uncaptured); PID 99751, 15:56:46Z | `cua-final-post-audio-20261101/mouse-probe-verdict.json`, `mouse-captured.log`, `mouse-uncaptured.log`, `mac-mouse-proof.json`, `mac-mouse-game.log` (N22) |
| Live process: guest memory not executable. 4.00 GiB in 14 regions, current protection `---` (64 KiB null guard) or `rw-`; no writable and executable region; the binary has no entitlements. Boot 9's binary hash was not recorded; the current build `832f8277…` shows 16 regions, `---` or `rw-`, in `cua-mac-final-20261101-vmmap.txt` | boot 9; current build 14:37Z | `boot9-vmmap.txt`, `boot9-guest-memory-proof.txt` (N6), `cua-mac-final-20261101-vmmap.txt` (N15) |
| PCF shadow emulation builds and reloads Riverwood with 0 failed pipelines | boot 16, 12:39Z | `boot-20261101T123909Z-pcf.log` (N9) |
| Present pacing: 60.6 fps steady state | 13:27Z | `boot-20261101T132730Z-pacing.log` (N12) |
| `ctest` 20/20, including the new graphics tests `depth_compare_smoke` and `base_vertex_fold_smoke --compare`; 21/21 with `audio_device_recovery` after the audio fix | commits `66000c1` and `5c52657`; audio fix | `depth_compare_smoke-20261101T123838Z.log`; N9, N12, N13 |

### Verified in the iPad Simulator

Dedicated `Skyrim JITless iPad` simulator (iPad Pro 13-inch M5, iOS 27.0). See
the [Simulator notes](../simulator/README.md) for commands and details.

| Result | When (UTC) | Evidence |
|---|---|---|
| Game app build: all 490 steps compiled and linked from the same `gen/`; receipt bound to the generated manifest | 12:41–12:51Z, bundle fix 12:52Z | `simulator/evidence/full-game-build-handle.json`, `simulator/build/app-build-receipt.json` (N10) |
| World rendering: the opening cart scene in 3D at 1376×1032; Skyrim's own new-game autosave written in the container | sim boot 3, 13:00:49Z | `shots/sim-boot3/`, `sim-boot3-run.out` (N11) |
| Live process: guest memory not executable. 4.00 GiB in 18 regions, current protection `---` or `rw-`. The current build `0e3af478…` shows 17 regions, `---` or `rw-`, in both CUA sessions | 13:07:12Z; current build 14:20Z and 14:44Z | `sim-boot3-guest-memory-proof-root.json`, `sim-boot3-vmmap-root.txt`, `cua-simulator-20261101-vmmap.txt`, `cua-simulator-cold-20261101-vmmap.txt` |
| Helgen character creation, about 145 fps uncapped with discrete Metal bindings | sim boot 4, 13:07Z | `shots/sim-boot4-noargbuf/`, `sim-boot4-noargbuf-run.out` (N12) |
| Riverwood from an imported copy of the Mac console save; F5 wrote `quicksave.ess` | sim boot 5, 13:14Z | `sim-boot5-riverwood-run.out` (N12) |
| Scripted D strafe, then F9 restores the quicksave viewpoint; 60–65 fps with bridge pacing. Mean frame difference: idle 2.30, strafe 35.28, restored 4.41 and 3.34 (of 255) | sim boot 7, 13:29Z; proof 13:38:16Z | `sim-boot7-restore-proof-root.json`, `sim-boot7-strafe-run.out` (N12) |
| Live keyboard on `5c52657`, no scripted input: real OS key events from the CUA session through the Simulator viewer. Return/Enter drove CONTINUE and console commands; the confirmation legend read "Enter Yes / Tab No" (keyboard mode); repeated D presses moved the player; console `save cuasimlive`, D presses to a different view, then `load cuasimlive` restored the saved view. Guest memory 4.00 GiB in 17 regions, `---` or `rw-` | PID 50939, 13:53:19Z–14:28:34Z | `cua-simulator-20261101-proof.json`, `cua-simulator-20261101-vmmap.txt` (N14) |
| Cold relaunch of the `5c52657` build with `LB_LOG=1` and `LB_AUDIO_METER=1`, no scripted input: virtual pad off and the generic controller ignored. Real OS keys from the CUA session: Return/Enter confirmation, CONTINUE loaded the saved `cuasimlive` view, 80 D taps moved to a bridge-centred view, console `load cuasimlive` restored the saved view in this new process (save file unchanged). A non-zero game mix reached an open SDL output device: 50 of 51 samples non-zero, max peak 0.296, max RMS 0.046; nobody listened. Guest memory 4.00 GiB in 17 regions, `---` or `rw-` | PID 70292, 14:39:57Z–14:44:38Z | `cua-simulator-cold-20261101-proof.json`, `cua-simulator-cold-20261101-vmmap.txt`, `simulator/evidence/game-launch-20261101T143957.538890Z.json` (N16, N17) |
| Final no-script session on the fixed build (executable `6b805108…`, matching the launch receipt), `LB_LOG=1`, `LB_AUDIO_METER=1`, `LB_SHOT` frames: CUA keyboard drove CONTINUE to Riverwood, console `save cuasimpostaudio` (3,122,746 bytes, SHA-256 `86e28e6e…`), D moved to a bridge-centred view and `load cuasimpostaudio` restored the saved view; the four earlier saves were unchanged. Audio: 71 windows all `(device open)`, 70 non-zero, peak up to 0.374, open after vmmap, no stall. Guest memory 4.00 GiB in 17 regions, `---` or `rw-`. Frame captures and `sim-world.png` (frame 18000) archived | PID 94615, 15:41:02Z–15:47:00Z | `cua-final-post-audio-20261101/sim-proof.json`, `sim-vmmap.txt`, `sim-shots/`, `sim-saves/` (N21) |
| Audio output-device recovery on the fixed build (`b44c19b`, executable `6b805108…`), `LB_LOG=1` and `LB_AUDIO_METER=1`, no scripted input: through a 1 s `SIGSTOP`, vmmap and a 3 s `SIGSTOP` of the game process, every 5 s meter window read `(device open)` (24 of 25 non-zero, peak up to 0.251), no stall logged; virtual pad off, generic controller ignored; saves unchanged. Nobody listened. The DXVK framework was relinked with byte-identical `__text` (only linker stubs, UUID and signature differ) | PID 90827, 15:28:54Z–15:31:06Z | `simulator/evidence/game-launch-20261101T152854.027766Z.json`, `sim-audio-recovery-20261101T152850Z.events` (N19) |

### Not yet verified

- **Hotkeys and held keys in the Simulator.** In the live check through the
  Simulator viewer, F5 did not write a quicksave, Escape did not open the pause
  menu and Tab moved focus to the viewer's controls; F9 and a continuous key
  hold were not checked. Why F5 and Escape did not act is not determined
  (scripted F5 wrote a quicksave in sim boot 5). The live save and load used the
  game console.
- **Mouse in the Simulator.** Clicks in the Simulator viewer arrive as direct
  touches, which the game ignores by design, and the iPad pointer (GCMouse)
  path has not been exercised there. Real mouse input is verified only on the Mac.
- **DualSense.** Untested on both platforms. The PS5 code path is covered only
  by SDL virtual-device IDs in `desktop_platform`.
- **Hands-on play.** Comfort over a human play session, audio judged by ear and
  a long soak are untested on both platforms. Most runs last a few minutes;
  the longest, Simulator PID 50939, ran about 35 minutes (13:53:19Z–14:28:34Z,
  frame 125,700). Shadow quality has not been judged visually.
- **Mouse look with the captured pointer and a physical mouse (Mac).** Mouse
  look is verified on the current build with capture off. With capture on (the
  default), the computer-use tool cannot test it: its injected moves carry zero
  native deltas, which SDL's relative mode never turns into motion (probe
  above). No physical mouse has been used.
- **Audio recovery limits.** Recovery from a real device loss, Bluetooth or
  AirPlay output, a default-device change, sleep/wake and iOS audio-session
  interruptions have not been exercised; the forced stall paused SDL's device
  rather than failing the hardware.
- **Screenshots of the CUA checks.** None was saved from PIDs 61677, 50939 or
  70292; those restores are recorded as direct observations in the proofs. The
  numeric reload proofs (Mac N7, Simulator boot 7) used scripted input.
- **Physical iPad.** Untested; Simulator results do not establish it.
- **Triangle fans on the Simulator.** None has been logged failing, but the
  Simulator's MoltenVK lacks indirect drawing, which it uses for fans
  (`macos/evidence/base-vertex-fold-review.md`, finding 4).

### Game-free checks

- All 20 genuine C runtime files and five C++ bridge/UI files compile as ARM64.
- The complete runtime links with locally built SDL 2.30.9, patched DXVK D3D9
  and checksum-pinned MoltenVK 1.4.2. No Homebrew SDL runtime is required by the
  default build.
- The real D3D9 backend draws a red clear and green triangle on the GPU through
  MoltenVK and Metal; readback verifies the resulting pixels.
  `depth_compare_smoke` checks the Apple shadow-compare path (PCF ramp,
  projected lookups, point step, reference clamp). `base_vertex_fold_smoke
  --compare` draws natively and with the Simulator's base-vertex fold forced
  and requires identical pixels.
- Authored PE32 test instructions pass actual recursive discovery, C generation,
  native compilation and expected-result checks. These include calls, integer
  flags, unaligned memory, SSE, x87, atomic operations and Windows FS-relative
  addressing. Missing code and unsupported instructions fail closed.
- Runtime tests cover compiled dispatch, guest heap/VirtualAlloc reuse, file
  access with Windows-style paths and guest threads. Darwin-specific tests
  cover thread identity, nanosecond timestamps, signal profiling and CPU counts.
  `desktop_platform` (499 checks) covers controller hot-plug under concurrent
  XInput polling and the DualSense-only pad filter. `audio_device_recovery`
  runs the real XAudio2 shim on SDL's dummy output: a suspended process keeps
  its device; a device that stops draining is closed, mixed on the timer and
  reopened with a bounded queue and doubling backoff.
  `AUDIO_RECOVERY_DRIVER=coreaudio` runs it on the host's real output (silent).
- Native WMAv2/WMAPro decoding passes public Microsoft and FFmpeg samples,
  including streamed packets and decoder reset. Real XAudio COM/mixer tests
  cover WMA, PCM, ADPCM, loops, stopped/active flush behavior and callbacks on
  the audio processing thread. These tests use a dummy output device.
- Hardened-runtime copies of the AOT tests run with no entitlements. A copied
  full-runtime test requires only the local third-party library-validation
  exception; it has no JIT, debugger or executable-memory entitlement. The whole
  4 GiB guest reservation is checked for non-executable data mappings, including
  after release and reuse.

## Build without game files

Use an Apple Silicon Mac with Xcode, Python 3.10+, CMake and Ninja available.
The current local build was exercised on macOS 27 with the Xcode 27 SDK. The
build requests macOS 13 deployment, but running on older macOS is unverified.
Bootstrap creates a local Python environment, fetches pinned public graphics,
DirectX and codec dependencies, builds private native audio libraries and fetches
small public codec test samples. It does not install global packages or download games.

From the checkout root:

```sh
macos/bootstrap.sh
macos/build.sh
ctest --test-dir macos/build/native --output-on-failure
macos/.venv/bin/python tests/jitless/run.py --build-mapping-probe
```

The default build does not make a fake `skyrim_macos` executable. Its products
are the real runtime objects, SDL/DXVK libraries and explicit test executables.
The GPU tests need a logged-in macOS desktop. Compilation and shader workers
default to two to avoid occupying the host with unrestricted parallel builds.

## Launchers (paused)

Launcher work is paused by the owner. The [single-title macOS
launcher](launcher/README.md) and the shared SwiftUI launcher in `apple/` have
had no launcher, phone or device testing
since 2026-11-01T10:48Z (N1), and none of the game results above went through
them. Play from a launcher has not started the game. The one launcher piece in
use is the command-line bridge, `macos/launcher/bridge.py`, whose `status` and
`prepare` commands ran the real preparation below.

The macOS launcher can still be built and opened without game files:

```sh
sh macos/launcher/build.sh
open 'macos/build/launcher/Skyrim Launcher.app'
```

It uses macOS 14+ SwiftUI shader effects.

## Prepare the owned Windows installation

The upstream-compatible input is the original Steam game, patch 1.9.32.0.8.
Special Edition and Anniversary Edition are not supported by this lifter.
Keep the original installation intact and use a separate private input folder
containing:

- `TESV.exe`: original executable, upstream SHA-1 prefix `f39aeceb`.
- `TESV_unpacked.exe`: the unpacked copy. Two exact identities are accepted:
  upstream's prefix `fed96638`, and the full SHA-1
  `17ec1ef6d72781359860f103f600711dfc219182` that the pinned Steamless v3.1.0.5
  produces from the `f39aeceb` original (see below).
- `d3dx9_42.dll` and `X3DAudio1_7.dll`: 32-bit DirectX June 2010 libraries.
- `binkw32.dll`: the original game's video library.
- The complete `Data/` directory, including `Skyrim.esm`, `Update.esm` and BSA
  archives. DLC and mods remain unverified. With real assets the mix is
  non-silent at the main menu and in Riverwood (`LB_AUDIO_METER`), but music and
  voices have not been judged by ear.
- The original `low.ini`, when present, is preserved in the private view for
  first-run graphics preferences. It is optional; preparation does not invent
  a replacement preset.

Executable prefixes are upstream compatibility checks, not a complete known
SHA-256 allowlist. Preparation records the full SHA-256 of the actual input
modules, essential ESM files, recompiler tools and every generated source file.
No game files, translated game code or Microsoft library code should be tracked
or published. Upstream's own runtime/lifter license grant also needs clarification
before distributing a derivative; this checkout remains a private prototype.

### Reproducible preparation from Steam depots

This is the route used on 2026-11-01 (N1–N4). Steam's console downloads each
depot into its own folder, `<content>/depot_<id>`, where `<content>` is Steam's
`steamapps/content/app_72850` folder. In the Steam console, with the original
Skyrim (app 72850) owned, enter each command on its own line. Two commands on
one line failed with `Access Denied`.

```text
download_depot 72850 72851
download_depot 72850 72853
download_depot 72850 72852
```

| Depot | Contents | Files | Bytes | Manifest downloaded |
|---|---|---|---|---|
| 72851 | Base game: `Data/` ESMs and eight BSAs, `binkw32.dll`, `low.ini` | 168 | 4,534,329,853 | `430694959351693705` |
| 72853 | English: `Data/Strings`, interface translation, voice BSAs | 12 | 1,479,202,547 | `5477471785942614203` |
| 72852 | `TESV.exe` only | 1 | 18,024,240 | `5176728229505148062` |

Steam's console also accepts a manifest ID as a third argument, for example
`download_depot 72850 72851 430694959351693705`, to request the same content
later. That pinned form was not exercised here.

Merge the three folders into one private view. The view holds real directories
and per-file links, matches paths case-insensitively, refuses links inside depots
and conflicting files, never modifies a depot, keeps any existing output and
writes `depot-view.json` (181 links here). Then check and prepare:

```sh
macos/.venv/bin/python macos/merge_depots.py <content>/depot_72851 <content>/depot_72853 <content>/depot_72852
macos/.venv/bin/python macos/launcher/bridge.py status --game-dir macos/build/steam-72850-view
macos/.venv/bin/python macos/launcher/bridge.py prepare --game-dir macos/build/steam-72850-view
```

The view defaults to `macos/build/steam-72850-view` (`--output` changes it).
`status` reported "Ready to prepare". `prepare` fetches the DirectX libraries
when needed, builds the Steamless converter, unpacks a copy of `TESV.exe`,
stages a private view under `macos/build/launcher/inputs/<id>`, discovers and
lifts all four modules into `gen/`, runs `macos/build.sh --game` with one job
and verifies the sealed result. Its JSON reports `stagedDirectory`, the view
used by `macos/run.sh` and `simulator/run_game.py`; `bridge.py status` reports
it again later.

Executable identities. The packed `TESV.exe` from depot 72852 has SHA-1
`f39aecebf6e332616c4c518617073f6e8250e0f8` (SHA-256
`69362f77bc56bf17570a33106d020b6c67fd8f32a294b82f8b687b9cf71efc72`), so it
passes the unchanged packed gate. Steamless v3.1.0.5 unpacks it to SHA-1
`17ec1ef6d72781359860f103f600711dfc219182`, not upstream's `fed96638`.
`fed96638` is upstream's own unpacked copy; upstream's launcher notes that
Steamless output differs between Steamless versions. The v3.1.0.5 default,
`--keepbind` and `--keepstub` outputs have identical mapped section hashes and
the same entry point, so commit `cb9227f` accepts that exact full SHA-1 as a
second identity. Details are in the [executable notes](executable/README.md).

Measured on this Mac (N3, N4): `prepare` started at 11:03:30Z. Unpacking,
staging, and discovery and lifting of all four modules finished when `gen/` was
published at 11:14:31Z, about 11 minutes (353 MB, 140 `.c` files). The one-job
game build linked about 13 minutes later, around 11:28Z.

| Module | Discovered | Lifted | Rejected |
|---|---|---|---|
| `TESV_unpacked.exe` | 90,900 | 90,390 | 510 |
| `d3dx9_42.dll` | 5,913 | 5,682 | 231 |
| `binkw32.dll` | 503 | 502 | 1 |
| `X3DAudio1_7.dll` | 86 | 79 | 7 |

TESV's rejects are almost all junk-decode signatures (data decoded as code);
every rejected function fails closed if reached. The depots take about 6 GB and `gen/` about 0.35 GB; the views are links.

### Manual route

With an already assembled private input folder:

```sh
macos/.venv/bin/python macos/inputs.py check /absolute/path/to/private/game
macos/.venv/bin/python macos/prepare_game.py /absolute/path/to/private/game
macos/build.sh --game
macos/run.sh /absolute/path/to/private/game /absolute/path/to/separate/saves
```

A private native Mac [executable preparation helper](executable/README.md)
preserves the original and uses a separate copy:

```sh
python3 macos/executable/unpack.py /absolute/path/to/owned/TESV.exe
```

This build-time tool checks the expected original and unpacked versions. It
unpacked the owned depot `TESV.exe` on 2026-11-01 (receipt
`macos/build/steamless/unpacked/<id>/receipt.json`, status `verified`, original
preserved). The conversion tool is separate from the compiled game's JITless
runtime.

The two DirectX libraries can be prepared independently from Microsoft's
official public redistributable, without running a Windows installer:

```sh
macos/.venv/bin/python macos/directx/fetch.py
```

To avoid changing the original installation, a helper can make a normalized
private view using links to the owned files and those DirectX libraries:

```sh
macos/.venv/bin/python macos/stage_game.py /absolute/path/to/original/install \
    --unpacked-exe /absolute/path/to/TESV_unpacked.exe
macos/.venv/bin/python macos/prepare_game.py macos/build/owned-inputs
```

The staging helper uses the unpacked path returned above; it does not acquire
missing game files or copy assets into source control. It preserves existing
output directories.

Generation runs discovery and lifting sequentially into private staging. It
publishes `gen/` only after input and tool hashes remain stable. An existing
`gen/` is preserved rather than overwritten. Upstream estimates hours for
discovery; on this Mac the whole generation took about 11 minutes (above).

The real-game target refuses missing or unbound generated code. The build
captures source/config identity before compiling and seals the native binary
and its graphics libraries after linking. Launch rejects stale inputs, generated
code, source/config state, binary or libraries. Saves and shader caches go into
the separate save directory.

## Run the native game

On the Mac, directly, as in the N5–N7 boots:

```sh
macos/run.sh <staged view> <saves>
```

`<staged view>` is the `stagedDirectory` reported by `bridge.py status`.
`<saves>` is a separate folder; the bridge's default is
`~/Library/Application Support/Skyrim Jitless/Saves`. Click the window once to
give it focus; the pointer is then captured, and Cmd-Tab releases it. Quit with
Cmd-Q or from the game menu. With a DualSense paired the game should switch to
its gamepad interface; that is not yet verified (see the input policy below).

A development .app (commit `d025944`) lets LaunchServices and UI automation
identify the real game window. It holds a byte-identical copy of the sealed
`skyrim_macos` with `run.sh`'s environment. Pass absolute paths after `--args`,
and repackage after a game rebuild (`--check` fails when the copy no longer
matches the sealed binary):

```sh
macos/package_dev_app.sh <staged view> <saves>
macos/package_dev_app.sh --check
open -n "macos/build/dev-app/Skyrim Native (Dev).app" --args <staged view> <saves>
```

For the iPad Simulator see [the Simulator notes](../simulator/README.md):
`sh simulator/build.sh --game --audio --jobs 2`, then
`simulator/run_game.py --preflight-only` and `simulator/run_game.py --env …`.

### Input policy (all Apple targets)

Since commit `5c52657`, macOS, iOS/iPadOS and the iPad Simulator share one
input policy:

- The game reads DirectInput keyboard and mouse, fed from SDL events.
- Touches never become mouse input.
- The virtual Xbox pad exists only with `LB_KEYS`, `LB_KEYFIFO` or `LB_TOUCH`;
  `LB_VPAD=0` turns it off.
- Only a DualSense (SDL controller type PS5) becomes an XInput pad. Other
  controllers, including the Simulator's own generic "Gamepad", are logged and
  ignored; `LB_ANY_PAD=1` admits them for development.
- Launch logs the input mode: `input: virtual pad on|off; N controller(s)
  attached at launch`, then for each controller either `XInput pad N is <name>
  (type T)` or `ignoring controller <name> (type T; ...)`.

Without a DualSense the game keeps its PC keyboard-and-mouse interface.

### Verification switches

Each is off unless set, except the 60 fps pacing. On the Mac, set them in the
environment of `macos/run.sh`; in the Simulator, pass them with
`run_game.py --env KEY=VALUE`.

| Variable | Effect |
|---|---|
| `LB_LOG=1` | Runtime log level |
| `LB_SHOT=F[,F...]` | Write the presented backbuffer at those frame numbers as `shot-<frame>.bmp` in the save folder |
| `LB_KEYS=F:Key[:hold],...` | Scripted SDL key presses at frame `F` (SDL key names; hold defaults to 10 frames). Also enables the virtual Xbox pad |
| `LB_VPAD=0` | Scripted keys act as a plain keyboard, without the virtual pad (needed for console typing) |
| `LB_AUDIO_METER=1` | Log the mixed stereo peak and RMS every 5 s, and whether an SDL output device is open |
| `LB_MOUSE_CAPTURE=0` | macOS: leave the pointer free, for automated and CUA runs |
| `LB_ANY_PAD=1` | Development: admit controllers other than a DualSense as XInput pads |
| `LB_MAX_FPS=N` | Apple present pacing: at most `N` fps, default 60; `0` disables it |
| `DXVK_FORCE_BASE_VERTEX_FOLD=1` | macOS: force the Simulator's base-vertex fold, so tests can compare it with native drawing |

## Apple-specific fixes

Local commits on `apple-launcher`. DXVK and runtime changes are guarded for
Apple or the Simulator; the commit messages and reviews report Android/Linux
code unchanged.

- `404c099`: `macos/merge_depots.py` joins Steam's separate depot folders into
  one private link view. DXVK `d3d9.floatEmulation = True`, from upstream's 0.25
  release, stops shader NaNs from blackening HDR and depth-of-field passes, on
  the Mac and in the Simulator.
- `cb9227f`: the exact Steamless v3.1.0.5 unpacked identity (above).
- `1a2ad5a`, first Mac boots:
  - Main-thread Cocoa pump. AppKit pumps events only on the process main
    thread, but Skyrim presents from its loading-screen thread too. Other threads
    drain SDL's queue with `SDL_PeepEvents`; text-input toggles run on main.
  - INI section insert. `ini_set` appended new keys after the last section, so
    `iSize W/H` landed under `[Decals]` and the game fell back to 640×480. New
    keys now go inside their section, and the Mac seeds `bFull Screen=0`.
  - DXVK Apple depth compare. MoltenVK's Metal translation cannot express a
    compare sampler aliased on a colour binding (failed pipelines, missing menu
    text, a multi-minute Metal compile). Apple builds declare only the colour
    sampler and compare in the shader.
  - `LB_SHOT` captures.
- `12750e4`, macOS input: physical keys no longer also press virtual Xbox
  buttons, and pad 0 is the first real controller. Controller slots are
  mutex-protected. DirectInput keyboard and mouse state is tracked from SDL
  events. The pointer is captured, guest threads get 8 MiB host stacks, and
  `desktop_platform` was added.
- `d385b35` `LB_VPAD=0`, `0c646bb` `LB_AUDIO_METER`, `d025944` development .app.
- `d984112`, PCF emulation (review `macos/evidence/depth-patch-review.md`). The
  first compare was right for POINT shadow samplers but compared a filtered
  depth under LINEAR. Shadow samplers are now NEAREST, with the border colour
  widened from red, and the reference is clamped to [0,1]. A new Apple spec
  constant, `SamplerDepthFilter`, selects a 2×2 bilinear PCF of four point
  compares when the D3D9 filter is LINEAR. A standalone Metal probe matched
  hardware PCF within 0.0036. `depth_compare_smoke` was added.
- `c683424`, Simulator game target: 490 build steps, 8 MiB guest-thread stacks,
  `DXVK_LOG_PATH` in the save folder, the identity resource copied into the
  bundle before signing, and `run_game.py`.
- `8754029`, Simulator only:
  - Base-vertex fold. The Simulator's Metal cannot draw with a non-zero base
    vertex, so the base is folded into the per-vertex buffer offsets.
  - Multi-layer attachment support check. Without layered rendering, MoltenVK
    rejected Skyrim's RGBA16F cube render targets. D3D9 renders one face at a
    time, so such images are accepted when they pass as a single 2D layer.
  - Discrete Metal bindings by default
    (`MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0`; a launch environment value
    overrides it). Argument-buffer writes are serialized to the host GPU
    process and held the game at 8–13 fps.
- `66000c1`:
  - 60 fps bridge pacing. `runtime/d3d9_bridge.cpp` paces Apple presents after
    Present returns, with no lock held (`LB_MAX_FPS`). DXVK's
    `d3d9.maxFrameRate` stays off, and a config test asserts it: it sleeps
    inside Present while holding the D3D9 device lock. Skyrim presents its
    loading screen from one thread while the main thread loads, so every load
    call waited behind that sleep and Simulator startup crawled at about 4 fps.
  - CPU index rebase (review `macos/evidence/base-vertex-fold-review.md`).
    Legal negative base vertices that cannot be folded are rebased on the CPU
    and drawn with base 0, so an unsupported draw is never recorded. The fold
    can be forced on macOS with `DXVK_FORCE_BASE_VERTEX_FOLD=1`, and
    `base_vertex_fold_smoke --compare` shows 0 differing pixels.
- `5c52657`, shared Apple input policy (above). The iPad Simulator had kept an
  always-present virtual pad, so the game stayed in gamepad mode and physical
  keys also pressed pad buttons. iOS now uses the macOS policy, and only a
  DualSense becomes a pad on every Apple target, matching the launcher's input
  gate. The first automated keyboard-mode check logged
  `input: XInput pad 0 is Gamepad`, the Simulator's own controller, which would
  have held the game in gamepad mode. `desktop_platform` now has 499 checks.
- Audio output-device recovery (N18, above): `runtime/xaudio2.c` no longer
  closes the output for good after a process suspension or a stall; it retries
  SDL audio initialisation and the open with backoff, and logs stalls, reopens
  and failed reopens. New ctest `audio_device_recovery`.

## Remaining acceptance

All native Mac and dedicated Simulator checks in the tables above have recorded
results, and the coordinator's review of the final artifacts verified the
prototype objective on 2026-11-01. Real mouse input cannot be tested in the
Simulator viewer.

Follow-ups outside that objective, all unverified: captured-pointer mouse look
with a physical mouse, a person playing with a DualSense, audio judged by ear,
a long soak on both platforms, physical iPad compatibility, shipping and the
paused launchers.
