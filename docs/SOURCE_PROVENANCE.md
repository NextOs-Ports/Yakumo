# Source provenance and license boundaries

Yakumo is a profile on top of [PortableKit](https://github.com/TeamGDB/PortableKit), which this repository includes as the `portablekit` submodule. The framework's own provenance — the recompiler and runtime, the installer's decryption of the player's executable, the save-data format, ad hoc networking, texture packs, and every third-party component vendored under `portablekit/third_party` — is recorded in PortableKit's [`docs/SOURCE_PROVENANCE.md`](../portablekit/docs/SOURCE_PROVENANCE.md). This page covers what is in this repository.

## The profile

`profiles/mhp3rd/host/mhp3rd_profile.cpp` gives PortableKit the facts of this one release: its disc id, the SHA-256 of its encrypted executable and of the decrypted executable the profile was generated from, the `~PSP` tag of that executable and the key the public PSP key tables give for it, its memory layout, its overlay slots and its save folders. They were read from the player's own disc and the game's own executable, not copied from other projects. The installer accepts exactly the one encrypted file the profile names and checks its output against the second hash. Developers can still prepare the executable outside the project and supply it through `profiles/mhp3rd/game`.

## Analog camera and the view's shape

`profiles/mhp3rd/host/camera/game_camera.cpp` and `game_aspect.cpp` were written from this project's own NPJB-40001 executable analysis and run-time traces. The camera caller, structure offsets, 16-bit yaw format, height-filter coefficient, the projection's parameters and the constants they are built from are observed facts. Continuous pitch uses an independently written spherical-orbit calculation. No third-party camera-mod code is included or adapted.

## Texture packs

PortableKit loads PPSSPP-format texture packs; its provenance page says how the format was learnt. The keys it produces were checked against a real community pack for this game, whose keys the implementation reproduces. No pack is included.

## Third-party components in releases

Nothing third-party is vendored in this repository itself. Yakumo's releases ship PortableKit's vendored components (listed on its provenance page) and these:

| Component | Used by | License | How it is included |
| --- | --- | --- | --- |
| [SDL3](https://www.libsdl.org/) | Window, input, audio output | zlib | External dependency, linked dynamically; Linux releases ship an unmodified build in `lib/` |
| [FFmpeg](https://ffmpeg.org/) (`libavcodec`, `libavutil`) | ATRAC3 music and H.264/ATRAC3plus movie decoding | LGPL-2.1-or-later (the Windows build: LGPL-3.0-or-later) | Linked dynamically; no FFmpeg source is in either repository. PortableKit's `cmake/FFmpeg.cmake` pins the version, its checksum and the configuration it is built with; PortableKit's provenance page gives them in full |
| [Noto Sans CJK JP](https://github.com/notofonts/noto-cjk) | Releases: fallback font for Japanese text | SIL Open Font License 1.1 | Downloaded by the release build, shipped in `fonts/`; not in the repository |

Released builds carry the notices in `profiles/mhp3rd/packaging/THIRD_PARTY_NOTICES.md`, together with the license texts; [`RELEASING.md`](RELEASING.md) describes how they are built.

## Contribution rule

Do not paste or adapt source from a project whose license is incompatible with the destination file. Reimplement required behavior from specifications, observations or independently documented semantics, and record the source of third-party material when it is intentionally included under a compatible license.
