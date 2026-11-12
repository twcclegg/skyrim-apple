# Surround audio (branch `helper/surround-audio`)

## What was built

- `runtime/xaudio2.c`: `LB_AUDIO_CHANNELS=6` reports a 5.1 device to the game (`GetDeviceDetails`: 6 channels,
  mask 0x3F, order FL FR FC LFE BL BR), gives a mastering voice created with the default channel count six
  channels, carries six channels from the mastering voice to the output and opens SDL with six. Unset, or `2`,
  is stereo as before; the stereo mixing and fold-down code is untouched. Any other value logs a line and stays
  stereo.
- In 5.1 mode a mono voice with no matrix set goes to the front pair only (it went to every destination
  channel before, which would have included the LFE). This is my reading of what XAudio2 does by default; it
  was not checked against Windows. The game sets explicit matrices for almost every voice (below), so it
  rarely applies.
- `LB_AUDIO_METER=1` in 5.1 mode adds two lines to each 5 s window: the RMS of each output channel, and the
  `SetOutputMatrix` calls since launch by shape (source x destination channels) with how many 6-wide matrices
  put anything on the centre, LFE and back channels.
- `ipad/native_bridge.mm` (visionOS only, and only with `LB_AUDIO_CHANNELS=6`): before SDL opens the device,
  the audio session is told the app plays multichannel content (`setSupportsMultichannelContent`) and which
  spatial experience is intended (`setIntendedSpatialExperience`). `LB_AUDIO_SPATIAL` = `head` (default: the
  bed stays put as the head turns), `fixed` or `bypass`; `LB_AUDIO_STAGE` = `small`, `medium`, `large`
  (default) or `auto`. It logs one line, `native: audio: 5.1 session: ...`, with what the session accepted and
  its current and maximum output channel counts.
- Tests: `macos/tests/audio_surround.c`, ctests `audio_layout_stereo` and `audio_layout_surround` (real shim,
  SDL dummy device, no game).

## What Skyrim does with a 5.1 device

One run of `whiterun-market-mac` (4300 frames, exit frame reached) on the macOS build with
`LB_AUDIO_CHANNELS=6 LB_AUDIO_METER=1 SDL_AUDIODRIVER=dummy`:

- The game asks for a 6-channel mastering voice itself: `mastering voice 6 ch at 48000 Hz (asked 6 ch)`.
- It sends positional matrices. By the end: 13,436 mono-to-6 matrices, of which 10,102 had a non-zero back
  channel and 3,366 a non-zero centre; none used the LFE. Also 33 stereo-to-6 (music and menu sound, front
  pair only), and 257 mono-to-2 and 34 stereo-to-2 into 2-channel voices.
- The rears carry sound: 30 of 36 meter windows had a back channel above 0.001 RMS (the other six are the
  menu and loading). Mean RMS over the 36 windows: FL 0.0183, FR 0.0172, FC 0.0017, LFE 0, BL 0.0084,
  BR 0.0087; highest window BL 0.0276, BR 0.0244.
- `X3DAudio1_7.dll`: its output does change with the channel count. In stereo the game's 3D voices get 1x2
  matrices; with a 5.1 device the same voices get 1x6 matrices with centre and back terms. The lifted code
  needed no change. Its coefficients were not compared with Windows.

This is one run, so there is no run-to-run spread; the counts depend on what the townspeople do.

## What was tested

- `audio_layout_surround`: device details and mastering voice report 6 channels; a mono square wave at 0.25
  with the default matrix measures 0.2500 on FL and FR and exactly 0 elsewhere; sent to back left by
  `SetOutputMatrix` it measures 0.2500 on BL and exactly 0 elsewhere. `audio_layout_stereo`: 2 channels,
  mask 3, 0.2500 on both. `audio_device_recovery` passes in stereo. `audio_voice_queue` passes when given its
  sample file.
- `ipad/native_bridge.mm` passes a syntax check against the visionOS, visionOS Simulator and iOS SDKs.

## What was not tested

- Nobody listened. The game run used SDL's dummy device, so nothing went through CoreAudio in 5.1. SDL
  converts to whatever the hardware has, so on a stereo Mac output SDL folds 5.1 down itself, with its own
  coefficients rather than the mixer's stereo fold-down.
- `audio_device_recovery` assumes a stereo queue size in one check and fails that check with
  `LB_AUDIO_CHANNELS=6` on the real CoreAudio device. The recovery logic scales with the channel count, but
  it has not been shown to pass in 5.1.
- No visionOS or iOS engine was built or run. Whether visionOS spatialises a 5.1 audio queue as a bed, or
  folds it to stereo first, is unknown. SDL sets the session category after the two session calls; whether
  they survive that is unknown.
- No CPU cost was measured. Six output channels instead of two is more mixing per voice in `route()`; the
  unrolled fast paths only cover stereo destinations.
- 7.1 and other layouts are not offered.

## Headset test (Thomas)

Set `LB_AUDIO_CHANNELS=6` in `launch-env.json` (one change; leave the others unset first).

1. In the log, check `native: audio: 5.1 session: multichannel 1 ... (set 1) ... maximum N`. If
   `multichannel 0` or the maximum is 2, the system refused and the rest will be stereo.
2. Check `audio meter 5.1:` lines show BL/BR above zero in a town.
3. Listen in Whiterun market: stand still and turn the game camera with the stick. A voice or the forge
   should move behind you. Then turn your head: with the default `head` mode the sound field should stay
   fixed relative to the room (and, with head tracking of the picture on, relative to the game world).
4. If it sounds stereo or wrong, try `LB_AUDIO_SPATIAL=fixed` and `LB_AUDIO_SPATIAL=bypass` to tell the
   system's spatialiser apart from the mix, and `LB_AUDIO_STAGE=medium`.
5. Listen for the centre: dialogue should not be noticeably quieter than in stereo.
