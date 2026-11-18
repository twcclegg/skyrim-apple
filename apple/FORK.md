# Shared Apple launcher

The single-title SwiftUI launcher uses a smoke-gray theme with white and gold accents. Its shared source serves the Mac, iPhone/iPad and Vision Pro targets. The public native build described in the root README targets iOS/iPadOS and was accepted on a physical iPad Pro M5. Building a launcher target without the runtime does not make the game playable.

Design and behavior references:

- [Dark Eco](https://github.com/zschwendi/dark-eco) / GOALPad: the centered cover, separate settings, gray adaptation of the smoke shader, and opt-in local-first save mirroring. Reference revisions included `3be15de7e764553f70eb5f3f9f02dce695840586` and `7fd9c27b704d49546eff8fcbb3affe7b4cdc1416`. No root license was found in the referenced GOALPad checkouts; this document does not create a license grant.
- [Titanfall 2 Apple](https://github.com/zschwendi/titanfall2-apple): separate cloud settings, conflict preservation and honest unavailable states. Reference revisions included `27620c4846223926887fb3c5dd0f9a2acbebfa4c` and `562716cff0583d2dc455ba0f352642822059ca9b`. Its runtime is not included here.

The icon is original generated silver/gold mountain-crest artwork. `apple/Artwork/provenance.json` records its origin and no-game-assets constraint. Packaging resizes that artwork for Apple asset catalogs. No Skyrim logo, extracted textures or characters are used for the app icon or launcher cover.

The shared app has separate local and Cloud signing configurations. Use your own development team, app identifiers and iCloud container. A source entitlements file alone does not prove provisioning or cross-device sync. The native iOS build additionally requires legitimate memory capabilities. See [Building and installing](../docs/BUILDING.md).

The current iPad implementation validates and runs the complete owned installation in its Documents/Skyrim folder. It requires DualSense or keyboard and mouse, holds game frames on pause/inactivity, and uses local Application Support saves. Cloud save transfer and the full lifecycle matrix are not yet accepted on the physical iPad. Vision Pro windowed and immersive work is separate and must obtain its own device evidence.
