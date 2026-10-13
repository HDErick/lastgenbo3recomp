# BO2 native renderer reference review

October 13, 2026. Read-only review of
`C:\BO2recomp\Project BO2\rexglue-bo2\src\d3d9`.

The user reports this renderer is broken. It is not BO3's renderer base, and no
code was imported. BO3 currently uses the pinned MW2 shader translator with its
own Vulkan target, draw and resource backend, plus RexGlue parsing/presentation.

Useful design references from `d3d9_renderer.cpp` and its headers:

- Guest clear flags, depth value and stencil value are read from device state;
  reverse-depth clears can be zero rather than the host's default one.
- Pixel shaders may be absent in depth passes; their color writes must be disabled.
- Surface tracking and resolve ownership need distinct records from texture
  uploads, particularly when the guest reuses the same underlying memory.

The D3D9 API, game-specific device offsets, hooks and SM3 shader artifacts are
not portable BO3 Vulkan implementations. Its per-frame host clear is not evidence
of the precise BO3 guest clear rectangle or timing and was not adopted.

BO3's next depth investigation instead follows its captured XDK clear program,
actual PM4 state, precise GPU sample coverage and observed MSAA alias layouts.
General EDRAM ownership, color clears, texture resolves and complete shaders
remain necessary before claiming a native gameplay frame or Switch performance.
