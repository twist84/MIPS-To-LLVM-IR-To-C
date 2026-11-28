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
3127/3127 PAD polls (raw stderr verified), 21910026110 guest attempts,
22115080673 EE cycles, 4494 fields, 394881 VU1 runs, 445669423 pairs,
zero interpreter fallbacks, IOP fault or timing observer failure.
Independently recomputed raw report and screenshot hashes match all
eight runs. All 84 DLL hashes changed, same filename inventory and target.

| Pair | V26 s | V27 s |
|---|---:|---:|
| 1 | 42.594 | 42.750 |
| 2 | 42.625 | 42.656 |
| 3 | 42.610 | 42.937 |
| 4 | 42.906 | 42.813 |

Median 42.6175 -> 42.7815s; nominal throughput -0.38%, one of four
pairs faster. No established speedup. User-reported total DLL bytes:

| Version | Bytes |
|---|---:|
| V21 | 5409792 |
| V22 | 6114304 |
| V23 | 8358400 |
| V24 | 8630272 |
| V25 | 7904768 |
| V26 | 11729408 |
| V27 | 11470848 |

V26 adds 48.38% versus V25, V27 remains 45.11% larger than V25.
These are on-disk DLL totals, not executable section sizes or measured
resident memory. Neither duplicated-body experiment establishes a
speedup. Restore V25 production lowering while retaining all prior
inventories and the self-loop regression. G2C.2 and G2D.1 are correctness
validated experiments, rejected for promotion; not a correctness revert.
Use V25 as the next matched performance control.

## G2E.1 — exact compact readiness scoreboard

Build on V25 direct edges and incoming-state specialization, without
duplicated bodies. For all emitted pair PCs (including indirect targets),
collect the read set and exact write-PC bitset for each of the 128 VF
lanes and 16 VI slots using the existing dependency decoder. Drop slots
never read or never written. Coalesce slots whose write-PC sets match:
all start at zero and each shared retirement writes the same issue
cycle+4 timestamp. Distinct partial-write sets remain independent.
Deduplicate each pair's max checks and retirement stores by compact slot.
Preserve issue and retirement order, scalar waits and cycles; no changes
to the interpreter, ABI, code guard, runtime, EE lowering or callback
ordering. No workload-specific assumptions. Straight-line path unchanged.
Structural fixtures check one shared slot for a full-vector write,
two for a partial write, and no allocation for unused readiness.
Retain O0/O3 architectural differential tests, including self-loop
predicate overwrite. Remove fast-block-specific assertions from the
active regression, since those paths are intentionally not emitted.
V28 candidate vs V25 control, retain V26/V27 for future experiments.
CI and retail validation pending. Measure both throughput and DLL sizes
before promotion; no automatic performance acceptance.

## G2E.1 — compact scoreboard correctness accepted, performance rejected

All five CI workflows passed at 9906181 after scoping the layout assertion
to the generated guest function rather than unrelated scalar helper
parameters. ee-graphics-g2e.zip contains four interleaved unprofiled
synchronous pairs, all 40M dispatches with exact reference report and
screen SHA-256 independently recomputed, 3127/3127 PAD polls in each
stderr footer, 184700 matched transitions per receipt, 21910026110
guest attempts, 22115080673 EE cycles, 4494 fields, 394881 VU1 native
runs, 445669423 pairs, no fallback, IOP guest fault or timing observer
failure. Inventory remains 84 native libraries, all hashes changed,
inputs unchanged.

| Pair | V25 s | V28 s |
|---|---:|---:|
| 1 | 42.328 | 42.484 |
| 2 | 42.250 | 42.672 |
| 3 | 42.250 | 42.641 |
| 4 | 42.282 | 42.672 |

Median 42.266 -> 42.6565s, throughput ratio 0.990845 (-0.92%).
Every candidate is slower; control spread 0.078s, candidate 0.188s.
The runner timing independently corroborates the direction (all
candidate elapsed times exceed all controls). Reject performance
promotion, restore V25 scoreboard, preserve V28 library inventory and
commit history. V28 total DLL bytes not supplied; size benefit unknown.
The smaller IR operation count is not a guarantee of lower native
cost (register allocation and code layout effects remain unmeasured).
Retain self-loop branch predicate regression. No async performance claim.

## G3A — narrow exact SIMD multiplication boundary and normalization

Build from V25 production lowering. In vu_mul4 replace the wide
low-23-bit unsigned comparison with the mathematically equivalent
packed 32-bit zero test on bits 15..22 (mask 0x007f8000). The existing
64-bit full product is still used for normalization and exact asymmetric
Booth correction. Keep the nonzero lane predicate and correction
helper unchanged. Normalize the 24/25-bit significand with a fixed
one-bit shift and per-lane select instead of a vector variable shift
by 0 or 1. No host floating-point approximation or change to
architectural flags, operand order, exceptional results or scalar
reference. Existing 81920 lane tuple comparisons per O0/O3 include
boundary-biased products and exception vectors. Add structural checks
and boundary-mask identity checks. V29 candidate vs V25 control.
CI, retail timing and DLL size comparison pending. LLVM may already
canonicalize these forms on some hosts; no speedup claim before replay.

## G3A — V25/V29 correctness accepted; timing neutral

All five CI workflows passed at 6b7de55. ee-graphics-g3a.zip contains
four synchronous unprofiled interleaved pairs, all 40M dispatches.
Receipt correctness and input integrity passed. All eight raw report
and screenshot SHA-256 match the reference; all PAD stderr footers
show 3127/3127 valid polls. Receipt reports 184700 matched transitions,
21910026110 guest attempts, 22115080673 EE cycles, 4494 fields,
394881 native runs and 445669423 native pairs per replay. Report
contains zero VU1 interpreter fallback, no IOP guest fault and
no timing observer failure. All 84 native DLL hashes changed,
with identical inventory and native target.

| Pair | V25 s | V29 s |
|---|---:|---:|
| 1 | 43.047 | 42.172 |
| 2 | 42.375 | 42.516 |
| 3 | 42.484 | 42.266 |
| 4 | 42.281 | 42.360 |

Median 42.4295 -> 42.313s, nominal throughput +0.2753%.
Two pairs faster, two slower; difference 0.1165s is below the
0.766s control and 0.344s candidate spreads. Retain V29 as
experimental control, not a measured performance improvement.
V28/V29 DLL totals still outstanding. No async performance claim.

## G3B — exact SIMD addition alignment and normalization

Build from V29. Express unsigned exponent maximum and shift saturation
with llvm.umax/umin.v4i32, and the bounded sum magnitude with
llvm.abs.v4i32. The aligned magnitudes each fit 25 bits, so the sum
cannot reach signed INT_MIN. Preserve signed zero by selecting the
intersection of operand sign bits when the integer sum is zero,
otherwise use the integer sum's sign bit. The sign of the second
operand already includes SUB's sign inversion. No modification to
guard-bit truncation, exponent saturation, underflow fraction,
Booth correction, ACC latch, sticky flags or architectural ordering.
No host FP approximation. O0/O3 scalar/vector differential retains
81920 existing randomized/boundary lane tuples per level and adds
524288 ADD/SUB tuples per level covering every exponent pair and
all four sign combinations with deterministic significands. These
are not exhaustive mantissa combinations. LLVM may already produce
equivalent native operations. V30 candidate versus V29; retain V25
as the prior control. CI, retail and DLL size validation pending.

## G3B — V29/V30 correctness accepted; timing neutral

All five CI workflows passed at 8c37c36. ee-graphics-g3b.zip contains
four synchronous unprofiled pairs. All eight receipt entries pass the
40M dispatch, 3127/3127 PAD polls, 184700 reference-matched sampled
transitions, 21910026110 attempts, 22115080673 EE cycles, 4494 fields,
394881 native VU1 runs and 445669423 native pairs gates. Independently
recomputed raw terminal report and screenshot SHA-256 match the prior
reference; PAD footers and all archived sampled transition maps match
each other. No fallback, IOP guest fault or timing observer failure.
All 84 DLL hashes changed; matched native target and inventory, inputs
unchanged.

| Pair | V29 s | V30 s |
|---|---:|---:|
| 1 | 42.312 | 42.453 |
| 2 | 42.609 | 42.859 |
| 3 | 42.438 | 42.437 |
| 4 | 42.391 | 42.265 |

Median 42.4145 -> 42.445s, throughput -0.07186%; no established
speed change. User-reported DLL totals: V28 7706624 bytes (-2.51%
versus V25), V29 and V30 each 7912448 bytes (+0.097% versus V25).
V30 has zero footprint growth over V29. These are file sizes,
not executable-section or resident-memory sizes. Retain V30 as
correctness-validated experimental control; no G3 speedup claim.
Retain V25 and all previous inventories. V28 rejection stands.

## G4A — VU0 synchronization attribution before elision

Audit the installed runner callback. runner_vu0_sync validates inputs,
services CMSAR1/VU1 before checking halted VU0, and only then chooses
halted return or scheduler catch-up/wait. A halted VU0 alone does not
prove callback elision safe. Custom callback semantics and the horizon
observer also forbid unconditional macro-chain synchronization removal.
No architectural or EE lowering change in this checkpoint.

Install a profiling-only wrapper when SDBZ_RUNTIME_PROFILE=1. Its
mutually exclusive entry-state scopes are vu0_sync_halted (no active
CMSAR1, halted VU0), vu0_sync_cmsar1 (CMSAR1 active at entry, possibly
also VU0 micro execution), vu0_sync_micro (nonhalted without CMSAR1)
and vu0_sync_invalid (invalid arguments). The original callback remains
installed unprofiled; the opt-in horizon wrapper preserves its
generation count and selects profiling only when enabled. Classification
itself is outside the timed scope. The wrapper always invokes the
original synchronization, preserves failure and service ordering.
Direct periodic device-service calls are not covered by these scopes.
The nested exclusive accounting avoids adding VU1 or other existing
child scopes twice. The receipt distinguishes missing VU0 callback
attribution from still-unmeasured noncallback synchronization, and
rejects partial scope inventories. Tests cover old/new/partial receipts.
No ABI or EE/VU1 artifact changes. Build a new runner, preserving the
previous validated runner executable, then replay with identical V30,
graphics EE DLL, fixed RTC and recorded PAD using the existing on/off
profiling harness (two repeats each). Review actual callback counts and
elapsed durations before designing G4B. Profiling overhead is diagnostic
and must not be interpreted as an optimization or as CPU time. CI and
retail validation pending.

## G4A accepted — matched VU0 callback attribution

All five CI workflows passed at a21ae8e. Archive ee-graphics-g4a.zip
SHA-256 949d2339b541e934c7f049bff76dde6eacd0488e8a884f8834db5694e91118cc.
Runner SHA-256 00d8b0545155181ff7296e713e3e003d845e9b8da2126e3aa97f6b68bb3f2c17.
All four replays reached 40M dispatches, 3127/3127 PAD polls, 184700
reference-matched transitions per receipt, 21910026110 attempts,
22115080673 EE cycles, 4494 fields, 394881 native runs and 445669423
pairs. Raw report and screenshot digests independently verified;
normalized reports identical, raw archived transition maps identical
across four runs, PAD footers verified. Input integrity passed.
Control 42.516/42.687s, profiled 49.922/50.016s; median overhead
17.29%. Mean runner elapsed 46.0198365s, covered 38.00376165s
(82.58%). VU1 compiled exclusive 11.687s (25.40%), EE native boundary
8.292s (18.02%), IOP 7.319s (15.90%).

Both profiled arms measured exactly 12343615 halted callbacks;
CMSAR1, micro and invalid categories each zero. Halted exclusive
324588000 and 323625400ns, mean 0.3241067s (0.7043% of profiled
runner elapsed), approximately 26ns per callback including timed
instrumentation. Classification and scope setup outside the measured
interval, periodic direct device-service calls unmeasured. Do not
interpret as an exact unprofiled saving or CPU time. Defer G4B:
callback elision would require proving service and observer safety
for a small measured exclusive cost. Retain the opt-in attribution
and the V30 inventory, no ABI changes or async claim.

## G5A — exact COP1 ADD/SUB discarded-significand fast path

In the integer ee_cop1_add_bits helper, identify operands whose raw
exponent is zero (flush) or whose alignment distance is >=25 (all
guarded significand bits discarded). If one contributes nothing,
return the surviving original raw operand with SUB sign already
applied. If both contribute nothing, return signed zero with the
intersection of operand signs. Neither case sets O/U: a surviving
nonzero original exponent already lies in 1..255, and no rounding
or normalization changes its significand. Otherwise execute the
unchanged integer adder, including the distance-24 guard bit.
No IEEE arithmetic, no operand exchange or fused MAC. ACC and
FCSR writeback remains unchanged. Add O0/O3 comparison with the
same helper forced through the full arithmetic branch, all exponent
pairs, sign combinations, both operations, selected raw edge
encodings and seeded random mantissas. Hardware vectors remain
in the original test suite. No assumption about runtime hit rate.

Extend replay_ee_graphics_profile.py with mutually exclusive
--candidate-source and --candidate-source-receipt pair, preserving
the VU1 comparison and profile-only modes. Independently validate
both source images against the original O5 translation, unchanged
ownership, coverage and native target. Hash both images and receipts
before/after; interleave unprofiled EE arms with one fixed runner
and V30 inventory, preserve raw outputs and exact architectural
gates. Regression exercises arm selection, receipt hashes and fixed
VU1. Rebuild only ee_recomp and the COP1 fixture, then use isolated
performance-first regeneration from original O5 to a fresh
build/ee-graphics-g5a directory. Compare against build/ee-graphics-sweep,
not the original O5 image. The new image also includes intervening
G3A/G3B shared vector-helper changes in EE-side VU0, because
module_builder includes the same ee_vu_fmac4_ir.h. This is therefore
a cumulative EE-image experiment, not isolated attribution of the
COP1 shortcut. No runner or VU1 rebuild needed; no cache promotion.
CI and retail replay pending.

## G5A — matched EE image correctness accepted; timing neutral

All five CI workflows passed at 2dbede43f730b39d618157099e8db5d8db834bfb.
The previously reviewed ee-graphics-g5a-comparison.zip and
ee-graphics-g5a.zip contain the isolated compilation receipt and four
matched synchronous unprofiled retail pairs. All eight replay arms
passed the fixed 40M dispatch, transition, PAD, architectural report,
screenshot and input integrity gates. This is cumulative EE-image
validation (COP1 shortcut plus EE-side G3A/G3B VU0 helpers), not an
isolated measurement of the COP1 shortcut.

| Pair | Graphics sweep s | G5A s |
|---|---:|---:|
| 1 | 42.703 | 42.219 |
| 2 | 42.078 | 42.297 |
| 3 | 42.110 | 42.125 |
| 4 | 42.078 | 42.156 |

Median 42.094 -> 42.1875s; nominal throughput -0.22%, within observed
variation. Retain G5A as a correctness-validated experimental EE control,
not a demonstrated speed improvement. No claim about async gameplay.

Relative to original O5 (not the graphics sweep image), 16252 functions
and 845746 function-scoped guest PCs retained, 12701 changed IR modules,
zero ownership/coverage changes. IR bytes 992143733 -> 983470709,
native object bytes 152560310 -> 152435481, linked DLL bytes
111471104 -> 111410176, executable section bytes 101812736 -> 101747712.
IR generation 176.625s, coverage scan 55.204s, native compilation
231.469s, linking 50.500s. Candidate DLL SHA-256
9a7a67b83aefe87d5a8233ee77ac838f3c497b230fee48571b2bf2b594e5f385.
Preserve original O5, graphics sweep, G5A and V30 separately.

## G6A — reuse canonical packed MMI SSA views

Implemented in bd5b929, ea25079, e98ca7e and be57b33.
Track exact pairs of low/high SSA value names in a block-local map
to their canonical <2 x i64> representation. Reuse an existing packed
value instead of reassembling both halves, record packed producers
when extracting their halves, and use zeroinitializer for an exact
zero pair. Different lane shapes remain explicit bitcasts. Scalar
low-half writes preserve the upper half and change the lookup key;
zero-register writes remain suppressed by the existing setter.
No new guest arithmetic, guest accounting, runtime ABI, or publication
policy. Both halves are still extracted eagerly; this is not lazy
128-bit register residency.

Source audit at be57b33: the MMI emitter creates no LLVM control-flow
edges within its packed lowering; tracked producers dominate reuse
within the contiguous MMI chain. Reset at guest-block entry and
tracking invalidation, non-MMI/invalid instructions, before and after
delay-slot MMI and on counted-ALU incoming-state restoration. Existing
dirty-half publication and V0 trace still use scalar SSA values.
Regression fixture checks direct packed producer/consumer reuse,
aliased destinations, scalar overwrite with upper-half preservation
and GPR0 suppression, with O0–O3 execution checks. Source review is
not proof of all generated IR validity or host register allocation.

Build ee_recomp and MMI fixtures, run the MMI CTest group, then generate
an isolated performance-first candidate in build/ee-graphics-g6a
using the original O5 IR/profile and expected 16252 functions.
Compare candidate receipt and image against build/ee-graphics-g5a
using replay_ee_graphics_profile.py with fixed V30, G4A runner,
RTC 1795671645, recorded PAD, fastmem word/scalar/quad enabled,
certificates disabled, four interleaved unprofiled pairs at 40M
dispatches. Preserve all existing artifacts. No runner or VU1 rebuild
required. CI and retail acceptance remain distinct; inspect both
receipts, every replay and median/spread before making a performance
claim. G6B (lazy extraction/direct vector publication) requires
evidence of residual host-side packing/state traffic and an independent
correctness checkpoint; do not infer native savings from raw IR counts.

## G6A — correctness accepted; native throughput neutral

All five workflows passed at c8d1c956d6430728a9df8bd72b8527d7a39e858b
(same production code as be57b33; its build/test run was cancelled).
The supplied ee-graphics-g6a.zip and ee-graphics-g6a-comparison.zip
were independently inspected. Archive SHA-256 respectively:
1e94e006523af50d62e7d13d65b8f75bd8be53ee458f3e2cd223b81a5d36995d
and 4ee3086e45f902df3326a8c2bc723b6ca3ddee2690b9a7f7d64dde2a775616ce.
Candidate comparison JSON SHA-256 matches the replay receipt:
f7b25e4d99d54a5dd7dc238ee9482d29b334584afe7a1774cac02f56dc21ad05.
Candidate DLL SHA-256:
669e6ddca5c64bc9489b4a0646f6de8127d2f6b217299ef3962904cb770c8330.

All eight terminal reports and PPMs independently match the fixed
reference SHA-256. All archived dispatch maps agree (184719 trace
lines, 184700 unique dispatch keys each); PAD footers 3127/3127
valid. Each completed 40M dispatches, 21910026110 guest attempts,
22115080673 EE cycles, 4494 fields, 3221 presentations, 394881 VU1
native executions, 445669423 native pairs; 84 native libraries,
zero interpreter fallbacks, IOP guest faults or timing observer
failures. Receipt reports unchanged input hashes. Original inputs
and binaries themselves are not included in these archives.

| Pair | G5A s | G6A s |
|---|---:|---:|
| 1 | 40.328 | 40.062 |
| 2 | 39.812 | 39.797 |
| 3 | 39.672 | 39.750 |
| 4 | 39.593 | 39.640 |

Median 39.742 -> 39.7735s, nominal throughput -0.0792%.
Two faster and two slower; median difference 31.5ms is far below
the within-arm spreads 735ms and 422ms. Retain G6A as a
correctness-validated experimental baseline, not a speedup.
No production-cache promotion or asynchronous performance claim.

16252 functions and 845746 function-scoped guest PCs retained;
zero ownership or coverage changes. Relative to original O5,
12702 IR modules changed. Compared to G5A, IR bytes 983470709 ->
983105758 (-364951); total native object bytes 152435481, DLL
111410176 and executable sections 101747712 all unchanged.
Generation/scan/compile/link 166.625/52.375/208.156/46.657s;
these single-build times do not establish compiler speedup.
Identical aggregate native footprint does not establish identical
machine code. G5A/G6A DLL digests differ, but the previous per-module
G5A object hashes and actual object bytes are not in the G6A archive.

Before G6B, compare the two existing receipts and representative
native objects to distinguish optimized-away packing from residual
SIMD/scalar transfers and architectural state traffic. Do not
implement speculative lazy residency solely because emitted IR
is smaller. If no substantive residual cost is established,
prioritize G8 IOP attribution (15.90% of profiled runner wall).
G7 remains lower priority absent new VIF1 evidence.

### G6B.0 — paired native object audit (read-only)

tools/audit_ee_packed_native.py compares existing G5A/G6A linked
O5 receipts and verifies both actual IR inventories, object hashes,
manifests, metadata and linked image hashes. Count changed IR,
changed objects, byte-identical objects after IR changes, and
canonical 2xi64 insert/extract operations in changed modules.
Report executable bytes independently of total object bytes.
Select representative modules with packing reduction in both
native-object identity classes, preferring small functions; optional
MSVC dumpbin output supplies actual static assembly for review.
No claims about executed instruction counts from lexical counts.
No compilation, cache mutation, runner execution or promotion.

From a Visual Studio developer command prompt:

```bat
git pull --ff-only
python tests\tools\test_audit_ee_packed_native.py
python tools\audit_ee_packed_native.py ^
    --control-receipt ee-graphics-g5a-comparison.json ^
    --candidate-receipt ee-graphics-g6a-comparison.json ^
    --control-dir build\ee-graphics-g5a ^
    --candidate-dir build\ee-graphics-g6a ^
    --output ee-packed-native-g6b-audit.json ^
    --samples 2 ^
    --dumpbin dumpbin.exe
```

The optional --dumpbin argument may be omitted if unavailable; retain
the digest and executable-size audit. The output must be new and
outside either experiment directory. The new CTest checks paired
receipt rejection and lexical counts, not an end-to-end retail object
audit. Final CI and local audit pending. Use the output to decide
whether lazy packed publication has a credible residual cost target.

## G6B.0 accepted — identical O5 native objects

All five CI workflows passed at 483d9e9c29541859288367f1e128d3797aa8d1b9.
The user-supplied ee-packed-native-g6b-audit.zip SHA-256 is
3ba99d277a5512b3744bef1724df433c7409ad7ea936f152d2a52e6fd3ba5142;
JSON SHA-256 6c8c06a7e026fb0528709aba00c9ca70d4ca49b41cc9b4fd2b5a4e67cf20957f.
Control receipt 775b471ebef0bdbe7276bd2c0f5eeacb9b854edf6295df148f7df3039d435438;
candidate receipt f7b25e4d99d54a5dd7dc238ee9482d29b334584afe7a1774cac02f56dc21ad05
matches the G6A replay receipt. All 16252 objects are reported
byte-identical by the local digest-verified audit despite 42 changed
IR modules and 6408 removed canonical vector insertions. Zero
extractions removed; aggregate executable object sections unchanged
at 95167472 bytes. Two sampled dumpbin outputs (0x425e10, 0x425758)
differ only in input path; /NOBYTES was ignored by this dumpbin.
Original/candidate object bytes are not included in the archive.
Linked DLL digests differ, so do not claim byte-identical DLLs.
G6A is an IR simplification, not a native optimization. Defer
G6B lazy residency absent new evidence of residual publication
cost. Keep G6A, preserve previous artifacts; move to G8 IOP
attribution ahead of G7 (VIF1 UNPACK cost previously ~40ms).

## G8A — sampled scheduled IOP service attribution

G4A measured IOP service at 7.319s (15.90% of profiled runner wall).
The existing outer scope includes EE-to-IOP clock observation, batch
planning, idle checks, kernel.run, RPC pump, clock consumption and
checkpoint diagnostics. It excludes separate HLE wait-path IOP runs.
Do not interpret it as R3000 execution time.

Add opt-in SDBZ_PROFILE_IOP_SERVICE=1, active only in the existing
runtime-profiled arm. Resolve the toggle once per session. Count exact
scheduled checkpoints, no-debt checkpoints, physical batches, requested
cycles, retired guest instructions and RPC pump calls, grouped by
zero guest instructions, guest instructions, or quiet fast advance.
Sample one varying offset per complete group of 256 physical batches;
a final partial group may contribute zero or one sample. Measure
planner (including existing pre-run trace check), kernel/quiet attempt
and RPC pump durations separately. Samples are elapsed steady-clock
time, not CPU time; counts classify completed runs, not a proof of
idle device state. Kernel time includes scheduler, guest CPU and
device work; RPC calls may be no-ops. Sampled durations do not cover
all service work and must not be added to the outer profile scope.
No extra guest reads, altered service cadence, changed event ordering,
new EE lowering, or runtime ABI change. Counters go to stderr only,
leaving the existing terminal-report normalization contract unchanged.
Unit test covers exact counts and sampling; matched replay verifies
behavior. Final CI and retail measurements pending.

Preserve the previously accepted G4A runner before rebuilding
ee_game_runner and the new ee_iop_service_attribution_test target.
Run the existing graphics profile on/off harness with G6A EE image,
V30 VU1 inventory, fixed RTC, recorded PAD and unchanged fastmem
settings, two repeats. Retain the existing IOP scheduling mode from
the accepted G6A replay; do not enable experimental coalescing or
quiet advances for this attribution. Collect the entire archive,
including IOP service profile lines in profiled stderr; control
must not emit them. Verify fixed architectural endpoints and raw
unprofiled report/screenshot hashes. No EE IR, EE DLL, or VU1
regeneration required. Select the next deeper IOP attribution or
optimization from measured counts and sampled costs rather than
assuming all zero-guest batches are dispensable.

## G8A — matched retail attribution results

ee-iop-g8a.zip SHA-256
672614cbf0bd8e51cc1312a7574d4fbc5da0aa9b97a1ac0d003496c8014a8f12.
Runner SHA-256 c30ef749a044b3cdaf3a39de10c43c65280960cc6f9779c17a005e1b15d86e6d.
G6A EE DLL and receipt match accepted identities; V30, fixed RTC,
synchronous workers, quiet/coalescing disabled. Receipt reports input
integrity; original binaries are not included in the archive.

All four arms reached 40M dispatches, 3127/3127 PAD polls, 184700
matching sampled transitions, 21910026110 EE attempts, 22115080673
EE cycles, 4494 fields, 3221 presentations, 394881 native VU1 runs,
445669423 native pairs, 84 programs, no fallbacks, IOP guest faults,
timing observer failures or IOP cycle debt. Independently verified
both raw control terminal report hashes, all four PPM hashes,
normalized architectural reports, archived dispatch maps (184719
trace lines, 184700 unique dispatches) and PAD footers.

| Pair | Control s | Profiled s |
|---|---:|---:|
| 1 | 41.204 | 48.343 |
| 2 | 41.188 | 48.500 |

Median 41.196 -> 48.4215s, combined profiling overhead 17.54%.
Mean profiled runner wall 44.6594785s; IOP exclusive 7.38566495s
(16.54%), VU1 compiled 11.38917265s, EE native 7.98029805s.
IOP service scope counts match 40031635 scheduled checkpoints.

Both profiled arms: 7689405 checkpoints without debt (19.21%),
42578752 physical batches, 37351298 no-guest batches (87.72%),
5227454 guest batches (12.28%), 80966947 executed instructions,
2764385078 requested cycles, zero quiet advances, 42578752 RPC
pumps. 145939 no-guest and 20384 guest samples per arm (166323
total; final partial sample group contributed none). No profile
lines in control. Mean pooled sampled nanoseconds per batch:

| Scope | No guest | Guest |
|---|---:|---:|
| Planner | 24.95 | 25.68 |
| Kernel | 79.83 | 653.70 |
| RPC | 25.64 | 24.90 |

Small timings include measurement overhead; naive scaling is not
a validated exclusive decomposition. In particular no-guest does
not imply no device work. Prior quiet/coalescing short matched
experiments at f8c379a/fb6da91 did not establish a speedup despite
avoiding many scheduler calls. Do not simply enable those options
or repeat their implementation. Next attribute inside the kernel
and select improvements to necessary work. G8A retail correctness
accepted; CI at 225f663 had four passed, Build and test running
at first review; no new performance gain claimed.

## G8B — sampled internal IOP kernel attribution

Add SDBZ_PROFILE_IOP_KERNEL=1, requiring SDBZ_PROFILE_IOP_SERVICE=1
and runtime profiling. Reuse the G8A sample decision; do not sample
extra kernel calls. Optional host sample record passed to the
scheduled HLE kernel run; no change to cycle budgets, wake ordering,
IRQ acknowledgment, thread selection, device advancement or guest
execution. ROM0 and separate HLE wait-path runs not instrumented.
Record separate disjoint sampled phases: wake/IRQ checks, ready
selection, idle deadline discovery, idle hardware advancement,
guest context setup, and guest CPU run. The last includes attached
device retirement. Guest post-run bookkeeping and syscall handling
are residual, not measured in these phases. Classify by final
service outcome, so a guest batch can contain idle phases.
No quiet-fast-path internal sample. Emit phase calls, sampled batch
counts and elapsed nanoseconds in stderr only. Timing overhead can
dominate short phases: do not scale into exact CPU time or add to
outer kernel time. Compare counts and coarse distribution, then
target redundant work with independent evidence. Test sampled and
ordinary idle/guest execution for identical clock and stop results;
full retail validation still required. No guest IR or AOT regeneration.
Preserve G8A runner, build ee_game_runner and iop_scheduler_cache_test
(and existing attribution/kernel/profile tests), then run the same
G8A on/off matched replay with SDBZ_PROFILE_IOP_KERNEL=1 added,
new output ee-iop-g8b, both quiet/coalescing disabled. Two repeats;
retain G6A/V30, fixed RTC and PAD stream. Final CI pending.

## G8B — matched retail kernel attribution accepted

All five CI workflows passed at fc59bd100c726ee3f936559e9be3d4143e69d440.
Archive ee-iop-g8b.zip SHA-256:
6beedbc8cbeac3a2294bde0975782358ec3db24e8d1d96d061f0387b6420fde9.
Runner SHA-256:
a02d72ff2844dd6c86059e3deb9629b57b88dc6f63b1306211c29c06964016c8.
G6A EE image, V30 inventory, fixed RTC, PAD stream and reference
identities match G8A. All four arms reached 40M dispatches with
3127/3127 valid PAD polls, 184700 matched unique dispatch samples,
21910026110 EE attempts, 22115080673 EE cycles, 4494 fields,
3221 presentations, 394881 VU1 runs, 445669423 native pairs,
zero fallback, IOP faults, timing observer failures and IOP debt.
Both raw unprofiled reports, all four PPMs, normalized reports,
184719 trace lines (184700 unique keys) and PAD footers independently
verified. Receipt reports input integrity; original binaries absent.

| Pair | Control s | Profiled s |
|---|---:|---:|
| 1 | 41.078 | 48.625 |
| 2 | 41.172 | 48.359 |

Median 41.125 -> 48.492s, combined profile overhead 17.91%.
Mean profiled runner wall 44.81346915s; exclusive IOP 7.7539528s
(17.30%). No performance gain claimed. Both profiled arms retain
G8A exact service counts: 42578752 batches, 37351298 no-guest,
5227454 guest, 80966947 guest instructions and 166323 samples.
Neither control emitted IOP profile lines.

Pooled mean elapsed ns per phase invocation (not per batch):

| Phase | No guest | Guest |
|---|---:|---:|
| Wake/IRQ | 27.27 | 49.19 |
| Ready | 28.35 | 66.32 |
| Idle deadline | 36.59 | 32.38 |
| Idle advance | 59.49 | 43.26 |
| Guest context | n/a | 35.80 |
| Guest CPU | n/a | 309.29 |

Per profiled arm, each idle phase was invoked 146223 times in
145939 sampled no-guest batches (284 additional iterations).
Guest batches: 38130 wake/ready, 9326 idle deadline/advance,
28804 guest context/CPU phase calls. Idle advance and deadline
calculation are the largest measured no-guest phases. Timer reads
are substantial relative to the durations, and these samples do
not identify SPU2 arithmetic alone as the bottleneck. Do not sum
with outer scopes or extrapolate guaranteed wall savings.

## G8C.1 — incremental SPU2 sample phase

In c_spu2, retain the absolute guest clock and derive the sample
remainder incrementally. In the common non-crossing case a widened
phase addition and range check replace absolute clock divisions;
sample_phase() reads the cached remainder, including for the
existing IRQ-deadline cache. When crossing, derive the exact edge
count and remainder from old phase plus cycles. Reset the cached
remainder with the absolute clock. Preserve the existing unsigned
clock-wrap behavior via the original quotient-difference fallback.
Invalidate the IRQ-deadline cache on exactly the old edge/wrap
conditions. No new scheduler shortcuts, no device work omitted,
no SPU2 rate change, no guest instruction lowering or EE runtime
ABI change. The existing hardware fixture covers partitioned,
zero, exact, multi-period, large uint32 advances and attached
CPU/kernel clocks; add all 768 fractional phase positions and
reset checks. Full audio/IRQ regressions and retail replay pending.

Add mutually exclusive --candidate-runner and required
--control-runner-sha256 to replay_ee_graphics_profile.py, retaining
the same EE/VU1 images, fixed PAD/RTC, receipt hash checks and
interleaved unprofiled comparisons. Mocked replay regression covers
pinned runner mismatch, combined comparison refusal, arm order,
runner hashes and unchanged image. Preserve G8B runner before
rebuilding. Run four matched pairs, both runtime/IOP profiles off
and quiet/coalescing disabled. Use G6A EE source, V30 inventory,
same reference and output ee-iop-g8c1. Compare median and spread
before retaining any performance claim. No EE IR, native EE or
VU1 rebuild required. CI pending.

## G8C.1 — correctness accepted, throughput neutral; production change dropped

All five workflows passed at 97b2eeb5ead5d0257e1952636c027850fb050fa5.
Archive ee-iop-g8c1.zip SHA-256
f02e7a83096d500e10653f56d7d5f2ed31b246975b889f8e80dbc642cd6a4dfa.
Pinned G8B control runner
a02d72ff2844dd6c86059e3deb9629b57b88dc6f63b1306211c29c06964016c8;
candidate runner
dd828c1a264299879d735806e86e5e8a6bbb0421de2fde204be15b7f4f22633a.
Receipt reports unchanged input identities, fixed G6A/V30, RTC/PAD,
both synchronous workers and profiling/quiet/coalescing disabled.
Binaries themselves are not supplied. All eight raw reports and PPM
hashes independently matched reference; all archived dispatch maps
(184719 lines, 184700 unique keys) identical; PAD footers 3127/3127.
All completed 40M dispatches, 21910026110 attempts, 22115080673 EE
cycles, 4494 fields, 394881 VU1 runs, 445669423 pairs, 84 programs,
no fallback/fault/observer failure. 42578752 logical and physical
IOP turns, all general kernel runs, zero debt; no IOP profile lines.

| Pair | G8B s | G8C.1 s |
|---|---:|---:|
| 1 | 42.828 | 41.187 |
| 2 | 41.812 | 42.391 |
| 3 | 41.906 | 41.985 |
| 4 | 41.875 | 41.828 |

Median 41.8905 -> 41.9065s, nominal throughput -0.03818%.
Two faster/two slower; 16ms median delta versus 1.016/1.204s
within-arm spreads. No demonstrated gain. Removed incremental phase
state and restored G8B SPU2 production implementation (a3fb546,
85274ba) to avoid maintaining an extra invariant without benefit.
Retain fractional-phase regression and runner-only replay tooling.
G8B remains pinned control.

## G8C.2 — inline existing device deadline fast paths

Use the G8B production clock (G8C.1 reverted). The idle-deadline
phase calls both SPU2 DMA and voice IRQ horizon queries on each
idle scheduler iteration. Move the existing cache-enabled
zero/positive voice deadline hit paths into the header; retain the
same uncached compute and cache refresh/invalidation logic out of
line. Inline the ordinary two DMA countdown minimum; defer to the
unchanged AutoDMA refill logic when either active transfer has
remaining bytes. Preserve zero-as-no-deadline, mixed DMA/AutoDMA,
ties, disabled-cache behavior and every existing horizon. No
persistent state, guest ordering, budget or device advancement
change. Compiler inlining is not guaranteed; these changes are
a candidate, not an established speedup. Existing hardware tests
cover overlapping DMA, AutoDMA and cached versus uncached IRQ
queries across sub-sample phases; add repeated queries and
cache-enable transitions. No EE lowering or VU1 changes.

Rebuild runner and hardware/kernel fixtures; run the same four
unprofiled matched pairs as G8C.1, output ee-iop-g8c2, **against
the preserved G8B runner** (not the rejected G8C.1 candidate).
Keep fixed G6A/V30, PAD/RTC, quiet/coalescing off. Final CI and
retail validation pending. If neutral again, move away from
deadline micro-optimizations and measure active guest execution
(AOT/interpreter coverage and dispatch cost) before structural work.

## G8C.2 — retained, small positive matched result

All five CI workflows passed at 8afa3e2bfe7867b7b113a8ddf2e343f52095425a.
Archive ee-iop-g8c2.zip SHA-256
20c056f6a9bf48b3fce68a88335bc8d35fd5fb865052041b4e54cb3357d732cd.
Pinned G8B runner
a02d72ff2844dd6c86059e3deb9629b57b88dc6f63b1306211c29c06964016c8;
G8C.2 runner
b0aa1735875a2ee4104cd3facb1fcc28683c896be07cd44500e151ba1fc7dd25.
Receipt reports unchanged input hashes, fixed G6A/V30, synchronous
workers, RTC/PAD, profiling and quiet/coalescing off. No IOP AOT
configuration or AOT load/terminal statistics in the archive.
Actual binaries not supplied. Independently verified all eight
terminal report and PPM SHA-256 hashes against the pinned reference,
all 184719 trace lines per arm (184700 unique sampled dispatches),
and 3127/3127 valid PAD footers. Every arm reached 40M dispatches,
21910026110 EE attempts, 22115080673 cycles, 4494 fields, 84 VU1
programs, 394881 native runs, 445669423 native pairs, zero
IOP fault/fallback/observer failure, 42578752 logical/physical IOP
turns, zero quiet advances and zero debt.

| Pair | G8B s | G8C.2 s |
|---|---:|---:|
| 1 | 42.625 | 41.968 |
| 2 | 42.094 | 41.750 |
| 3 | 42.047 | 41.797 |
| 4 | 41.781 | 41.656 |

Median 42.0705 -> 41.7735s, 297ms saved, nominal throughput
+0.711%. All four pairs positive (125..657ms saved). Four pairs
and within-arm variation limit confidence, but low-complexity
inlining has no extra state and passes all checks. Retain G8C.2,
do not claim statistically conclusive or general speedup.
Use pinned G8C.2 runner as control for G8D; no more idle deadline
micro-optimizations without new evidence.

## G8D.0 — establish interpreter versus IOP AOT baseline

The matched G8C.2 archive contains no IOP AOT activation; active
guest execution is interpreted. G8A observed 80966947 scheduled
guest instructions, not the complete session retirement denominator.
Existing IOP AOT loader activates relocated, import-linked module
text by exact base and FNV-1a hash, dispatches one entry per IRX,
and batches device advancement after translated retirement.
The latter can change device/MMIO visibility compared with the
interpreter. Do not assume identical behavior or promote AOT
without the replay gate. Do not enable SDBZ_IOP_AOT_VERIFY:
its legacy interpreter replay repeats MMIO side effects.

Add a single unprofiled --capture-iop mode to the graphics replay
harness. Require pinned runner, fresh separate capture directory,
one replay and no other comparison component. Use existing module
load callback; validate every captured binary/metadata pair for
size and FNV-1a identity. Record metadata plus SHA-256 in receipt,
keep guest bytes outside the diagnostic ZIP. No EE/VU1 changes.
Build existing iop_recomp and tools/build_iop_aot.py at O3 into a
fresh directory, preserving all prior captures and libraries.
Then --candidate-iop-aot compares interpreter and one directory
of AOT libraries with same pinned runner, same EE and VU1 images,
both profiles off. Hash every library and recheck inventory at
completion; strip inherited IOP AOT, capture and verify settings
from both modes, enable AOT only in candidate arms. Require
terminal module/active/entry/instruction counts, nonzero native
retirement and one loaded descriptor per supplied library (current
builder contract); not proof of full coverage or each module
executing. Apply existing report, PAD, transition and screenshot
gates; preserve diagnostic ZIP on failure. Four matched pairs
only if earlier arms pass. No auto-promotion. Do not infer exact
AOT coverage percentage by dividing by G8A's scheduled-only
instruction count. If mismatch, isolate first divergent boundary
before optimizing dispatch, timing or function discovery.
Mock tests exercise capture pairing, environment isolation, runner
pinning, inventory and terminal stats. Final CI/local tests pending.

## G8D.0 — existing IOP AOT rejected; G8D.1 exact slice budget

All five CI workflows passed at 1bd726b2827d8d39e51a1cb44d471880452ac056.
Capture archive SHA-256
439509184b974ee11d83aa9a3d6284d5a785d04f742765af60d3acc7d8c075e2.
Comparison archive SHA-256
19675e1bd1c48f66a956a268aef5129f1ade2fe6f3bc7cf6fb743964fb25913d.
Capture and interpreter control terminal report and PPM match
the accepted reference; capture receipt records seven linked
IRX identities. Raw capture binaries and AOT DLLs are not in
archives; receipt identities are not independent binary validation.
All seven candidate libraries load/activate at recorded bases.
Interpreter exit 4, 41.532s, 40M dispatches, 3127/3127 polls.
Candidate exit 1, 5.547s, 13/3127 polls, valid=0; last traced
dispatch 3664700. Candidate report is in_progress at dispatch 0,
not terminal. No candidate PPM, no terminal AOT counters.
No performance comparison is valid. Harness originally masked
early PAD rejection with missing terminal statistics.

Independently parsed stdout transitions: control 184700 unique,
candidate 10359, 3961 common, 178 different common, first unpaired
3500742 (candidate only), first different common 3501056, last
common sampled transition before first unpaired 3500032.
This is not an exact first guest divergence: earlier IOP clock
differences are visible at module entry return.

| Module | Interpreter cycles | AOT cycles |
|---|---:|---:|
| SIO2MAN | 3519 | 3519 |
| MCMAN | 1888233 | 1888233 |
| MCSERV | 1889363 | 1889364 |
| LIBSD | 1890482 | 1890483 |
| CRI_ADXI | 1892098 | 1892099 |
| ARKD_DVD | 1893878 | 1893913 |
| PADMAN | 9938981 | 9938525 |

All module return values agree. First guest RPC entry 1894748
vs 1894805 (+57 IOP cycles), seventh RPC completes 6433135
vs 6432667. At 50000 EE dispatches pending RPC checkpoint
1945925 vs 1948741 cycles (+2816). No faulted threads in those
snapshots. Do not blame PADMAN alone or infer RPC hang from a
pending checkpoint; token 7 eventually returns in both arms.

G8D.1: source inspection confirms that the old lifter checks
budget only after a translated branch AND delay instruction.
When the branch consumes the last budget slot, it can retire one
extra instruction beyond the kernel quantum. Add a budget check
immediately after branch retirement on JR/JALR, J/JAL and
conditional paths. On exhaustion return IOP_AOT_EXIT with delay
PC, selected next PC, in-delay flag and branch PC; interpreter
executes the pending slot on next scheduler slice. No ABI or
runner changes. Existing test only compared final state, allowing
an overrun to be compensated later. Add per-slice comparisons
for budgets 1..17 (retirement count, run result, PC/next PC,
delay identity, GPR/HI/LO, CPU cycles, RAM). This is a real
contract defect but not proven sole retail root cause. Attached
hardware advancement still occurs in native batches, rather
than after each retired instruction; that remains a separate
accuracy issue to isolate if needed. Do not enable legacy
SDBZ_IOP_AOT_VERIFY because it repeats live side effects.

Harness now records early exit code, PAD footer, partial AOT log
and sampled trace comparison before rejecting nonterminal arms;
never accepts partial evidence. Regression covers early rejection.
Build iop_recomp and iop_aot_test only, use existing validated
capture and new output build/iop-native-g8d1. Retain original
libraries for comparison; same pinned G8C.2 runner/EE/V30/RTC/PAD.
One matched pair to establish correctness first, output
ee-iop-g8d1; expand to performance repeats only after acceptance.
No EE IR/runner/VU1 rebuild or recapture needed. CI/local test
execution pending.

## G8D.1 — complete retail execution, strict mismatch remains

All five CI workflows passed at cad45e79eaed769bf2041561f53d8b7c643e6b31.
Archive ee-iop-g8d1.zip SHA-256
7742ddd906f6980b9d5992cff280eb3b1cc0928dfab2c9eb87e5693b22d596f8.
Receipt pins G8C.2 runner, G6A EE, V30 VU1, recorded PAD and
reference hashes. Seven updated IOP library hashes, unchanged
inputs reported. Native binaries absent from archive.
Both exit 4 at 40M dispatches, 3127/3127 valid polls,
21910026110 EE attempts, 22115080673 EE cycles, 4494 fields,
42578752 physical/logical IOP turns and zero debt. Both PPMs
match reference SHA-256 9382a6cdc6a77656a1d5e9652ceffd592bde1e8fcf365a4bddcc6e49d3842f08.
Interpreter report SHA-256
9d8743b4bb82fe4f0feafc8e7e0664f8ba47bba7238560ec5f533c8c6db2982d;
AOT report SHA-256
80151c22f20a493b138f989a9e1319d65dc605f808e86477c5e585f9df3100b9.
All seven module return cycles now exactly match the interpreter.
Control/candidate each 184700 unique sampled transitions, 184687
common, zero differing common, 26 unpaired, first 7580546.
All unpaired transitions are EE exceptions in clusters around
7.58M and 8.23M dispatches. Sole differing report field:
m2_iop_sio2.last_start_guest_pc, interpreter 0x20008, AOT 0x20258.
Candidate reports seven active modules, 7086693 entries,
68423064 native instructions, 529.9ms translated, 1.7ms refresh.
Wall 40.672 -> 39.515s, nominal +2.93% throughput (one pair,
strict mismatch, no performance acceptance). Do not waive the
trace or report gate.

## G8D.2 — exact non-RAM instruction and hardware boundaries

The old runtime's slow-path callbacks invoke c_cpu::read/write
while m_state.m_pc remains the native entry PC, and before
preceding native instructions retire device cycles. SIO2 CTRL
captures m_dma_cpu->state().m_pc at write time, explaining the
incorrect recorded instruction PC. The timing defect also permits
early MMIO reads/writes, including read-clear registers.
Return zero from native non-RAM callbacks, causing generated
code to exit with the precise instruction and delay-slot identity.
The interpreter performs the operation exactly once after prior
native retirements. Existing G8D.1 libraries support this protocol;
no ABI layout change or recompile necessary.

Cap native RAM-only slices to the existing hardware event horizon
(c_hw::idle_event_cycles_remaining), or one instruction if an
immediate/unknown event requires service. Permit retirement at
the event boundary, not execution beyond it. Keep the actual
hardware advance; preserve Timer5 rollover, sample and DMA
boundaries and do not batch across an SPU2 clock wrap. Conservative
AutoDMA/pending IRQ horizons may reduce performance; correctness
first. Disable legacy verify when hardware attached because
re-executing live retirement side effects is unsafe. No change to
kernel scheduling decisions, EE lowering or guest binary.
Regression adds an attached-hardware Timer5 program with compare,
count write/read, branch-delay MMIO read and read-clear MODE;
compare every slice for budgets 1..17. This is not proof of all
device behavior. Rebuild runner, iop_aot_test and run hardware/
kernel tests. Use same new runner for both arms, pin its hash,
retain G8D.1 libraries. One matched pair first, ee-iop-g8d2.
No EE IR/relink, IOP AOT rebuild, recapture or VU1 rebuild.
Compilation, CI and retail validation pending.

## G8D.2 — correctness accepted; IOP execution micro-optimization paused

All five CI workflows passed at b9bcaa0f951bc81bc9624bff375eef96e593fc08.
Archive ee-iop-g8d2.zip SHA-256:
47c0016ea53211a2b3d10f133fd060ac97391c4ed9d82a12abe459b7543c70b7.
Runner SHA-256:
6cb539ba63b4790d1e034eb041d04ce5c42eec7a614e7346057389bec40b977f.
Both arms used the same runner, G6A EE DLL, V30 VU1, fixed RTC
and PAD; seven G8D.1 libraries in candidate, all active.
Receipt reports unchanged inputs; binaries not included in archive.
Both exit 4, 40M dispatches, 21910026110 EE attempts,
22115080673 EE cycles, 4494 GS fields, 394881 VU1 runs,
445669423 native pairs, 3127/3127 valid PAD polls, zero IOP
fault, timing observer failure or IOP debt. 42578752 physical
and logical service turns, zero quiet advances. Independently
verified both raw report SHA-256
9d8743b4bb82fe4f0feafc8e7e0664f8ba47bba7238560ec5f533c8c6db2982d
and both PPM SHA-256
9382a6cdc6a77656a1d5e9652ceffd592bde1e8fcf365a4bddcc6e49d3842f08
against pinned reference. All 184719 trace lines identical between
arms (184700 unique dispatch keys); receipt reports zero
different/unpaired samples against reference. SIO2 last start PC
0x20008 in both. Native IOP 9106977 entries, 67846006 instructions,
584.4ms translated, 1.7ms refresh. Wall 41.937 -> 40.000s,
nominal throughput +4.8425%. Single pair, correctness accepted,
performance not established. Retain runtime and lifter corrections.
Do not default-enable IOP AOT or require extra performance repeats.
Preserve runner, captures and G8D.1 libraries. Further IOP AOT execution micro-optimization remains deferred.
Function splitting and parallel compilation are active in #174; see
IOP_AOT_SPLITTING.md. Next objective practical timing compatibility with
PCSX2 and ps2-web-recomp, not cycle-exact physical hardware.

## PR #180 — 3D gameplay performance acceptance (2026-11-28)

Firmware accuracy work is deferred; investigate measured EE, VU1, GPU
compute, batching and synchronization costs during actual combat.
Do not infer the bottleneck from whole-replay averages.

The presentation-submission timestamp fix (36d6bb4e) truncated gpu.cpp.
0c488efe restored the missing function/namespace ending and corrected
the render-pass store operation. 5c294994 adds eight asynchronous
detailed readback slots, 1024-dispatch query chunks, ordered retirement,
map failure accounting and a capture-stop drain. Detailed submissions
now update dispatch and submission counts. Normal batched submission
code remains byte-identical; detailed timing still separates compute
passes, so enabled-versus-disabled overhead must be measured. The
detailed ring can wait at saturation or capture stop; the normal path
does not acquire these slots. No EE/VU1 lowering changes.

Remote verification: gpu.cpp 2706 lines, blob
db4ae20b579183c2c401d4e88afc0303d659fd11, complete ending;
ee_game_runner_main.cpp unchanged blob
580340e6edc9008eb9c4685e7a94550732684cc7.
F5 overlay, F6 EE-thread scope profiling, F7 detailed GPU capture,
F8 EE per-PC timing; no F9 binding in the inspected window code.
F6 does not currently reconfigure the VU1 worker's startup-selected
detailed scope timing. Use interval deltas of worker totals and a
separate CPU-profile-enabled run when detailed VU1 attribution is
needed. The first GPU capture submission can lack raster metadata
for batches constructed before activation.

### Acceptance scorecard

| Criterion | Target | Latest result | Status |
|---|---|---|---|
| SC1 | Measured critical path, CPU/GPU overlap | Detailed GPU capture not yet validated | PENDING |
| SC2 | >=99% configured NTSC field rate, 3 x 120s active combat | No comparable current measurements | PENDING |
| SC3 | Mean <=101%, P95 <=110%, P99 <=150% of reference period; >=95% of 1s windows at >=95% speed; no recurring >100ms stalls | No current field-time distribution | PENDING |
| SC4 | >=110% unlocked reference speed (preferred 125%, stretch 150%) | No current comparable active-combat result | PENDING |
| SC5 | Normalized, attributable efficiency improvement | Diagnostic infrastructure only | PENDING |
| SC6 | Architectural correctness and native coverage retained | No lowering changes; build and retail validation pending | PENDING |
| SC7 | Visual correctness | Presentation pass retained; deterministic comparison pending | PENDING |
| SC8 | Menu, normal combat, expensive combat, effects/overdraw, transition | Pending comparable scene captures | PENDING |
| SC9 | All applicable CI; no unexplained >3% regression | New-head CI pending | PENDING |
| SC10 | Usable live CPU/EE/GPU capture, no silent losses; overhead quantified | GPU async implementation committed; live validation and worker toggle work outstanding | PARTIAL |
| SC11 | All criteria met; locked full speed and >=10% unlocked headroom | Not accepted | PENDING |

### Optimization ledger

| Commit | Change | Baseline | Result | Improvement | Correctness | Decision |
|---|---|---|---|---|---|---|
| 0c488efe | Source repair and presentation store operation | Truncated source | Complete remote source | Not a performance claim | Static remote check | Retain |
| 5c294994 | Detailed GPU asynchronous readback and query chunking | Per-submit blocking map; cutoff at 2048 dispatches | Eight reusable slots; all dispatches attempted; failure/pressure counts | Not measured | Static diff and remote file verification; CI and retail pending | Validate |

Next gate: live F7 capture in the expensive 3D scene. Confirm raster
(and other compute) rows, contiguous submission-local indexes, no map
failures and report retention after deactivation. Record ring pressure
waits and compare with detailed profiling disabled. Collect CPU and
VU1 interval measurements separately; do not add overlapping worker
and GPU times to EE wall time. Use the same native cache, resolution,
speed mode, character/stage and input progression for comparison.
No retail performance improvement claimed until measured.

### P1 capture — 20261128-070242763, fe649940

Replay provenance: clean tracked tree, O5, unlocked, async VU1 and GS,
CPU profiling on, EE per-PC off, no host audio. GTX 1660 SUPER.
40M dispatches, 2430 guest fields/presentations; overall 58.03 fields/s
is not combat throughput. Detailed capture: serials 2745–3086, 342
submissions, 32470 contiguous dispatch rows; zero failed mappings and
zero pressure waits. GPU compute 3606.808 ms: raster 2688.538 (74.54%),
decode 311.234, upload 303.658, CLUT 224.196, display 79.182.
Mean 10.546 ms/submission, median 10.039, P95 14.024, max 15.000.
Raster mean 7.861 ms/submission. During capture full two-second
intervals show 22–26.5 fields/s, EE native exclusive 625–631 ms,
IOP 178–184 ms, GPU submit CPU 104–129 ms. GPU compute alone cannot
explain field time; do not sum overlapping EE, VU1 and GPU timings.
VU1 entire replay: 84 programs, 392056 native runs, 440249063 pairs,
zero fallbacks. Worker execution 13.112 s, compiled exclusive 12.802 s.
Top PCs 0x730 7.221 s, 0x620 2.161 s, 0x420 1.123 s, 0x130 1.080 s,
0x5a0 0.748 s (96.34% combined). These are whole-replay VU1 timings.
SC1 PARTIAL, SC10 PARTIAL (valid live GPU capture; overhead/worker
toggle still outstanding); SC2/4 not met in captured unlocked combat.
Final acceptance still pending.

### P4A — raster workgroup experiment

Example submission 2746: raster dispatch 34 has 451 primitives,
6 tiles, 509 list entries, 1.325 ms; dispatch 29 has 2030 primitives,
79 tiles, 3901 entries, 1.059 ms. Few active groups and long ordered
primitive loops motivate testing smaller workgroups. Opt-in
PS2_RASTER_WORKGROUP=8 divides each existing 16x16 CPU tile into
four 8x8 GPU groups. Default 16 preserves current group layout.
Same ordered tile list, sample coordinates, render scale, shading,
and dispatch submission order. No change to binning or guest code.
This is a hypothesis, not a measured speedup; smaller groups may
increase redundant list traversal or alter behavior for aliased
targets. Require matched deterministic visual/VRAM correctness and
comparable combat A/B capture with 16 versus 8. Record effective
setting in log and replay provenance. No default switch until proven.

### P4A result — 20261128-070738141

Candidate d768a2ac, clean tree, PS2_RASTER_WORKGROUP=8 verified in
provenance and console. 35467 rows across serials 2705–3081 (377
submissions), no missing/duplicated indices, failed maps or pressure
waits. Baseline 32470 rows/342 submissions. Total mean compute
10.5462 -> 10.2927 ms (-2.40%), median 10.0390 -> 9.7007 ms,
P95 14.0243 -> 13.8244 ms. Raster mean 7.8612 -> 7.6160 ms (-3.12%).
Capture windows are different and manual PAD is not deterministic.
Matching exact 80-operation pipeline sequence: 38 baseline and 26
candidate submissions, total mean 9.5485 -> 9.4702 ms (-0.82%),
raster 7.1377 -> 7.0659 ms (-1.01%); dense raster index 34 median
1.3352 -> 1.3280 ms (-0.54%). Not a meaningful improvement. Whole
replay 2430 fields/41.8714 s versus 2425/41.9079 s (58.03 -> 57.86
fields/s). Fully captured combat intervals remain ~21–26.5 fields/s.
No equivalent deterministic screenshot comparison; do not accept
visual correctness merely from successful rendering. Retain 16x16
default and 8x8 opt-in only. SC4 FAIL for captured unlocked combat;
SC1/SC10 PARTIAL.

### P3A — verify the native VU1 inventory before new lowering work

Both live P40 captures used a launcher that hard-codes
build/vu1-native-v20. Prior G2A proved a synchronous V22/V23 speedup;
V30 was subsequently correctness-validated (see G3B). The selected
live DLL hashes were not recorded, so do not infer that the directory
name proves an exact historical version. New SDBZ_P40_VU1_AOT
override leaves all other P40 settings unchanged and logs the
selected directory. Provenance records directory basename, target
marker, library count and SHA-256 over ordered filename/content
digests (pre-run selection, not a loaded-binary attestation).
Tests exercise the override and fingerprint stability/change.
Compare existing V20 and V30 inventories at 16x16, same O5 EE
cache, CPU scope profiling enabled, F7/F8 disabled. No new lowering
or IR rebuild. Candidate report must retain 84 loaded programs and
zero fallback. Use comparable active combat field intervals and
VU1 ns/pair by PC; deterministic replay needed for acceptance.
Candidate whole-replay 0x730: 200803030 pairs, 7.212 s; baseline
199480795 pairs, 7.221 s. Do not mistake these for a measured effect
of the GPU experiment. Prioritize this inventory control before
further VU1 micro-optimizations, and separately acquire live F8
hot-PC attribution. No final performance acceptance.

| Commit | Experiment | Result | Decision |
|---|---|---|---|
| d768a2ac | 8x8 raster workgroups | Comparable raster mean -1.01%, no gameplay improvement | Retain opt-in only |
| P3A | Select and fingerprint existing VU1 inventory | Pending matched measurement | Validate V20 vs V30 |

### P3A result — 20261128-071148244 / 071322464

Both at 2ad71292 with clean tree, O5, native target, 84 libraries,
distinct inventory fingerprints (V20 72ad01e1..., V30 9a2f81a3...),
16x16 tiles/workgroups and async workers. 40M dispatches each;
manual input, F7 enabled during expensive combat in both runs.
No VU1 interpreter fallback or worker failure. V20/V30 compiled
exclusive 13.1246562/11.457634 s over 451915502/450664905 collected
pairs, 29.0423/25.4238 ns/pair (-12.46%, +14.23% throughput).
0x730 (same code hash) 35.8989 -> 31.1032 ns/pair; 0x620
28.5937 -> 25.0566; 0x420 18.6744 -> 16.5189; 0x130
15.8108 -> 15.6338; 0x5a0 30.2573 -> 25.8354.
EE overlap 4.437 -> 3.782 s. Runner wall 41.8893 -> 41.3662 s,
fields 2397 -> 2332; 57.2223 -> 56.3746 whole-run fields/s.
Seven full capture intervals (indices 13 through 19) average
24.3857 -> 24.6648 fields/s (+1.14%). No demonstrated gameplay
speedup. GPU rows 35644/35141, submissions 396/407, no failed maps
or pressure waits. Mean compute 10.3654 -> 9.8304 ms; workload
differs. EE native boundary remains ~614.5/614.9 ms per 2s interval.
Use V30 as experimental control (not an unconditional default).
SC4 FAIL, SC1/SC10 PARTIAL. Deterministic visual validation and
uninstrumented combat acceptance remain outstanding.

### P4B — finer raster primitive lists

P4A only subdivided workgroups; every subtile still traversed the
16x16 primitive list. Add independent PS2_RASTER_TILE=8 alongside
PS2_RASTER_WORKGROUP=8. Binning and triangle span clipping use the
selected tile size, as do tile grid allocation, shader origin and
dispatch mapping. Existing clipping margin and ordered primitive
insertion are retained. Default 16x16 and prior 16/8 mode preserved.
Finer lists should reduce redundant per-pixel traversal for small
triangles, but increase CPU binning and list memory traffic. No
speedup asserted. Native WebGPU smoke runs a second time with
8x8 tiles/workgroups, using the same correctness checks; CI pending.
Compare V30 at 16/16 vs 8/8, with detailed GPU capture in both arms.
Compare tile entries times tile area (upper-bound pixel iterations),
raster time, CPU batch scope, and combat rate; require deterministic
VRAM/visual equivalence before promotion. No new EE/VU1 lowering.

### P4B result — 20261128-072021087

Candidate a1f3b1b clean tree; same V30 inventory SHA-256 as
071322464 control; both O5 and 40M dispatches. 8x8 CPU tile
and workgroup confirmed. Candidate 32880 detailed rows/412
submissions, contiguous indices, no failed maps or pressure waits.
Control 35141 rows/407 submissions. Mean compute 9.8304 -> 8.7070
ms (-11.43%), mean raster 7.2975 -> 6.2627 ms (-14.18%).
Exact 80-operation sequence (39 control, 409 candidate submissions,
excluding first missing-metadata batch) mean compute 9.4787 ->
8.7291 ms, raster 7.0579 -> 6.2788 ms (-11.04%). Mean summed
tile-list entries 15361.87 -> 29853.61; entries times tile area
3932639 -> 1910631 estimated pixel iterations (-51.4%).
Median dense raster #29 0.9953 -> 0.7990 ms; #34 1.3279 ->
1.1063; #48 0.9559 -> 0.6901. #21 regressed 0.7905 -> 0.8365.
Seven full detailed-capture intervals: 346 fields/14.027s vs
357 fields/14.028s, 24.6648 -> 25.4490 fields/s (+3.18%).
EE boundary exclusive per field 12.440 -> 12.019 ms; IOP
4.190 -> 3.880; submit CPU 2.249 -> 2.030. Batch scope
0.003 -> 0.005 ms/field does not cover all GS worker binning
cost; do not infer full CPU binning overhead from it.
Candidate 84 native programs, 461349807 native pairs, zero fallback
or worker host failures. Manual inputs differ, no deterministic
visual equivalence established. Keep opt-in, use 8/8 V30 as
next experimental configuration; default remains 16/16.
SC4 FAIL; SC1/SC10 PARTIAL.

### P5A — capture complete combat EE hot-PC set

F8 existing per-PC accounting ranks by measured native boundary
duration with nested scope totals, prints top 25. Preserve all
ranked rows at normal shutdown in <report>.pc-profile.csv, with
raw calls, dispatches, attempted instructions, idle-charged
instructions, native boundary and nested ns. No added hot-loop
cost; do not treat this as LLVM instrumentation PGO data or
infer CFG edge frequencies from entry counts. Archive the CSV
with the regular full run, remove stale sidecar before replay.
Test stale cleanup and archive allowlist. Use V30, 8/8, F6 on,
F7 off and F8 on only in expensive combat, turn F8 off and
close normally or reach dispatch limit. Next select hot modules,
inspect generated IR and assess targeted optimization/PGO design.
No guest lowering changes; CI pending.

| Experiment | Result | Decision |
|---|---|---|
| P3A V30 | -12.46% VU1 ns/pair, gameplay +1.14% nominal | Experimental control |
| P4B 8/8 binning | -11.04% comparable raster, gameplay +3.18% nominal | Keep opt-in pending deterministic validation |
| P5A complete EE PC export | Pending combat capture | Attribute EE hotspots |

### P5A result — 20261128-072445332

Clean b59cb37, 40M dispatches, 2515 fields in 41.37548 s,
84 V30 libraries, native pairs 444387833, no fallback or worker
host failures; 3165/3165 normal GPU queries complete, no detailed
passes. Not a matched unprofiled comparison (F8 enabled during
combat; manual input). Last six full combat intervals 52–53
fields per ~2.003 s; approximately 26 fields/s, still short of
59.94 target. Whole-run rate 60.78 is misleading.

Full CSV has 2698 distinct entry PCs, 16729220 calls,
16728790 dispatches, 1840583763 attempted guest instructions,
1456025193 idle-charged, 384558570 non-skipped. Inclusive
4823355000 ns, nested 322060900, exclusive 4501294100 ns.
Rank by inclusive minus measured nested scopes, not attempted
guest instruction count or inclusive wall. 0x172e48 has
155.116 ms inclusive but 154.227 ms nested. 0x175210 alone
accounts for all idle-charged instructions; do not treat
those as executed host work.

| PC | Calls | Exclusive ms |
|---|---:|---:|
| 0x108220 | 193125 | 236.0655 |
| 0x1068e0 | 347623 | 150.0907 |
| 0x106a20 | 191249 | 125.1181 |
| 0x185cb0 | 592441 | 79.4239 |
| 0x110304 | 5766 | 75.2837 |
| 0x1a9a40 | 185250 | 74.3718 |
| 0x1a0c80 | 85500 | 69.169 |
| 0x1a2a10 | 277875 | 67.8228 |
| 0x186038 | 172899 | 64.9509 |
| 0x106950 | 78572 | 61.4202 |

Top 10 cover 22.30%, 25 cover 34.67%, 100 cover 57.67%.
Entry PCs averaging <=10 non-skipped instructions/call account
for 9390158 calls (56.13%) and 1.3170597 s (29.26% exclusive).
<=20 account for 12399838 calls (74.12%) and 1.9840394 s
(44.08%). Broad native boundary costs justify investigating
direct transfers, register residency and interprocedural
optimization, not merely one math routine. Exclusive time
still includes host boundary overhead; no causal attribution.

### P5B — profile-ranked read-only IR collection

Select union of top 25 PCs by exclusive boundary and call count.
Map exact entries and possible resume owners using current
ee_functions.json, validate metadata and selected IR SHA-256
against the captured translation-cache.json before writing a
fresh ZIP. Keep unresolved PCs explicit. No cache writes or new
lowering. Inspect original IR for the dominant native regions,
then design a bounded hot-module interprocedural or direct-call
experiment with correctness gates. Entry counts alone are not
instrumented PGO edge profiles. No speedup claimed.

### P5B/P5C — hot VU0 inline pilot

ee-hot-combat-ir.zip selection receipt matches all 27 IR hashes;
36 PCs cover 1.7482219 / 4.5012941 s (38.84% exclusive).
0x108220 has eight LQC2, sixteen full-mask broadcast FMAC
operations (four each VMULAX/VMADDAY/VMADDAZ/VMADDW), four SQC2,
JR and delay; 193125 calls imply 3090000 FMAC operations.
0x1068e0 dot product, 0x106a20 vector transform, 0x106950
reciprocal-square-root path. The current generated helpers
already implement exact packed VU arithmetic and synchronization.
No host float substitutions, callback elision or flag changes.

Local static pilot: LLVM 20.1.8, x86_64-pc-windows-msvc,
znver2, O2 middle-end/backend, default target feature set,
not exact LLVM 22.1.0 host target. Object sizes:

| PC | Base | Single-use FMAC inline | All FMAC inline |
|---|---:|---:|---:|
| 108220 | 19300 | 19300 | 32219 |
| 1068e0 | 11827 | 11526 | 11526 |
| 106a20 | 19245 | 17900 | 17900 |
| 106950 | 15725 | 14729 | 14729 |
| 106aa0 | 33753 | 27669 | 32600 |

Forcing sync inline did not change object sizes or static
instruction counts. Reject blanket FMAC inlining. Single-use
annotation reduces static instructions/calls/branches in these
four modules, not proof of wall-time speedup. 108220 unchanged.
P5C tool stages only four captured modules, authenticates
production metadata/IR/object/library, requires byte-identical
control re-emission, retains replacements only when object and
executable bytes do not grow. Unchanged objects are reused;
no production IR edits or promotion. The existing matched
replay harness accepts this plan with an additional baseline
library identity gate. CI and retail correctness/performance
pending. Next: if size gate passes, compare isolated DLL with
deterministic PAD replay, then investigate matrix FMAC arithmetic
and register residency. SC1 PARTIAL, SC4 FAIL.

### P5C result — ee-fmac-inline-comparison.zip

All five workflows at a5bebbc passed. Synchronous O5 control
and inline overlay, 40M dispatches, 2560/2560 PAD polls,
2003975 matched dispatch trace samples, zero differences or
unpaired samples. Final parsed reports and rendered image
match across baseline/control/candidate. Wall 51.204/51.500/
51.469 s, 2623 fields each. Candidate +0.06% throughput versus
control, less than A/A variation. Correctness accepted for this
replay; no demonstrated speedup, do not promote.

### P5D — exact packed discard fast paths

Isolated O5 overlay from original production objects, not the
P5C image. For multiply, return XOR-signed zero with both flags
clear only if every lane has an exponent-zero operand. For
add/sub, return the surviving operand (or intersection-signed
zero) only if every lane has at least one exponent-zero or
distance >=25 operand. Preserve the original full path,
including Booth correction, cancellation underflow, architectural
flags, ACC and synchronization. No approximate host floating
arithmetic or FMA replacement.

Local actual captured-IR test: LLVM 20.1.8 native O0/O3,
600000 result/overflow/underflow lane comparisons matched;
random raw operands, flush-to-zero, alternating lanes and
large exponent differences. Windows x86_64 znver2 static
O2 sizes (not production LLVM 22.1.0): 108220 19300->19942,
1068e0 11827->12031, 106a20 19245->19893, 106950 15725->16023,
106aa0 33753->33436 bytes. Actual branch hit rate and runtime
benefit unknown. New CTest uses production arithmetic template
and equivalent differential cases; CI pending.

Reuse the authenticated overlay builder with --mode vector-discard;
only these five hot modules, same byte-identical O5 control
re-emission, per-module object and executable growth at most 10%.
Original library identity and isolated cache enforced, deterministic
PAD replay required. Receipt now includes raster tile/workgroup.
Keep baseline unchanged.

Platform/vendor-specific optimization explicitly permitted:
use if constexpr for C++ compile-time target selection where
appropriate, and recorded LLVM target CPU/features for emitted
guest IR; not compiler-host inference. Portable exact arithmetic
remains the reference/fallback. ISA availability must be proven
before using intrinsics, and CPU features in artifact provenance.
No vendor-specific instructions forced in this checkpoint.

### P5D result — ee-vector-discard-comparison.zip

Synchronous O5 A/A/B, same V30 inventory directory, 8/8 tile/workgroup,
40M dispatches each. 2359/2359 PAD polls consumed, 1837171 sampled
transitions identical with zero missing samples. Raw terminal JSON
and PPM byte-identical across all three arms (report SHA-256
6bca8abc8587740e1ed136aa354b7e285665c99ea5ae5457466bb6b916b10086,
image 08eab311b1a620f325d9b72d2713c720f342273286b6cea7be15714840902d7a).
2422 fields and presentations, 441964423 VU1 native pairs,
zero fallback. Wall 51.281/51.359/51.407 s. Candidate -0.093%
throughput vs A/A control, less than A/A spread. Correctness
accepted for this workload only; do not promote for performance.
No controlled async speed result. CI at 05726db: four workflows
passed, Build and test pending at inspection.

### P4C — reject out-of-bounds pixels before loading GS setup

Raster loop currently reads the primitive setup ID and loads the
26-word immutable setup record, including three format calculations,
before the per-pixel bounding-box rejection. Move the rejection
ahead of setup loads via opt-in pipeline override RASTER_LAZY_SETUP
(81d3033). No CPU binning changes, same list traversal and shade
order, no VRAM/flag changes intended; last_setup always describes
actually loaded state. Setup loading reads immutable data only;
rejected primitives perform no shading. This may save repeated loads
and computations, but divergent lanes or compiler scheduling could
offset gains. Default 0 preserves baseline path; 1 enables candidate.
PS2_RASTER_LAZY_SETUP recorded in boot provenance and stderr.
New native WebGPU 8/8 lazy-setup smoke variant. Matched runner
replay --raster-lazy-setup uses the same authenticated O5
library and runner for baseline, A/A and B, forces sync and disables
runtime/PC/GPU profiling, requires confirmed per-arm pipeline markers,
PAD consumption, sampled trace agreement, exact final report and PPM
hashes; no production cache writes. User must rebuild runner only.
F5/F6/F7/F8 toggles should not be used during this correctness
comparison. This is a bounded GPU speed hypothesis, not an achieved
speedup. SC4 remains unmet; SC1 and SC10 partial.

### P4C result — ee-raster-lazy-setup-comparison.zip

Matched A/A/B using identical authenticated O5 DLL and runner.
8/8 raster tile and group, V30 directory, synchronous GS and VU1.
Confirmed lazy_setup 0/0/1. All reached 40M dispatches; 2603/2603
recorded PAD polls consumed, 2044579 common trace samples, zero
differences/unpaired. Independently confirmed byte-identical JSON
and PPM hashes across all arms. 2666 fields and successful
presentations; 388310365 native VU1 pairs, no interpreter fallbacks.
Wall 50.797/50.875/50.953 seconds; candidate -0.153% throughput vs
control, within same magnitude A/A variation. Correctness accepted
for this replay; keep lazy setup opt-in, not a demonstrated speedup.
No detailed GPU timing or matched async speed result.

### P5E — broad profile-selected native O3 pilot

Original O5 pipeline uses full LLVM O2 only for modules containing
specialized VU0 FMAC calls; other modules use the native O2 backend
without the full middle-end pipeline. Test a wider hot selection
from the P5A full CSV (top 250 entry PCs each by exclusive time and
call count, including metadata resume-owner candidates). For each
selected module require byte-identical O5 re-emission, then run
full O3 middle-end and native O3 backend with identical host CPU
and feature string. Do not change source IR, public function
inventory, guest ownership or portable cache keys. Parallel
bounded worker pools; retain replacements only if bytes change
and each object/executable section total is <=150% of original.
This is a deliberately permissive pilot size gate, not production
acceptance. Cold objects reused, isolated overlay marked nonproduction.
Record profile and cache hashes. Matched synchronous A/A/B required;
no PGO instrumentation or ThinLTO claimed: this is hotness-based
selection of optimization intensity, not edge-profile feedback.
Full production CI and repeated async combat benchmarks remain
required for any promotion. No measured speedup yet.

### P5E result — ee-hot-o3-comparison.zip

Build recovery succeeded. helper-plan(9).json: 161 selected modules,
91 accepted and 70 rejected by the per-module 150% size gate;
all 16252 functions retained, original O5 controls byte-identical.
Selected owner candidates represent 77.65% of the original profile's
exclusive boundary time; this is selection coverage, not proof that
accepted objects account for that entire percentage. Accepted object
bytes 1642697 -> 1934826 (+17.78%); executable bytes 1139290 ->
1458799 (+28.04%). Candidate DLL 112148992 bytes. Host znver2,
LLVM 22.1.0, x86_64-pc-windows-msvc.
Receipt baseline and candidate DLL hashes match plan. All three
arms 40M dispatches, 2541 fields/presentations, 12281599700 guest
attempts, 415895434 native VU1 pairs, zero interpreter fallbacks.
Recovered inventory 88 native programs, exceeding previous 84
count but not proof of inventory superset. 2478 PAD polls, 1935262
common trace samples, zero differences/unpaired. Independently
verified byte-identical raw JSON and PPM across arms.
Wall 50.797/50.579/50.766 s; candidate -0.368% throughput vs A/A
control, within A/A spread. Matched late dispatch range 24002560
to 39993344: 332 fields each, 17.839/17.859/17.798 s, 18.61/18.59/
18.65 fields/s; native elapsed 11.604/11.568/11.527 s (inclusive).
No measurable speedup. Do not promote O3 overlay. Synchronous
results are not representative of normal asynchronous play.

### P4D — matched validation of finer raster binning

Earlier P4B GPU profile showed meaningful raster time reduction,
but independent interactive runs cannot establish pixel equivalence.
Add same-library --raster-tile8 comparison to existing replay harness:
16/16 control and 8/8 candidate, lazy setup off, same O5 DLL/runner,
synchronous, no profiling. Require confirmed tile and workgroup
markers, matched PAD, trace and exact final report/PPM. Optional
--recording accepts a complete prior PAD0 recording for every arm,
validates baseline consumption as well and hashes the input before
and after. No new C++ changes or cache rebuild. Reuse P5E recording
for a comparable combat workload. Promotion still requires matched
image checks and representative normal async performance evidence.

### P4D result — ee-raster-tile8-comparison.zip

All three 40M dispatch replays complete, PAD 2478/2478 each,
1935262 sampled transitions common, zero differences/unpaired.
Actual GPU markers confirm 16/16, 16/16, 8/8; lazy setup disabled.
Identical raw report SHA-256
7d6ac0d1a09eee53c6cfc42708e090ae0ef497c424fe26628067454d4b104770
and PPM SHA-256
81cbb5383d37d7a7a9ae1658447512d4c78a9812dd456fb09af097d998db169a
independently verified. Identical runner/library and recording
hashes before/after. 2541 fields, 88 VU1 programs, 378121 native
runs, 415895434 native pairs, zero interpreter fallbacks.
Wall 51.032/51.672/52.156 s; candidate -0.928% throughput vs
control, below A/A variation. From dispatch 24002560 to 39993344:
332 fields each in 18.1495/18.0981/18.1693 s. Synchronous
correctness accepted; no measurable speed improvement. Keep opt-in.
Prior independently captured GPU timestamp improvements remain
diagnostic evidence, not proof of normal async speed benefit.

### P4E — diagnostic matched async raster comparison

Permit explicit --async-raster with --raster-tile8 --control
and an existing --recording; cannot combine with --sync.
Run same library and runner with asynchronous VU1 and GS,
no detailed GPU or CPU/PC profiling. Require PAD consumption
for each arm, same sampled dispatch transitions and exact PPM
hash. Terminal report comparison uses the existing graphics
replay normalization of host timing/profile fields only;
record raw and normalized report hashes. Preserve guest and
worker counters, report failure rather than relaxing other
gates. Existing sync replay retains byte-identical report check.
A synchronous recording may diverge on async execution; if so,
the archive is diagnostic only. No production changes, no
cache rebuild. One A/A/B is a screening measurement; repeat
and sustained combat acceptance gates still required.

### P4E result — ee-raster-tile8-async.zip

Actual 16/16, 16/16, 8/8 markers, lazy setup off. All completed
40M dispatches, PAD 2478/2478, 2541 fields/presentations, 88 native
VU1 programs, 378121 native runs, 415895434 pairs, no fallback.
Same final PPM SHA-256 81cbb5383d37d7a7a9ae1658447512d4c78a9812dd456fb09af097d998db169a.
Complete GIF/GS, path, VIF1 producer, presentation and SIO2
report sections identical. However A/A first differs at dispatch
11574272: 30107 different common samples and 207323 unpaired.
A/B first differs at 11560336: 30070 different common samples,
252236 unpaired. Final registers, PC and instruction counts differ.
A/A/B wall 41.734/42.422/41.969 s; candidate +1.08% throughput
vs A/A, within A/A variation. Late dispatch interval 24002560
to 39993344: 332 fields each, 12.0188/12.4157/12.1557 s.
Keep 16/16 default. Async scheduling variance is a blocker for
strict dispatch matching; do not label equal graphics workloads
as architectural equivalence. No cycle-accuracy detour.

### P6A — bound optional host diagnostic overhead once

The "profiling off" P4E arms still performed ~37M empty controller
polls and ~14.6M EE/VU1 overlap intervals. Polling is already a
single acquire check and changing it changes completion timing;
do not reduce guest-visible polling cadence. The native overlap
collector performs host clock reads at pending native boundaries.
Existing report_hotspots and boundary_timing also default on.
New --async-lean uses the same recorded workload, 16/16 GPU,
same authenticated O5 DLL and runner, async VU1/GS, no detailed
profiling. A/A force existing three host diagnostic options on,
B sets SDBZ_REPORT_HOTSPOTS, SDBZ_REPORT_BOUNDARY_TIMING and
SDBZ_VU1_NATIVE_OVERLAP_PROFILE to 0. Preserve dispatch tracing,
history and scheduler cadence; no C++ changes. This is a single
diagnostic overhead screening, not an assumption that tracing
explains slow 3D. Save exact graphics workload and final-image
hash comparison before strict trace checks, including when A/A
fails; explicitly not architectural equivalence. Verify reported
hotspots and overlap modes, and hash VU1 library inventory before
and after all async comparisons. Keep strict trace/report gates.
No production defaults change. SC4 remains unmet.

### P6A result — ee-async-lean-comparison.zip

Same authenticated O5 DLL, runner, 88-program VU1 inventory,
recording unchanged. All three runs 40M dispatches, 2478/2478 PAD,
2541 fields/presentations, 4345821 GPU committed draws, zero VU1
interpreter fallbacks. Independently verified identical final PPM
SHA-256 81cbb5383d37d7a7a9ae1658447512d4c78a9812dd456fb09af097d998db169a.
Actual stdout startup markers confirm hotspots/boundary 1/1,
1/1, 0/0; report confirms native overlap true/true/false.
Hotspot observations 40M/40M/0. Worker overlap intervals
15219452/15358832/0. Wall 42.735/43.000/37.329 seconds:
candidate +15.19% throughput vs control, A/A spread 0.62%.
Late dispatch 24002560..39993344: same 332 fields in
12.4863/12.2483/9.9754 s (26.59/27.11/33.28 fields/s),
candidate +22.79% throughput. Candidate late rate ~55.5% of
59.94 Hz. Native attempts and VIF1 producer counters differ
slightly; VU1 pairs 415851899/415891585/415745774, difference
candidate vs control -0.035%. Other selected graphics report
sections equal. A/A first different 11560411, A/B 11560534;
strict trace and architectural equivalence NOT established.
Performance signal materially exceeds A/A spread but remains
diagnostic, not a production acceptance or a claim of exact
guest timing. Do not promote defaults yet.

### P6B — isolate three independent diagnostic switches

Extend --async-lean with --lean-component hotspots, boundary,
overlap or all (default all). A/A keeps all three enabled;
candidate disables exactly the named collector. Validate each
startup report-options marker and worker overlap flag, not just
the requested environment. Preserve the strict gates and save
diagnostic evidence before checking trace equality. Reuse the
same recording and binaries. No C++ or production cache changes.
The single-switch effect need not add linearly due to thread
overlap. Compare late matched field windows and A/A variance.

### P6B result — single-switch diagnostics

Three A/A/B archives, all same authenticated O5 DLL, runner,
recording and 88-program VU1 inventory. All nine reached 40M
dispatches, 2541 fields, 2478/2478 PAD polls, zero VU1 fallbacks;
all nine PPM hashes independently verified identical
(81cbb5383d37d7a7a9ae1658447512d4c78a9812dd456fb09af097d998db169a).
Startup markers confirm exactly the requested switch disabled.
Each A/A pair uses identical graphics workloads; candidate draws
4345821 vs 4353515 control (-0.177%). Native pairs control
415895434 each; candidates hotspots 415892863, boundary 415825238,
overlap 415839620. No exact architectural match (A/A also diverges).
Late dispatch window 24002560..39993344, 332 fields per arm:

| Collector disabled | A/A/B wall s | Late A/A/B s | Whole speed gain vs A/A | Late gain vs A/A |
| --- | --- | --- | --- | --- |
| hotspots | 39.906/39.813/38.891 | 11.7061/11.6564/11.2657 | 2.37% | 3.47% |
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
