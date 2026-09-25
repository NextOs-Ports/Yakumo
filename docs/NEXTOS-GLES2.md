# NextOS GLES2 development

This branch adds an experimental GLES2 backend for AArch64 NextOS devices with
Mali-450 graphics. Vulkan remains the default. No game data or generated game
code is included. The initial target is the original 480 x 272 picture at the
game's 30 Hz simulation rate, preserving the game's geometry and lighting.

Configure with `-DMHP3RD_GRAPHICS=GLES2`. SDL3 and FFmpeg come from the target
firmware, without bundling or redirecting SDL. For cross compilation, set
`NEXTOS_TOOLCHAIN` to the current firmware toolchain and pass
`-DCMAKE_TOOLCHAIN_FILE=cmake/NextOSAArch64.cmake`.

Generate the executable and overlay corpora using host tools first. Build them
with the target compiler. Do not run an AArch64 code generator on the host.

The backend consumes the existing decoded GE draw calls. It retains CPU skinning,
lowers the existing vertex-lighting and fog shaders to GLSL ES 1.00, decodes the
original texture formats, maintains separate framebuffer targets, copies feedback
before sampling an active attachment, and writes framebuffers back to guest VRAM.
Movies upload to the same targets. The existing menu layout uses the matching
upstream ImGui GLES2 backend. Opaque scanout alpha is restored before swapping.

## Verification status

A dedicated `mhp3rd_gles2_tests` executable reads actual GPU pixels and checks
sprites, RGBA textures, alpha rejection, framebuffer sampling, attachment feedback,
capture, source-alpha blending, transformed geometry, ambient material lighting,
depth rejection, fog and ImGui initialization. These checks passed on an NVIDIA
RTX 4070 Linux host and on physical Mali-450/NextOS using the firmware SDL3 and
GLES2 driver, launched through EmulationStation. This does not establish game
graphics correctness, gameplay, audio or performance.

AArch64 game code and overlay builds and physical game rendering validation are in
progress. This is not a release or a completed port.

## Current limits

- Frame interpolation and HD texture replacement are not implemented in GLES2.
  The backend reports these limits and presents original game frames/textures.
- Touch input and mouse capture need integration; keyboard and SDL gamepads are
  available for development. The per-controller Back+Start exit is immediate.
- The automatic resolution and expanded-camera aspect options need matching
  GLES2 implementations. Use internal scale 1 and Original aspect for initial
  comparisons.
- Curved surfaces and missing PSP utility dialog graphics remain upstream limits.
- No claims are made for untested blend modes, lighting scenes or device drivers.

All synthetic tests use constructed data. Game-derived AOT sources, overlays,
images, executables and saves remain local and ignored by Git.
