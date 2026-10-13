# BO3 native memory-export gate

October 13, 2026. This is preparation for a native renderer. The current game
continues to use emulated Xenos Vulkan, including its existing memory exports.

## Captured programs

Two loaded VS in discovery03 export to guest memory. The MW2-derived translator
emits structurally valid SPIR-V while dropping those writes. Neither module is
an acceptable native replacement until the behavior is implemented.

| Program key | Static export writes dropped | Export blocks | Data registers per block | Full-fetch stride (dwords) |
| --- | ---: | ---: | --- | ---: |
| 5B9B7484417FB9B6 | 20 | 4 | eM0..eM3 | 64 |
| A4827826E2589916 | 10 | 2 | eM0..eM3 | 16 |

Each block writes eA using index arithmetic and c0/c255, then passes fetched
values through `max` into eM0..eM3. Fetches use unsigned integer
FMT_16_16_16_16 with yxwz destination swizzle. This resembles a copying path,
but a plain memcpy is not established: destination format, endian conversion,
red/blue swap, bounds, index selection, overlap and completion still matter.
The first program's differing input stride and index arithmetic especially
require the live constants. The final position export does not establish that
rasterization is disabled; inspect actual draw state.

The binary programs select full vertex-fetch slot **95** (group 31/sub 2), while
the saved textual disassemblies label those reads vf0. Use the binary microcode
and GPU-consumed registers as authority; this discrepancy must not cause a
future copy path to read the wrong fetch slot. The inspector derives slots from
the translated exact program's metadata, not the text label.

## Required behavior

The staged SDK's `spirv_translator_memexport.cpp` documents the existing path:
validate eA sign/exponent prefixes, pack using its format/numeric fields, apply
red/blue and endian transformations, clip to stream bounds and store only written
eM registers. It flushes exports at allocation boundaries and shader completion.
The native implementation also needs ownership/barriers, visibility to later
draws, required CPU readbacks and completion before guest memory reuse.

The native vertex-fetch arena is a read-only snapshot of selected input data;
it is not guest memory. Redirecting exports into it would not implement guest
writes. MW2's title-specific image-copy hook and fixed allocations were not copied.

## GPU-consumed state probe

`BO3_MEMEXPORT_PROBE_DIR` enables a probe immediately after vertex shader analysis
in Vulkan IssueDraw. It snapshots the complete register file and exact loaded
microcode on the command processor thread, plus draw/index metadata. It leaves
the original rendering path active and does not read or write guest buffers.
It captures at most 16 samples per shader filename key, up to eight shaders.
Existing output is preserved; I/O failure disables further probe writes and logs
the error. Raw snapshots stay in ignored runtime directories.

```powershell
python scripts/nx/prepare-runtime.py
C:\Windows\System32\cmd.exe /d /c scripts\nx\build-pc-vulkan.cmd
.\scripts\nx\launch-pc-vulkan.ps1 -RenderTargetPath fsi -MemexportProbe -DiscoveryRun native-memexport01 -Background -LogName pc-vulkan-native-memexport01.log
```

Load The Giant, play about a minute and close normally. This remains the emulated
Vulkan test, with the known white squares. After snapshots have been generated:

```powershell
python scripts/nx/inspect-memexport-probe.py --input runtime/native-memexport01/memexport --output analysis/bo3_memexport_state01.json
```

The inspector matches full canonical microcode SHA256 to the comparison corpus,
reports the actual VS constant window, c0/c254/c255 and referenced fetch slots,
and decodes c0 as an address-stream candidate for the two reviewed programs.
It does not prove final eA values, input/index/output bytes, copy equivalence or
GPU completion. Unknown programs remain explicitly unmatched.

The probe build linked successfully (`analysis/pc_vulkan_native_memexport_build01.log`).

## First state capture

The running PC test produced 32 complete samples: 16 for each reviewed program,
matching their complete canonical microcode hashes. All samples have VS constant
base zero and valid c0 stream prefixes. Their common output profile is:

- Format 26: FMT_16_16_16_16.
- Numeric format 2: unsigned integer.
- Endian 1: swap bytes within 16-bit words.
- No red/blue swap.
- Point-list, non-indexed draws in EDRAM mode 4 (color/depth).
- Vertex export mode 7 (multipass) suppresses rasterization. The earlier
  description of EDRAM mode 4 as export-only was incorrect.
- Full vertex fetch slot 95, confirming the binary operands.

The two-block program's samples have 4,352 or 14,400 invocations and respectively
34,816 or 115,200 stream elements: eight 8-byte elements per invocation.
The four-block program has different c254 indexing/stride and smaller bounded
streams. A single contiguous memcpy must not be substituted without deriving
the actual address and clipping behavior. Input bytes, output bytes, overlap,
GPU/CPU ownership and completion are still unverified.

Evidence: `analysis/bo3_memexport_state01.json` and
`analysis/pc_vulkan_native_memexport_provenance01.json`. The game was still open
when these snapshots were summarized; user gameplay/normal-close confirmation
is pending. These state samples alone do not establish visual correctness.

### Window visibility correction

The user reported that no game window was open. Investigation found capture01
still running/responding and continuing audio/presentation logs, with no new
crash report. It had been launched on the tool's isolated desktop; its samples
are startup/export state, not confirmed map gameplay. The agent stopped that
process and a second isolated test, preserving all captures. The launcher now
requests a normal window, and capture03 was launched on the interactive desktop.
Computer Use verified its visible Press A to Start screen. The user then
confirmed closing the test without a crash. The log records Window closing,
Execution complete and Title terminated at 11:13:33, and the process is gone.
No new crash/abort report was created. The latest response does not separately
confirm which map was played. Record:
`analysis/pc_vulkan_native_memexport_provenance03.json`.

## Offline copy-range model

Capture03 also has 32 complete snapshots, matching both exact program identities.
`scripts/nx/plan-memexport-copies.py` now models all 32 with strict checks for the
reviewed program, constants, auto-index/point-list state, raster suppression,
fetch byte order, source bounds, physical limits and source/destination overlap.
Unsupported state is reported explicitly; no guest writes are performed.

| Program | Invocations | Modeled bytes written | Clipped elements | Untouched destination bytes |
| --- | ---: | ---: | ---: | ---: |
| A4827826E2589916 | 14,400 | 921,600 | 0 | 0 |
| A4827826E2589916 | 4,352 | 278,528 | 0 | 0 |
| 5B9B7484417FB9B6 | 64 | 4,096 | 512 | 4,096 |
| 5B9B7484417FB9B6 | 32 | 2,048 | 256 | 2,048 |

The two-block path models one contiguous copy. The four-block path has 128-byte
ranges spaced 256 bytes apart; bounds clip later invocations. Do not overwrite
the 128-byte gaps. This is the address model's prediction, not a GPU-output
measurement. Reserved stream format bits vary in captured draws and are correctly
ignored; numeric format, channel swap, endian and actual format remain guarded.

For this observed uint16 profile, input 8-in-32 byte conversion plus yxwz
swizzle and output 8-in-16 conversion compose to byte identity. The offline
reference checks all 65,536 uint16 values and each raw bit position. Six tests
also check contiguous/strided ranges, partial bounds, ignored bits and rejected
states/overlap. These checks do not exercise the GPU, actual resource contents
or synchronization.

```powershell
python scripts/nx/test-memexport-copy-plan.py
python scripts/nx/plan-memexport-copies.py --input runtime/native-memexport03/memexport --output analysis/bo3_memexport_copy_plan03.json
```

Evidence: `analysis/bo3_memexport_state03.json`,
`analysis/bo3_memexport_copy_plan03.json`, `analysis/bo3_memexport_copy_model_tests01.log`
and `analysis/pc_vulkan_native_memexport03.log`.
Next verify actual input/output bytes against the emulated GPU result, then
implement the bounded copy operation with correct resource ownership, visibility
and completion in the native backend. Both shader replacements remain blocked.
