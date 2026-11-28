Warning: truncated output (original token count: 30974)
Total output lines: 2297

# G1 — graphics performance attribution

## G1A: matched synchronous profiling on/off

The validated graphics sweep (67b6646) established architectural equivalence, not
a repeatable speedup. Use the same validated graphics runner, EE O5 image and
84 matched VU1 libraries in both arms. Do not regenerate EE IR or VU1 AOT.
Replay the same PAD0 recording and fixed RTC against the accepted terminal
report, sampled transitions and GS screenshot. Disable async VU1 and GS,
boundary timing, PC profiling and hotspot diagnostics. Alternate control/profiled
then profiled/control, with two repetitions by default.

G1A is an attribution checkpoint, not an optimization or a speed acceptance.
Measure profiler perturbation using process wall time; use unprofiled matched
experiments for future speed claims. Preserve the original and experimental caches.

### Existing instrumentation audit (67b6646)

* `src/runtime/ee_runtime_profile.h` uses thread-local steady-clock scopes.
  Nested children are subtracted from parents. The sum of exclusive durations
  equals completed root-scope coverage; uncovered time is separately reported.
  This measures elapsed runner-thread time, **not** OS-scheduled CPU time.
* `ee_native_boundary` is translated boundary execution plus unattributed
  callbacks; do not label its entire exclusive duration as pure EE arithmetic.
* VIF1 consume/command/unpack/MPG, DMAC normal/VIF1/GIF chains, GIF ordered and
  consume, GS transactions/CLUT/copy/upload/read-image and WebGPU bridge,
  draw/batch/submit/wait/present/readback already have scoped collectors.
* `vu1_run`, `vu1_compiled` and `vu1_xgkick` are separately scoped. The
  per-program report identifies PC, library index and code hash and exposes
  attempted and successful pairs separately from profiled counts and compiled
  exclusive duration. Do not add program times to runner-thread scope totals.
* VU1 worker queue/execution/wait/overlap counters describe a different timing
  domain and overlap EE work. G1A deliberately disables both async workers.
* GPU compute timestamps cover sampled regular passes only; detail passes,
  pending/failed queries and unsampled passes are explicitly counted. A null
  GPU total means unavailable, not zero. CPU submission/wait is not GPU time.
* VU0 synchronization, COP1, MMI and VU1 dependency scheduling are not
  separately attributable in the current scope inventory. Report them as
  unattributed, not zero. G1B will assess coarse scope boundaries or isolated
  measurement without per-instruction always-on instrumentation.
* Existing interval records preserve dispatch/cycle/field/presentation
  endpoints despite periodic JSON report replacement. They are not inherently
  matched scene windows: wall-clock-triggered intervals can end at different
  guest dispatches between runs.

### Receipt contract

`tools/replay_ee_graphics_profile.py` writes
`profiling-receipt.json` and archives raw stdout/stderr, terminal report and
GS screenshot for each arm. The schema is
`ee-graphics-profile-receipt-v1`. All durations ending in `_ns` are integer
nanoseconds; process wall seconds are floating point. No device time is
implicitly added to another timing domain.

| Field | Meaning |
|---|---|
| schema | Versioned receipt identifier |
| inputs | SHA-256 of runner, guest ELF, both linked EE images, source receipt, PAD recording, reference report/trace/screenshot, optional configured memory card |
| vu1_inventory | target marker and filename-to-SHA-256 map |
| environment | shared effective replay environment; per-run profile override is recorded separately |
| recorded_polls, max_dispatches | fixed replay workload |
| mode | synchronous_runner_thread |
| runs.control / runs.profiled | ordered lists of independent run records |
| median_wall_seconds | process wall medians by arm, only after all runs pass |
| profiling_overhead_ratio | profiled/control median minus one; diagnostic only |
| correctness_accepted | matched replay plus valid profile accounting and unchanged input identities |
| performance_accepted | always false for this checkpoint |
| error | null or refusal reason |
| inputs_unchanged | final hash and VU1 inventory verification |

Each run retains command, exit code, directory, wall seconds, profile enablement,
PAD summary, sampled-transition comparison, guest instruction attempts, EE cycles,
GS fields, VU1 pairs/runs and raw terminal report/screenshot SHA-256.
Its `attribution` is null in unprofiled mode; otherwise:

| Field | Meaning |
|---|---|
| runner_thread | original ee-runtime-profile-v1, including exclusive/inclusive counters, calls, work units, coverage, wall and uninstrumented time |
| vu1_programs | original program rows, including code identity and profile counts |
| vu1_worker | original worker data, kept outside runner-thread accounting |
| intervals | original ee-runtime-profile-interval-v1 rows, with guest endpoints and counter deltas |
| unattributed | missing separate scope categories |

Validation requires the terminal dispatch budget and exit code 4, all recorded
polls consumed, identical nonempty sampled transition sets, identical normalized
terminal reports, identical GS screenshot, no VU1 fallback, no timing observer
or IOP guest fault. In addition, the profiled arm must emit the known profile
schema, clock and scope, have real profiled VU1 attempts and contiguous intervals
reaching the terminal guest endpoints. Exclusive sums must equal covered time;
covered plus uninstrumented time must equal wall; interval call, inclusive,
exclusive and work totals must reproduce terminal counters.

Architectural normalization is deliberately explicit: the dedicated
runtime_profile diagnostic object (validated independently); only
profiled_attempts, profiled_pairs and compiled_exclusive_ns per VU1 program;
and worker_execution_ns, worker_queue_ns, compiled_exclusive_ns,
xgkick_exclusive_ns, controller_wait_ns and ee_native_overlap_wall_ns
in the worker object. All other report fields must match, including unknown
fields with a timing-like suffix. Preserve the unmodified reports in the archive.
This is bounded replay evidence, not a proof of full machine-state equivalence.

## G1B and later

Analyze actual G1A receipt before prioritizing G2–G7. Rank exclusive scopes
and program costs; distinguish inclusive time, waits and GPU samples.
Examine scene-aligned guest windows using recorded endpoints where available.
Add missing attribution only where it can be measured accurately at existing
boundaries, with an explicit disabled path and directed regression coverage.
Collect async worker evidence separately; do not treat overlapped host durations
as additive wall time. Use interleaved unprofiled comparisons for actual
optimization acceptance.

No guest lowering, timing, runtime ABI or cache policy changes are made in G1A.

## G1A accepted — matched retail evidence (2026-11-26)

Archive: ee-graphics-g1a.zip. All five CI workflows passed at 809a207.
All four synchronous replays completed 40M dispatches, 184700 matching sampled
transitions each, 3127/3127 PAD polls, identical normalized terminal reports
and screenshot hashes, 21910026110 guest attempts, 22115080673 EE cycles,
4494 GS fields, 3221 presents, 394881 VU1 native runs and 445669423 pairs,
84 matched libraries, no fallback or IOP fault. Inputs unchanged.
Control 43.031/42.828s; profiled 49.406/49.406s. Median profiling overhead
15.09%; runner-thread mean wall 45.652s, covered 82.81%. No speed claim.

Mean exclusive elapsed seconds: VU1 compiled 13.036 (28.56% wall), EE
native boundary 7.635 (16.72%), IOP 7.234 (15.85%), WebGPU submission 1.359,
device service 1.340, VIF1 UNPACK 0.040. Uninstrumented 7.848s.
DMA VIF1 chain inclusive 17.419s versus exclusive 0.307s: nested VU1
execution accounts for much of the apparent DMA/VIF cost. Both GPU
timestamp series completed all 3882 regular compute passes, 7.080/7.116s
sampled GPU time, not additive with host elapsed time.

| VU1 PC | Code FNV1a64 | Native pairs | Mean compiled exclusive s |
|---|---|---:|---:|
| 0x0730 | 80e18246ed4922d9 | 204689960 | 7.412 |
| 0x0620 | fdffdb7fcbf68252 | 77732955 | 2.242 |
| 0x0130 | 2cf115e2c19ba013 | 64974482 | 1.068 |
| 0x0420 | 31d7741f1bfa69cf | 61907074 | 1.066 |
| 0x05a0 | cfae43a4ec036735 | 25493805 | 0.807 |

Top five account for 96.62% of measured VU1 compiled time; first alone
56.86%. Prioritize G2 direct control-flow lowering before VIF1 SIMD or
additional COP1/MMI instrumentation. VU0/COP1/MMI and dependency costs
remain unattributed; synchronous timing is not an async critical-path model.
G1B/G1C remain open. Retain v22 and existing EE/runner unchanged; compare
new VU1 libraries in isolation. No claim about unprofiled subsystem time
or async gameplay throughput.

## G2A — direct delayed-PC successor edges

The old dynamic emitter returns to one switch after every pair, including
ordinary sequential pairs. Retain the original discovery graph, instruction
emitters, local VF/ACC storage, exact code guard and native ABI. Each emitted
pair now has a local step-budget check and prelude. The graph enumerates
(PC, delayed NPC, pending E) states. For nonterminating states, the incoming
NPC identifies the next executed pair, not the new branch destination.
An unambiguous successor is direct when no indirect branch is reachable.
Ambiguous successors and all programs with reachable JR/JALR use a guarded
edge to one observed successor and the original reached-PC switch otherwise.
No speculative edges to unvalidated code. Preserve PC/NPC, flag history,
VI delay, dependency cycles, Q/P retirement and E delay semantics. No
per-pair profiling added. Straight-line lowering unchanged.

Existing indirect, conditional, wrap and pipeline fixtures exercise fallback
and guarded edges. Add a direct-only fixture with loop, delayed conditional
VI, direct return, branch in delay slot and E termination; verify generated
IR at O0/O3 against interpreter state. Build/CI and retail replay pending.

Use a new matched v23 inventory from the merged captures; retain v22.
The runner and EE library are not changed. Extend the profiling harness
with --candidate-vu1 and optional --profile-vu1 (both arms profiled).
The separate receipt schema ee-graphics-vu1-comparison-v1 reports candidate
inventory, per-pair wall seconds and descriptive relative throughput.
Default comparison is unprofiled; alternate arm order and use four repeats.
No acceptance of speedup is automatic. No cache promotion.

## G2A accepted — matched V22/V23 retail experiment

All five workflows passed at 952db5f. ee-graphics-g2a.zip contains four
interleaved pairs with profiling disabled. All eight runs reached 40M
dispatches, 3127/3127 PAD polls, 184700 identical sampled transitions per
run, 21910026110 guest attempts, 22115080673 EE cycles, 4494 fields,
394881 VU1 runs, 445669423 pairs, no interpreter fallback, IOP guest fault
or timing observer failure. Raw terminal report and GS screenshot hashes
were independently recomputed and match across all eight archives.
The input integrity receipt passed; all 84 library hashes changed while
the exact filename inventory and native target match.

| Pair | V22 s | V23 s |
|---|---:|---:|
| 1 | 43.063 | 41.782 |
| 2 | 42.875 | 41.468 |
| 3 | 43.047 | 41.657 |
| 4 | 42.907 | 41.890 |

Median 42.977 -> 41.7195 s: 2.93% less elapsed time, 3.01% more
throughput. All four pairs favor V23; slowest candidate beats fastest
control by 0.985s. This is accepted evidence for the fixed synchronous
workload, not proof of the same improvement under async gameplay.
V23 is the next experimental control; retain V22. No cache promotion,
no EE lowering or runner change. The receipt's performance_accepted
field deliberately remains false; acceptance is a separate review.

## G2B — two known delayed-PC successors

Record at most two distinct nonterminating incoming NPCs per emitted pair.
For a complete two-target discovery set without reachable indirect
branches, select either native successor with one comparison against the
incoming NPC. Otherwise guard each known successor individually and
retain dispatch fallback for unknown or third targets. Favor the ordinary
fallthrough edge first. No re-evaluation of a predecessor's VI predicate
after the delay pair, no change to guest issue, termination or validation.
Test both direct outcomes and guarded alternatives in the interpreter
differential fixture, including wrap and delayed branches. G2B build/CI
and matched V23/V24 replay pending. Retain V23, use fresh V24 inventory.

## G2B accepted — matched V23/V24 retail experiment

All five workflows passed at fa025ef. ee-graphics-g2b.zip contains four
interleaved pairs with profiling disabled. All eight runs reached 40M
dispatches, 3127/3127 PAD polls, 184700 identical sampled transitions per
receipt, 21910026110 guest attempts, 22115080673 EE cycles, 4494 fields,
394881 VU1 runs, 445669423 pairs, zero interpreter fallbacks and no IOP
guest fault or timing observer failure. Independently recomputed terminal
report and GS screenshot hashes match across all eight runs. Exact inventory
of 84 libraries and native target preserved, all 84 binary hashes changed.
Input integrity receipt passed.

| Pair | V23 s | V24 s |
|---|---:|---:|
| 1 | 41.750 | 41.235 |
| 2 | 41.625 | 41.406 |
| 3 | 41.547 | 41.125 |
| 4 | 41.406 | 41.141 |

Median 41.586 -> 41.188s: 0.96% elapsed reduction, 0.97% nominal
throughput gain. All pairs favor V24 but the 0.398s median difference is
close to the 0.344s control spread; retain V24 as the experimental
baseline, not a definitive magnitude of speedup. Compounded G2A/G2B
throughput ratio is approximately 1.0401, not a direct V22/V24 comparison.
No cache promotion, no EE or runner rebuild, no async performance claim.

## G2C.1 — specialize incoming state before block formation

Use the existing complete direct-flow discovery graph to collect NPC and
pending E facts for every reached pair, including terminating states.
Emit a constant incoming NPC and sequential NPC only when all observed
states agree and no indirect transfer is reachable. Emit an unconditional
retire or continue edge only if every incoming state agrees on pending E.
Mixed NPC/E states keep dynamic values. Programs containing indirect
transfers retain dynamic incoming state and existing guarded fallback.
Enter every native invocation directly at its known entry PC; retain the
emitted-PC dispatcher for unknown transfers. This is a first step toward
LLVM forming longer native blocks, not explicit multi-pair budget
hoisting. Preserve per-pair budget checks, callback order, dependency
cycles, VI delay, Q/P and flag retirement, final TPC and native ABI.
Extend O0/O3 differential fixture IR assertions for constant NPC and E
state, dynamic ambiguous NPC and guarded indirect cases. Build/CI and
V24/V25 matched replay pending. Retain all previous library inventories.

## G2C.1 accepted for correctness — matched V24/V25 replay

All five workflows passed at e160b54. ee-graphics-g2c.zip contains four
interleaved unprofiled pairs. All eight replays completed 40M dispatches,
3127/3127 PAD polls (raw stderr footers verified), 184700 matching sampled
transitions per receipt, 21910026110 guest attempts, 22115080673 EE cycles,
4494 fields, 394881 VU1 native runs, 445669423 pairs, zero interpreter
fallbacks, no IOP guest fault or timing observer failure. Raw report and
GS screenshot SHA-256 are identical across all eight. Exact inventory of
84 libraries and native target preserved, all 84 DLL hashes changed;
input integrity receipt passed.

| Pair | V24 s | V25 s |
|---|---:|---:|
| 1 | 41.844 | 41.359 |
| 2 | 41.453 | 41.375 |
| 3 | 41.313 | 41.218 |
| 4 | 41.391 | 41.484 |

Median 41.422 -> 41.367s, nominal throughput +0.13%, 3/4 pairs faster.
Difference 0.055s is within timing spread (control 0.531s; candidate
0.266s). Retain V25 as a correctness-validated foundation, not a
demonstrated speedup. No cache promotion or asynchronous claim.

## G2C.2 — bounded guarded multi-pair blocks

Form nonoverlapping runs of 3–8 reached, consecutive, nonbranch,
non-E pairs with a reached sequential exit. Exclude XGKICK and P/EFU
pairs from the fast block. At its entry test incoming NPC == PC+8,
pending E == false and remaining pair budget >= length. Failed guard
uses the original single-pair path, including partial execution up to
budget exhaustion. Retain every original emitted pair entry, including
interior entries and unknown indirect dispatch. The fast path reuses
the same pair body (issue cycle, dependency retirement, Q/P sync,
flag history, delayed VI snapshot, upper/lower ordering). Write-through
VF/ACC SSA cache spans only the single-entry fast block; reset for every
independent pair entry. Advance step count and PC/NPC/E once at block
exit; per-pair flag time still uses the exact step ordinal. No block
crosses a micro-memory wrap. No ABI, runtime, EE or code guard change.
Fast bodies duplicate bounded code; size increase is a tradeoff that
must be measured before promotion. V26 is experimental, retain V25.
O0/O3 differential fixtures test normal and forced single-pair paths;
a shortened test-only budget checks exact partial staged state on failure
against the single-pair path. CI and matched V25/V26 replay pending.

## G2C.2 accepted for correctness — matched V25/V26 replay

All five CI workflows passed at 614ec20. ee-graphics-g2c2.zip contains
four interleaved unprofiled synchronous pairs. All eight completed 40M
dispatches; receipt reports 184700 matching transitions per replay,
3127/3127 PAD polls (independently checked stderr footers), 21910026110
guest attempts, 22115080673 EE cycles, 4494 fields, 394881 native VU1
runs, 445669423 pairs, no fallback or IOP guest fault. Recomputed raw
terminal-report and screenshot hashes match all eight runs. All 84
library hashes changed with identical inventory and native build target;
input integrity receipt passed.

| Pair | V25 s | V26 s |
|---|---:|---:|
| 1 | 42.391 | 42.391 |
| 2 | 43.907 | 42.547 |
| 3 | 43.797 | 44.047 |
| 4 | 44.016 | 43.844 |

Median 43.852 -> 43.1955s, nominal throughput +1.52%. Two pairs faster,
one tied and one slower. Control spread 1.625s exceeds median difference
0.6565s; both arms are slower than previous session, no established
speedup. Retain V26 as correctness-validated experimental baseline,
not performance promotion. Receipt has hashes rather than library
sizes; size/code-cache cost is still unmeasured. No async claim.

## G2D.1 — guarded backedge/delay-pair fusion

For reached backward relative B and conditional branches, fuse the
branch and its ordinary nonbranch delay pair into a two-pair guarded
block. Exclude E-bit, callback and nested-branch delay pairs; require
reached backedge and (for conditional branches) fallthrough targets.
The same entry guard proves normal incoming NPC, no pending E, and
budget for both pairs. Execute both original pair bodies in order.
Capture the destination before executing the delay pair; do not
re-evaluate the condition after VI writes. Preserve per-pair cycle,
dependency, flag, Q/P and delayed-VI updates. Update PC/NPC/step once
at the exit and branch directly to the selected emitted target.
Original pair entries retain exact fallback for unusual incoming
state, insufficient budget, indirect targets and unsupported cases.
No loop trip-count assumptions or unrolling, no EE or ABI changes.
O0/O3 fixtures exercise taken/not-taken loop edges, self-loop delay
writes to the predicate, forced single-pair path, and test-only budget
exhaustion with exactly one pair remaining at a loop terminator.
V27 is the next candidate; retain V26. CI and matched replay pending.

## G2D.1 — correctness passed, footprint tradeoff rejected

All five CI workflows passed at 4c8f2e5. ee-graphics-g2d.zip contains
four unprofiled synchronous pairs, 40M dispatches each. Receipt correctness
and input integrity passed; 184700 matched transitions per replay,
3…20974 tokens truncated…% | 3.47% |
| boundary | 39.797/40.047/37.500 | 11.6718/11.7120/10.6000 | 6.79% | 10.49% |
| overlap | 39.938/39.797/39.094 | 11.6930/11.7278/11.1668 | 1.80% | 5.02% |

Each whole-run A/A spread <0.63%. Boundary timing is the largest
individual signal, followed in late combat by overlap and hotspots.
Do not sum speedups or claim matched async correctness. Control
speeds are higher than P6A; compare only within each paired run.

### P6C — strict synchronous validation of lean EE report mode

Add --sync-lean to validate hotspots and boundary timing disabled
together with the same recorded input. Force synchronous GS and
VU1, same 16/16 renderer and O5 DLL, lazy setup off. Keep native
overlap flag unchanged (inactive without worker). Confirm startup
options, unchanged VU1 inventory and input hashes, exact PAD and
sampled dispatch match. Compare every terminal report field except
m2_runtime (host-only hotspot collector deliberately disabled),
and exact final PPM. No other normalization in this sync mode.
No production defaults changed; prior async performance evidence
remains diagnostic. This separates correctness validation from
async scheduling variation without altering guest clocks.

### P6C result — ee-sync-lean-comparison.zip

All three 40M dispatch runs completed with identical 2541 fields,
12281599700 instruction attempts, 378121 native VU1 runs,
415895434 native pairs, no fallback, 2478/2478 PAD polls. Both
A/A and A/B matched 1935262 dispatch samples without mismatch
or unpaired samples. Independently verified raw JSON differences
limited to m2_runtime, exactly the intended hotspot collector;
final PPM SHA-256 identical (81cbb5383d37d7a7a9ae1658447512d4c78a9812dd456fb09af097d998db169a).
Runner, O5 library, VU1 inventory and recording unchanged.
Wall 51.125/50.937/46.235 s (+10.17% throughput vs A/A).
Late dispatch window 24002560..39993344: 332 fields each in
17.9498/17.8287/16.1396 s (+10.47% late throughput vs A/A).
Correctness accepted for EE report switches in matched sync mode.
Prior asynchronous speedup remains diagnostic, not a claim of
architectural equivalence under asynchronous scheduling.

### P6D — lean P40 gameplay defaults

P40 and P40-listen now default SDBZ_RUNTIME_PROFILE,
SDBZ_REPORT_HOTSPOTS, SDBZ_REPORT_BOUNDARY_TIMING and
SDBZ_VU1_NATIVE_OVERLAP_PROFILE to 0 if not already defined.
Explicit environment overrides remain authoritative, and the
launcher prints the actual choices. Existing provenance allowlist
already records all four. This changes only the P40 launcher;
non-P40 runner defaults and guest IR/cache inputs are unchanged.
Keep F5 overlay, F6 main-thread scope toggle, F7 GPU detail and
F8 PC profile. F6 does not dynamically enable startup-only worker
profiling/overlap or hotspot collection; opt in before launch
when those diagnostics are needed. Do not infer zero timings
are zero cost. Restore legacy whole-session profiling by setting
all four variables to 1. Raster default remains 16/16.
Validate ordinary uncapped combat with lean defaults, 88-program
inventory override, no profiling hotkeys active. SC4 still unmet;
repeat async and locked sustained combat acceptance remain open.

### P6D retail result — sdbz-graphics-pr148-neutral-20261128-085406603.zip

Clean da717f6, O5, native 88-program recovered inventory
b50458b3f63ba9b571ccaab94d8c275d81c5bc99506588eefeb13445de852f4c.
Runtime/PC profiling off, hotspots/boundary/overlap off;
16/16 raster, lazy setup off, async GS/VU1, unlimited speed.
40M dispatches, 2495 fields and successful presentations,
4409041 committed draws, 420393655 native VU1 pairs, no fallback
or worker host failure. Last progress at dispatch 39993344:
32.572s, 2495 fields (76.60 fields/s whole run, not combat).
Dispatch 24002560..39993344: 339 fields in 9.9594s
(34.04 fields/s, 56.79% of 59.94 Hz). Worker cumulative
execution 11.029s; 384249 collected jobs, one pending at
snapshot. Interactive input not matched to prior recordings;
compare only indicative performance. SC4 still fails.

### P4F — early triangle rejection before color/texture interpolation

Raster remains the largest measured GPU scope. Split plot into
prepare_plot and plot_prepared; preserve scissor, scanmask,
destination-alpha and depth tests, addresses, read order,
blending, alpha test, writes and primitive ordering. Pass the
already-read destination/depth values forward rather than
re-read VRAM. With PS2_RASTER_EARLY_DEPTH=1, triangles run the
existing tests after coverage and depth interpolation but
before computing vertex color and texture attributes.
Other primitive paths keep their existing interpolation order.
Default 0; shared plot refactor can also affect default codegen,
so check default output and timing against earlier lean sync
replay. Native smoke test and provenance added. No guest IR
changes intended; rebuild runner only. Potential GPU register
pressure or shader compiler scheduling effects remain unmeasured.
Harness --raster-early-depth requires --control --sync
--recording, forces 16/16, lazy setup off, lean diagnostics
in every arm, validates exact pipeline marker, inventory,
PAD, trace, raw terminal JSON and final PPM. No performance
benefit claimed until measured.


### P4G — shared bilinear texture coordinates

P4F replay: 46.812/46.485/46.687 seconds; identical raw terminal
reports, screenshots and sampled dispatch traces. No demonstrated gain;
leave early depth off. The synchronous worker is disabled and retains its
default overlap flag; configure the flag consistently and require zero
submitted jobs, no pending worker, and zero overlap intervals. Do not
normalize away architectural differences.

P4G adds PS2_RASTER_BILINEAR_COORDS=1 (default 0). Reuse the two wrapped
X and two wrapped Y coordinates for all four taps instead of wrapping
both axes separately for each tap. Applies to native atlases, decoded
textures and direct VRAM sampling; preserves tap order, per-tap VRAM
reads, format conversion, snapshot selection, and interpolation math.
Point sampling is unchanged. No guest IR change. No performance gain
claimed before a matched --raster-bilinear-coords --control --sync
--recording replay. Shader compilation and retail validation required.


### P4G result — ee-raster-bilinear-comparison.zip

Strict sync A/A/B passed; 40M dispatches, all 2478 PAD polls,
1935262 trace samples identical, no unpaired transitions, raw JSON
and final PPM byte-identical, 2541 presentations, 88 native VU1
programs, 415895434 pairs, zero fallbacks. All input identities
unchanged. Wall 46.297/46.109/46.156 s. At 24002560..40000000,
332 fields in 16.0989/16.1439/16.0734 s; candidate +0.44% throughput
versus control, no meaningful gain. Keep opt-in. SC4 still unmet.

### P4H — per-invocation plot address cache

PS2_RASTER_PLOT_ADDRESS_CACHE=1 (default 0) lazily caches framebuffer
and depth Loc values for the current pixel and setup index. Invalidate
both when the loaded setup changes. Cache no VRAM contents, retain
scissor, scanmask, alpha/depth tests, read/store order and primitive
order. A rejected primitive does not precompute an unused depth address.
Potential benefit: avoid repeated swizzle-table reads for overlapping
primitives sharing a setup. Register pressure and shader compiler
codegen may offset savings. Strict --raster-plot-address-cache --control
--sync --recording comparison, other raster variants off. No EE IR
changes. Native shader build, replay, and performance proof pending.


### P4H result — ee-raster-address-cache-comparison.zip

Strict synchronous A/A/B passed. All three 40M dispatches,
2478 PAD polls, 1935262 common trace samples with zero mismatch or
unpaired samples. Raw JSON and PPM independently SHA-256 verified
identical; 2541 fields/presentations, 88 VU1 programs, 415895434
native pairs, zero fallbacks; binaries and recording unchanged.
Wall 47.437/47.266/47.453 s. Late dispatch 24002560..39993344:
332 fields in 16.1537/16.2905/16.1219 s (20.55/20.38/20.59 fields/s).
Candidate +1.05% vs control, +0.20% vs baseline. No reliable gain.
Leave disabled. P4F/G/H all show negligible gameplay gains; stop
incremental raster arithmetic optimization for now.

### P4I — isolate normal GPU pass timestamp submission overhead

Regular compute submission acquires timestamp slots, writes beginning
and ending timestamps, resolves to a buffer, copies and maps readback
for each available slot even with EE runtime profiling disabled. Add
PS2_GPU_REGULAR_TIMESTAMPS=0 (default 1) to bypass regular query-slot
acquisition only. Retain device timestamp feature, F7 detailed
capture, normal compute dispatch order, queue submit, event polling,
and readback for guest-visible operations. The regular GPU-busy
estimate has no samples while disabled. Emit effective mode at init
and cumulative pass/query/detail counters at final collection.
--gpu-regular-timestamps-off --control --sync --recording confirms
supported timestamps and nonzero completed queries in controls,
zero in candidate, no detail capture or query failures in any arm,
all prior raster switches off, and exact terminal report/PPM and
trace checks. No guest IR changes. Diagnostic cost screen, not a
claim that GPU timestamp collection is the main combat bottleneck.


### P4I result — ee-gpu-timestamps-comparison.zip

Strict sync A/A/B passed. 40M dispatches, 2478/2478 PAD polls,
1935262 matching sampled transitions, identical raw reports and PPM,
88 native VU1 programs, 415895434 native pairs, zero fallbacks.
All 3202 regular compute passes identical in count; 3202 submitted
and completed queries in controls versus zero in candidate, no query
failures. Wall 47.984/46.109/46.282 s, candidate -0.37% throughput
versus control. Dispatch 24002560..39993344: 332 fields in
16.3170/16.1131/16.1377 s. No meaningful performance gain. Retain
regular timestamps by default. SC4 still unmet.

### P4J — selective dense raster workgroup subdivision

PS2_RASTER_DENSE_WORKGROUP=1 (default 0) creates a second raster
pipeline with 4x4 GPU workgroups. Use it only for 16x16 CPU tiles
with normal 16x16 groups, 1..32 occupied tiles and average list
length at least 64. Thresholds are a pilot, motivated by previous
profile batches with 6 tiles and 509 entries. Each chosen dispatch
has 16 times as many workgroups, same pixels and ordered tile lists;
other batches retain the original layout. No guest IR or shader
arithmetic changes. Scale changes recreate both pipelines. Detailed
capture retains the raster label; final summary distinguishes dense
and ordinary dispatch counts. More redundant list loads, partial
waves, and aliased VRAM access order are risks. Strict sync replay
requires actual dense dispatches, equal total raster dispatch counts,
matched sampled transitions, exact raw report and final image. No
speedup or general rendering equivalence claimed before validation.


### P4J result — ee-raster-dense-comparison.zip

Strict sync A/A/B passed; 40M dispatches, all 2478 PAD polls,
1935262 trace samples identical, raw JSON and final PPM independently
SHA-256 verified identical. 2541 fields/presentations, 88 native VU1
programs, 415895434 pairs, zero fallback; all input identities
unchanged. 16833 raster dispatches per arm, candidate 907 dense
and 15926 standard. Wall 47.468/47.203/47.938 s; dispatch interval
24002560..39993344: 332 fields in 16.4299/16.5435/16.5128 s.
No meaningful gain. Keep disabled. Stop raster micro-experiments.

### P4K — direct common packed GIF register writes in the GPU mirror

The P4J workload reports 48175336 packed events, including 17504664
RGBAQ, 12265725 ST and 17504664 XYZF2 events, and 4353515 GPU draws.
PS2_GS_PACKED_DIRECT=1 (default 0) bypasses the second register
switch and intermediate vertex register packing for common PACKED
RGBAQ, ST, UV, XYZF2/3, XYZ2/3 and FOG. These register writes already
preserve setup generation; preserve exact Q bits, masks, ADC draw
suppression, fog, vertex queue, ordering and draw path. All other
registers continue through the original path. No change to guest GIF
transaction parsing, GS counters, GPU shader, scheduling or guest IR.
--gs-packed-direct --control --sync --recording requires matching
raw terminal JSON, PPM, trace, input identity and actual option marker.
This is a CPU-side cost screen; no speedup claimed before validation.


### P4K result — ee-gs-packed-direct-comparison.zip

Strict sync A/A/B passed: 40M dispatches, 2478 PAD polls,
1935262 matched trace samples, raw JSON and PPM independently identical,
2541 guest fields, 88 native VU1 programs, 415895434 pairs, zero fallbacks.
Wall 47.266/47.360/47.719 s. Combat dispatch 24002560..39993344:
332 fields in 16.5708/16.2303/16.3738 s (20.04/20.46/20.28 fields/s).
No meaningful gain. Keep packed direct off. SC4 remains unmet.

### P4L — worker isolation, not another arithmetic pilot

Use replay_ee_worker_isolation.py on the unchanged authenticated O5
runner/DLL and 88-program inventory. Five lean runs: both async,
VU1 only, GS only, synchronous, both async again. Keep exact recorded
PAD, dispatch budget, 16/16 raster, original shader and regular GPU
timestamps. Capture worker fences, execution, copy and backpressure,
VU1 worker stats, presentation, per-section graphics hashes and a
fixed sampled combat interval. Verify input hashes, actual worker
mode, PAD consumption, terminal budget, presentation and native VU1
coverage; report mismatched workloads and field endpoints explicitly.
Do not imply architectural equivalence or add overlapping thread times.
This diagnostic isolates concurrency contributions before attempting
any further runtime modifications. No C++ or guest IR changes.


### P4L result — ee-worker-isolation.zip

All five arms completed 40M dispatches, 2478 PAD polls, 2541 fields
and successful presentations, 88 native VU1 libraries, zero fallback.
All final PPMs independently SHA-256 verified identical. Inputs unchanged.
Combat 24002560..39993344, fields 2209..2541 in all five arms:

| Mode | Wall s | Combat s | Combat fields/s |
| --- | ---: | ---: | ---: |
| Both before | 37.532 | 10.0790 | 32.940 |
| VU1 only | 37.407 | 10.3939 | 31.942 |
| GS only | 46.531 | 16.1266 | 20.587 |
| Sync | 48.078 | 16.4857 | 20.139 |
| Both after | 38.203 | 10.2226 | 32.477 |

VU1 async gives +58.6% over sync; dual async average +62.4%.
GS-only and sync workloads identical; async VU1 changes VIF/GS work
slightly, even between repeat controls, so no architectural match claimed.
Both-async worker VU1 execution 11.175/11.250 s, queue .454/.394 s;
47.30/46.88M empty polls, no explicit waits or host failures. GS worker
execution 1.518/1.495 s, copy .461/.502 s, fence .201/.147 s; zero
backpressure waits. Worker times are whole-run, overlap and cannot be
summed into a critical path. One VU1 job still pending at each async
snapshot. Stop ungrounded GPU micro-optimizations; focus on VU1 and EE.

### P4M — isolate VU1 worker native profiling in the combat window

SDBZ_VU1_WORKER_PROFILE overrides only the worker's existing detailed
scope collection; unspecified retains main-thread profiler default.
SDBZ_VU1_WORKER_PROFILE_WINDOW=begin,end logs two AOT snapshots at
existing progress checkpoints, without changing poll cadence or
publishing a job. Serialization locks the backend, so the profiled arm
is diagnostic and may perturb scheduling. --profile-vu1 runs both
async lean, worker-only profile, both async lean again. It verifies
actual mode, disabled EE main-thread profiler, endpoint equality, stable
program identities, nondecreasing counters and complete per-program
native timing coverage, ranking combat-only compiled exclusive ns and
ns/pair. Worker AOT may include an in-flight result not yet published;
no guest architectural equality claim. No lowering/cache changes.


### P4M result — ee-vu1-combat-profile.zip

All three runs stopped with return code 1 after exhausting the 2478
recorded PAD polls before 40M dispatches. No terminal report or PPM.
Do not accept architectural equivalence or a full performance comparison.
Input identities unchanged. Worker profile markers confirmed 0/1/0.
The profiled arm has one snapshot at dispatch 24002560, field 2210,
and a periodic report at dispatch 37984811, field 2500. Subtraction
of matching program identities yields 213416666 native pairs and
5783556100 exclusive compiled ns. Top programs:

| Entry | Code FNV | Pairs | Exclusive ns |
| --- | --- | ---: | ---: |
| 0x730 | 80e18246ed4922d9 | 96754096 | 3221800600 |
| 0x620 | fdffdb7fcbf68252 | 37186584 | 984428800 |
| 0x420 | 31d7741f1bfa69cf | 30360781 | 500554300 |
| 0x130 | 2cf115e2c19ba013 | 28525046 | 464545200 |
| 0x5a0 | cfae43a4ec036735 | 12117696 | 332875100 |

0x730 and 0x620 account for 72.73% of recovered compiled time.
The recovered interval is diagnostic, not a matched terminal replay.

### P4N.0 — inspect hot IR before further lowering changes

export_hot_vu1_ir.py reads the existing profile ZIP, subtracts matched
program counters, resolves the exact loaded library indices and validates
the retained inventory SHA-256, native target and full 16 KiB capture
FNV against the original capture filename. Export the top two captures,
current generated IR, O3 optimized IR and native assembly with compiler
version and provenance. These are regenerated artifacts, not proof of
the original DLL machine code. No retail replay or cache rewrite.
Prioritize 0730-13b05ccb361a4d95-0 and 0620-7c2a8125eeca102a-0.
No further speculative arithmetic change until inspected.

### P4N.1 — hot native code inspection

`ee-vu1-hot-ir.zip` matches the P4M capture identities and contains the
expected 16 KiB microcode images, generated IR, O3 IR, and regenerated
native assembly. The profile is only the recovered interval from dispatch
24002560/field 2210 through dispatch 37984811/field 2500; its replay failed
workload validation. It is useful for ranking code, not for claiming a
matched full-run result. Regenerated assembly is not the deployed DLL's
machine code.

| Entry | Pairs | Exclusive compiled ns | ns/pair | Generated `ee_vu1_program` instructions | Stack frame | Stack references |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 0x730 | 96754096 | 3221800600 | 33.30 | 21701 | 9208 bytes | 4710 |
| 0x620 | 37186584 | 984428800 | 26.47 | 11259 | 5336 bytes | 2650 |

Native assembly text is 858169 bytes for 0x730 and 494054 bytes for
0x620. Those are textual assembly sizes, not machine-code sizes. The
functions contain 80 and 47 call instructions and 629 and 318 branch
instructions, respectively. Their optimized IR contains 905 and 460 branch
instructions, with four remaining allocas in each. Stack-reference counts
include all references through `%rsp`/`%rbp`, so they are not a direct
measure of executed spill traffic.

The hot arithmetic is lowered primarily to AVX2 integer SIMD, including
`vpmuldq`; VU floating-point operations are implemented with integer bit
arithmetic. The exact Booth correction remains a guarded scalar cold path:
the vector helper checks four lane predicates and may call
`ee_cop1_mul_correct` for each marked lane. The export gives static call
sites, but neither the fraction of multiplications taking that correction
path nor its share of elapsed time. V25–V30 already tested nearby exact
multiply/add SIMD rewrites and did not establish a repeatable speedup.

The strongest new lead is live-range and register-pressure reduction in
the large `ee_vu1_program` functions, especially 0x730. However, the
assembly alone does not show which stack references are hot, and outlining
blocks could trade spills for call/dispatch overhead or code growth. Do
not promote an arithmetic or block-splitting rewrite from these static
counts. The next candidate should first measure dynamic hot-block and
multiply-correction frequency on the same combat interval, then compare
candidate code size and execution time against a paired control. This
inspection adds no full-run correctness claim and makes no optimization
change.
