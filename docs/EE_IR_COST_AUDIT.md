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

## Gameplay hot modules and selective O5 (2026-11-26)

The 06:40 gameplay profile covers native entry ordinals 26M..40M.
The twelve provided modules include five with calls to specialised
VU0 FMAC helpers. The hottest exclusive native entry, 0x00108220,
performs 8 LQC2, 16 FMAC operations, 4 SQC2, JR and its delay slot.
Previous O5 did not run the middle-end inliner and still paid for
many alwaysinline helper calls. The local LLVM 20.1.8 host-native
O2 comparison (Linux x86-64) gives:

| Entry | Backend-only mnemonics | O2 IR + backend mnemonics |
| --- | ---: | ---: |
| 00108220 | 3302 | 2468 |
| 00106a20 | 3433 | 2323 |
| 001068e0 | 3060 | 1714 |
| 00106950 | 3402 | 2311 |
| 00106aa0 | 6430 | 5588 |
| Total | 19627 | 14404 |

Call sites 1133 -> 518, object bytes 154720 -> 110840. Static
counts include internal helpers, not dynamic retired instructions.
Full O2 expands some other hot modules (001102b0: 3431 -> 5936;
00185830: 3065 -> 6012), so do not apply it globally.

The revised O5 pipeline identity is llvm-vu0-fmac-selective-O2-v1.
Select a module by an actual specialised FMAC call, not the presence
of generic helper definitions or game-specific addresses. The other
modules still use the host-native O2 backend alone. O0..O4 retain
their previous policy. The verifier, audit tool, cache builder and
linker agree on pipeline identity. A previous O5 object manifest is
incompatible and is rebuilt once from verified shared IR, without
re-emitting guest source IR or changing the known function inventory.

The uploaded 00108220 module was also linked with an isolated
synthetic RAM/sync harness. Two versions matched all 640 cases
(128 fixed-seed mixed arbitrary/edge-bit inputs, each with normal
completion, synchronization failure at calls 1/8/16, and misaligned
input). Compared return status, complete CPU and VU buffers, RAM,
both counters and ordered read/write/sync/exception records. Eight
alternating samples of 100,000 calls, finite input, no event-log
recording: baseline median 147.679 ms, selective 103.701 ms, 29.8%
less time. Every sample recorded exactly 3M guest attempts/slots and
1.6M sync calls. This is a host-local stubbed-memory microbenchmark,
not a retail game benchmark or hardware timing validation. The
initial MCJIT-based local harness crashed; the linked-object harness
was used instead. Repository regressions exercise actual decoded
VMUL, VADD, VMULA/MADD, failure paths, source identity and pipeline
migration; final-head CI and Windows retail throughput remain to
be checked.

Next run ordinary P40 with the same gameplay PC window and full
profiling. Do not use --overwrite-ir: only O5 native objects need
rebuilding. Compare native-exclusive PC costs normalized by calls
and post-spawn field throughput; asynchronous completion and manual
input remain workload-matching limitations.

## Selective O5 retail result (06:53:43)

Clean 0820c18, O5 pipeline llvm-vu0-fmac-selective-O2-v1, all
16,250 source IR hashes and lowering inputs identical to ce21604.
New native library SHA-256 81322ff69b6a6671cbd29cb824c14a5a78ac0e26744a17a1603c00c9ed8e7f9a.
Both 40M runs use the same 26M..40M PC window, fixed RTC, async VU1/GS,
full profiling and baseline IOP. No reported IOP guest fault, no VU1
interpreter fallbacks, zero IOP debt.

| Entry PC | Previous ns/attempt | Selective ns/attempt |
| --- | ---: | ---: |
| 00108220 | 43.36 | 28.01 |
| 00106a20 | 26.12 | 16.11 |
| 001068e0 | 14.82 | 10.16 |
| 00106950 | 28.98 | 19.95 |
| 00106aa0 | 81.98 | 49.31 |

Five-PC total native time 559.2425 -> 362.5967 ms (-35.16%),
699,123 -> 698,255 calls and 20,452,246 -> 20,427,994 attempts.
Process wall 46.672 -> 45.453 s; profile wall 41.7953 -> 40.8858 s;
EE exclusive 12.8512 -> 12.7502 s; IOP 7.3532 -> 7.2668 s.
Fields 4697 -> 4869. First spawn at 22,493,853; complete post-spawn
intervals 598 fields / 12.526043 s = 47.7405 fields/s, versus 44.377
previously. Post-spawn native exclusive 243.313 -> 215.985 ns/dispatch.
These phase estimates are not controlled execution traces. The five
PC instruction-normalized reductions (31..40%) have close workload
counts and are the stronger evidence for the targeted improvement.

0820c18 CI: original ps2autotests, ROM0, SIF DMA and WebGPU passed;
Build and test failed the historical O5 recovery inventory selection
regression. 0b6b2cc accepts the known legacy backend-only O5 pipeline
identity for guest inventory recovery only; native reuse/linking still
requires the current pipeline ID. Unknown pipeline IDs remain rejected.

Next host-only change f320cb1 outlines scalar memory bus/fallback
logic. This leaves address translation, RAM-span checks, endianness,
callback widths/order, scratchpad and rejected transfers unchanged.
80aacbc adds scalar ABI tests for RAM aliases, scratchpad, bounds and
MMIO read/write widths and unhandled reads. Native recompilation or
relink may be needed, but no guest IR change. Performance benefit and
latest CI are unverified. Compare the next normal P40 replay using
identical diagnostics and PC window; the remaining scalar-heavy
hot modules repeatedly call memory and trace helpers. Avoid bypassing
MMIO/TLB or guest-store trace semantics for speculative gains.

## Scalar memory follow-up (07:00:12)

Clean 07ee4f6, 40M dispatches, unchanged 16,250 guest IR hashes and
lowering provenance, selective O5, native library SHA-256
42f2fbc7d9cc456bc109168072fd7c318420fd8d2289140ad00126f53cba6a2d.
Same full diagnostics, fixed RTC, async VU1/GS, baseline IOP and
26M..40M PC window. No IOP guest fault, VU1 interpreter fallbacks or
residual cycle debt.

Compared to 06:53:43: process wall 45.453 -> 43.609 s, exclusive
EE 12.750 -> 11.942 s, IOP service 7.267 -> 6.875 s. First spawn
22,493,853 -> 21,552,226. Complete post-spawn intervals 598 fields
in 12.526043 s -> 619 in 12.6848695 s (47.74 -> 48.80 fields/s);
EE exclusive 215.99 -> 211.37 ns/dispatch. The total field count
actually fell 4869 -> 4386, so raw process times are not an isolated
memory-helper speedup. Post-spawn IOP cost also fell; manual input,
async completion and workload differences remain confounders.

The five optimized VU0 entries remain faster than the old backend-only
O5. Their latest ns/non-skipped attempt: 108220 27.74, 106a20 16.06,
1068e0 9.67, 106950 19.79, 106aa0 49.07. Scalar hotspots show mixed
small changes; avoid claiming a uniform improvement.

The latest post-spawn intervals spend 0.908 s on dispatch diagnostics
alone (plus timer sampling overhead, not isolated). 471a9f6 adds
cumulative guest field counts to ordinary progress/final timing lines,
including when detailed runtime profiling is disabled. 39856c6 checks
monotonic counts and final JSON agreement in the lean fixture. Measure
normal uncapped gameplay with SDBZ_RUNTIME_PROFILE=0,
SDBZ_REPORT_HOTSPOTS=0, SDBZ_REPORT_BOUNDARY_TIMING=0,
SDBZ_PROFILE_PCS=0; retain the normal snapshot and progress cadence.
Use differences in cumulative fields and elapsed_ms between progress
samples wholly after first spawn, not absent subsystem timings or the
full-run mean. This is a separate diagnostic-cost experiment, not a
new codegen change or exact workload comparison.

## Lean P40 repeatability (07:04:04 and 07:06:34)

Both archives identify clean eece224, fixed RTC 1795671645, selective
O5, async VU1/GS, baseline IOP, all four diagnostic controls disabled
and identical translation-cache.json SHA-256
69fb5b58326bf5b996967721f2ca722f927b7248b708b12c50e5339d26fb9d38.
Both stop at 40M dispatches with no IOP guest fault, VU1 interpreter
fallback or worker failure. Each has one outstanding worker job at
the stop boundary; all attempted presentations succeeded.

| Measurement | 07:04 | 07:06 |
| --- | ---: | ---: |
| Native process wall (s) | 46.640 | 47.547 |
| First spawn dispatch | 22307633 | 19338869 |
| First post-spawn sample dispatch | 22315008 | 19341312 |
| First post-spawn elapsed (ms) | 22986.8 | 20849.8 |
| First post-spawn fields | 4375 | 3738 |
| Terminal elapsed (ms) | 41955.8 | 42995.3 |
| Terminal fields | 5527 | 5084 |
| Post-spawn fields/s | 60.7307 | 60.7798 |
| Successful presents | 3949 | 3614 |
| GPU draws | 4511358 | 4998470 |
| VU1 native executions | 374804 | 411796 |
| VU1 worker polls | 48549059 | 53488311 |
| Empty polls | 48289998 | 53205091 |
| Blocking waits | 115742 | 128575 |
| Controller wait (s) | 4.7441602 | 5.2445352 |

Mean 60.7553 fields/s, about 101.26% of the nominal 60 fields/s
target, versus the preceding instrumented 48.7983 fields/s. This is
a repeated observation, not a matched-workload speedup or proof of
sustained full speed at every field. Controller input is live and
asynchronous completion remains a source of workload divergence.
Host audio is disabled. Final timing is included here; the full-run
wall time includes different amounts of boot/menu and gameplay.

The empty-poll rate is about 99.47% in both captures, but counts
alone do not identify CPU cost. The VIF1 poll path already checks
the atomic ready flag before taking the worker mutex. The two main
runner checkpoints are at the top of the dispatch loop and in
service_devices before scanout. They are not interchangeable:
other host/device work may occur between them and host-ready
publication can change guest IRQ order. Keep the scanout wait,
COP2 sync, register-write/PATH3 fences, reset/discard and rejection
paths intact. Do not simply throttle or remove acquire checkpoints.
A future experiment needs either proven duplicate boundaries or
separation of host readiness from guest completion eligibility
with explicit ordered-event tests.

eece224 CI passed SIF DMA, ROM0, original autotests and native
WebGPU. Build and test failed game_runner_end_to_end_3 because
the 300-dispatch lean fixture inherited the 1024-dispatch progress
interval from the preceding suppression test. f846bbc explicitly
selects a 256-dispatch progress interval for the lean fixture.
This is test-only, no additional retail replay required.

## Scalar store trace hot path (07:23:22)

Clean 34dc040, selective O5, 16,250 regenerated modules, native
library SHA-256 76d9ae790a65235167c1c8019972e7f4004cc9b7ff8f058976a9c4a1d4bd1be7.
Fixed RTC, live input, async VU1/GS, baseline IOP, lean diagnostics.
40M dispatches, native process wall 45.046 s, terminal runner elapsed
40416.8 ms, 5887 fields, 4194/4194 successful presentations, 1693
skipped fields, 4481260 draws, 366058 native VU1 runs, zero fallback,
IOP guest fault or worker failure. One outstanding VU1 job at stop.
First spawn 23367441; first post-spawn progress 23371776 at
23084.1 ms with 4801 fields; terminal 5887 fields.
1086 fields / 17.3327 s = 62.6561 fields/s, +3.13% vs prior lean
mean 60.7553. Not a matched workload or isolated instruction cost.
Presentation rate and minimum frame speed are not established.

34dc040 CI passed SIF DMA, ROM0, original autotests and WebGPU;
Build and test failed the new scalar trace fixture (expected
DISPATCH despite ending in JR RA). ade1d11 corrects the expected
RETURN and adds more informative failure diagnostics; CI pending.

74ee385 moves the fused quadword RAM probe into the emitted caller,
so successful untraced RAM stores bypass diagnostic-only context
loads and the large helper call. Failed probes retain the original
FIFO/ordered per-lane fallback and trace checks, including enable
changes between lanes. Regression extends dirty-SP/live-FP context
testing to fused success, untraced fallback and traced fallback.
No guest timing or memory semantics intentionally changed.
New lowering requires one deliberate known-inventory IR refresh.
Repeat lean P40, compare first post-spawn progress through terminal
timing; final CI and actual speed benefit of the quadword change
remain unverified.

## Quadword store replay (07:32:57)

Clean 8d9889e, selective O5, fixed RTC, lean diagnostics, async
VU1/GS and baseline IOP. 40M dispatches, native wall 45.734 s,
terminal elapsed 41079.8 ms, 5725 guest fields. First spawn
23633526; first post-spawn progress 23633920 at 23444.3 ms with
4661 fields. 1064 fields / 17.6355 s = 60.3329 fields/s versus
62.6561 in the scalar-only replay and 60.7553 prior lean mean.
4068/4068 successful presents, 1657 skipped fields, 4365192 draws,
362390 native VU1 executions, zero fallbacks, worker failures or
IOP guest faults. One VU1 job outstanding at stop. Native DLL SHA-256
073ac9584d0c83b577f1bd9b19eef27520ec0d58eff9c667a8697ea512997cae.
All five workflows passed 8d9889e. No demonstrated quadword gain;
different live input and async worker timing confound comparison.
Keep the change for now; consider controlled hot-module A/B if
subsequent measurements do not improve.

233f14d centralizes nine memory lowering sites on modular 32-bit
address arithmetic: no addition for zero displacement, constant
address for zero base register, and 32-bit addition otherwise.
Preserves signed immediate, high-bit truncation, alignment traps,
LQ/SQ rounding, and original full GPR base for trace context.
Directed tests cover positive wrapping, absolute addressing,
negative offsets and wrapped alignment faults. No new bus or
timing semantics; backend may already fold some old expressions,
so native speed benefit is not assumed. CI and retail validation
pending; known-inventory IR refresh required.

## Effective-address replay (07:42:51)

Clean ce795f0, selective O5, 16,250 modules, fixed RTC, lean
diagnostics, async VU1/GS and baseline IOP. Native library SHA-256
dd054cedc2503906a131d2a28354a1b41e72630b0d5006f6f3f777d81f576683.
40M dispatches, process wall 43.875 s, terminal elapsed 39173.7 ms,
6257 guest fields, 4449/4449 successful presents, 1808 skipped,
3666448 GPU draws, 341793 native VU1 executions, zero fallbacks,
worker failures or IOP guest faults. One VU1 job outstanding at stop.
First spawn 28716720; first post-spawn progress 28721152 at
26687.6 ms with 5520 fields. 737 fields / 12.4861 s = 59.0256
fields/s versus previous 60.3329. Later spawn and different work
make the shorter overall wall time noncomparable. No demonstrated
speed benefit for effective-address simplification. All five
workflows passed ce795f0.

b71e84c avoids repeated GPR backing-store publication on internal
branch-likely paths with audited, nontrapping scalar delay slots.
After the existing common flush it marks only published low
halves and all published upper halves clean, retaining their SSA
values. Dirty unpublished low halves remain dirty; slot writes
mark their destination dirty normally. Other slot types retain
the conservative path. Regression exercises a packed POR value
and scalar SP value across both branch outcomes, tests the taken
slot's cached SP read and checks one static publication of each
POR half. No hardware timing change. CI and retail validation
pending; deliberate IR refresh required. Actual runtime benefit
is not assumed; the optimization targets redundant host stores.

## PCSX2-inspired follow-up: lazy GPR loading and guarded word fastmem

PCSX2 reference files: x86/ix86-32/iR5900.cpp (block linking and
event check), x86/iCore.cpp (register caching), x86/ix86-32/
iR5900LoadStore.cpp (constant addresses), and x86/ix86-32/recVTLB.cpp
(fastmem and fault backpatching). This project retains LLVM AOT and
its existing guest clock; no PCSX2 timing or x86 emitter code imported.

af13da2 removes eager loading of the entire live-in GPR set at
block entry. Predecessor publication analysis remains unchanged.
The existing SSA cache loads actual operands on demand; helper
flush/invalidation remains. Directed regressions assert exact GPR
load sets for scalar control transfers, alongside existing branch,
delay-slot, packed-half and observer tests.

882e0c2 prototypes a runtime-disabled-by-default word fastmem path.
SDBZ_EE_FASTMEM_WORD=1 explicitly installs the stable 32 MiB
little-endian physical RAM span in the HLE runner. Generated LW,
LWU, SW, LWC1 and SWC1 check the runtime descriptor and full 4-byte
extent; the direct arm loads/stores align 1 and the other arm calls
the existing memory helper. Existing guest alignment faults and
pre-write diagnostic hook remain. Aliases, scratchpad, MMIO,
out-of-range addresses, null runtime, ROM0 and non-little-endian
runner configurations do not opt into this path. This is NOT
fault-based fastmem, strict TLB support or an assumption that all
guest addresses are physical. The runtime ABI fields append to
the previous layout. New objects must be linked with matching host
ABI and current provenance. d5a383d strengthens the direct-write
test and partial-span fallback case.

Retail checkpoint: the previous b71e84c branch-likely publication
change has not yet had its own replay. Regenerate once, run lean
P40 with SDBZ_EE_FASTMEM_WORD=0 and then repeat with =1 without
overwriting IR. Same native library and translation manifest must
be verified. Use fixed RTC and identical input if possible;
live inputs and async workers remain confounders. Compare post-
spawn field throughput and verify trace/worker/guest fault status.
The disabled guard itself has a cost; do not assume a faster
enabled run means a gain versus the pre-fastmem baseline. Final
CI and measured performance remain pending. Further widths,
alias specialization and block-level linking require evidence.

## Word fastmem retail A/B (08:00 and 08:01)

Both clean 2c6abdb, 40M dispatches, fixed RTC, lean diagnostics,
async VU1/GS, host audio off. Identical cache manifest SHA-256
be8cb7183aa20b576d2e30c804af84446ac7b4817b535f2adadbed04cfe5e22c
and native library SHA-256
a0e1435d494a9a875451302b7aa533857bb267c054b80aa4583d9900f2f4d71a.
Second log explicitly reports enabled direct physical word fastmem.
First has no enabled marker; original provenance omitted this flag.

Off: spawn 26920920, first subsequent progress 26927104 at 25998.7ms,
4760 fields; terminal 40384.9ms, 5614 fields.
854 / 14.3862s = 59.3624 fields/s, wall 45.234s, 3990 presents,
1624 skipped, 3959215 draws, 369366 native VU1 runs.
On: spawn 23113735, first subsequent progress 23117824 at 22738.7ms,
4614 fields; terminal 40000.0ms, 5711 fields.
1097 / 17.2613s = 63.5526 fields/s, wall 44.671s, 4065 presents,
1646 skipped, 4375811 draws, 364442 native VU1 runs.
Both zero VU1 fallbacks, IOP guest faults, worker failures;
CPU/GPU display digests agree within each capture.
Observed +7.06% post-spawn throughput, not isolated or deterministic
because input and async timing vary. All five workflows passed 2c6abdb.

0b73bf4 generalizes the guarded scalar lowering to 8/16/64-bit
accesses. SDBZ_EE_FASTMEM_SCALAR=1 enables all four scalar widths;
WORD=1 with SCALAR=0 retains word-only direct access. Other
widths use a separate appended runtime span descriptor. Alignment,
sign extension, trace order and whole-span fallback unchanged.
Directed tests include signed/unsigned loads, preserved upper
GPR half, exact-width stores and independent flag/fallback checks.
Provenance records both flags and PS2_SCALE. New IR refresh
required once, then compare word-only and all-scalar using the
same cache. Keep PS2_SCALE=1 and lean fixed-RTC settings.
Latest CI and broader fastmem performance are pending.

## Extended scalar fastmem A/B (08:12 and 08:13)

Both clean 172cb52, 40M dispatches, fixed RTC, lean, 1x, host audio
off. Same translation manifest SHA-256
7bd5b937d84a773b68783f03054a6883af178692497c7ffd61c9ef6e8b7d8543
and native library SHA-256
ea254f8e6e5e41d2f1c7b3a071518bbd29e80c6242cf5695265e6c57833c5812.
Both word enabled; scalar flag explicitly 0 then 1 in provenance
and corresponding enabled log markers.

Word only: first spawn 21635490, progress 21643264 at 21598.7ms
with 4261 fields; terminal 40630.4ms and 5458 fields.
1197 / 19.0317s = 62.8951 fields/s. Wall 45.484s, 3866 presents,
1592 skipped, 4655381 draws, 381059 native VU1 runs.
All scalar: first spawn 22938942, progress 22945792 at 22553.2ms
with 4606 fields; terminal 40299.6ms and 5718 fields.
1112 / 17.7464s = 62.6606 fields/s. Wall 44.969s, 4060 presents,
1658 skipped, 4451110 draws, 365924 native VU1 runs.
Both zero fallbacks, guest IOP faults and worker failures,
one VU1 job pending at stop, matching CPU/GPU digests within
each run. All five workflows passed 172cb52. Difference -0.37%;
no additional throughput gain established. Keep scalar optional.

3913de6 adds independent SDBZ_EE_FASTMEM_QUAD=1 for LQ/SQ
and LQC2/SQC2. Guarded 16-byte physical spans use native vector
loads/stores; traced stores keep original per-lane ordering;
FIFO, aliases, scratchpad, null/partial spans use original helpers.
Guest alignment/masking unchanged. New regression cases cover
both lanes, rounded address, partial span and traced/FIFO fallback.
New IR refresh required. Compare word+scalar with quad disabled
then enabled using identical cache at 1x. Final CI pending.
Large function compilation cost remains an open optimization
target; prefer compact guards and cold-path outlining before
expanding further or splitting functions.

## Quadword fastmem A/B and binary-size objective (08:25/08:26)

Both replays clean 7599bfc, word/scalar enabled, quad flag 0 then 1,
40M dispatches, 1x, lean, fixed RTC, live input and async VU1/GS.
Observed post-spawn throughput 63.45 versus 63.28 fields/s; wall
45.265 versus 42.922 s; fields 5472/6057, presents 3877/4322,
native VU1 executions 388620/332454. No fallback, IOP guest fault
or worker failure reported. All five workflows passed 7599bfc.
No further speed gain established; quad remains opt-in.

Performance work now explicitly targets native binary size as well
as throughput and compilation time. Recent user-reported O5 scalar
build: 16250 modules, 150.49 MB aggregate object files, 160.9 s,
4897 reused; largest compilation time 15.55 s for sub_0x369520.
Object bytes include relocation and symbol metadata and are not
the linked DLL size. Link tool already reports library bytes.

eb88399 folds known immediate ALU operands and LUI into literal
SSA values, and removes an alignment fault arm only for statically
aligned addresses while retaining the original register publication
boundary. Unknown and misaligned addresses keep existing checks.
Directed regression exercises LUI sign extension, wrapping,
literal effective addresses and precise misalignment faults.
0f53542 reports the ten largest validated native objects including
cache hits, without changing the LLVM optimization pipeline.
Both changes require current CI; the intentional lowering change
requires regeneration and a fixed-RTC lean P40 retail comparison.

Next: measure final DLL and total object bytes with matched inventory,
then target repeated cold paths, helper duplication, and selective
size-oriented pipelines for non-hot/large functions. Benchmark
against hot native modules before committing a size/speed tradeoff.
Do not equate smaller textual IR or fewer basic blocks with smaller
machine code. Preserve PS2 timing, observable state and exception
semantics; no blanket size optimization enabled yet.


## Read-only function ownership baseline

Use the existing shared IR and native object cache. The audit does not
recompile or relink the guest and does not modify dispatcher ownership.

On a configured Ninja Windows build:

```bat
cmake --build build --target ee_discovery_audit
python tools\audit_ee_function_overlap.py build\ps2-game-hle\ir --objects build\ps2-game-hle\O2-native\objects\objects.json --discovery-tool build\ee_discovery_audit.exe --json ee-ownership-audit.json
```


The metadata supplies the original ELF path; override with --elf if it
has moved. The object manifest locates the linked DLL automatically;
override with --library for another linked image. Supply any available
historical expansion text logs with repeated --log arguments. Without
logs, nonstatic inventory entries are deliberately not asserted to be
confirmed runtime misses. Static discovery provenance is reconstructed
from the validated EEIDXC02 discovery snapshot and original ELF using
the same decoder and address-taken predicate as the recompiler.

The audit identifies real emitted guest blocks from the IR entry
switch, including blocks hidden by append-time dispatcher ownership
filtering. Guest-PC coverage is the union of instruction annotations
and literal instruction accounting calls. Repeated annotation counts
remain separate. Containment compares observed instruction and
block-start sets, and shared terminal blocks are candidates rather
than proven mergeable regions. Aggregate object size, executable
section bytes, ELF file-backed executable segments and linked image
size are separate quantities; PE raw executable sections include file
alignment padding. Do not infer a byte saving from overlap alone.

No lowering inputs or runtime paths are changed. The diagnostic
executable and Python tools are outside the IR/object provenance
fingerprints. Build only the diagnostic target; no overwrite or
retail gameplay replay is needed.


Multi-block shared tails are additionally identified by matching
guest-PC membership and emitted guest-label successors, requiring
a closed connected region with a path to a terminal. This is
a pairwise candidate count, not a count of unique globally
mergeable regions. The JSON ranks these regions separately from
single shared terminal blocks.

The boot-session wrapper now preserves positive runtime-cache-miss
and speculative table-sibling observations in an ELF-SHA-256-bound
ee-discovery-events.json ledger beside the profile's boot logs.
It is diagnostic-only, published after a child execution, and
not consumed by the dispatcher or compiler. The audit loads it
automatically when using the corresponding object manifest. Use
--log for earlier archives whose events predate this ledger.


## Checkpoint 1 — full retail ownership baseline

User-supplied ee-ownership-audit.json, 16,252 modules, all with
guest entry switches. All five CI workflows passed bb13b46.
No retail replay was needed. Exact local Git HEAD and object
hashes were not embedded in the audit; size and inventory
measurements describe the supplied cache, not an independently
authenticated revision.

| Metric | Value |
|---|---:|
| ELF file bytes | 4,196,528 |
| File-backed executable ELF segment bytes | 4,195,968 |
| Aggregate IR bytes | 992,143,733 |
| Native object file bytes | 152,560,310 |
| Native object executable section bytes | 95,237,244 |
| Linked DLL bytes | 111,471,104 |
| Linked executable raw bytes | 101,812,736 |
| Unique observed guest PCs | 780,826 |
| Module-scoped guest PCs | 845,746 |
| Excess PC copies | 64,920 (7.676%) |
| Unique guest block starts | 160,826 |
| Module-scoped block starts | 176,718 |
| Excess block copies | 15,892 (8.993%) |
| Multiply emitted block starts | 3,549 |
| Overlapping modules | 662 |
| Strictly contained modules | 13 |
| Partially overlapping modules | 661 |
| Shared terminal candidates | 1,348 |
| Multi-block shared-tail pair regions | 2,567 |

Literal accounting calls recover 137,842 unique guest PCs absent
from instruction comments. Within-module excess annotations: 44,934.
Native object executable bytes per unique guest instruction: 121.97.

Modules with any cross-module overlap account for 9,067,049 executable
object bytes (9.52%), including their nonshared code, helpers and
overhead. Nonoverlapping modules account for 86,170,195 bytes (90.48%).
Even eliminating all code in overlapping modules (not a feasible
or safe transformation) cannot address the bulk of this object
executable footprint. No linked-DLL byte saving is established.
The 13 strictly contained modules account for 934,392 executable
object bytes; containment alone does not authorize their removal.

Static entries: 15,601. Nonexclusive source counts: ELF entry 1,
direct call 7,862, address-taken heuristic 8,066, static indirect
call 122. No STT_FUNC evidence in the report. 651 nonstatic entries
lack historical runtime-source evidence; 430 (66.05%) overlap.
Only 65 of 7,621 heuristic-only modules overlap (0.85%).
27 nonstatic entries are covered by earlier inventory modules;
the report cannot prove historical miss origin or time of coverage.
Nonstatic overlapping modules total 6,563,598 executable bytes.

Largest candidate: 0x193ac8 / 0x193ad0 share a 366-block,
1,305-PC closed tail candidate. PC 0x1938b8 appears in 25 modules.
The eight entries from 0x193ac8 through 0x193b78 inclusive total
947,128 executable object bytes. 0x299da0 / 0x29a728 also overlap
heavily. Largest individual module 0x369520 has 5,323 guest PCs,
857 blocks and 600,125 executable object bytes, zero overlap.

### Checkpoint 2 decision

For actual deduplication, prioritize targeted recognition of existing
internal entry points in the nonstatic expansion inventory, starting
with the 0x193xxx cluster and using shared-tail evidence to find
candidate owners. Prove legal resume entry, exact guest block and
delay-slot semantics, state publication, loop safepoints, exception
and interrupt behavior, and dispatcher ownership before aliasing
anything. Do not infer equivalence from PC sets. Where an entry is
not a legal existing resume block, investigate a shared-tail strategy
separately rather than silently dropping or redirecting it.

First implementation should be shadow-only candidate/certificate
generation with directed synthetic equivalence tests, followed by
opt-in routing only for proven existing block entries. No broad
heuristic pruning or global canonical-block redesign. Keep guest
code and dispatcher behavior unchanged until the next implementation
is explicitly undertaken and validated. A routing change requires
retail correctness replay before default activation.

For the larger binary-size objective, prioritize lowering/code
expansion reduction independently: nonoverlapping modules comprise
90.48% of executable object bytes; the largest module has zero
overlap. Audit cold paths, repeated helper definitions and per-entry
prologue/dispatch overhead; 10,647 modules with at most 32 guest
PCs account for 22,074,416 executable object bytes. Preserve accuracy
and measure size/throughput at unchanged inventory and optimization
profile. No blanket optimization profile change or size claim.


### Checkpoint 2a — shadow internal-entry comparison

Run against the exact IR inventory used by the ownership baseline:

```bat
python tools\audit_ee_entry_shadow.py build\ps2-game-hle\ir --baseline ee-ownership-audit.json --json ee-entry-shadow.json
```

This read-only tool uses the ownership baseline only to select nonstatic
overlap targets and potential overlapping owner modules. It checks the
inventory and aggregate byte count, then scans the actual IR for guest PC
coverage and switch targets. Instruction membership alone is not a legal
resume case. For exact nonzero cases, compare the emitted entry prologue,
all non-guest module definitions/declarations, and reachable LLVM text
including auxiliary branches, state stores, helper calls, exception and
loop exits, with bijective renaming of generated SSA temporaries and
auxiliary labels. Unrecognized constructs are rejected. Identical
observed emitted IR is intentionally only a shadow result, not proof
of native ABI compatibility, instruction-equivalent timing, or safe
module deletion. A candidate matching the requested entry is checked
against each source metadata-owned dispatch PC until the first failure.
Source/owner IR SHA-256 and metadata SHA-256 bind the evidence to inputs;
no object or dispatch metadata is rewritten. A matching result does not
prove that a module has no other direct native callers.

CTest ee_entry_shadow covers positive and negative synthetic comparisons,
different helper environment, prologue, exception return, state write
and branch target, and actual generated synthetic ELF IR. No production
lowering or runtime input is changed. Retail cache shadow results are
required before selecting a safe routing implementation. No gameplay
replay or recompilation is required to produce the report.


### Checkpoint 2b — isolated native coalescing experiment

Retail shadow report (metadata SHA-256 bf39f4727e00e6b213a62eebcde424f45830b8a6f10fc0cc257e2e0132b1826a):
430 nonstatic overlap modules, 36 covered entries, 13 matching at
the requested entry, 11 matching all published dispatch PCs. 49
matching pairs, 65 not legal resume blocks, 7 differing reachable IR.
The eight sources 193ad0, 193ad8, 193b00, 193b78, 193e00, 193e28,
193e88, 193ed0 directly match 193ac8 (921,560 executable object bytes).
Three further candidates at 33b880, 3e0cc8, 3e18e4 bring the total
to 928,664 executable object bytes, not a measured DLL saving.

The isolated experiment requires current metadata and object digests,
recomputes matching emitted IR for each published PC, forbids static
or boot-entry removal and transitive owner chains, and verifies the
exact published PC inventory. It uses an independent metadata/object
manifest and links to a new directory; the normal cache is untouched.
Original owner vaddr and native pointer identity change for transferred
PCs. This is explicitly not an enabled production optimization.
Do not run it with native ALU certificates or auto-expansion. Actual
native link and runtime correctness/performance validation are required.
Use a new output directory each time. If a link fails, retain the
normal cache and inspect the independent experiment directory.

```bat
set "SDBZ_NATIVE_ALU_CERTIFICATES=0"
python tools\build_ee_dedup_experiment.py build\ps2-game-hle\ir build\ps2-game-hle\O2-native\objects ee-entry-shadow.json build\ee-dedup-experiment --link
```


### Checkpoint 2b — measured retail link

User-supplied dedup-plan.json confirms 16,252 -> 16,241 modules
and exactly 160,826 dispatch PCs. Excluded 1,434,549 object-file
bytes and 928,664 executable object bytes. Linked DLL 110,445,056
bytes vs baseline 111,471,104 (-1,026,048, -0.92%); executable raw
sections 100,879,872 vs 101,812,736 (-932,864). Dedup DLL SHA-256
4ab4664517a8ffa4adc3f767777c440783fee9ef4191f9f219354543e5a8f0a8.
No claim of runtime equivalence yet.

### Checkpoint 2c — matched runtime replay

Use tools/replay_ee_dedup_experiment.py to run the existing native
runner directly with each independently verified DLL. This avoids
the production translation/cache manager and disables cache expansion.
The baseline records real PAD0 polls, and the second run must consume
the complete recording in order. Both use the same fixed RTC, disc,
memory card (discard guest writes), runner binary and dispatch budget.
Core P40 settings are applied; caller-specific optional runtime
flags are retained and recorded, and AOT capture is disabled.
A mismatched poll stream or incomplete run is a diagnostic result,
not evidence of equivalence. Upload the generated zip even on failure.
Native ALU certificates remain disabled; owner identity changes.
No production cache files should be replaced.
