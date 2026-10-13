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
- Point-list, non-indexed draws in EDRAM mode 4 (export-only).
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
Computer Use verified its visible Press A to Start screen. Map gameplay and
normal-close confirmation remain pending. Record:
`analysis/pc_vulkan_native_memexport_provenance03.json`.
