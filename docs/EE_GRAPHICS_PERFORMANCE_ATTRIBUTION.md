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
