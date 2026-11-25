# Native game boot research — Super Dragon Ball Z (USA)

> **Current checkpoint (2026-11-25):** [Retail gameplay, audio and runtime validation](SDBZ_RUNTIME_VALIDATION_20261125.md). The earlier boot milestones below are historical; the HLE-first runner now reaches bounded interactive training gameplay with audible audio. Full compatibility and hardware accuracy remain open.

> **Current M2 implementation plan:** [SDBZ runtime progression, GS/VIF/VU and EE ISA coverage](M2_SDBZ_NATIVE_RUNTIME_AND_ISA_PLAN.md). This plan supersedes earlier static-gap priority assumptions where the 2026-11-19 07:27 boot provides newer runtime evidence.

This is a **bounded, experimental EE native-code runner**, not a general-purpose PS2 emulator. No copyrighted game ELF is stored here.

## Getting started

Build the existing CMake project and install `llvmlite` into the Python
environment used by CMake and the runner:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
python3 tools/run_game.py /path/to/SLUS_214.42 \
  --build-dir=build --work-dir=build/sdbz \
  --hle-startup --kseg-ram --trace --auto-expand=16 \
  --report=build/sdbz/first-boot.json \
  --max-dispatches=128 --timeout-seconds=60
```

To continue with existing translations instead of rebuilding the initial
function set, use `--no-recompile --auto-expand=N`. This reuses the
existing linked library and metadata from the same ELF, and only compiles and
relinks if execution reaches a new guest function PC. The ELF path and entry
are validated against saved metadata. A `translation-cache.json` provenance
record also stores SHA-256 fingerprints for the full ELF, built `ee_recomp`,
the translation/emission/link scripts, linked runtime sources and headers,
the function metadata, and the resulting shared library. This record is
written only after a successful link and refreshed after auto-expansion
or an explicit PDB relink. `--no-recompile` rejects absent/mismatched
provenance or a different `--emit-opt`, instead of silently running old
generated IR or a stale DLL. After updating the lifter or runtime,
build `ee_recomp` and run once **without** `--no-recompile`; older
work directories without a provenance record cannot be reused safely.
This is a local consistency guard, not a cryptographic authenticity
mechanism or a replacement for testing native instruction semantics.

`tools/run_game.py` invokes `ee_recomp`, verifies each LLVM IR module,
emits native objects, links a guest-PC dispatcher library, and starts
`ee_game_runner`. The game binary remains local. `--auto-expand=N`
compiles one newly reached guest function, relinks, and replays from entry,
up to N times; this is not an instruction-step emulator. `--max-dispatches`
counts *native function entries*, not MIPS instructions. A replay timeout
catches guest loops that do not return to the dispatcher.

On Windows, `--pdb` requests a matching
`game-translated.pdb` beside `game-translated.dll` using Clang and
the LLD MinGW linker. This passes `-g -gcodeview` for runtime and
dispatcher C++ compilation and `-Wl,--pdb=...` at link time.
The DLL and PDB are checked after linking and recorded in the
`.dll.link.json` manifest. A GCC/binutils-only toolchain is not
supported for PDB output. To generate a PDB without rebuilding the
guest IR **or launching the game**, use `--no-recompile --pdb --link-only`
with the existing `--work-dir`. This relinks the DLL using already-generated
guest objects and writes `game-translated.dll` and
`game-translated.pdb` side by side. Generated
guest functions have native symbols but do not yet carry source-level
LLVM debug metadata, so a PDB does not provide reconstructed MIPS/C++
source lines; it primarily makes native symbols and compiled runtime
information available to Windows debuggers. If you change the
`--emit-opt` setting, perform a fresh compilation first.

**Fast IR generation:** A full `ee_recomp` discovery now writes
`ee_discovery_cache.bin` alongside `ee_functions.json`. For each later
`--auto-expand` function, the runner passes this snapshot to
`ee_recomp --discovery-cache=...`, avoiding repeated whole-ELF fixed-point
JALR discovery and thousands of unrelated CFG analyses. Each snapshot stores
decoded guest instruction PCs, known function entries, and JALR research
results; the loader verifies its format and an ELF-content fingerprint before
use. A stale/corrupt snapshot fails rather than silently producing wrong IR.
The cache is optional: old work directories without it fall back to full
discovery. Only-function coverage now describes the selected function rather
than re-analysing every discovered function. Synthetic regression compares
cached and uncached IR byte-for-byte and verifies invalidation. This speeds
up the **IR generation** stage; the separate object emission and DLL linking
stages are addressed below. Real SDBZ runs remain user-owned.

Native host-object generation now supports verified incremental reuse. In
`run_game.py`, `--emit-jobs=N` (default 2; use 4 or 8 if RAM permits)
processes LLVM IR modules in parallel when more than one object needs
recompilation. Once an `objects.json` manifest has been written by the
updated emitter, subsequent auto-expansions reuse unchanged native objects
and emit just the newly discovered function, instead of rebuilding all
functions on every replay. Source IR, native object bytes, LLVM version,
host target triple and optimization level are checked before reuse.
Previously generated manifests have no IR hash and therefore receive a
one-time full rebuild. Only after complete linking is the higher-level
translation-cache provenance updated. Parallelism speeds up emission; a
single newly discovered function still requires a new link and game replay.
Run `run_sdbz_native.bat` on your Windows machine to update, build and
execute the local ELF and pause before the console closes.
The launcher now probes `--no-recompile --link-only` when a previous
`translation-cache.json` exists. If its complete source/ELF/binary provenance
is valid, it resumes the already translated library and only auto-expands
newly reached functions; if stale or absent, it performs the normal fresh
translation. This avoids repeating every old compile/link/replay iteration
when only the native runner's kernel HLE changes. The local ELF is always
executed only by the user on their Windows machine.

The batch launcher locates Visual Studio's `VsDevCmd.bat` (via `vswhere`
when installed), sets up the MSVC x64 headers/libraries/SDK environment,
and checks that `<cstdint>` is present before invoking CMake. This also
works when the `.bat` file is double-clicked from an ordinary Explorer shell;
if the C++ headers are missing, install the Visual Studio **Desktop
development with C++** workload.
 The ELF is never
included in CI.

Native host-object generation defaults to `--emit-opt=0` for fast iteration.
To compare LLVM backend optimization, run with `--emit-opt=2` (supported
levels 0–3). This option changes **host machine-code optimization**;
it does not modify guest code, patch emulation, or supply PS2 kernel state.
An O0/O2 synthetic-ELF equivalence test checks that the guest's resulting
register state and exit behavior agree. For game comparisons, prefer
a separate `--work-dir` per optimization level to avoid overwriting
previously linked objects and to keep benchmark results reproducible.
When auto-expanding an existing work directory via `--no-recompile`,
the cached optimization level must be preserved; use a fresh translation
to change `--emit-opt`.

`--report=FILE` produces a JSON report with the stop reason, COP0 exception
fields, all 32 low GPR halves, the instruction at EPC when available, and the
last 64 native dispatch transitions. Reports are written even when the guest
returns a non-zero blocked-execution status. Report history tracks **native
function dispatches**, not individual MIPS instructions. The report has a
`snapshot_kind` field: `stopped` after an orderly runner exit and
`in_progress` while code is still executing. An in-progress snapshot is
written at replay entry, every 16 dispatches, and on transitions into or out
of a registered syscall callback; this permits diagnosis when a hard replay
timeout kills a translated function without ever returning to the dispatcher.
The in-progress PC is the **last observed** PC, not necessarily the instruction
that was executing when the timeout occurred.
The report also contains a rolling `syscall_history` of the latest 32
installed guest-handler invocations, including the guest's A0-A3 arguments,
handler PC, resume PC, dispatch index, V0 return value, and whether the
handler returned. An unfinished entry has `returned: false`; its `v0`
value is not a valid result until the callback completes. This makes it
possible to identify repeated, unchanging syscall arguments without assuming
that repeated dispatches imply progress. With `--auto-expand`, each
replay overwrites the report, so its latest snapshot belongs to the most
recent attempted run. For timeouts, the Python driver prints the last saved
dispatch and PC if the report is readable. `--kseg-ram` opts into mapping the EE KSEG0/KSEG1 direct-RAM mirrors
onto physical RAM; it does not implement TLB, PS2 hardware MMIO, or device
side effects.

The runner initializes 32 MiB of EE RAM and a separate 16 KiB EE
scratchpad at `0x70000000..0x70003fff`, copies PT_LOAD segments, initializes
PC and SP and stops on exceptions, uncompiled PCs, exhaustion, or unsupported
host services. The scratchpad mapping follows the region boundaries in
`ps2-web-recomp/runtime/include/ps2/memory.hpp` and its memory resolver.
It supports bounded 8/16/32/64/128-bit loads and stores through the existing
generated-code memory helper ABI. The EE's VU0 physical memory apertures are
also mapped when the runtime has an `m_vu0_state`:
`0x11000000..0x11000fff` for VU0 micro-program memory and
`0x11004000..0x11004fff` for VU0 data memory. These addresses match
`ps2-web-recomp/runtime/src/memory.cpp`; the mapped micro-program array is
the same one used by `ee_vu0_fetch_micro_instruction`.
The map rejects cross-boundary reads/writes without silently wrapping.
The EE physical-address dispatcher also supports **opt-in** direct aliases:
`0x80000000..0xbfffffff` aliases physical `0x00000000..0x1fffffff`;
`0x20000000..0x3fffffff` aliases physical `0x00000000..0x0fffffff`.
This follows `ps2-web-recomp/runtime/src/memory.cpp::physical()`, but RAM
accesses remain bounded by the actual allocated RAM size rather than
silently mirroring unbacked physical addresses.

A host may now attach `s_ee_runtime::m_mmio_read` and `m_mmio_write`.
Within the physical EE hardware aperture `0x10000000..0x12ffffff`,
the dispatcher supplies the **physical address**, exact access width
(1/2/4/8/16 bytes), and 128-bit value. A 16-byte FIFO access is a
single callback, not two 8-byte callbacks. VU0 memory has priority.
Unimplemented devices return zero on reads and ignore writes by default.
The host can install device implementations through these hooks; the
native game runner now installs a minimal EE **INTC** model for
`INTC_STAT=0x1000f000` and `INTC_MASK=0x1000f010`. Its 32-bit writes
acknowledge status bits (write-one-to-clear) and toggle enabled-mask bits
(write-one-to-toggle), matching `ps2-web-recomp/runtime/src/hw.cpp`.
It raises/clears COP0 Cause.IP2 whenever a masked interrupt is pending.
The native runner's host MMIO bus now also exposes the DMA interrupt
status register `D_STAT=0x1000e010`. Its low 16 bits are write-one-to-clear;
the upper 16 bits are write-one-to-toggle channel masks. Bits 0..9 report
the ten DMA channels, and masked pending channels assert COP0 Cause.IP3.
These register rules come from `ps2-web-recomp/runtime/src/hw.cpp`.
Separate INTC and DMAC models preserve their own pending COP0 lines.
Devices must explicitly call `c_ee_intc::raise(cause)` or
`c_ee_dmac::raise(channel)` when a real hardware event occurs.
The native host also supports **normal-mode scratchpad DMA** for
`fromSPR` channel 8 (`0x1000d000`) and `toSPR` channel 9
(`0x1000d400`), using their `CHCR`, `MADR`, `QWC`, `TADR` and
`SADR` register offsets. Writing `CHCR.STR` from 0 to 1 in normal
mode transfers `QWC` 16-byte chunks between physical EE RAM and
the 16 KiB scratchpad. `SADR` wraps on the scratchpad boundary;
`MADR` advances, `QWC` clears, `STR` clears, and `D_STAT`
latches the matching channel completion bit, asserting IP3 if enabled.
The transfer behavior is based on
`ps2-web-recomp/runtime/src/dmac.cpp::run_normal`.
Unlike the reference's fast RAM mirroring, this experimental backend
rejects unaligned/unbacked RAM transfers atomically instead of copying
partially or pretending to complete. Other IPU/SIF device engines,
MFIFO, most DMA channels, and cycle-based DMA completion are not implemented.

The controller now also exposes **normal-mode device DMA** for VIF0
(channel 0, `0x10008000`), VIF1 (channel 1, `0x10009000`), and
GIF (channel 2, `0x1000a000`). The native host may install a
`c_ee_hw_bus::bind_device_transfer` callback which receives one
**complete**, contiguous, little-endian 16-byte/QWC stream and the
channel index. Channel 2 represents GIF PATH3, and VIF1 accepts only
RAM-to-VIF uploads (`CHCR.DIR=1`), not reverse GIF/GS image downloads.
Only normal-mode (`CHCR.MOD=0`) is supported for these channels.
On an accepted transfer, MADR advances, QWC and STR clear, and
D_STAT/COP0 IP3 report channel completion. A missing/rejecting callback,
unsupported mode/direction, or an invalid physical RAM span preserves
STR and the other channel registers, without pretending that the GPU
or VIF executed anything. The callback must reject an unavailable
consumer **before** committing side effects; the DMA host cannot undo
work performed by a callback that returns false.

The optional **GIF PATH3 parser** (`c_ee_gif_path3`) can now be
connected to channel 2 through `bind_device_transfer`. Based on
`ps2-web-recomp/runtime/src/gs/gif.cpp::walk`, it decodes GIFtags
(NLOOP, EOP, FLG, NREG and REGS), packed 128-bit GS register payloads,
reglist 64-bit register values (including odd-register padding), and
image/IMAGE2 quadword runs. PACKED tags with PRE emit a separate PRIM
register event; NREG=0 represents 16 registers. The parser preserves
state across separately completed DMA transfers when a GIFtag's payload
continues in the next transfer. An external consumer receives a single
ordered batch of raw events per DMA transfer, plus offsets into the
original input for image payloads, and must accept the **entire** batch
atomically. On rejection, the parser state stays unchanged and the
DMAC does not advance or raise completion. It does not infer GS register
side effects or perform rasterization; there is deliberately **no**
default event sink and the native game runner still leaves it unbound.

The opt-in **GS register-state sink** (`c_ee_gs_state`) can now receive
the PATH3 parser's complete event batches. Based on
`ps2-web-recomp/runtime/src/gs/gs.cpp::write/write_packed`, it captures
PRIM/PRE, RGBAQ, packed STQ and Q reset, UV, XYZ/XYZF (including
ADC routing), FOG, TEX0/TEX2 merging, per-context texture, frame,
scissor, depth, alpha and test setup, and the image-transfer registers
BITBLTBUF (0x50), TRXPOS (0x51), TRXREG (0x52) and TRXDIR (0x53).
GS now has its own **4 MiB VRAM**, separate from EE RAM and the EE
scratchpad. For PSMCT32, PSMCT24, PSMCT16, PSMCT16S,
PSMT8, PSMT4, PSMT8H, PSMT4HL, PSMT4HH,
PSMZ32, PSMZ24, PSMZ16 and PSMZ16S host-to-local transfers
(TRXDIR=0), it decodes BITBLTBUF's 256-byte destination base units
and 64-pixel buffer width, TRXPOS's 11-bit destination coordinates,
and TRXREG's transfer rectangle. PSMCT32/24 use 64x32-pixel GS pages;
PSMCT16/16S use distinct 64x64-pixel pages with 16x8 blocks and the
reference's shared 16-bit column permutation. PSMCT16S selects a
different block permutation from PSMCT16. PSMT8 uses 128x64-pixel
8-KiB pages, 16x16 blocks and the reference's word/byte swizzle;
its page-row stride is max(1, DBW/2) after integer division.
PSMT4 uses 128x128-pixel, 8-KiB pages with 32x16 blocks and
the reference's 4-bit word/nibble permutation, using the same
minimum page-row stride rule as PSMT8.
PSMT8H, PSMT4HL and PSMT4HH share PSMCT32's 64x32-pixel
page layout and address permutation, but store their indices
in a 32-bit GS word's high byte, low nibble of that byte,
or high nibble of that byte respectively.
PSMZ32/24 use 64x32-pixel, 8 KiB pages with 8x8 blocks,
and PSMZ16/16S use 64x64-pixel pages with 16x8 blocks.
All four depth formats use their own block permutations from
the reference, sharing only their 32- or 16-bit color-column
swizzling tables. PSMZ16S differs from PSMZ16.
PSMCT32 writes four bytes per pixel; PSMCT24 writes three bytes while
preserving its GS word's high byte; PSMCT16/16S write two bytes while
preserving the adjacent halfword. PSMT8 writes one raw 8-bit palette
index per pixel into its swizzled byte position; PSMT4 writes
one raw 4-bit palette index into the selected half-byte,
preserving the other nibble. PSMT8H writes one upper-byte
index while preserving the bottom 24 bits. PSMT4HL/4HH
preserve the other 28 bits of the 32-bit destination word.
These uploads only store indices; they do not convert them to colors.
PSMZ32 stores four bytes per depth value; PSMZ24 stores the low
three bytes of a GS word while preserving its upper byte, and
PSMZ16/16S write two bytes while preserving the adjacent halfword.
Pixel coordinates and byte
addresses wrap at their respective 11-bit and
4-MiB boundaries, matching the reference implementation.

IMAGE/IMAGE2 quadwords can span separately completed GIF/DMA batches.
The transfer tracks completed pixels, ignoring trailing quadword padding
once the rectangle has been filled. The previous bounded 4 MiB raw-image
capture remains available for diagnostics; this is not a texture
decoder or rendered frame. PSMCT32 and PSMZ32 pixels cannot split across 16-byte
IMAGE quadwords; PSMCT24 and PSMZ24 retain incomplete 1- or 2-byte
pixels between event batches. Neither PSMCT16 nor PSMCT16S,
nor PSMZ16 nor PSMZ16S, requires
partial-pixel carry because each GIF IMAGE quadword holds eight
complete 16-bit pixels; PSMT8 has 16 complete one-byte indices per
quadword; PSMT8H also has 16 indices per quadword. PSMT4 and
PSMT4HL/PSMT4HH each have 32 packed 4-bit indices per quadword
(low nibble first). Odd-width rows do not introduce per-row padding;
indices remain packed in raster order across rows. Final RGBA
conversion and texture filtering, other unsupported PSM formats,
host-to-local HWREG is supported through GIF A+D register 0x54.
GS local-to-host
(TRXDIR=1) is supported through read_image and VIF1 reverse DMA.

Every event batch is applied to a temporary state copy and its VRAM
pixel writes are staged until **all** events are supported, input offsets
are valid and any image payload fits the capture bound. Unsupported
host-to-local destination formats (including reserved PSM 0x03),
zero destination buffer width, zero-size transfer rectangles and side
effects such as GS SIGNAL/FINISH/LABEL and TEXFLUSH, or IMAGE
without proper setup, reject the whole batch. Rejection preserves the
GS state, VRAM, and upstream GIF parser/DMA progress. The GS VRAM is
zero-initialized and cleared by the consumer's reset; this is a
deterministic host reset policy, not a claim about physical GS power-on
contents. Applications opt in by
binding `c_ee_gs_state::gif_events` as the GIF parser's event sink.
The native runner deliberately does not bind the parser or GS sink by
default, because a working GS renderer is not yet present.

This is **not** a VIF interpreter, a GS primitive rasterizer, GPU
presentation, device DMA chain engine for channels 0..2, MFIFO, or a
timed hardware queue. Only PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8,
PSMT4, PSMT8H, PSMT4HL, PSMT4HH,
PSMZ32, PSMZ24, PSMZ16 and PSMZ16S IMAGE uploads have
GS VRAM effects;
real SDBZ image-stream and PCSX2 differential validation remain needed.


### GS HWREG host-to-local uploads (A+D register 0x54)

The GIF PATH3 state consumer now implements **HWREG (0x54)** writes
through PACKED A+D as eight little-endian image-data bytes, precisely
as in `ps2-web-recomp/runtime/src/gs/gs.cpp::write()`. HWREG is
valid only after a supported `TRXDIR=0` transfer has been configured.
It shares the same pixel decoding and swizzle staging as IMAGE/IMAGE2,
covering all thirteen color, depth, and indexed PSMs. For 24-bit
PSMCT24/PSMZ24, the unfinished 1-2 pixel bytes are carried across
HWREG writes, IMAGE qwords, and separate GIF DMA calls. Four-bit
indices are decoded low nibble first across odd-width rows. After
TRXREG's rectangle completes, surplus HWREG/image data is ignored
rather than writing extra pixels. A+D and IMAGE transfers in one GIF
event batch retain their original order, including VRAM updates
observed by a later TRXDIR=2 copy or TEX0 palette load.

HWREG writes take part in the existing transactional GIF and DMA
commit: a later unsupported GS register cancels all staged VRAM
pixels, register changes and pending carry. `m_hwreg_write_count`
and `m_hwreg_bytes` record cumulative HWREG input, but HWREG
does *not* increment `m_image_qwc` or append to the IMAGE diagnostic
capture buffer. Reset clears all counters. The current support is
the GIF A+D register path; a separate GS privileged-register/MMIO
HWREG input path is not asserted to exist.

### GS local-to-host downloads (TRXDIR=1)

The GS consumer now supports **TRXDIR=1** local-to-host image readback
from all 13 verified GS pixel formats. The source is selected using
BITBLTBUF.SBP/SBW/SPSM, TRXPOS.SSAX/SSAY and TRXREG.RRW/RRH.
`c_ee_gs_state::read_image(data,qwc)` returns 16*qwc bytes in GS
raster order, unswizzling the stored pixels. 32/24/16/8-bit values
are packed little-endian, and all 4-bit formats are low-nibble first.
PSMCT24/PSMZ24 readback retains partial pixel bytes across separate
DMA reads (a byte-oriented cursor), and a final, partial quadword is
zero-padded. Extra read requests after the rectangle complete also
return zeros, matching the reference's `render_read_image()` behavior.
Pixels and GS VRAM addresses wrap as with GS uploads; the download
does not alter the internal CLUT.

`c_ee_dmac::bind_device_download` connects an optional GS producer
to **VIF1 channel 1 reverse DMA (CHCR.DIR=0)**. The DMAC prevalidates
the entire EE RAM destination range, requests 16*qwc bytes from the
producer and writes them back to EE RAM only after the full request
succeeds. On missing/refusing producer or invalid RAM, MADR/QWC/STR,
the interrupt status and RAM remain unchanged. VIF1 DIR=1 still
uses the existing forward device consumer. Neither direction
implements VIF commands, FIFOs or cycle-accurate DMA scheduling.
The GS readback cursor advances only on successful reads. Host
applications can bind `c_ee_gs_state::dma_download` through
`c_ee_hw_bus::bind_device_download`; no default GS binding is added
to the native runner.

### GS local-to-local VRAM copies (TRXDIR=2)

The state-only GS consumer also executes **TRXDIR=2** at the GS
register write, after decoding BITBLTBUF source and destination
base/width/PSM, TRXPOS source/destination 11-bit coordinates and
DIR (bits 59..60), and TRXREG width/height. It supports all 13
verified PSMCT, PSMZ and indexed PSMT formats on both sides,
including unlike source/destination formats, with raw numerical
pixel values converted by the destination's existing write rules.
PSMCT24/PSMZ24 preserve destination high bytes, 16-bit formats
preserve adjacent halfwords, and indexed 4-bit/8-bit formats
preserve neighboring nibbles and high-byte lanes.

Like the `ps2-web-recomp` reference's `local_copy()`, the copy
reads every source pixel from a **frozen source snapshot** before
applying destination writes. Thus overlapping source/destination
regions behave deterministically. DIR bit 0 reverses the *source*
horizontal pixel selection; DIR bit 1 reverses its vertical selection.
Both source and destination coordinates wrap at 2048. GIF event-batch
ordering is preserved: earlier IMAGE uploads and earlier local copies
in the same batch are visible at a subsequent TRXDIR=2, but later
writes are not. All local-copy writes are staged and either
committed with the entire batch or fully rolled back if a later
event fails, including the copy counters and GS register state.
A safety bound of `k_max_local_copy_pixels` (4 Mi pixels
total staged per GIF batch) rejects oversized copies rather than
allocating unbounded temporary storage. TRXDIR=2 does not consume
GIF IMAGE payload and does not reload an existing CLUT snapshot.

Host-to-local uploads still use TRXDIR=0; device rendering,
other GS GPU behavior, and real SDBZ/PCSX2 differential validation
remain outstanding.

### Indexed CLUT snapshots (CSM1 and CSM2)

The GS state-only consumer now loads a **512-entry raw palette snapshot**
on supported TEX0/TEX2 CLD requests, rather than deriving an index's
color directly from live GS VRAM. CSM1 handles the PSMT8 index 3/4 bit
swap and 4-bit 8x2 entry layout. CSM2 reads TEXCLUT.CBW/COU/COV,
including page-spanning rows. Palette base CBP uses the same
256-byte GS VRAM units as the other GS buffers. Supported CPSM
storage formats are PSMCT32, PSMCT16 and PSMCT16S, using their
existing swizzles. Palette entries are returned as **raw 32- or
16-bit values**; 16-bit channel expansion, TEXA alpha expansion,
TCC/TFX, texturing filters, and rasterization are not implemented.

CLD=0 and CLD=6/7 do not load. CLD=1 forces a load; CLD=2/3
load and record CBP in one of two cached slots; CLD=4/5 skip the load
if the associated CBP matches, otherwise load and replace that slot.
These markers and CLUT contents are kept in transactional GS state,
including across GIF/DMA batches, and cleared by host reset.
TEXCLUT writes alone do not reload a palette. Images written *after*
a TEX0/TEX2 load do not silently change its snapshot; an IMAGE before
TEX0/TEX2 in the **same** event batch is visible to the palette load
using a palette-only staged-byte overlay. A later unsupported event
rolls back both VRAM and palette state.

`read_clut_entry(slot)` and
`read_indexed_clut_value(context,x,y)` are diagnostic read APIs,
not a GS renderer. Indexed reads support PSMT8, PSMT4, PSMT8H, PSMT4HL and
PSMT4HH, returning loaded raw palette words; all 4-bit modes
apply the TEX0.CSA bank selection. The indexed palette storage modes and load semantics here
follow `ps2-web-recomp/runtime/src/gs/gs.cpp::load_clut()`.
Explicitly unsupported CPSM values and formats with unknown CLUT
semantics reject CLD-triggered loads instead of inventing colors.
Real SDBZ/PCSX2 differential validation and full GS rasterization,
texture filtering, palette alpha treatment, the depth test and
depth rasterization, and other GS transfer directions remain outstanding.

`fromSPR` channel 8 also supports
**synchronous chain mode** (CHCR.MOD=1). It copies a preloaded QWC segment
when present, then consumes 16-byte DMA tags from scratchpad, selecting
the tag's upper-32-bit MADR and low-16-bit QWC. It updates CHCR tag bits,
advances SADR over the tag and data (wrapping at 16 KiB), and terminates
on tag ID 7 or on an IRQ tag when TIE is set. An END-tag completion
clears STR and asserts D_STAT channel 8 according to its enabled mask.
Unlike the reference runtime's streaming implementation, this backend
preflights the **entire** chain's physical RAM destinations before any
copy. Invalid spans or an unterminated cycle (limited to 4096 tags)
leave STR set, preserve channel registers, and do not partially copy
RAM or raise a false completion interrupt.

**toSPR** channel 9 now implements the reference's general DMA
chain mode (`CHCR.MOD=1`), including a preloaded QWC segment and the
following tag IDs: REFE (0), CNT (1), NEXT (2), REF (3), REFS (4),
CALL (5), RET (6) and END (7). DMA tags live in EE RAM at TADR,
carry QWC, destination/source-selection and IRQ flags, and can choose
inline payload or an aligned physical RAM reference. CALL/RET use two
address-stack registers (ASR0/ASR1 at channel offsets 0x40/0x50);
TIE-enabled IRQ tags terminate after their payload. Transferred
quadwords go to scratchpad, with SADR wrapping at 16 KiB.
TADR, MADR, CHCR tag/stack fields, QWC, STR, D_STAT and COP0 IP3 are
updated on successful completion.

As in the fromSPR implementation, all tag addresses and source RAM
spans are checked on a read-only pass before any scratchpad writes.
Malformed tags, unbacked RAM and nonterminating loops (4,096 tag
guard) leave STR set and do not fabricate completion. This differs
from real PS2 streaming side effects; differential ELF/PCSX2
validation remains necessary. MFIFO, cycle scheduling, and
device-side DMA interpretation remain unsupported.

The EE DMA control register `D_SQWC=0x1000e030` is now backed by the
shared hardware bus. For SPR channels 8 and 9 in `CHCR.MOD=2` when
`D_SQWC.TQWC=(D_SQWC >> 16) & 0xff` is nonzero, **interleave mode**
copies one quadword per RAM slot, skips `D_SQWC.SQWC=D_SQWC & 0xff`
RAM quadwords after each group of TQWC transfers, and always consumes
scratchpad source/destination quadwords contiguously, wrapping at 16 KiB.
The final group's skip is also included in the resulting MADR.
Both directions update MADR, SADR and QWC, clear CHCR.STR, and latch
channel completion via D_STAT/IP3. When TQWC is zero, the reference
falls back to ordinary contiguous normal DMA even if MOD=2.
As with normal DMA and fromSPR chains, **every RAM qword is
prevalidated** before any writes; an invalid sparse transfer leaves
data/registers intact and does not generate a false completion.
These synchronous rules follow
`ps2-web-recomp/runtime/src/dmac.cpp::run_interleave`; hardware
cycle timing remains unimplemented.
A translated native dispatch now samples pending, enabled EE COP0
interrupts **at mapped function-entry boundaries**. When an interrupt is
pending and IE/EIE/IM permit it, the existing exception helper sets
Cause.ExcCode=Int, EPC to the next guest entry PC, Status.EXL and the
configured BEV exception vector, without invoking the next guest function.
This also detects pending levels immediately after a translated ERET returns.
The synthetic PC-zero entry sentinel is excluded. A `max_dispatches` limit
can postpone a checkpoint until the next call; the pending level persists.
The runner reports the exception rather than misdiagnosing it as a missing
entry, but it cannot execute an unavailable BIOS handler.

This is **not** cycle-accurate EE interrupt delivery. A translated function
that runs indefinitely never reaches the next checkpoint; handling
interrupts within such code requires instrumentation and a timed scheduler.
The host does not yet provide device event timing, complete guest BIOS
interrupt handlers, or general GS/SIF/GIF/VIF behavior.

This still does **not** implement MFIFO,
TLB translation, VU1, guest address/alignment exceptions, or comprehensive
hardware-specific device semantics. VU0 code writes are visible to the current interpreter fetch;
future compiled microcode engines will need explicit invalidation if
they cache decoded programs. `--hle-startup` is opt-in and only covers explicitly
recognized kernel calls; it does not implement BIOS, IOP, GS, SIF or general DMA.

## Confirmed real-ELF checkpoints

For the original US executable `SLUS_214.42`:

- ELF entry is **0x00100008**; main PT_LOAD begins at **0x00100000**.
- Initial entry CFG: **177 supported instructions** across **26 blocks**;
  LLVM IR parses/verifies and emits an x86-64 object.
- Full executable scan previously discovered **7,901 candidate functions**.
- At entry **0x001001c8** the guest invokes `SetupThread` (0x3c);
  at **0x001001e4** it invokes `SetupHeap` (0x3d).
- The native boot path calls into **0x0017f018**, **0x0017edc8**,
  **0x00174ca0**. The experimental host then successfully services two
  `CreateSema` (0x40) invocations, each with init/max count 1.
- The guest registers custom syscall **0x83 → 0x0017ee80** and
  **0x5a → 0x0017ee48** through `SetSyscall` (0x74).
- With seven native translations, the trace reaches **20 function
  dispatches** and stops at **0x0017eec4** invoking registered syscall
  **0x83**. The call arguments reference EE KSEG0 addresses
  **0x80000000..0x80080000**.

The native runner now has an experimental registered-syscall handoff:
when a SetSyscall-installed service is invoked, it dispatches the registered
guest handler as translated code, preserving the interrupted RA. On a return
through a dedicated synthetic return address it restores RA, clears EXL, and
resumes the instruction after the original syscall. Callback V0 and guest
memory writes are retained. A missing translated handler is surfaced as a
normal external dispatch PC so `--auto-expand` can compile it on replay.
Unexpected exceptions or departures from compiled code are *not* treated as
successful callback returns.

The user's submitted `first-boot.json` confirms the previous 20-dispatch
stop at `0x0017eec4`: COP0.Cause 0x20 (syscall), EPC `0x0017eec4`,
service `0x83`, with KSEG0 arguments `0x80000000` and `0x80080000`.
The installed handler is `0x0017ee80`, which is real game code. Its
execution and eventual return require a fresh game run; reaching a higher
PC or dispatch count must be verified rather than assumed.

The callback path is not a PS2 kernel exception implementation: nested
callback handoffs, accurate privilege/interrupt scheduling, and BIOS-specific
callback ABI details remain outside the currently verified scope.

## Why the callback keeps repeating

Disassembly of the user's US `SLUS_214.42` ELF confirms this isn't
necessarily a hardware-polling loop:

- Guest handler `0x0017EE80` loads 32-bit words from address A0 forward to
  address A1, comparing against A2. It returns the matching address in V0,
  or zero on an unsuccessful search.
- Startup function `0x0017EF08` invokes that handler through the registered
  `0x83` syscall, initially scanning physical EE RAM mirrored through
  `0x80000000..0x80080000` for the function addresses `0x0017EE80`
  and `0x0017EE48`. The caller subsequently adjusts the found addresses
  and repeats searches until a specific relative layout condition matches.
- The RAM-only runner previously stored successful SetSyscall(0x74)
  registrations solely in a native `installed_syscall_handlers` array.
  The equivalent `ps2-web-recomp` kernel HLE ALSO writes the registered
  handler to physical guest RAM at `0x00001800 + 4 * syscall_number`
  (KSEG0 `0x80001800 + 4 * syscall_number`), making the address visible
  to the game's kernel-memory search. The runner now mirrors successful
  registrations into the same guest-visible table, without inventing
  arbitrary kernel-memory contents. Other BIOS-initialized kernel data
  may still be required; a higher dispatch count is not proof of boot.

The runner's `--hle-startup --kseg-ram` path now mirrors the syscall
table without a snapshot. A synthetic ELF integration test verifies
uninitialized entries, two distinct handler slots, replacement of a
previously installed handler, and physical/KSEG0 alias reads.
For example, service 0x83 maps to physical 0x00001A0C, and service
0x5A maps to physical 0x00001968. Invalid registrations continue to
be rejected before altering the host dispatch table or guest RAM.
This behavior is modelled after `ps2-web-recomp/runtime/src/kernel.cpp`
(`kSyscallTable` and syscall 0x74). The existing guest-callback
integration test still verifies actual registered callback execution.

The runner also accepts `--kernel-ram=FILE --kseg-ram` to load an **optional,
exactly 524,288-byte raw snapshot** of physical EE RAM addresses
`0x00000000..0x0007FFFF` (equivalently KSEG0 `0x80000000..0x8007FFFF`)
*before* loading the game's ELF segments. This should be captured from an
actual running PS2/PCSX2 environment **at the game entry point**, before
these kernel patch searches execute. A PS2 BIOS ROM dump is not the same
as an initialized EE RAM snapshot. No proprietary BIOS image is bundled.

First test `--hle-startup --kseg-ram` **without** `--kernel-ram`.
Only if a later verified kernel-memory dependency persists, use
`--no-recompile --auto-expand=16 --kernel-ram=/path/to/ee-kernel-ram.bin`
with the existing translated library to test a captured snapshot.
A snapshot only supplies memory data; it does not create missing
kernel threads, interrupts, DMA or device behaviour. The table
mirroring is scoped to accepted SetSyscall registrations, not a
complete synthesized PS2 BIOS/kernel RAM image.

### Verified US SDBZ first boot without a kernel snapshot (2026-11-15)

The user's actual US `SLUS_214.42` boot trace confirms that the
guest-visible SetSyscall table solves the first kernel handler lookup:
service `0x83` returns `0x80001A0C` for `0x0017EE80` and
`0x80001968` for `0x0017EE48`, with
`--hle-startup --kseg-ram` and no `--kernel-ram`. Startup then
installs a second handler for service `0x5A`, invokes that guest
callback successfully twice, and reaches native dispatch 41.

The new first unsupported service is syscall `0x64` at guest
EPC `0x00174F24` (A0=0, V1=0x64). This is
`FlushCache`, not an unsupported instruction. The reference
`ps2-web-recomp/runtime/src/kernel.cpp` handles both
`0x64` (FlushCache) and `0x68` (iFlushCache) by returning zero.
The native runner now implements the same opt-in HLE for both
services, preserving its syscall instruction-context validation and
continuing to reject unrecognized services. This is suitable for
the current statically translated immutable executable image; it
does not implement self-modifying guest code or invalidate JIT
translations. A synthetic ELF test checks both return values,
the unsupported-service boundary and opt-in behavior.

A real-game rerun after this change is required to establish the
next boundary; the previous 41-dispatch result is **not** a boot.

### Dynamic kernel RAM handler registration (US SDBZ, dispatch 47)

After the two FlushCache calls succeed, the next actual US SDBZ
stop is SetSyscall at EPC 0x0017F494 with service number 0x5B
and handler pointer 0x80076000. ELF disassembly explains the
sequence: 0x0017F4A0 calls the previously registered 0x5A
guest copy syscall twice; 0x0017F540 installs 0x740 bytes from
source 0x00462A70 at KSEG0 0x80076000 and 0x0017F558
copies another 0x28 bytes to 0x00082000. The caller flushes
instruction caches with modes 0 and 2 before registering the
relocated code as syscall 0x5B.

The reference ps2-web-recomp accepts this SetSyscall pointer,
writing it into the physical kernel table without requiring
its address to be an executable ELF segment. The native runner
now accepts aligned, backed KSEG0/KSEG1/direct RAM handler
addresses, and accepts zero to clear handlers, in addition to
normal compiled-ELF callbacks. It continues to reject
misaligned and unbacked pointers. Out-of-range indices are not installed.
A synthetic ELF runner test registers KSEG0 code, verifies the
guest-visible table and physical RAM, checks zero unregister,
and confirms invoking an uncompiled RAM handler produces an
explicit refusal rather than silently executing invalid code.

**Next real-game milestone (observed):** US SDBZ immediately
invokes the registered 0x80076000 handler at dispatch 50,
service 0x5B, EPC 0x0017F4EC, A0=0xFC. The US ELF's
0x00462A70 source blob begins with a small lookup routine:
its six-entry table at relocated 0x80076710 maps 0xFC to
0x80076440. Do not hardcode the lookup result as an HLE.

The native runner now includes a bounded, fail-closed MIPS
RAM-instruction interpreter for a registered, backed, non-ELF
callback. It supports the initial lookup's LUI/ADDIU/LW/BNE/
SLTIU/DADDU/JR and delay slots, plus a deliberately limited
set of scalar ALU/memory/branch operations. The host preserves
the same sentinel-return and V0/memory side-effect semantics
as compiled callbacks. Unsupported instructions, nested
delay-slot branches, unbacked memory and infinite loops
stop with diagnostic PC/opcode/instruction counts. This
does NOT claim the entire 0x740-byte handler blob can
execute or that later dynamic code is complete. Running
the real ELF again is required to establish the next
unsupported instruction or kernel service.

### Out-of-range SetSyscall registration (US SDBZ, dispatch 77)

The next actual no-snapshot US boot reaches 77 native dispatches.
The relocated 0x80076000 callback executes five lookups
for keys 0xFC, 0xFE, 0xFD, 0xFF, and 0x12C, returning their
guest handler pointers and allowing successful registration of
the first four services. At dispatch 77, SDBZ requests
SetSyscall(0x12C, 0x80076488) from EPC 0x0017F494.

The reference ps2-web-recomp/runtime/src/kernel.cpp does not
register a handler for a syscall number >= 0x100: it logs the
unsupported index, returns zero, and continues. The native runner
previously stopped at this request; it now follows the reference,
reporting an ignored out-of-range registration while leaving
both native installed handlers and guest-visible kernel table
unchanged. The table address is not computed until the index
is validated, preventing overflow or accidental writes beyond
the 256-entry table. Successful and ignored SetSyscall requests
both return V0=0. Invalid in-range handler pointers still fail.

A synthetic ELF test installs a valid handler, requests
registrations for 0x12C and 0xFFFFFFFF, checks both returns
are zero, and reads the original kernel table entry and
the word just beyond its boundary to ensure neither changed.
A real-game replay is needed to identify the next blocker.

### R5900 MMI vector-fill blocker (US SDBZ, dispatch 88)

With ignored SetSyscall(0x12C) matching the reference kernel,
the latest real US boot resolves six relocated RAM callbacks,
installs syscall 0x08 at 0x800766C0, and continues through
0x0017F670 into 0x0018E408. Auto-expand discovers this
48-instruction MIPS function but reports two unsupported
instructions. At dispatch 88, execution halts at 0x0018E434.

Exact US ELF words and R5900 classification:
- 0x0018E434: 0x70081EE9 = PCPYH, MMI3 function 0x29,
  sub-op 0x1B. This broadcasts rt's low halfword into
  each 16-bit lane of the low doubleword, and independently
  broadcasts rt's upper doubleword's low halfword across
  the high doubleword. rs must be zero.
- 0x0018E440: 0x70634389 = PCPYLD, MMI2 function 0x09,
  sub-op 0x0E. It writes rd.lower64 = rt.lower64 and
  rd.upper64 = rs.lower64.

The native LLVM builder now emits both 128-bit operations
exactly, the decoder marks these encodings supported, and
the pure MMI reference helper and lifter use the same semantics.
A multi-opt-level LLVM MCJIT regression verifies distinct
source halves, both operand orders, and rd/rs aliasing.
The real-game vector-fill routine needs recompilation: existing
translated DLLs and cached per-function LLVM IR from before
this fix still contain an UNSUPPORTED exit. The runner now guards
`--no-recompile` with input and binary fingerprints so subsequent
compiler/runtime changes cannot silently reuse such stale code.
This is **not** a new real-game boot milestone: a fresh translation
of the US ELF after PR #32 is still needed to establish whether
the next obstacle is a guest instruction, HLE service, or device dependency.

On Windows, use the Visual Studio Developer Command Prompt (x64):

```bat
cd /d C:\Dev\MIPS-To-LLVM-IR-To-C
git pull --ff-only
cmake --build build --parallel 2
set "CXX=C:\Program Files\LLVM\bin\clang++.exe"
python tools/run_game.py "PS2_GAME\SLUS_214.42" --build-dir build --work-dir build\sdbz-mmi --hle-startup --kseg-ram --auto-expand=32 --max-dispatches=100000 --timeout-seconds=120 --trace --report build\sdbz-mmi\first-boot.json
```

Do **not** add `--no-recompile` on the first replay after the fix.
No new US ELF progress beyond dispatch 88 is claimed until this
local replay is captured.

### EE interrupt-handler registration (US SDBZ, dispatch 92)

A **fresh** Linux replay using the user's privately supplied US ELF
(SHA-256 `1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`)
and the CI-built translator after PR #32 verifies the previous vector-fill
blocker is cleared. The function at 0x0018E408 has all 48 instructions
supported, returns through dispatch 88 to 0x0017F6CC, then the game
calls 0x00180940 (dispatch 89) and enters 0x00174990 (dispatch 91).

The first new real-game stop is **dispatch 92**, at an ordinary MIPS
SYSCALL instruction 0x0000000C (EPC 0x00174994, exception code 8,
Cause 0x00000020, Status 0x00000002). The guest requests service
V1=0x10, A0=11, A1=0x0017FC10, A2=0, A3=0, with a GP register of
0x00503070. This is **AddIntcHandler**, confirmed by
`ps2-web-recomp/runtime/src/kernel.cpp::handle_syscall/add_handler`,
not a CPU instruction or missing ELF translation. The current
runner fails closed rather than inventing an HLE result.

The new INTC/DMAC kernel HLE implements AddIntcHandler (0x10),
RemoveIntcHandler (0x11), AddDmacHandler (0x12) and
RemoveDmacHandler (0x13) according to the reference. Both handler
lists share a monotonically assigned ID sequence beginning at 1.
Add preserves cause, address, A3 argument, and guest GP; A2=0
inserts at the front, A2=-1 appends, and any other A2 ID inserts
before that ID (or appends when not found). Remove deletes matching
IDs and returns the number of *remaining* handlers for the requested
cause, rather than a success flag. This HLE is opt-in under
`--hle-startup`. A synthetic MIPS ELF executes these
kernel services and checks return registers and traces across
both lists, without relying only on decoder classification.

**Unverified after this change:** the real US ELF must be replayed
to establish whether AddIntcHandler registration succeeds and which
instruction, kernel service, or hardware action comes next. Handler
registration does not yet dispatch guest callbacks on hardware
interrupts; IRQ acknowledgement/delivery remains incomplete and is
not silently simulated. No BIOS or callback results are fabricated.

### Alternate entry into an already translated syscall wrapper (US SDBZ, dispatch 97)

The real US ELF replay after the interrupt-handler registration HLE
successfully executes `AddIntcHandler(11, 0x0017FC10, 0)` at dispatch
92, returning ID 1. It then traverses `0x0017ED60` (dispatch 95)
and `0x0017F640` (dispatch 97), reaching the **interior instruction**
at `0x00174994`. The ELF contains `SYSCALL` (`0x0000000C`) at
that PC, immediately after a `v1=0x10` instruction at
`0x00174990` and before `JR $ra` at `0x00174998`.

When compiled independently from `0x00174994`, the native lifter
correctly includes the shared successor block `0x00174998`.
The original `0x00174990` translation already owns that return block.
Previously, `run_game.py` rejected the overlap and stopped
auto-expansion even though the new entry was valid.

The driver now keeps the existing dispatcher owner of each overlapping
guest block PC and publishes only the *newly uncovered* entry PCs
from an additional translation. The actual LLVM function is not
truncated: all successor instructions remain compiled and executable
along internal paths. The native dispatcher still exposes exactly one
owner per guest block entry, while direct branches into different
entry points retain their CPU/register semantics. This supports
alternate entries into shared immutable ELF code; it is not a solution
for mutable guest-generated code or arbitrary overlapping instruction
sequences.

A synthetic ELF executes a direct `JAL` to a syscall wrapper followed
by a computed `JALR` into the same wrapper's interior `SYSCALL`
instruction. Both calls must complete via native LLVM-generated code,
and the shared successor PC must occur exactly once in dispatcher
metadata. The real US ELF must be replayed again after this change
to establish the next blocker beyond dispatch 97.

### R5900 ERET ERL/ErrorEPC semantics (US SDBZ, dispatch 98)

After the alternate-entry metadata change, a fresh real US ELF
translation now runs to **dispatch 98**, entering the SYSCALL
instruction at 0x00174994 again with V1=0, and stops at
0x80000180 with exception code 8. The exception is *not*
evidence that SDBZ expects syscall 0: the earlier COP0
return sequence was wrong.

The ELF itself contains `MTC0 $ra, ErrorEPC` at 0x0017F62C followed by
`ERET` at 0x0017F634. The native runtime used to **always** clear
EXL and return from EPC, regardless of the ERL flag. Consequently
ERET resumed at the previous syscall EPC (0x00174994) rather
than the guest's programmed ErrorEPC. This produced the spurious
second syscall and blocked initialization.

The R5900 architectural behavior, also present in
`ps2-web-recomp/runtime/src/kernel.cpp::eret`, selects
ErrorEPC and clears **ERL** if Status.ERL is set. Otherwise
it selects EPC and clears **EXL**. The unrelated status bit
must remain unchanged. Native MIPS ELFs explicitly load
different ErrorEPC and EPC targets via MTC0, execute ERET,
and check the resulting control flow, status, and register
contents in both EXL and ERL cases. An additional native
unit regression checks all combinations including ERL and EXL
simultaneously.

**Unverified after this fix:** the original US ELF must be
replayed to establish the actual target of its ErrorEPC
return and the next initialization dependency. No fake
ErrorEPC or BIOS register contents are introduced.

### EE interrupt mask syscalls (US SDBZ, dispatch 104)

After correcting ERET, the original US ELF reaches dispatch
**104**, with a SYSCALL at 0x001749E4 (opcode 0x0000000C,
V1=0x14, A0=11; the caller is 0x001757E8). This is
`EnableIntc(11)`, confirmed by the reference kernel.
The earlier failure at dispatch 98 has been cleared by the
ERL/ErrorEPC fix, and the shared native code translations at
0x0017F650 and 0x0017F660 are traversed correctly.

The reference `ps2-web-recomp/runtime/src/kernel.cpp` services
0x14/0x15 (EnableIntc/DisableIntc), 0x16/0x17
(EnableDmac/DisableDmac), and 0x1A–0x1D interrupt-context
variants use `hw::intc_set_mask` / `hw::dmac_set_mask`.
They return **1 only if a mask bit changes**, otherwise 0.
The native runner now routes these calls through the
existing EE hardware MMIO models: INTC_MASK uses write-one-
to-toggle at 0x1000F010, and DMAC D_STAT uses the upper
16 write-one-to-toggle mask bits at 0x1000E010.
Reads and pending interrupt levels remain consistent with
those device models. Invalid causes/channels fail closed;
no phantom interrupt callback is invoked.

A synthetic MIPS ELF runs all regular and interrupt-context
variants, verifies repeated enable/disable return values and
reads INTC_MASK and D_STAT from guest code after changes.

**Unverified after this change:** replay the real US ELF from
fresh or provenance-compatible translated code to establish
the next concrete boot blocker. Native interrupt delivery
and guest callback scheduling are not implemented by these
mask-management services alone.

### EE dormant thread creation (US SDBZ, dispatch 126)

The last captured native US ELF trace reached dispatch **126** after
enabling INTC cause 11 and creating a third semaphore. At syscall
EPC 0x00174AA4, V1=0x20 and A0=0x01FFEF60, the guest requested
`CreateThread`. This is a kernel HLE blocker, not an
unsupported R5900 instruction.

The reference `ps2-web-recomp/runtime/src/kernel.cpp::create_thread`
allocates the lowest free ID in [1, 255], creates a *dormant* thread,
and copies the descriptor's entry (+4), stack (+8), stack size (+12),
GP (+16), priority (+20), attributes (+28), and options (+32).
The native runner now reserves ID 1 for the already-running ELF
main thread, copies these fields from a backed, aligned 36-byte guest
descriptor, and returns the new ID. Exhausting the ID pool returns
-1. Invalid guest descriptor memory is rejected rather than
inventing a thread. No new thread executes at CreateThread time.

A synthetic ELF native execution regression checks both successful
registrations (IDs 2 and 3), the descriptor fields, and refusal of an
unaligned pointer, alongside opt-in HLE behavior. Existing
`StartThread` (0x22), scheduling, and context switching
remain unsupported: these must be implemented with preserved EE CPU
and thread state, not by treating new threads as no-ops.

**Next real-game test is user-owned:** after this code passes CI
and is merged, the user will run the private US ELF locally and
supply the JSON report and trace. CI contains no proprietary ELF
and does not claim progress beyond dispatch 126.

### EE GetThreadId syscall (US SDBZ, dispatch 133)

The user's local `boot.log` and `first-boot.json` after the
CreateThread PR confirmed that the runner reached **133 native
dispatches**. At dispatch 126 the native HLE created a dormant
thread, ID 2, entry 0x001759A0. At dispatch 133 the guest executed
SYSCALL at EPC **0x00174B94**, service **0x2F** in V1, which is
`GetThreadId`; the CPU's A0 register happened to contain thread
ID 2, but GetThreadId takes *no arguments*. It must report the
**currently running thread**, which remains main thread ID 1;
CreateThread does not schedule the dormant thread.

The reference `ps2-web-recomp/runtime/src/kernel.cpp` implements
`GetThreadId` as a read of `g_current`; `start_main_thread` sets
`g_current=1`. The native runner now tracks `current_thread_id=1`
separately from the 256 thread descriptors and returns it from
HLE service 0x2F without switching context. No StartThread or
thread scheduling is simulated. A synthetic EE ELF regression calls
GetThreadId before and after CreateThread, requires results 1 and 1
while the dormant thread receives ID 2, and confirms the service
does not consume A0.

**Performance observed in user log:** initial full discovery 10,371.9 ms,
subsequent cached discovery about 245–250 ms per emitted function;
per-function LLVM IR emission about 1–5 ms. Cached discovery was
confirmed in the user's local run. Remaining costs include the
per-function rebuild/relink/replay cycle. Further native game behavior
beyond dispatch 133 remains unverified until the user reruns the ELF.

### EE ChangeThreadPriority (US SDBZ, dispatch 136)

In the user's next local US SDBZ run the native translation cache was
successfully reused without additional compilation. The game reached
**dispatch 136**: at EPC `0x00174B34` it executed SYSCALL service
`V1=0x29`, with `A0=1` (main thread ID) and `A1=1` (requested
priority). The previous `GetThreadId` service at dispatch 133 returned
main thread ID 1 as expected.

The reference `ps2-web-recomp/runtime/src/kernel.cpp` handles
`ChangeThreadPriority (0x29)` and `iChangeThreadPriority (0x2A)`
with the same priority bookkeeping: thread ID zero aliases the
current thread; unknown IDs return -1; the call returns the thread's
**old** priority; and the new priority is masked to 7 bits (0..127).
The native runner now implements these metadata and return semantics.
The initial main thread has priority 0; this observed call returns
0 and updates the main thread's priority to 1.

**Scheduling remains deliberately unsupported.** A priority change
cannot activate dormant threads. The reference kernel may trigger
rescheduling when multiple runnable threads exist; implementing that
requires real CPU-context switching and scheduler behavior rather than
fabricating a successful thread switch. Native synthetic tests cover
old/new priority bookkeeping, ID 0 aliasing, a dormant thread's
priority, the interrupt-safe variant, 7-bit masking, invalid IDs,
and continued execution of main thread 1.

Only the user runs the proprietary US game ELF; native CI runs
synthetic fixtures and does not assert progress past dispatch 136.

### EE OSD configuration syscalls (US SDBZ, dispatch 142)

The next user-owned SDBZ log (`boot(3).log`) reused 33 cached
functions, then incrementally compiled `sub_0x17F140` as function 34.
After `ChangeThreadPriority` updated the main thread's priority
from 0 to 1, execution reached **dispatch 142** with syscall
`V1=0x4B` (`GetOsdConfigParam`) at EPC `0x00174D54`.
`A0=0x01FFEF80` points into backed guest RAM.

The reference `ps2-web-recomp/runtime/src/kernel.cpp` implements
`SetOsdConfigParam (0x4A)` as a 32-bit read from the guest pointer
and `GetOsdConfigParam (0x4B)` as a 32-bit write to the guest pointer,
both returning zero. A default 0 OSD word represents the neutral
configuration in this runner, which has no widescreen host option
yet. The native runner now holds this configuration word and uses
the existing guest-memory abstraction for all reads/writes.
Invalid or unaligned guest pointers are refused rather than
silently succeeding or changing kernel state.

A synthetic MIPS ELF regression verifies the default word, a
Set-to-Get round trip with `0x12345678`, zero return values,
unmapped GET and unaligned SET pointer rejection, and the
existing opt-in `--hle-startup` boundary.

Only the user executes the private US game ELF. Progress
beyond dispatch 142 is unverified until another local test.

### Expansion budget, compact logs and cached multi-pass boot (US SDBZ, dispatch 1274)

The user's fourth local run, after OSD configuration HLE, reached
**1,274 native dispatches** before stopping at guest PC `0x004E2640`.
It compiled **64 additional function entry PCs**, growing the native
library to **100 functions** with no failed LLVM translations or
unsupported-instruction reports. The stop was a `DISPATCH(3)` with
`status=0` at the *last permitted replay*, not a new unhandled kernel
service: the Windows launcher previously used `--auto-expand=64`,
so it exhausted its expansion budget immediately before it could
translate the next PC.

`tools/run_boot_session.py` now wraps the existing, cache-provenanced
`run_game.py` without changing the translator or its fingerprint.
The launcher performs up to **two bounded passes of 128 new guest
functions each**. If a pass exhausts its limit *and* the report proves
a missing compiled `DISPATCH(3)` exit, the next pass uses
`--no-recompile` to keep the native functions already built.
Exceptions, non-dispatch exits, timeouts and compile errors are not
misrepresented as reasons to continue blindly.

During expansion, tracing is disabled to avoid repeating complete
guest dispatch traces for each relink/replay. The wrapper saves
`expansion.log` (all per-pass compilation/link diagnostics), then
performs **one final traced replay** to produce the familiar
`boot.log` and `first-boot.json` for debugging. Errors during
expansion copy diagnostics into `boot.log` rather than leaving a
stale success trace. Work persists beyond the two-pass budget for
the next invocation; the 256-translation ceiling is intentional.
The next real-game frontier after `0x004E2640` is not yet verified.

Synthetic Python orchestration tests use mock runner reports to
exercise multiple passes, reuse flags, halting on a real blocker,
preserving errors, and final trace generation. The private ELF
continues to be tested only by the user on Windows.

### Real-game replay 70: wall-clock timeout with 170 guest functions (US SDBZ)

The user's fifth local run used cached ELF translation and successfully
compiled **70 additional functions**, increasing the linked guest library
from 100 to **170 functions**. The next replay stopped due to the
**120-second wall-clock limit**, not function-discovery exhaustion:
`run_game.py` reported `replay=70`, with its last durable guest
snapshot at **5,374 dispatches**, PC `0x0030BBB4`. The report was
`snapshot_kind=in_progress`, not `stopped`, so the last sampled
PC need not be the exact instruction at host timeout.

The final 64-dispatch history alternated between `0x0030BBB4`
and `0x0030C1A0`. Static disassembly of the user's US ELF
shows that `0x0030BB70` performs a bounded 12-digit conversion
with a per-digit call to `0x0030C1A0` (the one-byte writer).
Consequently repeated visits alone do not establish an infinite
loop or a new kernel-service blocker.

The native runner now writes its in-progress JSON checkpoint
every **128 dispatches**, or when the guest syscall callback state
changes, rather than every 16. This reduces repeated file
reopen/truncate/serialize work eightfold, while preserving
on-exit full stop state. It also prints periodic
`EE progress:` lines every **256 dispatches**, reporting:
wall elapsed milliseconds, accumulated guest native-dispatch
milliseconds, accumulated report-write milliseconds, and the
slowest completed native dispatch PC/duration. A terminal
`EE timing:` line reports totals for a normally completed
runner. These diagnostics distinguish checkpoint overhead from
native instruction work without guessing, and do not assume
the game has booted correctly.

Only `src/tools/ee_game_runner_main.cpp` is modified;
the translator, object linker, and native DLL remain unchanged,
preserving the 170-function translated cache. The next user-owned
Windows run is needed to measure the new timing counters and
determine whether the slowdown is a native loop, a long
game initialization, or report/dispatch overhead.

### Translated guest-loop safepoints after replay-0 timeout (US SDBZ)

The sixth Windows run confirms that the previously observed stall is
**not** an ordinary 120-second startup: the runner processed **5,376
native dispatches in 299.7 ms**, of which 4.4 ms were native function
calls and 294.6 ms were durable report-writing. It then stopped
returning to the host dispatcher and timed out at 120 seconds.
The last guest return PC was `0x0030BBB4`; the guest-visible
snapshot is the last *completed* dispatch, not the actual instruction
currently executing inside the stalled native function.

A guest CFG can loop forever *within one generated LLVM function*,
bypassing both `--max-dispatches` and the host's durable progress
snapshots. To make these failures diagnosable without killing a
stalled process, the CFG lifter now instruments **backedge destination
headers only** with an opt-in visit budget. The budget is stored in
two appended `s_ee_runtime` fields. At every translated guest
function entry, the host resets the remaining count. Upon exhausting
the configured number of header visits, the generated code exits
with `EE_EXIT_LOOP_BUDGET (5)` and writes the *exact header guest PC*
to `cpu.m_pc`. This is a deliberate diagnostic stop, **not** a
PS2 architectural exception or a successful boot. Before the guard
can exit, backwards dataflow liveness forces predecessor blocks
to flush all architectural GPR changes; this prevents a watchdog
exit from returning stale register values.

The direct native runner has a new `--loop-budget=N` option
(default 0, disabled). The Windows batch selects 1,000,000
loop-header visits so a sustained non-yielding guest loop stops
cleanly long before the OS-level timeout. Non-branch CFG paths
and guest programs using a null `s_ee_runtime*` are unaffected.
A synthetic counting-loop ELF regression checks a 32-iteration
budget, unchanged architectural t0 value, precise loop-header PC,
and a stopped JSON snapshot.

**Translation cache caveat:** Unlike runner-only fixes, these
safepoints change the emitted machine code and require rebuilding
translated guest functions. The provenance check intentionally
rejects an old native library, to avoid running unguarded guest
objects. It does not delete the work directory, but the new run
re-emits objects and discovery data using the updated recompiler.
Once the next trace returns a precise loop-header PC, investigate
the guest arithmetic/branch or relevant device state before
raising the loop limit.

### EE semaphore syscalls following guarded boot (US SDBZ, dispatch 5848)

After the loop-safepoint retranslation, the user's seventh real-game
run rebuilt **184 translated functions** and executed **5,848
dispatches**. The prior freeze around dispatch 5,376 did not recur:
the last traced replay completed in roughly 0.52 seconds.
The next genuine unsupported kernel service is `0x44`
(`WaitSema`) at EPC `0x00174CE4`, with A0/semaphore ID 1.
The main thread is still ID 1; the earlier CreateSema for ID 1
initialised it with **count=1** and max=1.

The reference kernel's `wait_sema` consumes one available count
and returns the requested ID. Only a wait on count zero blocks the
running thread and requires scheduling. The runner now implements
the immediately-completing paths for `WaitSema (0x44)`,
`SignalSema/iSignalSema (0x42/0x43)`,
`PollSema/iPollSema (0x45/0x46)`,
`DeleteSema (0x41)`, and
`ReferSemaStatus/iReferSemaStatus (0x47/0x48)`.
It keeps the count, initial count, maximum, attributes and option
of each 64-slot kernel semaphore. Polling an unavailable token
returns -1; signaling a semaphore already at its maximum fails
without incrementing; a nonexistent/deleted ID returns -1.
Status queries write the six-field 32-bit guest ee_sema_t layout,
reporting zero waiters because no guest wait has been scheduled.

**Blocking WaitSema still fails closed**: the guest remains at
its SYSCALL exception with EPC/EXL intact and a diagnostic
saying the EE thread scheduler is required. This avoids
fabricating a successful wait or jumping to an uninitialized
thread, and preserves the known-dormant CreateThread children.
Synthetic ELF tests check count consumption, signal/poll,
overflow, delete/invalid ID, status layout, refusal of a zero
count blocking wait and unmapped status pointers.

These changes are runner-only; the new 184-function translation
cache stays valid. The real game must be rerun to confirm the
next actual bootstrap boundary.

### EE heap-boundary syscall after semaphore progress (US SDBZ, dispatch 5861)

The eighth user-owned US SDBZ run confirms that semaphore ID 1's
`WaitSema` fast path succeeded at dispatch **5848**, decrementing
its original count from 1 to 0. Auto-expansion then compiled **five**
additional guest functions (**184 -> 189**) with no unsupported
instructions, reaching dispatch **5861**. The new genuine
SYSCALL stop is `EndOfHeap (0x3E)` at EPC `0x00174C84`
(A0=0x01000020, but this syscall takes **no arguments**).

The `ps2-web-recomp` EE kernel's `SetupHeap (0x3D)` stores
the resulting heap-end boundary on the *current thread* and
returns that same value in V0. `EndOfHeap (0x3E)` returns the
thread's saved heap end. The native runner previously only
validated and logged the heap extent during SetupHeap;
it neither stored it nor updated the result register.
This discrepancy also meant that the caller's later heap
queries could not work even with a standalone 0x3E stub.

The runner now saves the successfully validated 32-bit
heap boundary in `threads[current_thread_id].m_heap_end`,
sets V0 on both SetupHeap and EndOfHeap, and rejects
invalid current-thread state instead of inventing a
boundary. The existing memory-range and stack-overlap
checks still run *before* committing heap state.
The initial unconfigured heap end is zero. This is
kernel bookkeeping only, **not** malloc/sbrk.

Synthetic ELF tests verify the initially unconfigured
value, SetupThread followed by SetupHeap's special
size `0xFFFFFFFF` (expected end `0x01FFB000`),
a later explicit size `0x2000` (expected end
`0x00642000`), and that EndOfHeap ignores A0 and
returns the last stored end. Without `--hle-startup`,
the first EndOfHeap remains an unhandled guest syscall.

Only the EE game runner and synthetic tests/docs change.
The previously translated **189 game functions remain reusable**
without invalidating their cache. The next user-run
is needed to verify the actual subsequent boot path.

### First real-game unsupported instruction: PCPYUD (US SDBZ, dispatch 6032)

The ninth user-owned Windows run got past the new EndOfHeap HLE
and auto-translated **26 additional functions** (189 -> **215**),
with no IR verification or linker failures. It reached **6,032
dispatches**, then stopped with `UNSUPPORTED(4)` at guest PC
`0x0018E154` in `sub_0x18E0D8`. The translated function
contained 56 decoded instructions; exactly one was explicitly
unsupported by the decoder.

Inspection of the user-provided US ELF (SLUS_214.42), mapped at
guest vaddr 0x00100000 from file offset 0x80, establishes the
**exact instruction word 0x70491BA9** at guest PC `0x0018E154`.
This decodes to **PCPYUD $v1, $v0, $t1**: R5900 MMI3,
funct 0x29, subopcode 0x0E. It concatenates the *upper
doublewords* of its sources: the destination low 64 bits
receive `rs[127:64]`, and the destination high 64 bits
receive `rt[127:64]`. The operation does not touch HI/LO
or raise an architectural exception.

The EE decoder now validates MMI3/0x0E as `pcpyud`, and
LLVM emission writes both complete 64-bit halves using
the pre-write sources, including cases where `rd == rs`
or `rd == rt`. A regression checks the exact 0x70491BA9
game opcode and JIT execution at O0-O3, distinct source
halves, zero-register behavior and destination aliasing.

**Cache consequence:** this changes decoder validity and emitted
LLVM guest instructions. The provenance check must invalidate
and rebuild the 215-function translated cache; reusing the old
`sub_0x18E0D8` would still produce `EE_EXIT_UNSUPPORTED`.
The next real-game run will establish the following genuine
runtime boundary. No additional MMI opcodes have been enabled
without exact implementations.

### Post-PCPYUD memchr loop: I/O-vector argument provenance (US SDBZ, dispatch 6048)

The tenth and eleventh user-owned Windows reports confirm that the
exact `PCPYUD` implementation passed the former unsupported-instruction
boundary. The subsequent stop is a reproducible `LOOP_BUDGET (5)`
at **`0x0018E194`**, reached after **6,048 dispatches**, without
another unsupported opcode. The saved argument registers after one
million guarded loop-header visits are:

- `A0=0x020F2FC5` (one million bytes above original `0x01FFED85`,
  now **past the 32 MiB EE RAM boundary**)
- `A1=0x0A` (newline)
- `A2=0x0007DACB` (514,763 bytes still to scan)

The original US ELF's `0x0018E0D8` is a byte-search/memchr-like
routine. Its backward-branch at `0x0018E194` decrements A2 and
increments A0 correctly: the loop budget is behaving as designed,
not identifying a broken branch. The direct caller at `0x0018D1E4`
passes `A0=S3`, `A1=10`, `A2=S2`. Immediately beforehand, the
caller loaded `S3=[S4+0]`, `S2=[S4+4]` at
`0x0018D1BC` and `0x0018D1B8`. The branch at
`0x0018D1CC` has **`addiu S4,S4,8` in its delay slot
at `0x0018D1D0`**. That delay slot runs after the two
loads, even when the branch is not taken. Thus the
suspicious **length comes from an I/O-vector descriptor**,
not a computed memchr-loop counter. At the stop S4 was
`0x01FFEC18`, but it already points to the *next* entry;
the consumed pointer and length were at `0x01FFEC10`
and **`0x01FFEC14`**, respectively (not `0x01FFEC1C`
as previously inferred). The initial length is approximately
`0x00171D0C` (a code-like address), which is not plausible
for a stack-local log output buffer. Its cause is
not yet established: an earlier state write, bad descriptor
setup, or an ABI bug could each explain this symptom.

The native runner now supports two **opt-in diagnostics**:

- `--watch-word=0xADDR` watches one aligned, backed guest 32-bit
  word between dispatches and logs its previous and new value
  together with the translated function PC on change. Invalid
  and unbacked addresses are rejected, rather than silently
  reading zero. It cannot pinpoint a guest instruction inside
  one translated function or writes that cancel each other.
- `--trace-entry=0xPC` logs A0-A3, SP, RA when execution
  enters the chosen translated guest PC, and prints the first
  four guest words at A0 or A1 where backed. It does not
  interpret game-specific C layouts or modify guest state.

The **November 16, 00:38 local run** again stopped at dispatch 6048
with the same register state. Although `run_sdbz_native.bat`
had gained optional diagnostic arguments, the command lines in both
`expansion.log` and `boot.log` contained neither
`--trace-entry` nor `--watch-word`. The earlier diagnostic
instructions apparently did not survive into the launcher's
environment, so that run provided no new state provenance.

Until this specific blocker is fixed, the **SDBZ-only Windows launcher
now enables both diagnostic arguments by default**, using the known
guest entry `0x0018CF68` and watched guest word `0x01FFEC14`.
It prints the resolved arguments before starting the session, and
the boot-session wrapper forwards them in both the expansion and
final traced replay. The generic `run_game.py` and
`ee_game_runner` keep both options opt-in.

For the next local run, no special shell setup is required:

```bat
run_sdbz_native.bat
```

To turn off the SDBZ launcher defaults for a different experiment,
set `SDBZ_BOOT_DIAGNOSTICS=0`; explicitly supplied
`EE_TRACE_ENTRY` and `EE_WATCH_WORD` environment variables
continue to override their defaults. These options require
**no guest translation-cache invalidation**. Compare the
watchword's first change with the I/O-vector state at
`0x0018CF68`, then trace the producer function rather
than raising the loop budget.

### First producer isolated by word watchpoint (US SDBZ, dispatch 6029)

The first run with the corrected watchpoint verified that
`[0x01FFEC14]` changes from `0x00000000` to **`0x00171D0C`**
during **dispatch 6029**, when translated guest execution resumes at
`0x00194478` and exits at `0x00193670`. The output formatter
(`0x001937E8`) builds a 32-bit pointer/length vector on its
stack and `0x00193670` forwards that vector to the
`0x0018CF68` output path. The latter was traced at dispatch
6031 with `A1=0x01FFEBE0`, `A2=7`, and its first two
vector entries `(0x01FFEBF0,5)` and `(0x00171D27,0)`.
The seventh length field is `0x01FFEC14`, reached by iterating
seven eight-byte entries.

The original US ELF confirms `0x00171D0C` is **executable
instruction data** in another routine (not a plausible
`size_t` length), and `0x00171D27` also points into
that routine's instruction bytes. The defect therefore lies
at or upstream of the formatter's vector construction.
The watchpoint identifies the **native dispatch segment** that
committed the write, but does not yet identify a single guest
store or whether the formatter was supplied an invalid
varargs pointer, reused guest stack memory, or a wrongly
translated value. The previous `0x0018CF68` entry trace
only captured state after the vector was constructed.

The runner's opt-in `--trace-entry` now additionally emits
32-bit low halves of `S0-S7`, `FP`, `GP`, `V0`, `V1`,
and eight guest stack word pairs beginning at `SP+0x10`
(where mapped). When the watched word changes, it also
records `S3`, `S5`, and `FP` before and after the native
dispatch, and the adjacent 32-bit word (the associated
pointer in this particular output-vector layout). These
observations are read-only and generic to the runner. The SDBZ launcher now defaults
`EE_TRACE_ENTRY=0x00194478` to capture **before**
the suspect dispatch segment, retaining
`EE_WATCH_WORD=0x01FFEC14`. A subsequent user-owned
boot is needed to determine whether `FP` or a stack
value was already wrong on entry; no guest-code semantic
patch is justified from these observations alone.

### Instruction-level EE guest store provenance (pending real US SDBZ replay)

The user's **16 November 2026, 01:02** local trace again stops at
**6,048 native dispatches**, with `LOOP_BUDGET` at `0x0018E194`.
The new formatter entry snapshot, taken at dispatch 6029 PC
`0x00194478`, has `S3=0x01FFEC00`, `S5=0x01FFED85`,
`FP=7` and `RA=0x00194478`. The watched seventh I/O-vector
length at `0x01FFEC14` changes from zero to `0x00171D0C`
during that native dispatch. This establishes the affected
**dispatch segment**, not yet the executing store instruction.
The reported length is also a valid guest executable address.

The next diagnostic instruments every explicitly lowered guest memory
store **before** its existing memory helper call. Its emitted LLVM call
carries the exact guest instruction PC, original opcode, effective and
affected memory addresses, store width, source operand and lane, base
register value, and live SSA values for SP/FP/RA. Reading architectural
GPR backing fields at this point would risk capturing stale values,
because the emitter caches registers until a basic-block boundary.
The trace helper does not change CPU or guest memory state.

The host opt-in `--trace-store=0xADDR` requires an aligned, backed
32-bit guest word; only overlapping guest stores are printed, up to
64 matches per run. Partial-word/doubleword stores use the aligned
affected range for filtering but still report their unaligned effective
address and original source register. Their **source_value** is not
misrepresented as the merged final memory value. Ordinary stores and
SWC1/SQC2/SQ stores are also included. Guest addresses are inspected
only through the existing validated memory mapping; the trace itself
does not read or modify guest memory.

The SDBZ Windows launcher now enables
`--trace-store=0x01FFEC14` alongside the existing entry and word
watchpoint when `SDBZ_BOOT_DIAGNOSTICS=1`. `EE_TRACE_STORE`
overrides this address; `SDBZ_BOOT_DIAGNOSTICS=0` disables the new
default as with the old probes. The driver forwards this flag to
both expansion and final replay.

Because this changes LLVM emission and appends native runtime ABI
fields, the existing translated guest library must be **re-emitted
and relinked**. `run_game.py` fingerprints `ee_recomp`, all
`include/ee` and `src/runtime` files, and the driver itself.
The Windows launcher validates cache provenance before deciding
whether to reuse objects, and will take its full-translation path
when those inputs change. Do not force `--no-recompile` with the old
objects. The result of the next real ELF run is **not yet known**;
the word should not be patched or the loop budget raised.

The synthetic native ELF regression checks a real translated `SW`
at PC `0x00100014`, including opcode `0xAD090000`, data source
GPR9=7, base GPR8=`0x00100200`, stack/register context,
exactly one matching store event, a nearby nonmatching watched
word and invalid/unbacked address refusal. Existing native state and
post-dispatch word assertions remain in place.

### US formatter V0 write provenance (16 November 2026, 01:37 run)

The real-game trace from merged PR #56 identifies **the exact store**
responsible for the malformed seventh vector length:

```text
EE guest-store: pc=0x00194794 opcode=0xae620004 ins=SW
  effective=0x01ffec14 width=4
  src=gpr2 source_value=0x0000000000171d0c
  base=gpr19 base_value=0x0000000001ffec10
  sp=0x01ffebd0 fp=0x00000007 ra=0x00194478
```

This decodes to **`sw $v0, 4($s3)`**. The *effective address is
correct* for the observed `$s3`. The previous formatter dispatch
entry at `0x00194478` had `$v0=1`; the store later sees
`$v0=0x00171d0c`. This strongly shifts attention from store address
generation to the **source of the guest V0 value**. A preceding
`SQ` at `0x004ddf38` also overlaps the descriptor, but it does
not explain the later 32-bit length written by `0x00194794`.

The run still ends after **6,048 dispatches** with
`LOOP_BUDGET` at `0x0018e194`; no semantics fix or new boot
progress is claimed. The submitted expansion log rebuilt **216
translated guest modules** as required after the PR #56 emitter ABI
change; the new store probe did run.

The next narrow diagnostic is `--trace-v0=START:END`, an opt-in
aligned guest-PC window (maximum 0x2000 bytes), which reports
only *changed low-64-bit V0 values* produced by LLVM-translated guest
instructions. Its code-generation path attaches the original MIPS
opcode and PC to the SSA before/after V0 values at each emitted
register assignment. It does not invent a return value, read guest
memory, change CPU state, or bypass guest control flow. A host-side
64-event cap prevents unlimited trace volume.

The SDBZ launcher defaults to
`--trace-v0=0x00194478:0x00194794` alongside the existing store
probe. `EE_TRACE_V0` overrides the interval and
`SDBZ_BOOT_DIAGNOSTICS=0` disables the default. The guest
translation cache must be rebuilt because the LLVM emitter and
native runtime ABI changed; keep the existing fingerprint validation
rather than forcing `--no-recompile`.

Synthetic regression additionally uses an `ADDIU V0,zero,7` in
a **JR delay slot** at `0x0010001c`, verifying the exact original
opcode, PC, old/new SSA values and range filtering. This validates
the diagnostics only: identifying the actual SDBZ producer instruction
still requires the next user-owned local ELF run.

### SDBZ US 02:10 trace: DECI2 open reached; prior formatter path diverged

The new 2026-11-16 02:10 real-game replay reached **6,119
translated dispatches** and stopped on a *real guest SYSCALL*
(`0x0000000c`) at EPC `0x001750D4`, with COP0 Cause=0x20
and `V1=0x7C`, `A0=1`, `A1=0x01FFEA80`.
PS2SDK names syscall 0x7C **Deci2Call**, and PCSX2's
R5900OpcodeImpl.cpp documents operation 1 as OPEN:
read four words from A1, record word 1 as DECI2 channel address,
word 2 as handler, and return `V0=1`.
The runner's HLE filter had no service 0x7C entry.

The new V0 trace default `0x00194478:0x00194794` was active,
but no guest instruction in that window executed and no V0
provenance event occurred. It is **not evidence that the bad
formatter store was fixed**. Compared with the prior 01:37 trace,
the first different native control-flow edge occurs at
**dispatch 5874** (`0x0018EBB0 -> 0x0018DF98` previously,
now `0x0018EBB0 -> 0x0018E07C`). The formatter route
`0x00194478 -> 0x00194794` is absent from the new run.
The V0 tracing milestone changed generated SSA register
read/liveness bookkeeping; this apparent advancement must
not yet be considered a demonstrated native-semantics correction.
The guest execution path needs comparison with instrumentation
disabled and with a reference emulator before treating it as
stable.

The next HLE implementation covers **DECI2 OPEN (1) and
CLOSE (2) only**, matching PCSX2's documented return value 1
and registration/clearing of channel and handler state.
OPEN validates that A1 is word-aligned and maps to 16
accessible guest bytes before reading `addr[1]` and
`addr[2]`; invalid memory leaves the guest exception
unhandled. CLOSE clears state. Operations that require an
actual DECI2 transport, including request-send, remain
**unsupported and fail closed**, rather than silently
reporting successful transmission. No PCSX2 debug console,
IOP subsystem or synthetic external response is supplied.

Synthetic translated-MIPS tests cover OPEN, CLOSE, unchanged
descriptor memory, the return value, opt-in HLE isolation,
invalid/unbacked descriptors and request-send refusal.
The user must rerun the real US ELF after merge to see
whether the game proceeds to another unsupported service.
This HLE-only change does **not** require regenerating guest
LLVM objects: `ee_game_runner` is rebuilt by the launcher;
the validated translated DLL may be reused.

References:
- PS2SDK: `ee/kernel/include/syscallnr.h` (`__NR_Deci2Call=0x7c`)
- PCSX2: `pcsx2/R5900OpcodeImpl.cpp` (`__Deci2Call`)

### SDBZ US 02:21 trace: DECI2 REQSEND at 6,130 dispatches

After PR #58 merged, the user's US ELF (SLUS_214.42) replay
reached **6,130 dispatches**. At dispatch **6119**, the runner
acknowledged `Deci2Call(OPEN)` at EPC `0x001750D4`:
`A1=0x01FFEA80`, channel `0x005612D0`, handler
`0x00176260`. The guest then called
`Deci2Call(REQSEND)` at the same EPC during dispatch **6130**:
`A0=3`, `A1=0x01FFEA30`. HLE intentionally declined
this operation prior to the REQSEND implementation.
The guest V0 trace still did not execute its configured
formatter-PC interval; no evidence yet establishes the
correctness of the earlier `0x00194478` formatter path.

PCSX2's `pcsx2/R5900OpcodeImpl.cpp::__Deci2Call` shows
the **guest-visible** operation-3 semantics: if no DECI2
channel is registered, return 1; otherwise inspect the
*eight-word channel descriptor* registered by OPEN,
clear **word 3** at `channel+12`, and return 1.
The optional Shift-JIS console output is a host debug
effect; this native runner does not simulate delivery
to a physical IOP/debug transport or invoke the
registered handler. Operations 4 (POLL), 5 (EXRECV),
and 6 (EXSEND) similarly return 1 without mutating
guest memory in PCSX2's HLE; the native runner's
compatibility path follows those explicit behaviors.
Unknown operations remain unsupported.

For REQSEND with a registered channel, the runner
validates its alignment and that all eight guest
words are backed, *before* reading or changing
memory; invalid pointers leave the syscall exception
unhandled. It samples word 1 (packet length),
word 4 (packet pointer) and word 3 (pending field)
for the trace; it writes only `channel[3]=0`.
The request argument `A1` is debug metadata in
this reference implementation, not the registered
channel descriptor.

End-to-end synthetic MIPS tests cover OPEN,
REQSEND with/without registration, channel-word
mutation, unchanged length/packet pointer,
POLL/EXRECV/EXSEND, CLOSE, invalid channel
pointers, and fail-closed unknown operations.
This change only touches the native game runner
and integration tests. The existing validated
guest translation cache is reusable.

Reference: https://github.com/PCSX2/pcsx2/blob/master/pcsx2/R5900OpcodeImpl.cpp

### SDBZ US 02:32 trace: SIF0 DMA initialization at dispatch 6,162

After PR #59 (DECI2 REQSEND), the user's US SDBZ ELF trace
advanced to **6,162 dispatches** with no other unknown
instruction. The last instruction is `SYSCALL` at guest EPC
`0x00175084`; `V1=0x78` identifies
`sceSifSetDChain` (SifSetDChain), the EE kernel service
that starts the IOP-to-EE SIF0 DMA channel 5 in chain mode.
At the syscall, `V0=0x1000C000`, which is precisely
the physical base of channel-5 DMA registers.
`A0=0`, `A1=0x00180000`, `A2=0x20561600`,
`A3=0x20`. Unlike SIFSetDma, SifSetDChain is
**void** and does not have a meaningful return value;
the HLE preserves guest V0 rather than arbitrarily
setting it to zero.

PS2SDK's `ee/kernel/include/kernel.h` documents the
expected register sequence: clear SIF0 `QWC`
(physical `0x1000C020`) and set `CHCR=0x184`
(physical `0x1000C000`) for chain, TIE and STR.
The SIF0 register bank did not previously exist in
`c_ee_dmac`. This milestone adds stateful channel-5
CHCR/MADR/QWC/TADR/ASR registers to the EE host MMIO
bus, and makes the opt-in HLE write through that bus.
Starting SIF0 without an IOP producer **does not**
copy bytes, fire a fictitious DMA interrupt, or
clear STR. It remains busy until the missing producer
or another explicit register write handles it.
Unsupported access widths and scratchpad-specific
registers are still refused.

Unit tests verify SIF0 MMIO register reads and
writes, the absence of spurious DMAC completion,
preserved STR, unsupported SADR and reset semantics.
End-to-end synthetic ELF tests validate the
actual guest `LW` results for QWC/CHCR/MADR,
V0 preservation, opt-in isolation and refusal of
the next unsupported syscall.

References:
- https://github.com/ps2dev/ps2sdk/blob/master/ee/kernel/include/kernel.h
- https://psi-rockin.github.io/ps2tek/

Caveat: this implements the kernel's *register
initialization*, not IOP-to-EE SIF traffic, SIF
register handshake or an IOP RPC backend. It should
be tested with the user's local US ELF; reaching
later callsites would not establish full IOP
compatibility. The prior branch divergence near
dispatch 5,874 and formatter V0/store issue remain
open.

### SDBZ US 02:54 trace: SIF register syscall at dispatch 6,177

After PR #60's `SifSetDChain` support, the user's 2026-11-16
02:54 local replay advanced from 6,162 to **6,177 dispatches**.
The game installed a DMA channel-5 interrupt handler,
enabled that DMAC interrupt and reached `SYSCALL` at
`EPC=0x001750B4`, `V1=0x7A`, `A0=0x80000000`.
That is `sceSifGetReg(SIF_SYSREG_SUBADDR)`, not a
hardware-register read. This register is part of
the EE kernel's software-maintained SIF state. The
query occurs during the command-layer startup.
Neither the translated instruction decoder nor a
GS DMA transfer failed here.

The opt-in EE kernel HLE now supports:
- `sceSifGetReg` (0x7A) for physical mailbox register
  IDs 1 (MSCOM), 2 (SMCOM), 3 (MSFLAG), 4 (SMFLAG)
  and software-only system IDs `0x80000000`
  (SUBADDR), `0x80000001` (MAINADDR) and
  `0x80000002` (RPCINIT).
- `sceSifSetReg` (0x79) against the same register
  owner; EE-owned MSCOM writes replace the value,
  MSFLAG writes set bits, SMFLAG writes clear bits,
  and software system IDs retain their values.
  The IOP-owned SMCOM register cannot be written by
  the EE. Unsupported IDs fail closed.
- Exact 32-bit physical MMIO reads/writes at
  `0x1000F200`, `0x1000F210`, `0x1000F220`,
  and `0x1000F230` through the same state object.
  Unsupported widths are refused.
- Reset and explicit future IOP-side mailbox publishing
  hooks. An IOP-side boot, SIF DMA payload transport,
  command-buffer delivery, readiness flags and RPC
  replies are **not** simulated by EE register reads.
  In particular, `SIF_SYSREG_SUBADDR=0` and
  `SMFLAG=0` initially, reflecting the absence
  of an IOP producer rather than inventing one.

A native synthetic MIPS test exercises software
SIF system-register roundtrips, EE mailbox visibility
through guest `LW`, MSFLAG, read of IOP-owned
SUBADDR, unsupported-ID refusal, and the opt-in
isolation of HLE. Hardware model unit tests also
verify EE/IOP register ownership, OR/clear behavior,
width restriction and reset.

Reference behavior: ps2dev/ps2sdk
`ee/kernel/include/sifdma.h`, `ee/kernel/src/sifcmd.c`,
and ps2tek's SIF mailbox descriptions. The
`snuri00/ps2-web-recomp` runtime uses a preinitialized
IOP command buffer, SMFLAG readiness and RPCINIT=1;
this project **does not** copy those constants without
implementing the corresponding IOP functionality.
The follow-up game replay may therefore stall in
a command-init wait loop; if so that is evidence of the
missing IOP/EE transport, not a license to fake readiness.

The older V0 formatter/dispatch divergence around
5,874 remains unresolved and should be compared
against a PCSX2 reference independently.

## Validation limits

CTest contains `game_runner_end_to_end` which creates small synthetic MIPS
ELFs, translates and links them to native host code and verifies that
dispatch and selected kernel calls resume correctly. The real-game observations
above were captured locally from the user's privately supplied ELF, not in CI.

The native host has bounded EE RAM, opt-in direct aliases, scratchpad,
VU0 apertures, a host MMIO bus, INTC and D_STAT register models, and bounded
normal/interleave SPR DMA channels 8/9 and chains for both channels,
but still no general TLB, BIOS, other DMA channels,
device scheduling, or full interrupt emulation. The syscall handling is deliberately
opt-in, experimental, and incomplete. Reaching the PCs above is not evidence
of full console initialization, correct kernel behaviour or graphical output.

## M2 implementation checkpoint — 2026-11-19 (after 07:27 matched batch)

- **M2.0 instrumentation implemented (F for tooling, not a game fix):** native runner exports bounded host-dispatch PC/edge frequency and observed COP0/INTC/DMAC/GS/thread/RPC state deltas inside `first-boot.json.m2_runtime`. These are `native_dispatch_boundary` observations, not individual guest instruction counts or validated R5900 cycles. Saturation is explicit rather than silently claiming full coverage.
- **M2.0 ISA inventory implemented (S and sampled R, not semantics T):** `ee_recomp` exports a bounded, unique-CFG-PC decoder/lifter inventory in `ee_coverage.json.m2_instruction_inventory`. Opt-in `--trace-instructions` now also emits `<trace-path>.isa.json`, classifying actual recorded words (kind 0=native reached, 2=RAM interpreter committed) separately from excluded annulled branch-likely slots (kind 1) and HLE boundaries (kind 3). Both artifacts explicitly qualify coverage and cannot imply architectural correctness.
- **M2.1 offline analysis tooling implemented (R observation, predicate unproven):** `tools/analyze_m2_dispatch_trace.py` consumes an existing `--trace` log and its *matching final report*, pins input SHA-256 digests, deduplicates repeated dispatch indices caused by yield handoffs, and exports hot PC/transition counts, temporal windows and concrete logged interrupt/DMAC/RPC observations. It does not modify guest state.
- **07:27 runtime findings (R, from the matched log):** 13 committed SIF RPCs complete by approximately dispatch 6,941, including the one successful SIO2MAN start; 15 logged DMAC channel-5 IRQs; 195 logged INTC callback entries, last near dispatch 12,359, though the final clock reports 230 GS fields. The late update sequence is dominated by `0x00173250 ↔ 0x001734e4`, with recurrent `0x00172e20 → 0x00172a90 → 0x00172e3c → 0x00172998 → 0x00172e48`. Both are native host-dispatch sequences, **not decoded guest wait predicates**. The loop's health and first unmet producer remain unknown.
- **Acceptance boundary:** M2.0 artifacts and regression tests exist. M2.1 is **not gated complete** until disassembly/live guest-state identifies PC→predicate→producer, and any defect receives an architectural regression. No extra interrupt/IRQ, DMA, RPC, or draw was fabricated. Existing 07:27 CPU images remain black and WebGPU run provenance remains unverified.

To reproduce *offline* without another guest boot:

```sh
python tools/analyze_m2_dispatch_trace.py \\
  --boot-log "boot(20261119-072747).log" \\
  --report "first-boot(20261119-072746).json" \\
  --output "m2-dispatch-analysis.json"
```

CI is checked per exact branch HEAD (do not infer the result of a newer commit from a prior green run). PR #142 remains draft and unmerged.

### Provenance and bounds follow-up

The offline M2 analyzer accepts optional `--catalog <matching-iop-disc-catalog.json>` and `--revision <verified-commit-sha>`; absent explicit revision is reported as unknown rather than inferred from a subsequent checkout. It hashes the supplied catalog and reports the catalog's ELF SHA-256 as *catalog-supplied*, not an independent ELF rehash. It also captures the **actual native runner invocation** found in the log and separately reports whether `--gs-webgpu-snapshot` was passed; no PPM file alone establishes this. The max-record safety cap now uses a constant-size overflow flag rather than storing every dropped index. These remain host diagnostic observations and do not change guest execution.

### M2.1 verified-USA-ELF correction

The USA SLUS_214.42 ELF SHA-256 matches the 07:27 catalog. See [M2.1 physical GIF DMA and VU predicate map](M2_SDBZ_0727_ELF_GS_DEPENDENCIES.md). The apparent `0x00173250 ↔ 0x001734e4` hot loop is a finite VU/COP2 register-save helper, **not** the game idle predicate. The exact DMA predicates include GIF CHCR STR at `0x1000a000` (ELF `0x00102814`) and the mode-1 check through `0x001730c0` from `0x00105020`. New bounded read-only GIF state counters observe channel 2 CHCR/MADR/QWC/TADR and GIF write/rejection sequence. A missing producer is **not yet proven**; no synthetic completion was added.

### M2.3 — Correct cumulative GIF/GS traffic diagnostics

The 07:27 report's two `m_vertex_count` writes describe only the **current primitive**: `PRIM` resets that counter. Its `m_image_qwc=16384` also resets on each `TRXDIR=0`. Therefore neither is a valid lifetime numerator/denominator for accepted 4,584 PATH3 tags. The native GS consumer now tracks lifetime committed PRIM/XYZ2/XYZ3, ADC versus kick, PACKED/REGLIST/IMAGE/IMAGE2 tag formats, A+D register addresses and total IMAGE QWC, exported under `m2_gif_gs` (JSON schema `ee-m2-gif-gs-v1`) as well as a concise console line. Tests exercise multiple PRIM/TRXDIR resets, IMAGE2, and atomic rejection; guest-visible GS/DMAC semantics are unchanged. See [the M2.3 traffic interpretation](M2_3_GIF_GS_COMMITTED_TRAFFIC.md). A new matching boot is required before concluding whether SDBZ lacks drawing traffic, has VIF/VU1 paths, or the WebGPU bridge misses actual committed draws.

## M2.3 real SDBZ replay — 2026-11-19 09:09 matched batch

Validated the same final boot's `boot(20261119-090909).log`,
`first-boot(20261119-090907).json`, catalog, cache and three PPMs.
The *final* native runner command included
`--gs-webgpu-snapshot=...`; WebGPU reported successful init and a
`512x448` GPU readback with `draws=2275`. This is confirmed
**GPU compute raster work**, not scanout or a successful menu frame.

- PATH3: 4,584 committed tags/batches, 4,568 PACKED tags and
  16 IMAGE tags; no REGLIST/IMAGE2.
- Exactly 2,275 PRIM writes, all `PRIM & 7 == 6` (**sprites**);
  4,550 XYZ2/XYZF2 vertex kicks, zero ADC vertices.
- The 29,655 PACKED events are all A+D (`reg=0xE`).
  `RGBAQ` and `PRIM` each appear 2,275 times;
  `XYZ2` appears 4,550 times. The only other A+D
  writes are repeated frame/depth/offset/test setup and sixteen
  `BITBLTBUF`/`TRXPOS`/`TRXREG`/`TRXDIR` groups.
  Crucially, **no TEX0/TEX1 or other texture-state writes**
  were committed. This is a **clear/setup-like sprite workload**,
  not evidence of actual textured title/menu traffic.
- Cumulative IMAGE QWC = **262,144** (4 MiB of accepted payload).
  The final individual IMAGE transfer had 16,384 QWC.
- Readback inspection: `gs-display.ppm` 512x448 entirely black;
  `gs-upload.ppm` 1024x64 entirely black;
  `gs-webgpu.ppm` 512x448 has 229,375 black pixels and
  one RGB(254,0,0) pixel at (0,0). This does not establish
  that the sprite colour/target is correct until source attributes
  are captured and compared with display FB pointers.
- The 100,000-dispatch budget ended at `0x00172e48` with
  `GIF_REJECT=0`, 230 *reference-compatibility* GS fields;
  no unsupported opcode exit. The catalog identifies 13 committed
  SIF RPCs and one successful SIO2MAN load, but cannot
  prove PADMAN, other modules or title application initialization.

**Next discriminator, implemented read-only:** bounded first/last
8 committed PATH3 sprite pair snapshots contain raw PRIM/effective
PRIM, RGBAQ, XYZ pair, XYOFFSET, SCISSOR, FRAME, ZBUF, TEST and
context selection. `sprite_pair_kicks_total`,
`sprite_textured_kicks_total` and
`sprite_nonblack_rgb_kicks_total` provide totals.
The new diagnostics cannot create or reject guest DMA,
change GS state, or synthesize a visible frame.
A further matched native boot is needed to obtain these
new raw attributes before implementing a speculative GS fix.

**Disposition:** M2.3 proved *GPU packet delivery* and corrected
false lifetime-vertex conclusions; visible native boot remains
incomplete. M2.4 isolates the earliest unfulfilled game
application/IOP producer independently of sprite clear behaviour.

See [the 09:09 sprite analysis](M2_3_0909_GS_SPRITE_PROVENANCE.md).


### M2.4 — 11:29 ELF-derived native red-screen failure and wait producer

The 11:29 matching boot confirms **2,275 red untextured
sprite pairs**, all with `RGBAQ=0x3f800000000000ff`,
`SCISSOR_1=0`, alternating FRAME bases `0x70`/`0x00`
and FBMSK=1. The single `RGB(254,0,0)` pixel at
(0,0) in the WebGPU readback is therefore consistent
with correct scissor clipping and FBMSK, not proof
of a GPU implementation bug.

Disassembly of the verified USA ELF shows the red sprite
function `0x00102670` is invoked **only after**
a pending command-buffer wait at `0x00102560`
(`0x00502368 + 4*(0x005030ac ^ 1)`), 3001 polling
iterations, EE Timer 0 COUNT comparison against 60001,
and recovery calls `0x00173888/0x00173460`.
The actual trace first calls the red loop at dispatch
36,205. This is **the game's explicit timeout/recovery
fallback**, not a menu render.

A bounded, read-only RAM probe now captures the first
wait-entry and first red-loop-entry flag/queue state
under `m2_sdbz_dma_wait`, with a synthetic regression.
The **original expected completion producer is still
unproven**; do not manufacture an IRQ/DMA completion,
override scissor or invent a menu frame. See
[M2.4 guest failure path](M2_4_SDBZ_COMMAND_SLOT_TIMEOUT.md).

M2.4 probe follow-up: `m2_sdbz_dma_wait.pending_wait` now retains the first wait entry whose **polled opposite** flag is nonzero, independent of `wait` and `red_loop`.


## M2.4 VIF1/DMAC1 expected completion — evidence and diagnostic gate

The **11:29 final boot** registers `AddDmacHandler cause=1
handler=0x00102a60` and enables DMAC1. Its ELF handler
decrements the selected guest command pending count at
`0x00102b08..0x00102b10` after channel status/STR
checks. That final trace contains **15 DMAC channel 5
callbacks and zero channel 1 callbacks**. Consequently
DMAC1/VIF1 completion is the strongest known candidate
for the unretired buffer slot, not an unrendered menu frame.

The current native `ee_dmac.cpp` routes VIF1 starts to
`transfer_device_normal(1)`, which refuses source-chain
mode; for normal VIF1 outbound data it uses a sink currently
bound to a GIF PATH3 parser accepting only channel 2. This
**does not establish that the guest's stalled transfer was a
chain**: only the actual CHCR/MADR/QWC/TADR at the failure
boundary can settle that. Compared with
`twist84/ps2-web-recomp:SDBZ`,
`runtime/src/dmac.cpp::run_chain` and
`runtime/src/vif.cpp::transfer`, an actual implementation
will need genuine VIF1 payload parsing/state effects and
proper producer-driven DMA completion, not an IRQ shortcut.

The bounded `m2_sdbz_dma_wait` probe now additionally
reads **physical VIF1 channel 1 state** through a new
host-only `c_ee_dmac::vif1_channel_state()` accessor:
`CHCR`, `MADR`, `QWC`, `TADR` and `D_STAT`
status/pending. These are attached to `wait`,
`pending_wait` and `red_loop` snapshots.
No guest MMIO read, DMA scheduling, timer update, CPU
write, IRQ or other guest-visible action is performed.
The test explicitly starts an **unsupported synthetic
VIF1 chain (CHCR=0x105)** and verifies it remains
unretired while the probe captures its immutable channel
state and does not fabricate a completion interrupt.

**Next decision:** Confirm which VIF1 mode, address and
QWC the guest actually reached, and whether STR remains set,
then implement the necessary real VIF1 delivery and
channel-1 completion semantics in a batch of independently
verified tests. Never clear STR or pending flags merely to
avoid the red fallback loop.


### M2.5 — 13:53 VIF1 source-chain blocker (observed, then bounded implementation)

Verified USA ELF: first wait dispatch 10,411 had opposite slot flag 0
and VIF1 CHCR=0. First **pending** wait dispatch 10,495 had opposite
slot flag 1, physical VIF1 CHCR=`0x1c5` (DIR/chain/TTE/TIE/STR),
TADR=`0x00771400`, QWC=0, D_STAT channel-1 pending=0.
At first red fallback dispatch 36,205 the state is **identical**.
The 100,000-dispatch result still produces only the 2,275 red
fallback sprites. This proves a started, unsupported VIF1 source
chain, not a graphics display problem.

This batch provides a bounded transactional VIF1 chain walker
with TTE/TIE and real sink acceptance, source-span validation
and explicit rejection reasons; it also captures the first
64 bytes at the true DMA tag pointer in native wait snapshots.
The production runner still lacks a real VIF1/VU1/GIF PATH2
consumer. Until that consumer accepts the actual stream,
channel 1 deliberately stays active and raises no interrupt.
See [M2.5 VIF1 chain](M2_5_VIF1_SOURCE_CHAIN.md).


## M2.6 — Native VIF1, VU1 memory and GIF PATH2 first implementation (19 November 2026)

Following M2.5's valid source-chain transport but missing device sink, the native runtime now routes channel 1 to `c_ee_vif1`, channel 2 to the existing independent PATH3 parser, and VIF DIRECT/DIRECTHL GIF packets through a separate PATH2 parser into the same actual GS/WebGPU consumer. The decoder/state machine is adapted from `twist84/ps2-web-recomp:SDBZ/runtime/src/vif.cpp` under the MIT license, reformatted for the local `c_`/`s_`/`m_`, snake_case and Allman conventions.

The transactional parser handles split 32-bit VIF commands, V4/V3/V2/V1 UNPACK with cycle fill/skip/mask/row/col, VU1 16 KiB code/data memory, MPG uploads, STCYCL/MODE/BASE/OFFSET/ITOP and physical VU1 memory windows. VIF FBRST preserves resident code/data as the primary reference does. It refuses unimplemented MSCAL/MSCALF/MSCNT, MSKPATH3 and unhandled VIF IRQ stalls without falsely retiring DMA. Rejected packets preserve channel-1 STR, source addresses and D_STAT; successful complete original chains still must pass DMAC source validation and device consumption to raise the guest's genuine channel-1 status.

New `ee_vif1_device` regression exercises packet continuation, UNPACK, masking, micro uploads, physical apertures, PATH2 events and a real bounded synthetic source chain with TTE/TIE. **A synthetic DMA completion is not proof of native SDBZ boot.** The source bytes at `0x00771400` were not captured by the 13:53 user-owned boot, because it predates M2.5's raw source-prefix probe. VU1 micro execution and XGKICK/PATH1 require their own implementation and tests when actual commands are observed.

See [M2.6 Native VIF1 Consumer](M2_6_NATIVE_VIF1_CONSUMER.md). Keep the existing branch/PR #142 draft and unmerged; revalidate exact-commit CI, and use only a new matched Windows boot batch for game progression claims.


## M2.7 — Bounded VU1 interpreter and genuine GIF PATH1 (19 November 2026; partial)

Implemented a **strict subset** of VU1 micro execution from the primary ps2-web-recomp reference: MPG-resident microcode, MSCAL/MSCNT, E-bit delay slot, floating-point VF operations, VI branch view, memory operations and genuine XGKICK into PATH1. Unsupported opcodes, flag/Q/P pipelines, MSCALF, mixed-path arbitration and IRQs **fail closed**: no DMA completion or fabricated rendering. The new test exercises MPG → original VU1 microinstructions → GIF PATH1 tag/body, opcode rejection and arithmetic. See [M2.7 VU1 first batch](M2_7_VU1_NATIVE_FIRST_BATCH.md). The 13:53 USA game logs predate the VIF source prefix; do not assert native boot progression. PR #142 remains draft, unmerged.


### M2.7 Batch B — Reference FMAC/flags and XGKICK fixture (19 November 2026)

Ported ps2-web-recomp PS2 floating-point bit arithmetic to C++17 and expanded VU1 ACC, MADD/MSUB, FTOI/ITOF, CLIP, MAC/status/clip flags and a four-issued-pair flag delay buffer. Added lower flag opcodes and focused arithmetic/CLIP regressions. Fixed synthetic GIFtag NREG to one. Q/P latency, precise VF/VI stalls, lower special opcodes, MSCALF, GIF arbitration and VIF IRQ remain unsupported and fail closed. Native USA boot advancement remains unverified. PR #142 stays draft/unmerged.


## M2.7 Batch C on a new post-merge branch — 2026-11-19

PR #142 is squash-merged to `main` as `c576a0cec1732d21efb04889cc95a627f0883ece`. Follow-on `feature/sdbz-m2-7-vu1-q-p-pipelines` adds reference-derived VU1 Q/P instruction execution, pipeline synchronization, EFU/RNG/lower-specials, VF/VI dependency scoreboarding, and an independent CTest regression. The four-shard build-once workflow remains unchanged. This is source/regression work, **not verified USA game progression**.

**Last matched real game evidence remains** the command-slot pending/VIF1 CHCR `0x1c5`, TADR `0x00771400` and red fallback. No local Windows boot rerun has yet established game VIF packet acceptance or a new framebuffer. Continue to fail closed on unsupported DMA/VIF/GIF operations; never clear STR, raise D_STAT, free the guest command slot or invent graphics as a substitute for hardware execution.

Before requesting another local Windows run, require exact-commit CI success for Build and test (game and fast-0/1/2), SIF DMA, ROM0 and Native WebGPU, followed by coherent M2.8 GIF path arbitration as needed.

## M2.5/VIF1 NEXT DMA address bug found in matched 16:14 Windows batch — 2026-11-19

The completed USA run still reached dispatch 10495 with pending command flag 1, VIF1 CHCR `0x1C5` and TADR `0x00771400`, and reached its red fallback at dispatch 36205. It ended after the 100000-dispatch budget with the flag and registers unchanged. New read-only probe `chain_reject=3` is specifically **invalid_tag_address**. The captured tag at `0x00771400` is `0x0077144020000003` (NEXT, QWC 3, destination `0x00771440`) and TTE code `0x5000000313000000`. The DMAC walker wrongly read ADDR from TTE data, generating `0x13000000`, and was corrected on the follow-on PR #143 branch. Regression fixtures now use the architectural first-word encoding, with real-tag and rollback tests. This bug predates Batch C.

Same-invocation snapshots: GS display and GS upload are all black; WebGPU readback has one nonblack red pixel. Diagnostic GS counters still report 2275 nonblack untextured sprite kicks; these are not proof of correctly displayed graphics. The disc catalog records 7 static candidate IRX paths but just 1 final-replay committed runtime load (`SIO2MAN.IRX`) and 13 SIFRPC commits, not seven runtime module starts. Do not claim progress before the next post-fix run.

The pre-fix batch is matched by `20261119-161404` through `20261119-161408` filenames. The translation-cache input hash includes the current VU1 code; compilation/rebuild happens through `run_ps2_game.bat`. Do not blame stale executable without evidence. Continue HLE-first, preserve transactional DMA semantics, and do not fake guest completion or graphics.


## Windows expansion configuration (2026-11-19)

`run_ps2_game.bat` accepts two independent environment variables without changing the HLE-first execution path:

```bat
set EXPANSION_PASSES=64
set EXPANSION_PER_PASS=128
run_ps2_game.bat
```

Defaults remain **8 passes** and **128 new functions per pass**. Valid values: `EXPANSION_PASSES=1..64`, `EXPANSION_PER_PASS=1..128`. The wrapper passes both values through as `--expansion-passes` and `--expansion-per-pass`, and reports them at boot-session start. The max pass count increases from eight to 64; the 128-per-pass safety cap in `run_game.py` is unchanged. As before, each pass stops early on genuine guest blockers or when it does not hit its budget. `LLVM IR [N/7901]` is a compilation progress indicator, not proof of native boot advancement or rendering correctness. The already-running process cannot be reconfigured until its next invocation.


## Matched USA Windows 17:50 boot — VIF1 fixed, first IOP LOADCORE and EE syscall gaps

Match these eight artifacts by invocation and runtime state: `boot(20261119-175056).log`, `expansion(20261119-175057).log`, `first-boot(20261119-175056).json`, `iop-disc-catalog(20261119-175059).json`, `translation-cache(20261119-175059).json`, `gs-display(20261119-175056).ppm`, `gs-upload(20261119-175056).ppm`, `gs-webgpu(5).ppm`. The USA `SLUS_214.42` ELF hash remains `1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`.

This is real progress beyond the previously blocked 16:14 VIF1 source-chain NEXT tag (TADR `0x00771400`, CHCR `0x1C5`, `invalid_tag_address`). New command-wait snapshot at dispatch **10411** has VIF1 CHCR/MADR/QWC/TADR **all zero** and no pending DMA. Previous 100000-dispatch red-loop stall is absent. It does **not** prove full VU1/GIF completion or display correctness.

Bounded expansion consumed 12 passes (the 12th added 43 newly compiled functions) and stopped early on a different real guest exception, not the configured 64-pass limit. Cached translation count: **3500 LLVM-IR functions**. Final traced replay exited after **13909 dispatches**; EE COP0 Cause `0x20` indicates SYSCALL (code 8), at EPC `0x00175104`, V1=**0x7F**. This is PS2 EE `GetMemorySize`; `ps2-web-recomp:SDBZ/runtime/src/kernel.cpp::handle_syscall` returns `kRamSize` (32 MiB). The missing explicit handler now returns configured EE RAM size to V0, with no invented guest memory or GPU side effects. A translated synthetic ELF test asserts V0=0x02000000 and still rejects the following unknown syscall.

The final-replay catalog shows 7 ELF-static IRX path candidates, **14** genuine committed SIFRPC operations, **2** attempted IRX loads but **only 1 success**: seq13 `SIO2MAN.IRX` linked and started; seq14 `MCMAN.IRX` returned load_status=7, link_status=5, `unresolved=loadcore#7` (no module/thread fabricated). The IOP HLE bridge previously registered only `loadcore#6` (`RegisterLibraryEntries`). `loadcore#7` is `ReleaseLibraryEntries`. The follow-on implementation registers both, validates a real loaded IRX's export-table ownership and exact entries, supports release and subsequent register, and refuses forged, conflicting and duplicate releases transactionally. Further MCMAN imports or guest IOP execution may still fail later; a successful boot is NOT asserted.

All three same-run GS snapshots are **uniform black** (`gs-display` 512×448, `gs-upload` 1024×64, `gs-webgpu` 512×448). The runner reports 2 scheduled GS fields, 37 accepted GS batches, 40 tags, 2 vertex kicks, and IMAGE transfers; those are diagnostics, not a visible game frame. PR #144's raylib presentation was merged separately into `main`; this PR #143 remains a separate draft pending combined-tree validation and Windows rerun.

Acceptance for this next HLE batch is all four GitHub workflows green on the *final* exact commit plus a matched new Windows run demonstrating whether `GetMemorySize` returns and `MCMAN.IRX` import linking advances. Do not manufacture completion, IOP exports, GS events, or frame presentation.

## 2026-11-19 20:13 matched native Windows boot — IOP SYSCLIB memory functions

The user's `20261119-201359` final replay, expansion and report agree with
`201402` IOP catalog and translation-cache metadata (same USA
`SLUS_214.42` ELF SHA-256
`1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`).
The expansion session reused its translation cache, added **zero** new functions
in the recorded pass, and retained 3,524 hashed IR modules. Do not confuse
this with the 7,901 total static discovery count or the number of functions
executed.

**Boot progress:** The prior VIF1 source-chain invalid-tag rejection did not
recur (observed CHCR/TADR zero and no pending DMA at dispatch 10,411).
The previously unsupported EE `GetMemorySize` syscall now committed its
32-MiB result and execution continued from dispatch 13,909.
Final replay reached **14,240** native dispatches (previously 13,909)
before stopping at guest RAM callback PC `0x800750D4`
(`0x40840000`, `MTC0 a0, Index`), with
`UNSUPPORTED(4)`. The surrounding installed handler writes COP0 Index,
PageMask, EntryHi, EntryLo0, EntryLo1 and issues `TLBWI`. The bounded
RAM interpreter deliberately lacks that full TLB behavior: no writes
or virtual-memory translation are fabricated here.

**Earlier demonstrated IOP failure:** SIFRPC seq 13 loaded and started
`SIO2MAN.IRX` normally. Seq 14 advanced beyond the previous
`loadcore#7` failure but rejected `MCMAN.IRX` with
`unresolved=sysclib#12`; no MCMAN thread was started.
PS2SDK's IOP SYSCLIB import table identifies ordinal #12 as `memcpy`.

**Coherent IOP HLE batch:** Implement ordinals #10–#17
(`memchr`, `memcmp`, `memcpy`, `memmove`, `memset`, `bcmp`,
`bcopy`, `bzero`) with validated IOP RAM/KSEG0/KSEG1 spans,
bounded length and overlap-safe source snapshots. Return the correct
guest pointer/value for the non-void functions and leave void results
unfabricated. Reject unsupported ranges and imports without writes.
The new `iop_sysclib_memory` regression covers import registration,
byte results, overlapping copies, aliases, invalid spans, oversized
lengths, zero-length behavior and unknown ordinals.

**GS:** The final raylib HWND / wgpu-native DX12 surface initialized.
The runtime counted 37 accepted GIF batches, 40 tags, 16 image events,
two vertex kicks and one GPU draw. The three uploads named
`gs-display(20261119-201359).ppm`,
`gs-upload(20261119-201359).ppm` and `gs-webgpu(6).ppm`
are all black, not visible game frames or proof of scanout.

**Validation boundary:** The HLE changes are source-level updates
motivated by this new real run; only CI can validate their synthetic
regressions. This 20:13 capture predates these changes.
The next matched local Windows batch must show whether
`MCMAN.IRX` now links/starts or encounters another import and whether
the registered EE RAM TLB callback is still the first terminal failure.
Do not add TLB no-op fallbacks solely to bypass the stop.

## Proactive next batch: real SDBZ EE _SetTLBEntry RAM callback

**User-authorized work ahead of another Windows boot:** The 20:13 matched final
replay stopped at `0x800750D4`, word `0x40840000`, inside the guest-installed
syscall `0x56` (`_SetTLBEntry`). The diagnostic surrounding code shows this
specific `SLUS_214.42` handler checking `index < 48`, writing
COP0.Index, PageMask, EntryHi, EntryLo0 and EntryLo1, issuing `SYNC.P`,
then `TLBWI` and returning the index; invalid indices return `-1`.
This is concrete reachable code, not an arbitrary speculative opcode.

- `ee_ram_interpreter.cpp` now decodes a bounded COP0 MFC0/MTC0
  register subset, SYNC.P, TLBR, TLBP and TLBWI. MTC0 delegates to
  `ee_cop0_write` to retain Compare/timer side effects. Unknown register
  encodings and TLBWR (which needs genuine Random/Wired timing) remain
  unsupported, never silently treated as success.
- `c_ee_tlb` holds exactly 48 persistent guest-visible entries in
  `ee_memory`, independent of thread-context snapshots. It accepts only
  documented PS2 PageMask sizes; TLBWI validates index and mask before
  committing all entry words. TLBP compares VPN2/ASID or global pairs;
  TLBR reads back committed values. Its explicit translation helper
  distinguishes even/odd PFNs, valid/dirty pages, ASID mismatch and
  invalid scratchpad mappings.
- **Important architectural boundary:** The native HLE memory fast path
  still follows the existing `ps2-web-recomp:SDBZ` physical/RAM policy.
  Merely recording authentic TLB entries does **not** activate
  an unverified virtual memory translation mechanism: TLB lookup and
  fault delivery are not yet integrated into every native EE memory
  access, and full hardware TLB parity is not claimed. A caller must
  explicitly request translation and handle any failure. This is
  preferable to claiming virtual-memory correctness through a no-op.
- New `ee_ram_tlb` regression replays the exact observed instruction
  words at `0x800750C8`, checks real committed entry state and return
  value, invalid-index and invalid-mask failure paths, independent PFNs,
  read-only dirty bit, ASID, TLBR/MFC0, TLBP miss/Index.P and continued
  fail-closed TLBWR behavior. Runtime traces record `tlb_writes` on
  successful RAM callback returns.

**Reference and scope:** The primary `ps2-web-recomp:SDBZ` reference uses
direct HLE physical memory and does not provide full EE TLB hardware
translation. The original guest sequence was taken from the user's
`boot(20261119-201359).log`. Argument order and the 48-entry size
are cross-checked with PS2SDK `ee/kernel/include/kernel.h` and
`ee/kernel/src/tlbfunc.c`, and page-mask interpretation with PS2TEK.

This source change is **not** evidence of a new Windows guest progression.
A later matched run must confirm that this actual RAM handler completes,
the prior `MCMAN.IRX` import failure is cleared, and the next stop is
diagnosed without inventing a working boot or GS frame.

### Windows WebGPU SDK startup repair

The user's 19 November 2026 Windows launcher initially aborted with
`[wgpu-native] SDK directory exists but is incomplete:
C:\\Dev\\MIPS-To-LLVM-IR-To-C`. The validator correctly refused to
overwrite user files; the actual fault was in `run_ps2_game.bat`.
`cmd.exe` expands percent variables before executing a parenthesized block,
so assigning `EE_WEBGPU_ROOT` and reading `%EE_WEBGPU_ROOT%` inside the
same block passed an empty `--root`, resolving to the repository directory.
Assigning it immediately *before* the installer block now passes
`build\\_deps\\wgpu-native` and preserves a user-provided SDK path.
This fixes fresh setups without removing the SDK path safety check.

## Proactive IOP SYSCLIB string-family batch

Before another Windows SDBZ run, the user requested further proactive kernel
work rather than blocking on repeated local auto-expansion. The real
`MCMAN.IRX` seq14 import failure proves that this game boots through
IOP SYSCLIB dependencies. The PS2SDK source
`iop/system/sysclib/src/exports.tab` is the ordinal authority (0-based).
It maps #20–#34 to `strcat`, `strchr`, `strcmp`, `strcpy`,
`strcspn`, `index`, `rindex`, `strlen`, `strncat`, `strncmp`,
`strncpy`, `strpbrk`, `strrchr`, `strspn` and `strstr`.

The `iop_sysclib_strings` implementation exports that coherent family and
executes real guest-memory effects through IOP c_cpu, never host addresses.
CString scans are bounded to 65536 bytes; source and destination accesses
are limited to IOP RAM and its KSEG0/KSEG1 aliases. Guest return values,
NUL terminators, overlap-free checked writes, exact/truncated comparison
semantics and unknown-address rejection are implemented. No write begins
without full destination-span validation. #29 `strncmp` now uses the same
bounded path; the older isolated implementation is removed.

The `iop_sysclib_string` CTest exercises all 15 imported symbols, correct
pointer/length/difference values, NUL search, substring and span semantics,
concat/truncation/padding, KSEG aliases, invalid-source/destination rollback
and unsupported ordinal #35 rejection.

**Deliberate exclusions:** SYSCLIB `setjmp`/`longjmp` (#4/#5),
formatting (#18/#19/#42), stateful `strtok` (#35/#43), and hardware
services remain unknown/unimplemented, never silent success. Exact
PS2 `MCMAN.IRX` post-#12 demand and the EE `_SetTLBEntry` continuation
still require new matched Windows diagnostics; these source additions do
not establish runtime advancement or new visible GS frames.

### SYSCLIB ASCII classification follow-on

The PS2SDK `iop/system/sysclib/src/exports.tab` also identifies #6
(`_toupper`), #7 (`_tolower`), #8 (`look_ctype_table`) and #9
(`get_ctype_table`). Its `src/nonstd/ctype.c` defines a 128-byte
ASCII classification table. The old #8 HLE used host `std::is*`
categories and did not match this guest table (e.g. digit '0' is
`0x04`, space is `0x18`, TAB is `0x08`).

The bridge now explicitly computes reference-compatible ASCII flags,
provides guest `_toupper`/`_tolower`, and reserves/stages the actual
128-byte classification array in IOP RAM immediately before the
kernel HLE stub block. SYSCLIB #9 returns that guest pointer; #8 reads
the same bounded classification domain and rejects out-of-range input.
The `iop_sysclib_ctype` CTest checks exported ordinals, the guest
table bytes, case conversion and fail-closed bounds. This does not
manufacture any IOP module start or proof of live game progression.

### Static IRX import inventory — proactive dependency visibility

`tools/iop_irx_imports.py` now reads the imported library name and ordinal
from **file-backed PT_LOAD** sections of a bounded ELF32/MIPS IRX.
Its signature matches `src/iop/irx_loader.cpp::resolve_imports`:
`0x41e00000` import header, eight-byte library name at +12, followed by
actual `JR RA / ADDIU zero,zero,ordinal` stubs. It never executes, links,
relocates or loads an IRX, and ignores non-file-backed and unsupported tables.

`tools/iop_disc_catalog.py` records each resolved static candidate's
`static_imports`, `static_import_count` and `static_import_status`,
plus a catalog-wide static count. Malformed or unfamiliar IRX import layouts
are reported as scan failure diagnostics without blocking a valid game boot.
`run_boot_session.py` prints one static import summary before native
translation; the already-uploaded `iop-disc-catalog.json` format now
carries the detail needed to plan *coherent future IOP HLE batches*
without discovering one unresolved import per separate Windows session.

The synthetic `iop_irx_import_scan` and existing extended
`iop_disc_catalog` CTests verify real PT_LOAD signatures, import order,
per-library ordinals, malformed headers, non-loaded data exclusion, and
catalog-to-runtime evidence isolation. Static imports remain candidate
evidence, **not** proof of actual SIFRPC requests, module starts, or
execution. No user game IRX contents are committed to this repo.
