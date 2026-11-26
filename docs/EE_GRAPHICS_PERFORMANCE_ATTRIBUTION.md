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
