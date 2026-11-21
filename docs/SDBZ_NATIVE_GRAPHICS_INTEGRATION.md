# SDBZ native graphics integration — post-PR #145

**Target:** Super Dragon Ball Z (USA), `SLUS_214.42`  
**Base:** `main` after squash-merged PR #145 (`451ddff0d5ac2a1823de0666107e78299ce9cefb`)  
**Working branch:** `feature/sdbz-native-gs-presentation`  
**Status:** Graphics integration workstream opened; no new retail replay or first rendered game frame verified.

## Goal and inherited evidence

Connect original guest-produced EE/VIF/VU/GIF/GS activity to faithful native WebGPU
rendering and scanout. Keep the current LLVM-IR-based recompiler and HLE-first
device architecture; use `twist84/ps2-web-recomp` branch `SDBZ` as the primary
reference, with PCSX2/Sony manuals for instruction-level edge cases.

PR #145 introduced real EE/IOP/HLE progress and VU0 `VRSQRT`/`VMULq`.
Its four CI workflows passed on head `64c5e73779338cc37835739c2b434264be96cbee`.
The last supplied matched retail replay predates the final VU0 fixes: it stopped
at EE PC `0x00106988` (`VRSQRT`), after 3,654,765 EE dispatches and
15 GS fields started. Advancing beyond `0x00106988` and `0x00106990` has
**not** yet been demonstrated by a post-fix game replay.

Existing code already supports a CPU-authoritative GS VRAM model, committed
GIF event mirror into native WebGPU, native compute readback, optional raylib
Win32 presentation, and distinct CPU display/upload diagnostic images.
Earlier game replays have proved GIF PATH3 sprite events reached the GPU
(see `docs/M2_3_0909_GS_SPRITE_PROVENANCE.md`); this is not proof of
the first visible SDBZ game frame.

## Batch 1 — Deterministic graphics provenance

Build bounded, read-only diagnostics from **committed** producer/consumer
boundaries. Keep statistics scoped to a single matched replay:

1. Account for real VIF1 DMA chain acceptance, VIF DIRECT/PATH2,
   VU1 XGKICK/PATH1, and GIF DMA PATH3 separately. Do not infer a PATH
   from a generic GS vertex counter. Include accepted/rejected packet
   counts and first bounded rejection reason, guest PC when attributable,
   and DMA channel/source metadata.
2. Record committed GS draws and image transfers, primitive types,
   target FRAME/ZBUF/SCISSOR context, texture-state writes, and the
   output's relevant GS display source (PMODE, SMODE2, DISPFB, DISPLAY).
   Keep first/last bounded samples instead of enormous guest dumps.
3. Record the actual field/present boundary separately from a GPU
   snapshot request and from VRAM upload completion. Distinguish
   `field_started`, `valid_scanout`, `present_attempted`,
   `present_succeeded` and `image_readback_succeeded`.
4. Compare CPU display readback and native GPU display for the **same**
   guest register snapshot. Where valid, report dimensions, nonblack
   pixel counts and deterministic checksums to narrow framebuffer
   mismatches; never call a black PPM a renderer failure on its own.

Deliverable: focused smoke/regression tests for the real committed event
order, bounded report fields and invalid display configurations. No guest
state mutations for instrumentation; rejected GIF/VIF transactions must
not produce mirror callbacks, device completion, IRQ or present.

## Batch 2 — GS/GIF/VIF/VU graphics parity

For blockers reached in the next matched retail replay, apply coherent
reference-backed implementation batches:

- Preserve original VIF1 state, VU1 code/data residency and dual-issue
  execution, XGKICK packet ordering and real GIF PATH1 arbitration.
- Ensure VIF DIRECT/PATH2 and DMA GIF PATH3 have independent streaming
  parsers and commit only accepted input; investigate packet ordering
  against the primary reference.
- Verify each accepted GS register, primitive and image-transfer event
  reaches the native bridge with equivalent state. Compare FRAME/XYOFFSET,
  SCISSOR, TEST, RGBAQ, TEX0/TEX1/TEX2, PRMODE/PRMODECONT and relevant
  transfer modes before changing shaders or GPU pipelines.
- Fix format, CLUT, blend, depth, swizzle or target-selection parity only
  with reproducible source events and a regression case.

Fail closed for unsupported commands. Never inject display traffic or
alter scheduling, thread priorities, DMA completion or interrupt state
just to produce a picture.

## Batch 3 — Real presentation and acceptance

Keep raylib as a native host window/input loop and wgpu-native as the
GS renderer and swapchain presenter. Only an actual guest field with a
supported guest-configured scanout may request presentation.

Acceptance evidence from one matched retail run must identify:

- Post-merge commit SHA, launch flags, replay index, log/report pair,
  and VU0 progress beyond the prior `VRSQRT`/`VMULq` stop.
- Genuine guest producer path and committed GS primitive/render-target
  writes, plus a valid GS scanout configuration.
- Distinct successful WebGPU presentation and readback events with
  corresponding capture(s), dimensions and pixel statistics.
- Nonblack visible game imagery attributable to actual guest output
  before claiming the **first rendered game frame**. CPU/GPU and
  field/VRAM-upload distinctions must remain explicit.

A blank window, synthetic smoke image, offscreen snapshot alone, GS
field count or exhausted dispatch budget does **not** satisfy this gate.

## Validation and workflow

Work exclusively on this new draft PR until explicitly asked to squash
merge. Keep native WebGPU Linux, Windows MSVC, Windows Clang, main
build/CTest shards, SIF DMA acceptance and ROM0 execution green at
each final tested commit. Compare `ps2-web-recomp/SDBZ` before changing
device semantics, and add targeted tests per implementation batch.

Local Windows workflow keeps the existing build and translation cache:

```bat
git pull --ff-only
git rev-parse HEAD
set "MAX_DISPATCHES=4000000"
set "BOOT_TIMEOUT_SECONDS=300"
set "EE_WEBGPU_SNAPSHOT=1"
set "EE_WEBGPU_WINDOW=1"
run_ps2_game.bat --full
```

The launcher forcibly disables both optional graphics flags for --quick;
use --full for native WebGPU snapshots and window presentation.
Do not delete/rebuild the cache wholesale;
let translation provenance invalidate affected IR.

**Evidence rule:** Treat a ZIP of matched full/quick boot logs, JSON,
translation cache and any available PPMs as authorization to investigate. Identify replay
boundaries and never sum final-JSON counters across replays. Prioritize
causal graphics findings without regressing authentic EE/IOP execution.

## Implemented Batch 1: read-only graphics provenance

The draft PR now introduces:

- `c_ee_gif_path3::counters()`: parser-local accepted/rejected batch
  counts, committed GIF tag/primitive/packed/reglist/image event counts,
  and accepted input/image QWC. Counters increment **only after the GS
  event sink accepts** input; resetting a parser also resets its counters.
- `m2_gif_paths` in the matched runner report: distinct VU1 XGKICK/PATH1,
  VIF1 DIRECT/PATH2, and GIF DMA/PATH3 parsers. A VIF1 DMA without
  generated GIF traffic does not count as PATH1 or PATH2.
- `m2_gs_presentation` in the same report: guest fields from the
  scanout clock; real raylib/WebGPU presentation attempts and successes;
  supported source attempts; fields skipped by host polling; committed
  GPU draws; and raw privileged GS display registers.
- Independent CPU GS display, native WebGPU display and completed upload
  capture metadata: requested/readback/file-written states, dimensions,
  nonblack RGB pixel counts and FNV-1a RGBA checksums. CPU/GPU digests
  are compared only if both used the **same original** PMODE, SMODE2,
  DISPFB1/2, DISPLAY1/2 and CRT mode. Checksums are diagnostics, not
  a mathematical proof of pixel equality.

Regressions: `ee_gif_path3` verifies committed parser counters,
rejection rollback and retry; `ee_gs_presentation_probe` verifies
field gating, host-poll gaps, source matching, capture validation and
JSON. Its checks run in Release builds as well.

This instrumentation neither creates a GS draw nor changes guest DMA,
interrupts, VBlank, RPC, or scheduling. It does **not** prove that
the game has rendered its first visible frame. A matched full Windows
replay was supplied on 21 November and is analysed in
`docs/SDBZ_PR148_GRAPHICS_REPLAY_20261121.md`: code progressed
beyond the previous VU0 blocker, native GS presentation succeeded,
but the actual stopping failure is guest IOP thread 81 executing
unsupported text-like opcode `0x6e736168` at `0x001cdf78`.
