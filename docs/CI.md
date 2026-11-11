# Continuous integration

Two workflows, split by whether they need game-derived code.

| Workflow | Runs on | Needs game files | Triggered by |
|---|---|---|---|
| `CI` (`.github/workflows/ci.yml`) | GitHub-hosted `macos-26` (Apple Silicon) | No | pull requests and pushes to `main` |
| `Full build (private runner)` (`full-build.yml`) | the self-hosted M5 (`skyrim-private`) | Yes, on that Mac only | pushes to this repository, manual runs |

## CI (hosted, game-free)

`macos/bootstrap.sh`, `macos/build.sh` (runtime without the game), the CTest suite (runtime, audio and graphics
smoke tests, AOT and JITless fixtures, Python tooling tests), and `ipad/tests/test_public_build.py`. Pinned
downloads (MoltenVK, FFmpeg, the DirectX redistributable) are cached between runs.

## Full build (private runner)

Generates `gen/` from the runner's own game installation, then builds the macOS game with its tests, the visionOS
engine and the unsigned visionOS app. Generation takes 20–40 minutes and is reused until the lifter or its
Python inputs change; builds after that take about 10 minutes.

Rules that keep it private and safe on a public repository:

- **No `pull_request` trigger.** Only pushes to this repository (including your own PR branches) and manual runs
  reach the runner, so code from someone else's fork never runs there.
- **Nothing leaves the Mac.** No artifacts are uploaded: the engine and app contain compiled game code. Only the
  generated-code key and output hashes go into the job summary. Logs are public, so build steps must not print
  game paths beyond the runner's own folder names, or file contents.
- **Non-admin user.** The runner runs as the dedicated `mac-ci` user and writes only to `~/actions-runner` and
  `~/skyrim-ci`.
- **No stored GitHub credentials.** Registration uses a one-time token you paste yourself; the runner then holds
  only its own runner credential.

### Part-time availability

The M5 isn't on all the time. Jobs wait in the queue until it connects and start then; GitHub drops a job that
has waited 24 hours, and a newer push to the same branch cancels the older queued job. To run CI, start
`./run.sh` (below); to stop, press Ctrl-C. A job that is cut off midway just fails and can be re-run.

### One-time setup on the M5

1. Log in as `mac-ci` (non-admin). Full Xcode with the visionOS SDK and Python 3.10+ must already be installed.
2. Copy the game installation there (or onto its disk image) with `TESV_unpacked.exe`, `d3dx9_42.dll` and
   `X3DAudio1_7.dll` beside `TESV.exe`.
3. Clone this repository and run `tools/ci/setup-m5-runner.sh /path/to/Skyrim`. It checks everything, installs
   cmake, ninja and the Actions runner (checksum-verified) into `mac-ci`'s home, and prints the two commands left.
4. Register: open **Settings → Actions → Runners → New self-hosted runner** in the repository, copy the token,
   and run the printed `./config.sh … --token <token>` command.
5. Start it with `cd ~/actions-runner && ./run.sh` whenever the Mac is free and on AC power.

### Repository settings to check once

- **Settings → Actions → General → Fork pull request workflows:** "Require approval for all outside
  collaborators". This only affects the hosted `CI` workflow, which can't reach the M5 anyway.
- **Settings → Actions → Runners:** the M5 should be the only runner, labelled `skyrim-private`.
