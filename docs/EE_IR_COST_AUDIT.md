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
