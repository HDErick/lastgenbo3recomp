# PC Vulkan white-square investigation

October 13, 2026; branch `native-rendering-pc`.

## Evidence

The Giant renders large pale squares under both FBO and confirmed FSI. The
user observes them at windows and on the glowing upper part of the starting
pistol. The menus use video playback, according to the user; successful menu
display does not validate the 3D renderer.
The user explicitly confirms that these squares did not occur before the Vulkan
implementation. Track this as a Vulkan regression relative to the earlier
D3D12 baseline. The older model-color flashing is a separate paused issue.

Portable RenderDoc 1.46 captured a confirmed FSI gameplay frame. Replay on the
same PC succeeded and reproduced the squares. The capture contains 4,225 draws
and 6,030 actions. Capture, extracted shader code and textures remain local
under ignored `build/draw-capture/`; they contain game data.

| Capture resource | Size / format | Observation |
| --- | --- | --- |
| 31126 | 880x720 R10G10B10A2_UNORM | Earlier scene image, no large white squares |
| 31133 | 320x180 R16G16B16A16_UNORM | Bright-region image, abrupt black/white values |
| 31138 | 160x90 R16G16B16A16_UNORM | Contains the same block pattern seen on screen |
| 31185 | 880x720 R8G8B8A8_UNORM | Later scene image already contains the squares |
| 1096 / 221 | 1280x720 / 2560x1440 | Final output and presentation preserve the defect |

This localizes the defect to the glow/bloom processing chain. It does not yet
prove whether shader arithmetic, constant state, sampler filtering or fixed-point
render-target resolve/upload introduces the corruption. It does not establish a
fault in general world lighting or the particle textures.

### Resolve-chain evidence

The EDRAM storage buffer immediately after event 21580 contains 6,532 distinct
finite half-float values across the 320x180 draw, from 0 to 2.984375, and no
NaN/Infinity. The bound render-target format is RGBA16_FLOAT (format flags 23),
base 2,498,560 dwords, tile pitch 5,120 dwords at 32bpp, 1x sampling. Decoding
the captured buffer using the translator's tile addressing gives, for pixel
(115,95), `(0.58984375, 0.108642578125, 0.028717041015625, 0.2052001953125)`.

The uploaded 320x180 RGBA16_UNORM texture contains only 0 and 65,535 in all
230,400 channels: 164,307 zeros and 66,093 maximum values. The number of zero
channels exactly matches the source draw. The following 160x90 / 80x45
textures are also binary. Captured filtering samplers are linear.

The resolve at event 21586 uses shader resource 289, source FP16, destination
format 26, and exponent bias +11. Its push words are `401866760`, `591104`,
`740609`, `6154`, `0`. The translated compute shader applies the exponent and
clamps to [0,1] for UNORM packing. This strongly narrows corruption to resolve
conversion/scaling, rather than particle source images or vertex geometry.
The intended Xbox conversion contract still needs verification before repair.

Capture SHA256:
`add16402818b13f12e66defcec3cd954421e0eca8bb1c6c18fbd1c86cfd63971`.

Capture identifiers apply only to `white-squares_frame3002.rdc`:

- Event 21580, fragment shader resource 678: samples the earlier scene 31126;
  its result is resolved and uploaded into 320x180 resource 31133 at event 21594.
- Events 21603 / 21721, fragment shader 540: successive downsample draws using
  resources 31133 / 31138.
- Events 21742 / 21763 and 21807 / 21828, fragment shaders 493 / 567: subsequent
  filtering draws using the smaller effect textures.
- Event 21876, fragment shader 595: combines the earlier scene 31126 with
  160x90 resource 31138 and color-correction resources.
- Resource 31185 is uploaded at event 21991 and sampled at event 22000.
- Event 22256 presents source image 1096 into swapchain image 221.

FSI writes guest EDRAM through a storage buffer, so ordinary attachment pixel
history cannot directly identify each guest draw. Inspect the translated shader,
constants and resolved intermediate images before changing rendering behavior.

## Capture tooling

The optional host loads the RenderDoc API before graphics initialization and
supports a request-file trigger. The launcher uses `renderdoccmd capture` with
process-local implicit-layer discovery. Forced explicit-layer launch attempts
failed with `ErrorExtensionNotPresent`; the corrected launch selects actual FSI.
No global Vulkan registration or driver changes were made.

RenderDoc's Android-device detection repeatedly launched its bundled `adb.exe`,
which raised application exception 0x40000015. The bundled executable was
reversibly renamed `adb.exe.disabled`, and its processes were stopped. PC replay
completed successfully. Android support is unnecessary for this PC investigation.

## Status and next check

Investigation is paused in favor of native renderer development, as discussed
with the user. A native renderer may bypass this emulated resolve path, but
must correctly implement BO3's FP16-to-fixed resolve and +11 exponent-bias
contract. Mass Effect's documented native resolve handling is not a universal
solution for this combination. Use this capture as a targeted correctness test.
Discovery01 loaded The Giant successfully with the forwarding hooks enabled;
the user confirmed the same white squares. Those hooks do not implement a
native renderer and therefore do not test whether replacing Vulkan's emulated
resolve path repairs the regression.

Inspect the first bright-region draw and the filtering passes: capture shader
constants, raw normalized 16-bit values and sampler modes. Distinguish negative
fixed-point values interpreted as unsigned from saturated shader output and
incorrect filtering before proposing a repair. The diagnostic capture tooling
is not a visual fix.
