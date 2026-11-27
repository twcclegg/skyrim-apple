# Skyrim running on Apple Vision Pro

That is the whole goal, and the one rule every person and every AI agent working here follows.

**What it takes to play:** one person, their input devices, one M2 Apple Vision Pro and, if wanted, wall
power. Nothing else. Picture them inside a Faraday cage: if an idea needs anything outside it, the idea is out
of scope. (The cage is a figure of speech. It means no other computer helps run the game; it is not a rule
against networking or a place to test.)

The game runs **on the headset**: its CPU, its GPU, its memory. No other machine does any of the work while
someone plays.

## Out, permanently

Do not propose, research, prototype or keep as a fallback:

- Streaming the game or its frames to the headset from a Mac, a PC or the cloud. This covers Moonlight,
  Sunshine, ALVR, CloudXR, Steam Link, GeForce NOW, `RemoteImmersiveSpace`, foveated streaming and anything
  like them.
- Doing any part of a frame on another machine: rendering, simulation, upscaling, frame synthesis, encoding.
- "Play Skyrim VR or Special Edition from a PC instead."

## In

- Anything that runs on the headset: renderer, CPU, engine and data changes, foveation, reprojection, frame
  synthesis, input, comfort.
- Other machines as **build, test and measurement hosts**. Their outputs may ship inside the app (for example
  a prebuilt pipeline cache). At play time only the headset computes.
- Input devices: a paired controller, keyboard, mouse or trackpad, hands and eyes. They do no computing for
  the game.

## Reading requests

"Push past the M2's limits", "search wide" and "whatever it takes" all mean better on-device techniques. They
never mean moving work off the device. If a request seems to ask for offloading, ask before doing anything.

The current plan is [docs/VISION_PRO_PLAN.md](docs/VISION_PRO_PLAN.md).
