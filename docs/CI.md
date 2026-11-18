# Continuous integration

Two workflows, split by whether they need game-derived code.

| Workflow | Runs on | Needs game files | Triggered by |
|---|---|---|---|
| `CI` (`.github/workflows/ci.yml`) | GitHub-hosted `macos-26` (Apple Silicon) | No | pull requests and pushes to `main` |
| `Full build (private runner)` (`full-build.yml`) | your self-hosted Mac, labelled `skyrim-private` | Yes, on that Mac only | pushes to this repository, manual runs |

## CI (hosted, game-free)

`macos/bootstrap.sh`, `macos/build.sh` (the runtime without the game), the CTest suite (runtime, audio and
graphics smoke tests, AOT and JITless fixtures, Python tooling tests), and `ipad/tests/test_public_build.py`.
Pinned downloads (MoltenVK, FFmpeg, the DirectX redistributable) are cached between runs.

## Full build (private runner)

Generates `gen/` from the runner's own game installation, then builds the macOS game with its tests, the visionOS
engine and the unsigned visionOS app. Generation takes 20–40 minutes and is reused until the lifter or its Python
inputs change. The job is skipped unless the repository's Actions variable `SKYRIM_PRIVATE_RUNNER` is `true`, so
forks and repositories without a runner never queue it.

Rules that keep it private and safe on a public repository:

- **No `pull_request` trigger here, and approval for fork pull requests.** This workflow runs only for pushes to
  the repository (including your own PR branches) and manual runs. That alone does not keep forks off the runner:
  a pull request runs the workflow files from its own branch, so one from a fork could change `ci.yml` to run on
  `[self-hosted, skyrim-private]`. Requiring approval for fork pull request workflows (below) closes that, as long
  as each approval follows a look at the pull request's `.github/` changes. Keeping the runner stopped while you
  are not using it narrows it further.
- **Nothing leaves the Mac.** No artifacts are uploaded: the engine and app contain compiled game code. Only the
  generated-code key and output hashes go into the job summary. Logs are public, so build steps must not print
  game file contents or generated code: the steps that compile `gen/` write their output to `$SKYRIM_CI_HOME/logs`
  on the runner and print only whether they passed.
- **A non-admin account.** Run the runner as a dedicated standard user that holds nothing else.
- **No stored GitHub credentials.** Registration uses a one-time token; the runner then holds only its own
  runner credential.

A runner that isn't always on works: jobs wait in the queue until it connects, GitHub drops a job that has waited
24 hours, and a newer push to the same branch cancels the older queued job.

### Setting up a runner

On an Apple Silicon Mac, as a dedicated standard (non-admin) user:

1. **Prerequisites:** full Xcode with the visionOS SDK, Python 3.10+, `cmake` and `ninja` on `PATH` (for example
   in a Python venv: `python3 -m venv ~/ci-tools && ~/ci-tools/bin/pip install cmake ninja`), and `cabextract`,
   which `macos/bootstrap.sh` always uses (install it with Homebrew from an admin account; `/opt/homebrew/bin` is
   readable by the runner user).
2. **A game folder** that user can read, with `TESV_unpacked.exe`, `d3dx9_42.dll` and `X3DAudio1_7.dll` beside
   `TESV.exe` (see [BUILDING.md](BUILDING.md)), so generation needs no .NET. The job also checks reused
   generated code against this folder.
3. **Install the runner:** in the repository, **Settings → Actions → Runners → New self-hosted runner** (macOS,
   ARM64). Follow its download and checksum steps, then configure with the extra label:
   `./config.sh --url <repository URL> --labels skyrim-private --token <token>`.
4. **Runner environment:** add to the runner folder's `.env`:
   ```
   PATH=<folder with cmake and ninja>:<folder with python3>:/usr/bin:/bin:/usr/sbin:/sbin
   SKYRIM_CI_GAME_DIR=/path/to/your/Skyrim
   SKYRIM_CI_HOME=/path/to/a/private/cache/folder
   ```
5. **Enable the job:** **Settings → Secrets and variables → Actions → Variables**, add `SKYRIM_PRIVATE_RUNNER` =
   `true`.
6. **Run it** with `./run.sh` whenever the Mac is free and on AC power.

### Repository settings to check once

- **Settings → Actions → General → Fork pull request workflows:** "Require approval for all outside
  collaborators". Required once a runner is registered: without it, a fork's pull request can edit `ci.yml` to
  run on the private runner (see above). Set on twcclegg/skyrim-apple on 2026-11-18. Never approve a workflow
  run from anyone else's fork or pull request.
- **The job itself** runs only in `twcclegg/skyrim-apple`, only for a push or a manual run, and only when
  `twcclegg` both caused and triggered the run (`full-build.yml`).
