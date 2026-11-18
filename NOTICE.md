# Credits and licensing

The Skyrim static recompiler and compatibility runtime originate in [ttom795/Skyrim_Android](https://github.com/ttom795/Skyrim_Android), based on upstream source revision `0819237bac51dc6f68c1a299c30359376ba0331d`. Credit for that foundation belongs to its authors. That upstream checkout did not provide a root license; this snapshot does not invent a license grant for their work or relicense it.

The Apple adaptations and launcher are published here by Zachary Schwendiman. The accepted native iPad implementation was prepared from local revision `98a28a4`, with the working launcher view changes included in this source snapshot. Public setup scripts and documentation may include subsequent portability changes.

The shared launcher references Dark Eco's single-title library, separate settings, and local-first save-sync behavior. Titanfall 2 Apple's cloud-save UI was another reference. Their respective authors retain their work. References do not imply that the underlying game runtimes or game assets are included in this app. See [the launcher notes](apple/FORK.md).

Third-party components retain their own licenses, including:

- SDL2: the 2.30.9 release, fetched unmodified at build time by `tools/fetch_sdl.py`; [zlib license](https://github.com/libsdl-org/SDL/blob/release-2.30.9/LICENSE.txt), plus notices for its incorporated components.
- DXVK / dxvk-native: zlib license, with copyright notices retained in the source tree; see the [upstream license](https://github.com/Joshua-Ashton/dxvk-native/blob/master/LICENSE). Built from the [twcclegg/dxvk-native](https://github.com/twcclegg/dxvk-native) fork (git submodule `third_party/dxvk`, branch `skyrim-apple`), which carries the Android port's mobile-GPU patches and this project's changes as separate commits on top of `native-1.9.2b`.
- MoltenVK: obtained separately from the [KhronosGroup/MoltenVK](https://github.com/KhronosGroup/MoltenVK) project; its license and third-party notices apply.
- FFmpeg: the decoder-only dependency is built without GPL or nonfree options; [FFmpeg licensing](https://ffmpeg.org/legal.html) and the source's LGPL notices apply.

The original app icon and launcher crest are generated artwork created for this project, without supplied Skyrim assets or a Skyrim logo. The README's gameplay image is a capture from the tester's owned installation; it is documentation, not a distributable game asset pack.

Skyrim, The Elder Scrolls, Bethesda, and other game names and content remain the property of their respective owners. This repository does not include or grant rights to the game, its binaries, any game-derived generated code, or its data. It is an unofficial project with no endorsement from those owners.
