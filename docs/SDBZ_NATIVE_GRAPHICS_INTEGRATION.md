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
scripts\run_ps2_game.bat --full
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


## Current retail graphics blocker — VIF1/VU1 producer gate

The completed November 21 matched Linux 6,000,000-dispatch neutral/Cross
replays established authentic game input handling: the original EE branch
`0x00109D50` executed 0/8 times. The same runs produced **zero textured or
nonblack sprite draws**. Prior *full* Windows GPU captures from the retail
ELF agreed byte-for-byte with CPU scanout and were entirely black. The GS did
render genuine untextured sprites, but their original RGBAQ RGB bytes were
zero and GIF PATH1 committed zero VU1 XGKICK batches. Therefore, changing
WebGPU shaders or forcing colours would not address the evidenced missing
guest graphics production.

This diagnostic batch adds the read-only `m2_vif1_producer` JSON object to
both interim and stopped full runner reports. It distinguishes:

- `mpg_uploads`: committed VU1 microprogram uploads.
- `unpack_commands`: committed VU data uploads.
- `direct_qwc`, `direct`, `directhl`: genuine PATH2 traffic.
- `mscal`, `mscalf`, `mscnt`, `vu_program_starts`: **committed**
  VU1 start commands, which do not themselves prove XGKICK was executed.
- `vu_start_attempts`: parsed VU1 start commands, including DMA retries;
  `vu_start_rejections` and `last_rejected_vu_* ` preserve rejected
  microinstruction PC/lower/upper even when a later packet succeeds.
- `rejected_dma_attempts`, `first_rejection` and last rejection fields:
  separate unsupported VIF/VU instructions from a guest that never
  attempts a VU1 start.

The adjacent `m2_gif_paths.paths[*]` records
`committed_image_nonzero_bytes` and
`committed_image_payload_fnv1a64` for the original GIF IMAGE/IMAGE2
payloads accepted by the GS sink, separated by PATH1, PATH2 and PATH3.
A large `committed_image_qwc` with zero nonzero payload bytes indicates
that only zero-valued image data reached those parser boundaries.
Nonzero input indicates some real image bytes reached the sink; it does
**not** establish texture sampling, CLUT correctness, frame-buffer
writes or a visible display. Digests are over the concatenated accepted
IMAGE event bytes in transaction order, and equal digests are only a
diagnostic hint, not pixel-equivalence evidence.

The command histogram is accumulated on a **transactionally committed VIF
machine**, not guessed from DMA QWC. Rejected chunks do not count as
committed commands. Attempted-start/rejection counts are diagnostic only:
the same rejected guest packet may be retried, so they are not unique
guest operations. No IOP/EE/GS, DMA completion, VU1 state, guest instruction,
IRQ, scheduler or display semantics were changed.

### Next full retail replay

After checking out PR #148 and preserving the verified translation cache,
run:

```bat
git pull --ff-only
scripts\run_ps2_game.bat --full --neutral
```

Inspect the generated neutral ZIP's stopped `first-boot.json`:
compare `m2_vif1_producer`, all three `m2_gif_paths.paths` including
image payload nonzero counts/digests, `m2_gif_gs` and
`m2_gs_presentation`, plus all three PPMs.

Interpretation:

1. Zero `mpg_uploads` and `vu_start_attempts` indicates graphics
   microprogram dispatch is **not yet reached**. Investigate original
   EE/ARKD/SIF/asset-boot progress, not shaders or VU1 arithmetic.
2. Nonzero `mpg_uploads` and VU attempts, but zero committed starts with
   `vu_start_rejections > 0`, identifies a VIF/VU implementation blocker.
   The recorded failing VU instruction directs a focused reference-backed fix.
3. Committed VU starts with zero PATH1 XGKICK identify a microprogram
   control-flow or data-producer gap. Compare actual VU1 program trace and
   output before altering GIF/GS behavior.
4. Nonzero accepted PATH1 packets with zero visible pixels shifts
   investigation to *committed* GIF/GS render state, display selection,
   image upload and CPU/WebGPU parity.

This is a **diagnostic milestone**, not a claim of a visible frame.
No synthetic first frame or fabricated game GS writes are introduced.

## PR #148: bounded GPU framebuffer and display-field provenance (22 November)

The later USA retail replay at commit `1cf23fe` has **47,217 guest textured
sprite kicks**, **20,262,313 nonzero committed PATH2 IMAGE bytes** and no VIF1
DMA rejections at 6,000,000 EE dispatches. The selected display readback
was black, while sampled draws targeted FRAME base 0 and the selected DISPFB
used base 112. Do not treat the older TEXFLUSH PATH2 rejection as the current
blocker or force a guest-visible buffer swap to make the image appear.

The native runner now provides an **opt-in field-start probe** independent of
the raylib window. Windows CMD, after pulling PR #148:

~~~bat
set "SDBZ_GS_FIELD_PROBE=build\gs-field-probe.jsonl"
set "EE_WEBGPU_SNAPSHOT=1"
scripts\run_ps2_game.bat --full --neutral
set "SDBZ_GS_FIELD_PROBE="
~~~

The direct native runner also accepts `--gs-field-probe=FILE`; the CLI
argument overrides the environment variable. WebGPU is initialized for
this probe even when no window or final-snapshot option is requested.
The JSONL path's parent directory must already exist. The existing
translation cache is not touched by the probe; normal launcher translation
provenance rules still apply. The diagnostic is **opt-in** because a GPU
readback can stall and materially slow the run.

For late boot graphics, set `SDBZ_GS_FIELD_PROBE_START_DISPATCH=4350000`
before launching. This waits for the first field boundary at or after
that dispatch and preserves the full sample budget for later fields.

The JSONL file begins with a configuration record, then contains **at most
64** `field_started` records. Each is captured after the actual guest
scanout scheduler advances, and includes:

- Guest field count and skipped/coalesced fields; native dispatch count,
  compatibility EE cycle, guest EE PC, sprite and GPU draw totals.
- Original PMODE, SMODE2, DISPFB1/2, DISPLAY1/2 and CRT mode; both GS
  FRAME_1/2 register values and whether each was written by guest GIF.
- Three **separate GPU-side** target readbacks: the *selected display*
  source, FRAME_1 and FRAME_2. FRAME targets use their original FBP, FBW
  and PSM but a common crop and width/height derived from the currently
  selected guest DISPLAY; unlike DISPFB, FRAME FBP uses bits 0..8 and FBW
  uses bits 16..21. Report each target's 64-byte-block base, stride, format,
  success, dimensions, RGB nonblack-pixel count and FNV-1a RGBA digest.
- One diagnostic PPM for the **first successful** sample of each target,
  plus one for its **first nonblack** sample if different. Files are named
  `<jsonl-path>.field<N>.<display|frame1|frame2>.ppm`. The JSONL
  `ppm_written` flag refers only to that field's write, not to earlier
  images.

These captures use the GPU's own GS VRAM and **do not** write back into the
CPU GS VRAM, change FRAME/DISPFB, create guest fields, commit GIF/DMA,
schedule interrupts, or perform a presentation. The readback moment is
the host's first checkpoint after `fields_started` advances; it is not a
guarantee that the entire newly started field has been drawn, and missed
field boundaries are reported rather than synthesized. A readback can
also fail if the selected crop or FRAME pixel format is unsupported.

Interpret evidence in order: if a FRAME target becomes nonblack while
DISPFB stays black, investigate legitimate guest buffer switching and
copy/scanout order. If all GPU targets stay black, inspect rasterization,
clip/test/blend/texture state. If the display GPU target becomes nonblack
but the actual window does not, investigate surface presentation. Nonblack
sprites, nonzero upload bytes and valid GPU readbacks do not alone prove
the first rendered and presented game frame.


### Mount a virtual memory card

Set `SDBZ_MEMCARD` to a working copy of an existing raw PS2 memory card
image before running the launcher. Slot 1 accepts an 8 MiB card image
with spare bytes (8,650,752 bytes total). Use a formatted card with the USA
save for the USA executable.

~~~bat
set "SDBZ_MEMCARD=C:\PS2\memcards\Mcd001-working.ps2"
set "EE_WEBGPU_WINDOW=1"
scripts\run_ps2_game.bat --full --neutral
~~~

The guest's MCMAN and MCSERV modules communicate with the card through
SIO2. The runner writes changed card data back on normal shutdown. Use a
separate working copy for each emulator instance. Set `SDBZ_TRACE_MEMCARD=1`
to log serial commands and `set "SDBZ_MEMCARD="` to boot without a card.
Slot 2 remains empty.


Auto-expansion replays keep card writes in memory so each attempt starts
from the same on-disk image. The final full replay can persist writes when
the runner stops normally. Quick runs that finish during discovery and both passes of a
matched replay leave the mounted image untouched. The direct runner also accepts
`--memcard-discard-writes` for a read-mostly diagnostic replay.
