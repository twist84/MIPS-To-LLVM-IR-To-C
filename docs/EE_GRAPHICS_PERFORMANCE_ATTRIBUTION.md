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
