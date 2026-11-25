# M2 — SDBZ native runtime progression, GS/VIF/VU and EE ISA coverage

> **Record status (2026-11-25):** Historical M2 implementation plan; its initial boot/ISA gaps are not a current feature inventory. See [current status](README.md), [retail validation](SDBZ_RUNTIME_VALIDATION_20261125.md) and [accuracy plan](ACCURACY_PLAN.md). Any branch, PR, pending replay or feature-status claims below refer to the documented stage.


> **Current checkpoint (2026-11-25):** [Retail gameplay, audio and runtime validation](SDBZ_RUNTIME_VALIDATION_20261125.md). The earlier boot milestones below are historical; the HLE-first runner now reaches bounded interactive training gameplay with audible audio. Full compatibility and hardware accuracy remain open.

**Status:** implementation plan; not a claim of successful title-screen boot or GPU presentation  
**Date:** 2026-11-19  
**Repository:** `twist84/MIPS-To-LLVM-IR-To-C`  
**Working branch:** `feature/sdbz-hle-first-bootstrap`  
**PR:** [#142](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/pull/142) (draft; do **not** merge without instruction)  
**Starting code commit:** `b08870e82dc52a166920df048f7c195e2d15c147`  
**Game:** USA `SLUS_214.42`, extracted `PS2_GAME` disc folder, HLE-first execution  
**Primary architecture and behavior reference:** [`twist84/ps2-web-recomp`, `SDBZ`](https://github.com/twist84/ps2-web-recomp/tree/SDBZ)  
**Hardware manuals:** Sony EE Core [User's Manual](https://docs.alexrp.com/mips/ee.pdf), [Instruction Set Manual](https://docs.alexrp.com/mips/ee_insns.pdf), and [VU User's Manual](https://docs.alexrp.com/mips/ee_vu.pdf), v6.0 (April 2002). PCSX2 is a secondary functional cross-check, not the primary HLE architecture or an independent R5900 cycle oracle.

This plan combines the native-boot/GS investigation with an external **static** audit of unsupported EE instructions and peripherals, supplied 2026-11-19 as `Pasted text(20261119-074045).txt`. That audit inspected the three manuals and branch code and reported a decoder operand sweep, **but did not build the complete project, run SDBZ, or run CTest**. Its static gaps are hypotheses to reproduce and classify; they do not by themselves establish the current boot blocker. The original audit is an external input, not a substitute for source-level and runtime validation.

## 1. Evidence baseline and correction to prior timing claims

Treat these 07:27 inputs as **one matching batch**, never mix timestamps from other runs:

- `boot(20261119-072747).log`
- `expansion(20261119-072746).log`
- `first-boot(20261119-072746).json`
- `iop-disc-catalog(20261119-072749).json`
- `translation-cache(20261119-072750).json`
- `gs-display(9).ppm`, `gs-upload(10).ppm`, and separately supplied `gs-webgpu.ppm`.

Observed: **419** compiled functions out of **7,901** ELF-discovered entries (discovery count is **not** the expected number of dynamically needed functions); four expansion passes added 128, 128, 128 and 34; the final runner stopped at the **100,000-dispatch cap** with PC `0x00172e48`, `RETURN` status 1, and 1,137,072,516 guest instruction attempts. It reported **161 scheduler yields, 230 GS fields, 4,584 accepted GIF tags, two recorded vertices**, and 16,384 image QWC. The IOP catalog contains **seven ELF-static IRX path candidates** but only one *observed successful runtime start*, `SIO2MAN.IRX`, across 13 committed SIF RPCs.

The final CPU display (512×448) and GS upload (1024×64) were black. **The final 07:27 command did not contain `--gs-webgpu-snapshot`.** Consequently the separately uploaded one-red-pixel `gs-webgpu.ppm` has **unverified run provenance** and must not count as proof of a GPU frame, primitive submission or this run's WebGPU execution.

**Timing correction:** the external audit states the cycle source is unavailable, Count/Compare and VBlank cannot advance, and the game is still blocked at `0x00175210`. This describes an earlier stage. The current `--hle-startup --ee-clock=reference-compat` runner has a working **approximate** guest-instruction-budget clock and serviced **230 GS fields**. This is **not validated R5900 pipeline timing**. Neither mark timing calibration complete nor treat the old `0x00175210` wait as the current blocker. See `docs/M1_A_EE_TIMING_STATUS.md` (especially its later compatibility-clock section) and `src/runtime/ee_cycle_scheduler.cpp`.

**Current question:** what guest-visible state or hardware/IOP/GS dependency keeps the game on its repeating update path without meaningful framebuffer output? A repeated path can be legitimate idle execution; a high dispatch count, unchanged image, or a registered callback does not independently prove a defect.

## 2. Priority and evidence rules

Implement in **small, tested batches**. For each candidate gap label its evidence:

- **R (reached):** exact PC/opcode, runtime service or DMA event occurred in a matched game trace.
- **S (static):** instruction appears in executable code, decoder enumeration or ELF string, but reachability is unproven.
- **U (unverified):** external/manual claim awaiting independent code/manual validation.
- **T (tested):** semantics are verified by focused decoder, LLVM execution and/or real hardware-reference differential regression.
- **F (fixed):** demonstrated incorrect/missing behavior, code corrected, and baseline-preserving CI passed.

Do **not** infer execution from an ELF static IRX string or a syntactically recognized opcode. Distinguish `VALID` decode, actual LLVM lift, guest execution, modeled hardware effects, and correctness tests.

Work order is driven by **R evidence + ability to unlock initialization/rendering**, not by how many manual opcodes are missing. Do not switch to a full CPU emulator or execute every unneeded subsystem. If evidence shows a genuine missing VIF/VU/IOP stage, address it before nonblocking scalar ISA completeness.

## 3. Implementation batches (required deliverables and gates)

### M2.0 — Reproducible diagnosis and instruction-coverage inventory

1. Pin/record revision, ELF SHA-256, CLI options, report schema and batch timestamp. The user runs `git pull` then `scripts\run_ps2_game.bat` which **configures/builds**; never presume an old EXE based on familiar output. Confirm whether `EE_WEBGPU_SNAPSHOT=1` was actually forwarded into the final runner.
2. Implement a deterministic, bounded **ELF instruction inventory** (not naive linear sweep through data), using function/CFG discovery and the actual decoder. Summarize by opcode/family: encountered in executable discovered blocks, `VALID`/`UNSUPPORTED`/`INVALID`, LLVM lowered, covered by tests. Separately ingest **reached** opcode evidence from an optional bounded guest instruction trace. Report each count's sampling/coverage limitations.
3. Provide counters/exportable JSON for unique hot PCs, hottest transitions, repeated state deltas and last relevant syscall/interrupt/DMA/RPC without adding millions of trace lines. Never change host guest timing while instrumenting.
4. Add synthetic fixture tests for classification, correct detection of a previously unseen opcode, and stable JSON schema. Keep per-step aggregate output short; detailed traces belong in diagnostic files.

**Gate:** reproducible artifact tells us whether runtime-supported instruction holes were actually encountered in this boot. Do not use the external audit's “45 supported / 54 unsupported out of 99 MMI” or “about 82 macro operations missing” as canonical counts until regenerated with documented enumeration rules.

### M2.1 — Explain the hot update loop (highest boot priority)

1. Reconstruct ELF call/return, CFG and state flow around `0x00172e48` and its recurring callers/callees, including `0x00172e20`, `0x001730c0`, `0x00102670` as trace-guided starting points, not presumed single culprit.
2. Identify exact wait predicates and guest-owned variables: thread/semaphore state, interrupt enable/pending/ack, DMA completion, SIF RPC replies, host timer/Count/Compare, GS CSR/field transitions and application-state changes.
3. Correlate bounded transition histograms with concrete memory/register reads and writes, comparing beginning and end of the run. Identify whether loop is a healthy idle/update pump, an unmet real dependency, or a control-flow/return-state error.
4. Fix only the **first demonstrated** defect with a deterministic regression. Do not “solve” a wait by synthesizing callbacks, timers, successful RPCs, arbitrary interrupts or GPU draws.

**Gate:** written PC→predicate→producer dependency explanation and evidence that the proposed correction changes guest-visible state appropriately, not just the dispatch count.

### M2.2 — IOP / SIF bootstrap dependency graph

1. Audit the 13 committed RPCs and one successful `SIO2MAN.IRX` start in chronological order, including real arguments, responses, module IDs, thread IDs, imports, callbacks and awaited semaphores. Log only actual committed requests and guest-visible results.
2. Map each ELF-static candidate (`PADMAN`, `SIO2MAN`, `MCMAN`, `MCSERV`, `LIBSD`, `CRI_ADXI`, `ARKD_DVD`) to **observed requested / unrequested / blocked / unresolved**; no assumption all seven should load at this point.
3. Compare the exact required HLE boundary and native disc-folder transport with `ps2-web-recomp:SDBZ`. Keep distinction between HLE kernel services and executing game-provided IRX R3000A code.
4. Implement missing supported services in coherent batches with explicit state-machine and negative-path tests. Do not claim static imports or HLE reply stubs are executed IRX code.

**Gate:** a request/reply/handler dependency graph with the first confirmed incomplete transition or a documented healthy chain.

### M2.3 — GIF, GS, VIF/VU1 and native WebGPU provenance

1. Report committed GIF PATH tags by PACKED/REGLIST/IMAGE/A+D, `NLOOP`, PRIM and vertex writes; meaningful per-register counters and transfer destination/bounds; host-to-local transfers and framebuffer/display circuit state.
2. Trace VIF0/VIF1/DMAC channel requests and VIFcode **if reached**. Determine whether lack of `UNPACK`, `MSCAL`/`MSCALF`, VU1 micro execution, `XGKICK` or GIF PATH1 really blocks native SDBZ graphics. Missing support is a *candidate*, not evidence it was used.
3. Compare `src/runtime/ee_webgpu_bridge.cpp` event consumption and `src/runtime/ps2_webgpu/gs/` with `ps2-web-recomp:SDBZ`. Check the 2-vertex/4,584-tag ratio before changing primitive semantics.
4. Explicitly enable native WebGPU for the final replay, verify adapter initialization, callbacks/committed GIF events, submitted draw count, readback width/height and output bytes. Always report **`draws=N`** or a specific failed reason. Compare GPU output with the **matching** CPU PPMs; delete prior stale outputs before launch.
5. If real guest VIF/VU1/GS commands are present but unsupported, implement the smallest correct transfer/execution path with reference-backed unit tests and native GPU smoke. A successful PPM is not a present event or visible game frame.

**Gate:** distinguish **(a)** no guest drawing yet, **(b)** committed graphics traffic not reaching the GPU bridge, **(c)** unsupported VIF/VU path, **(d)** invalid display setup or VRAM state, **(e)** actual native WebGPU defect. Any rendered output must be attributed to the same run.

### M2.4 — Targeted architectural correctness batches (parallel backlog, not automatic boot prerequisite)

Reproduce static claims against current code and Sony manuals before enabling the decoder:

| Candidate family from external audit | Current assessment / approach | Proof required to close |
|---|---|---|
| `ADD/SUB/DADD/DSUB/ADDI/DADDI` | Fail-closed for `EX_Ov`; add signed overflow detection and guest EPC/BD/EXL exception delivery | in-range + overflow, 32/64-bit and delay-slot tests |
| `TGE[U]/TLT[U]/TEQ/TNE` and immediate variants | Decoder rejects despite apparent `EX_Tr` lowering; audit reserved fields and branch-delay exception behavior | false/true trap and correct exception context, then un-gate |
| Scalar `LH/LW/LD/LQ` etc. misalignment, coprocessor usability | VU0 quadword exception path already exists; extend scalar `AdEL/AdES` and `CpU` only with precise context | aligned, misaligned, side-effect-free failure, EPC/BD |
| `MFSA/MTSA`; `FCR0`/COP1 condition forms; COP0 `BC0*` and read-only register semantics | Confirm exact architectural encodings and guest demand; implement with tests | decoder→LLVM→native execution, reserved-field and nontrapping paths |
| Missing packed MMI | External sweep reported 54/99 unsupported, e.g. `PEXTLW`, `PPACW`, `PINTH`, `PSLLW`, `PSRAW`, `PMULTW`, `PMFHI/PMFLO`, `PLZCW` | priority driven by reached ELF PCs; 128-bit lanes, signedness, saturation, HI/LO, aliasing |
| VU0 macro and micro | Transfers, BC2, `LQC2/SQC2` and eight named macro operations already supported. Remaining macro families, `VCALLMS/R`, upper FMAC, dual issue, branch/control flags, Q/ACC forwarding and hazards remain candidates | test supported set first; implement reached families, VF0/VI0 and sticky flag/hazard fidelity |
| VU1, VIF0/VIF1 and PATH1 | Audit claims micro engine, VIFcode and `XGKICK` absent; first prove request/traffic in SDBZ | executable VIF/VU fixture, correct GIF PATH1 producer/consumer |
| TLB, privilege/exception tiers, caches, `CACHE`/`SYNC`, perf/debug registers | Broad hardware fidelity gaps; lower boot priority unless a current predicate reads/uses them | exact guest-visible effects and reserved/illegal behavior, not unconditional success |
| Calibrated R5900 pipelines/Count/Compare | Separate M1-A accuracy program, **not** the same as the working reference-compatible HLE clock | independent hardware/reference differential before enabling `validated_r5900` |

All new instructions must remain **fail closed** until *decode, lift, runtime effects, and error behavior* are tested. Prefer families of logically related opcodes per commit, not one-at-a-time patches or an unchecked “mark VALID” sweep. Track static presence separately from boot reachability.

### M2.5 — Validation, documentation and evidence-based promotion

1. Run targeted unit/IR/native tests for every changed family (e.g. `tests/decoder/decoder_test.cpp`, `tests/smoke/mmi_ops_test.cpp`, existing VU0 and GIF/GS/IOP/DMAC test families); add new fixtures for observed cases.
2. Run full CTest, native WebGPU Windows/Linux smoke, SIF DMA acceptance, ROM0 execution and CI on the **exact new commit**. Record workflow URLs and whether jobs truly completed. No green claim from an in-progress run.
3. Preserve `scripts\run_ps2_game.bat` startup, `build/_deps/wgpu-native`, live in-place `[current/discovered]` IR progress, native artifact cache integrity and independent compiled-guest DLL linking. No unrelated framework replacement.
4. Before requesting a local test, finish all possible source, ELF-free fixture and CI work. Ask for a new matched five-file boot batch **only** when there is a concrete instrumentation/fix checkpoint; when graphics is enabled also request CPU display, upload and WebGPU PPMs and confirm the final runner includes `--gs-webgpu-snapshot`.
5. Compare exactly the same metrics with 07:27. A change is progress only if a real awaited state transitions, a new game phase is reached, actual committed draw/transfer events change, or a verified framebuffer becomes nonempty. Raising `--max-dispatches` alone is **not** success.
6. Keep this plan and `docs/GAME_BOOT_PROGRESS.md` updated with commit, tests, unknowns, next actionable blocker, and whether M2 remains partial.

## 4. Explicit stop/advance rules

- If M2.1 shows a **normal** update loop, pivot to the exact graphics/IOP init state it is waiting to reach rather than “fixing” scheduling by guesswork.
- If a **reached unsupported opcode** appears, immediately prioritize its documented opcode family and prove native semantics before continuing.
- If **VIF1/VU1 traffic** is present, prioritize the smallest correct VIF/VU1→GIF path before speculative GS renderer modifications.
- If **all committed GIF activity is uploads and setup**, do not treat a black GPU frame as an independent WebGPU failure.
- If the **GPU receives actual draw calls** but GPU/CPU VRAM disagree, isolate the earliest incorrect register, transfer, primitive or display circuit using paired reference tests.
- If the first proven gap is **IOP kernel or game IRX execution**, implement its actual request/state transitions rather than fabricate immediate replies.
- Keep offline M1-A timing calibration independent; preserve explicit `reference_compatibility` vs `validated_r5900` labels.

## 5. Milestone acceptance checklist

- [ ] Bounded, reproducible hot-loop predicate/dependency report tied to exact guest PCs and events.
- [ ] ISA inventory with static/reached/VALID/lowered/tested axes, no inflated coverage claim.
- [ ] Evidence-backed IOP requested/started/unrequested module map.
- [ ] GIF and (if relevant) VIF/VU traffic breakdown explaining the two recorded vertices.
- [ ] Fresh WebGPU-enabled run proven by runner args, initialization, `draws=N`, and same-batch readback.
- [ ] Confirmed faults fixed with architecturally correct, fail-closed behavior and regression tests.
- [ ] Full CI green at the exact implementation HEAD, draft PR #142 unmerged.
- [ ] A **meaningful guest-visible change** from 07:27, or an evidence-backed explanation of why the observed state is expected and the next correct dependency.

### Local validation commands (when requested)

```bat
git pull
set EE_WEBGPU_SNAPSHOT=1
scripts\run_ps2_game.bat
```

Collect the newest matching `boot`, `expansion`, `first-boot`, `iop-disc-catalog`, `translation-cache` diagnostics and the three PPMs. A PPM without confirmed final runner flags and matching timestamps is not evidence.

### Related repository documents

- [Native game boot progress](GAME_BOOT_PROGRESS.md)
- [HLE-first runtime architecture](HLE_FIRST_RUNTIME_PLAN.md)
- [Native WebGPU integration](NATIVE_WEBGPU.md)
- [M1-A compatibility timing and validation boundaries](M1_A_EE_TIMING_STATUS.md)
- [Primary-reference architecture audit](M1_A_PS2_WEB_RECOMP_REFERENCE_AUDIT.md)

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

M2.4 follow-up: preserve both `wait` (first entry) and `pending_wait` (first **nonzero** opposite-slot flag) separately; the first call may succeed. Red-loop entry is a third independently bounded sample. Do not mistake the first successful wait for the stuck one.


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


## M2.6 — VIF1 native consumer (19 November 2026; partial)

The authoritative next milestone is [M2.6 Native VIF1 Consumer](M2_6_NATIVE_VIF1_CONSUMER.md). The native runtime now ports VIF1 command parsing, STCYCL/MODE/BASE/OFFSET/ITOP/ROW/COL/MASK, UNPACK, MPG and DIRECT/DIRECTHL semantics from `twist84/ps2-web-recomp:SDBZ`. Commands and VU1 memory changes are transactional, and split-packet tails persist across DMA starts. Channel 1 now routes to VIF1 rather than the GIF PATH3 sink; GIF PATH2 uses a separate parser and the existing committed GS consumer. Original 16 KiB VU1 code/data memory is visible through its physical EE apertures and survives VIF FBRST.

A standalone `ee_vif1_device` CTest regression checks device parse, data/code uploads, PATH2, FIFO, split packets, refusal/rollback, and successful *synthetic* source-chain TTE/DMAC1 completion. All user coding standards (Allman, snake_case, `c_`/`s_`/`m_`) are retained; MIT upstream attribution is kept in the source.

**Not yet accepted:** VU1 MSCAL/MSCALF/MSCNT micro execution, XGKICK/PATH1, VIF IRQ/stall/resume and MSKPATH3 arbitration remain fail-closed. In particular, this batch is not evidence of original SDBZ DMA completion, natural release of the guest pending flag, a new displayed frame or disappearance of the red fallback. The only confirmed 13:53 boot predates the raw source-prefix probe and does not contain the tag bytes at `0x00771400`. The next same-invocation Windows log is needed to select the next reference-derived implementation batch, not to justify a shortcut.

Preserve existing M2/M1-A clocks, GIF PATH3, WebGPU, SIF, IOP and EE ISA behavior. Complete CI on the exact post-documentation commit before requesting local validation. Keep [PR #142](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/pull/142) draft and unmerged.


## M2.7 — Bounded VU1 interpreter and genuine GIF PATH1 (19 November 2026; partial)

Implemented a **strict subset** of VU1 micro execution from the primary ps2-web-recomp reference: MPG-resident microcode, MSCAL/MSCNT, E-bit delay slot, floating-point VF operations, VI branch view, memory operations and genuine XGKICK into PATH1. Unsupported opcodes, flag/Q/P pipelines, MSCALF, mixed-path arbitration and IRQs **fail closed**: no DMA completion or fabricated rendering. The new test exercises MPG → original VU1 microinstructions → GIF PATH1 tag/body, opcode rejection and arithmetic. See [M2.7 VU1 first batch](M2_7_VU1_NATIVE_FIRST_BATCH.md). The 13:53 USA game logs predate the VIF source prefix; do not assert native boot progression. PR #142 remains draft, unmerged.


### M2.7 Batch B — Reference FMAC/flags and XGKICK fixture (19 November 2026)

Ported ps2-web-recomp PS2 floating-point bit arithmetic to C++17 and expanded VU1 ACC, MADD/MSUB, FTOI/ITOF, CLIP, MAC/status/clip flags and a four-issued-pair flag delay buffer. Added lower flag opcodes and focused arithmetic/CLIP regressions. Fixed synthetic GIFtag NREG to one. Q/P latency, precise VF/VI stalls, lower special opcodes, MSCALF, GIF arbitration and VIF IRQ remain unsupported and fail closed. Native USA boot advancement remains unverified. PR #142 stays draft/unmerged.
