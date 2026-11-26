# EE IR and native instruction-cost audit

The 40M baseline (fixed RTC, asynchronous VU1/GS, baseline IOP)
spent 19.793 s of 46.824 s in exclusive native EE boundaries,
7.257 s in IOP service and 2.142 s in dispatch diagnostics. The
four-mode IOP experiment preserved the observed synchronous-VU1
workload but did not improve performance. Keep both IOP shortcuts
off by default. Prioritize actual host instruction cost rather than
source IR line counts.

## Batch 1: scalar instruction accounting

A consecutive run of 3..16 nontrapping, GPR-only instructions from
the emitter's explicit whitelist can increment the guest-attempt and
reference-slot counters once each, before the run. There is no memory
or helper call, architectural exception, delay slot, control transfer,
COP access or other event producer within the fast run. V0 destinations
are excluded because a separate diagnostic write helper may call out.

The counter helper selects the fast path only when both the timing
observer and instruction trace pointers are null (or the whole runtime
is null, for legacy JIT clients). Otherwise the original per-instruction
path executes, with its original observer order, attempt and slot
numbers. Merge differing cached GPR SSA values with PHIs; preserve the
dirty-half tracking and original native dispatch/loop boundaries.
Reference clock, guest opcodes and delay-slot rules do not change.
The strict native ALU certificate parser currently rejects the new
multi-block form, so a batch is not a new execution-horizon certificate.

## Batch 2: outline cold instruction observations

When neither an observer nor a trace is installed, the normal counter
helper only updates both counters and checks the two pointers. On the
cold path, read the updated counters and invoke the original observer
then trace in order, including callbacks that install a trace during
the current observation. Annulled slots update only the reference
counter and use the same guarded publication path.

O5 is host-native O2 **backend only**, not the Clang default O3
middle-end. An alwaysinline attribute does not guarantee inlining in
that profile. Emit the observation gate directly inside both counter
helpers. The backend can use in-memory increment instructions and
avoid all nested calls and stack setup when observations are disabled.

Measured on the supplied original retail module
`sub_0x1906f0_0x1906f0.ll`, replacing just the observation helpers
and compiling with Clang 17 -O3 Windows x64 reduced total static
mnemonics from 1303 to 727, including internal helper definitions.
This comparison is not the production O5 pipeline.

A separate local LLVM 20.1.8 Linux x86-64 host-native O2 backend
(znver4) emitted the original counting path with nested calls;
the final candidate has two memory increments, a combined
observer/trace pointer check and no nested calls on the quiet path.
Eight alternating 20-million-call microbenchmark samples:
original median 89.20 ms, candidate 28.61 ms (3.12x accounting
throughput). Both counters exactly 20,000,000 in every sample.
This isolates accounting only, not retail speedup or Windows host
performance. Static code/object size can grow because cold paths
and the original observed path remain available.

## Static cost tool

`tools/audit_ir_cost.py` uses the same target machine setup and
middle-end profile selection as `verify_ir.py`. It reports counts of
defined IR instructions (all helpers and guest functions), selected
pipeline IR instructions, static x86 assembly mnemonics/calls/branches,
object bytes and hashes. It does not modify any source/cache. O5
does not run a separate LLVM IR optimization pipeline, so the
pipeline IR counts are unchanged by design. Use `--compare-dir`
to compare identical module basenames from two retained IR generations;
object bytes include relocation/metadata. Static counts are not
dynamic execution-weighted costs.

## Retail validation

Lowering changed: recover the known entry manifest using
`SDBZ_EE_OPT=0` and `--recover-cache --accept-new-ir`.
The launcher retains the previous canonical cache in a backup
and rebuilds the selected native profile on the next P40 run.
Do not bypass provenance checks or erase the discovered entry set.

First repeat a matched short synchronous-VU1 run with fixed RTC,
same controller stream, identical initial card state and full diagnostics
to compare all existing architectural trace records. Then run normal
uncapped asynchronous P40 gameplay with IOP experiments off,
input replay and recording off, and identical profile/clock/renderer
settings. Compare title/gameplay host costs separately and record
the native wall time and exact workload counters; asynchronous
VU1 completion remains a known repeatability limitation.

Lean report controls from the preceding runtime-only changes:
`SDBZ_REPORT_HOTSPOTS=0` disables per-dispatch hotspot maps while
retaining bounded fault history; `SDBZ_REPORT_BOUNDARY_TIMING=0`
and `SDBZ_RUNTIME_PROFILE=0` remove forced boundary time sampling.
The full diagnostic defaults remain unchanged. Measure with
identical diagnostic settings when comparing codegen, then measure
lean mode separately. Do not treat missing timing fields as measured
zero work.

Next: use the hot PC profile and original/native IR counts to select
specific register caching, memory access, MMI/COP2 and control-flow
lowerings. Measure each change on the actual backend rather than
assuming smaller source IR means less dynamic native work.

## First regenerated retail runs (73ff235)

Two complete 40M O5 runs at 06:34:18 and 06:35:27, fixed RTC,
async VU1/GS, baseline IOP, full runtime profiling and hotspot maps.
Both have the identical translation-cache manifest and native library
SHA-256 prefix 5ec0985422fdb780. All 16,250 source hashes changed from
the earlier 8fab876 cache; the same ELF and module inventory are retained.
No reported IOP fault, VU1 fallback, timing observer failure or residual
IOP debt. Both reach spawn. Manual input and host completion timing
prevent treating these as exactly matched workloads.

| Metric | 8fab876 baseline replay | 06:34 | 06:35 |
| --- | ---: | ---: | ---: |
| Process wall (s) | 51.453 | 51.078 | 47.532 |
| Profile wall (s) | 46.824 | 45.609 | 42.507 |
| EE native exclusive (s) | 19.793 | 14.745 | 13.052 |
| IOP service (s) | 7.257 | 8.441 | 7.938 |
| Fields | 5094 | 5829 | 5744 |
| Fields/s | 108.79 | 127.80 | 135.13 |
| Non-idle-charged guest attempts | 2222410928 | 2300403380 | 2434007009 |
| Spawn dispatch | 22054541 | 25137707 | 24816127 |
| Pre-spawn fields/s | 143.86 | 168.53 | 178.49 |
| Post-spawn fields/s | 43.08 | 40.45 | 44.11 |
| Post-spawn exclusive native ns/dispatch | 321.63 | 295.92 | 245.26 |

Phase figures sum complete logged profile intervals wholly before or
after first spawn; the straddling interval is excluded. This is a
spawn-based proxy, not an exact title or active-gameplay classifier.
The first common sampled dispatch mismatch is 4,839,424. At the
preceding common progress sample 4,833,280, native inclusive ms are
911.3 / 727.4 / 639.9. These are observations, not an architectural
trace equivalence or isolated codegen speedup. CI timing observer
JIT crash was reproduced with two MCJIT engines sharing an owned
target machine; d024d18 gives each its own. Original autotest workflow
also failed setup (raylib GitHub 403 and missing test files), not a
verified guest transcript regression.

For the next gameplay profile set SDBZ_PROFILE_PCS=1 and optionally
SDBZ_PROFILE_PCS_FIRST=26000000 / SDBZ_PROFILE_PCS_LAST=40000000.
Bounds are inclusive prospective native dispatch ordinals, default
1..UINT32_MAX. The full report/profiler remains unchanged; only PC
attribution is filtered. This is host-only and needs no regenerated
IR. Choose the lower bound based on actual gameplay entry and use
normal uncapped asynchronous P40. Do not interpret PC ranks as
static instruction counts or compare profiled wall time directly to
an unprofiled run.
