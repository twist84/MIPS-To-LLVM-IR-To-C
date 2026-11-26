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


### Checkpoint 2c — initial async retail replay

Baseline finished 40,000,000 dispatches (exit 4), 43.938 s process
wall, 39,051.7 ms runner elapsed and 5,123 guest fields.
Candidate reached the same dispatch budget in stdout scheduler
totals, 42.844 s process wall, but returned 1 because PAD0 replay
consumed 3,542 of 3,544 recorded polls. Its JSON is an in_progress
snapshot at 39,358,862 dispatches, not a terminal report. Last
candidate progress 39,993,344 / 39,018.2 ms / 5,120 fields.
First sampled common transition difference at dispatch 11,426,816;
first spawns 24,243,296 and 24,162,759. Both observed zero VU1
fallbacks/worker failures and no IOP guest fault in available reports.
The baseline trace samples a transferred entry 0x193b00 at 16,994,304;
no transferred PC sampled in candidate, which is not evidence of zero
executions. No actual dedup regression or speed gain established;
both library hashes unchanged. The input recording alone does not
guarantee deterministic device scheduling.

Replay wrapper now supports --control (A/A before A/B) and --sync
(disable async VU1/GS for every arm), compares sampled dispatch
transitions and requires terminal dispatch-budget snapshots/exit 4,
not merely report existence. An A/A failure prevents claiming a
matched A/B result, but the candidate is still run for diagnosis.
Synchronous replay is an accuracy experiment, not the P40
throughput benchmark. No production changes or cache invalidation.


### Checkpoint 2c — synchronous A/A/B acceptance

Retail ee-dedup-comparison-sync.zip: all three runs stopped at
40,000,000 dispatches, exit 4, 3,217/3,217 PAD0 polls consumed
on both replays. 209,808 common sampled transitions with no
differences or unpaired samples on either comparison. The complete
terminal JSONs are byte-identical, SHA-256
9adccfd5617ec2ef633fb89d4eb385600840f73729f756773ba040daced173f5.
GPU PPMs are byte-identical, SHA-256
ccad7fe1da5af5bbe9d03aa590f38bc5c58d5bd9f0c359516e62d7d8ae0ff079.
All arms reached 4,582 fields, 4,643,431 GPU draws, 3,297 presents,
22,547,591,829 reference EE cycles, 22,217,865,368 guest instruction
attempts, 377,264 native VU1 runs and no fallback/IOP guest fault.
A redirected PC, 0x193e28, occurs at dispatch 9,956,352 in all arms.
No claim of execution coverage of every redirected PC. Wall seconds
42.469 / 41.406 / 41.515; no demonstrated throughput improvement
or regression. This validates the 1,026,048-byte DLL saving for
the synchronous retail workload. Keep the experiment opt-in;
do not replace the normal cache or enable broad discovery pruning.

### Checkpoint 3a — size-oriented native pipeline pilot

The bulk of native expansion remains in nonoverlapping modules.
Test the effect of full middle-end O2, Os and Oz against the
existing selective O5 profile with the same native O2 backend,
CPU and features. A local LLVM 20.1.8 Linux pilot on 12 earlier
hot IR modules measured 109,104 executable object bytes with
selective O5 policy, 106,776 with Os and 103,101 with Oz (-5.50%);
some modules grew. This is not the Windows retail cache or a
linked image, and no runtime conclusion follows.

tools/benchmark_ee_native_size.py verifies the current O5 target
and selected IR/object digests, samples the current inventory by
guest-PC count and native code size, includes the largest modules,
re-emits each selected module in four independent pipelines and
reports actual executable/object bytes, times and exact O5 object
reproduction. No objects or IR are written. CTest emits four
native objects from a synthetic guest function. Do not adopt a
global size profile or extrapolate a selected sample to the
whole image. Use the resulting size/runtime tradeoffs to choose
a selective experimental profile and validate it independently.


### LLVM 22 size-pilot compatibility

LLVM 22 removed pipeline-level Os/Oz (llvmlite 0.48+). Use the
O2 pipeline with verified function attributes: optsize for Os,
optsize+minsize for Oz. LLVM 20 retains its original size pipeline.
The report records the effective size optimization method per variant;
these policies are not identical across LLVM major versions. The
in-memory IR copy is annotated, never the original file. Tests cover
attribute placement and helper/declaration handling even on LLVM 20.
Production O5, target settings, cache and runtime are unchanged.


### Checkpoint 3a — Windows retail native size results

LLVM 22.1.0 / znver2: all 55/55 O5 objects reproduced exactly.
Executable bytes: O5 2,580,264; O2 2,852,750 (+10.56%);
Os 2,666,544 (+3.34%); Oz 2,567,056 (-0.51%). Oz object file
bytes 3,076,237 vs 3,468,711 (-11.31%). Oz wins 38 of 55,
O5 wins 17; the per-module minimum is 2,517,347 (-2.44%).
For 24 modules with at most 32 guest PCs, all shrink under Oz:
67,686 -> 51,021 executable bytes (-24.62%). This is a selected
sample, not a whole-image savings estimate. Larger bands are mixed.

### Checkpoint 3b — isolated selective Oz build

Try Oz only for modules with 1–32 guest PCs; require both native
executable bytes and total object bytes to decrease. Keep all other
O5 objects unchanged. Verify source IR and original object hashes,
pilot manifest identity and host target; preserve byte-identical
metadata, original symbol names and ownership. Link a fresh experiment
with the explicit mixed-pipeline manifest, never publish into the
normal cache. No deduplication is combined with this experiment.
Runtime acceptance requires independent synchronous matched replay.


### Checkpoint 3b — measured Windows selective Oz link

Retail size-plan.json, LLVM 22.1.0 / znver2, original manifest SHA-256
`ee903728352c1d28a83211826fbfe5c735d0a027e18b194f6db10b4abdcc1c5c`.
10,571/10,647 replacements accepted; 76 retained O5. All 16,252
function modules retained. Selected executable bytes 22,074,416 ->
17,125,674 (-4,948,742, -22.42%); total object bytes saved 13,751,818.
By guest-PC band: 1–4, 3,567/3,567 accepted, 1,588,663 executable
bytes saved (-46.44%); 5–16, 3,031/3,033, 1,472,633 (-27.42%);
17–32, 3,973/4,047, 1,887,446 (-14.21%). Trial wall time 73.625 s
(excludes linking and other setup). DLL 111,471,104 -> 104,656,384
bytes (-6,814,720, -6.11%, 99.81 MiB); linked executable raw sections
101,812,736 -> 96,803,840 (-5,008,896, -4.92%). Linked DLL SHA-256
`f6aef76a2e6d9c9dd4e5b95459b181e7dd28a6aee55aaf8f68c461089dad5cf2`.
The size experiment does not include the earlier owner coalescing.
All five CI workflows on d8262a5 passed. Runtime validation is still
required; use the existing synchronous A/A/B replay wrapper with
`--experiment build/ee-size-experiment --control --sync`. Compare
terminal reports, GPU snapshots, PAD0 replay, sampled transitions,
wall time and guest counters. Do not promote to production based on
size alone. No new IR generation, object emission or relink needed.


### Checkpoint 3c — synchronous selective Oz A/A/B acceptance

Retail ee-size-comparison-sync.zip: all three exit 4 at 40M
dispatches; 3,302/3,302 PAD0 polls, 255,866 identical sampled
transitions per replay, no unpaired samples. All terminal JSONs
SHA-256 a2802537937575b6dfaba798914313f2c283694d960fb0a008abadc08d2651e5
and all GPU PPMs SHA-256
663b40d2b4bb2b382d12ac28f1c29a89378ff50c3392d1486994c3f667f6d76d
match byte-for-byte. All arms: 23,035,978,613 reference EE cycles,
22,527,534,836 guest instruction attempts, 4,681 fields,
4,578,541 GPU draws, 3,390 presents, 380,083 native VU1 runs,
zero VU1 interpreter fallbacks, final PC 0x185cb0.
Wall seconds 43.093 / 42.282 / 41.156; runner elapsed seconds
39.022 / 38.481 / 37.5407. Candidate wall -2.66% and runner
elapsed -2.44% versus control. Single synchronous workload,
runtime profiling disabled; no general throughput claim.
Small-function policy validated on this workload, not all modules.

### Checkpoint 3d — extend the isolated selection to 128 guest PCs

The 33–128 band has 4,114 modules and 29,317,682 O5 executable
object bytes. Reuse the previous <=32 size-plan and its accepted
objects only after verifying original manifest/metadata identity,
complete prior trial selection, size gates and object digests.
Compile only the additional 4,114 candidates; link a separate
image preserving the previous selection and metadata. Retain O5
when either size metric fails. Report incremental as well as
cumulative savings. This reuse is not a runtime certificate;
the extended image needs its own replay. No production change.


### Checkpoint 3d — measured Windows medium-function selective Oz link

Retail size-plan(1).json, same source manifest and metadata as the
<=32-PC experiment. Reused all 10,647 prior trials and 10,571 accepted
objects. Of 4,114 new trials, 2,997 accepted, 1,117 retained O5.
New executable object bytes saved 1,521,365 / 29,317,682 (5.19%);
new total object bytes saved 7,028,283. Cumulative replacements
13,568 / 14,761 trials, all 16,252 function modules retained.
Cumulative executable object bytes saved 6,470,107; total object
bytes saved 20,780,101. Experiment stage wall time 99.453 s.
DLL 101,750,272 bytes (97.04 MiB), down 9,720,832 bytes (8.72%)
from original O5 and 2,906,112 bytes from the <=32-PC image.
Linked raw executable sections 95,263,744 bytes (90.85 MiB),
down 6,548,992 bytes (6.43%) from original O5. DLL SHA-256
a053549e424bc3d9c0beead7a9341fa68fba5d743c6c7828ad9823e694f2e126.
The marginal code savings are lower than the <=32-PC band.
No owner coalescing or production cache changes. Runtime acceptance
requires another matched synchronous A/A/B replay of the new image.


### Checkpoint 3e — synchronous medium-function selective Oz acceptance

Retail ee-size-comparison-128-sync.zip: baseline, control and candidate
all stopped at 40,000,000 dispatches with exit 4; both replays consumed
2,940/2,940 PAD0 polls, 176,276 matching sampled transitions, zero
differences or unpaired samples. Libraries unchanged, no replay error.
All terminal JSONs SHA-256
3c87f908b34d09a85ee25f9d4d4c2da404dfd45595c282a0087e0bbe3c919af6
and all GPU PPMs SHA-256
f683fb5289e4978a01e6eb1a9d19285437e1780f253afae21e624412ca8ad47b
are byte-identical. Candidate DLL SHA-256 matches the 3d link receipt.
All arms: 20,665,443,624 reference EE cycles; 20,479,515,230
guest instruction attempts; 20,481,365,577 reference slots;
4,199 fields; 4,966,017 GPU draws; 3,024 presents; 412,650
native VU1 runs; 464,411,184 native pairs; zero interpreter
fallbacks; final PC 0x2ff580. Wall seconds 42.907 / 42.750 /
42.640, candidate -0.26% versus control. No measured performance
regression on this synchronous workload; not a general speed claim.
The <=128-PC experiment remains isolated and not a production cache.

### Checkpoint 3f — complete the selective Oz size inventory

Add explicit --all-modules selection to build_ee_size_experiment.py.
Resolve the threshold from the maximum guest-PC count in the current
audit. Reuse verified <=128-PC trials and accepted objects, including
absolute paths to objects from earlier size experiments. Only the
remaining 1,491 large modules need new trials, assuming the same
16,252-module inventory. Continue using the two-metric size gate.
Measure actual linked size before requesting another runtime replay.
Do not change the normal O5 compiler profile or combine owner coalescing.
Larger modules have mixed size results; the goal is measurement, not
a prediction that Oz will improve runtime or code size for every module.


### Checkpoint 3f — measured full-inventory selective Oz link

Windows retail size-plan(2).json, LLVM 22.1.0 / znver2, same source
manifest and metadata as the <=128-PC experiment. Reused 14,761 trials
and 13,568 objects. All 16,252 modules evaluated, 14,014 accepted,
2,238 retain O5. All function modules and published PCs retained.
The remaining 1,491 trials accepted 446 replacements, saving
395,721 executable bytes and 2,337,035 total object bytes.
Stage wall time 180.954 s (not including link).

| Guest PCs | Accepted / tried | Original executable bytes | Executable bytes saved |
| --- | ---: | ---: | ---: |
| 1–4 | 3567/3567 | 3420534 | 1588663 |
| 5–16 | 3031/3033 | 5369953 | 1472633 |
| 17–32 | 3973/4047 | 13283929 | 1887446 |
| 33–128 | 2997/4114 | 29317682 | 1521365 |
| 129–512 | 417/1316 | 28955963 | 318646 |
| 513+ | 29/175 | 14889183 | 77075 |
| Total | 14014/16252 | 95237244 | 6865828 |

Executable object bytes 95,237,244 -> 88,371,416 (-7.21%);
aggregate object bytes 152,560,310 -> 129,443,174 (-15.15%).
Linked DLL 111,471,104 -> 100,830,208 (-9.55%, 96.16 MiB).
Linked executable raw sections 101,812,736 -> 94,864,896 (-6.82%).
DLL SHA-256
96ee5bc10813db53dfab2d7fdb9104e31ec736ebad84c2a42e71b2e82c4812e2.
All five CI workflows passed on 49eed3e. The additional DLL saving
versus the <=128-PC experiment is 920,064 bytes, so the marginal
benefit of the large-module size trials is modest. The final
full-inventory DLL still requires independent synchronous matched
replay. Production cache and discovery remain unchanged; no owner
coalescing is included.

Next checkpoint: after runtime validation, prioritize lowering-level
code expansion and repeated helper inventory rather than further
blanket optimization level sweeps. Measure actual emitted helper
bodies and call sites before outlining; do not assume that identical
helper names imply equivalent definitions, or that outlined helpers
improve hot-path throughput.


### Checkpoint 3g — full-inventory selective Oz replay accepted

Retail ee-size-comparison-all-sync.zip: all three stopped at 40M
dispatches (exit 4), both replays consumed 3,123/3,123 PAD0 polls,
186,029 matching sampled transitions per replay, zero mismatches
or unpaired samples. Libraries unchanged, no replay error.
Terminal JSON SHA-256 e88e0729b86050537166fd109dfd84e509cf49dbe5877427f3cbcb1e50c3d24d;
GPU PPM SHA-256 20d5296384f2151610280f2c35e7f1804a4974902f91fbc3fca5f12dbd34db27.
Both byte-identical across all three arms. Reference EE cycles
21,885,929,018; guest instruction attempts 21,674,108,653;
reference slots 21,676,097,062; fields 4,447; GPU draws 4,913,924;
presents 3,211; native VU1 runs 407,157, pairs 459,420,098,
zero interpreter fallbacks; final PC 0x19e620. Wall time
43.157 / 43.031 / 42.812 seconds (-0.51% candidate/control).
No observed regression in this matched workload. Not an exhaustive
test or evidence to change production compiler/cache policy.

### Checkpoint 4a — helper inventory before lowering changes

Audit exact internal IR definition bodies and static direct guest/helper
call sites. Compare current O5 objects against the full selective Oz
overlay, checking per-module IR/object digests and trial provenance.
Attribute x86-64 COFF/bigobj executable spans to exact named function
symbols using the next symbol boundary or section end; exclude ambiguous
aliases. Spans may include padding and omit inlined or renamed functions.
Report exact-body groups and unattributed bytes separately; these are
not estimates of removable code. Prioritize cold, repeatedly emitted
helper dependency families after evidence arrives. No runtime changes,
IR regeneration, relinking or replay required for this read-only audit.

### Checkpoint 4a — measured helper inventory

All 16,252 modules, 115 distinct exact helper bodies. O5 named helper
spans 17,136,845 of 95,237,244 executable bytes; mixed 8,226,064
of 88,371,416. Guest named spans increase 78,100,399 to 80,145,352;
this is attribution, not proof of instruction-level growth. Cold
publication/observer/trace family: O5 5,458,432, mixed 3,829,399
named span bytes, 50,722,492 repeated IR body bytes. Each definition
is identical across the full inventory. Publication alone is still
16,252 named copies (3,292,279 mixed span bytes). Prioritize this
family before changing hot counters or quadword paths.

### Checkpoint 4b — isolated shared observation experiment

Retain exact three helper bodies in one deterministic owner module,
rename the publisher to a shared external definition and replace
other publishers with an ABI-identical declaration. The two leaf
helpers remain internal to the owner. Verify all three original body
hashes, direct dependency closure and symbol uses in every module.
Keep original guest metadata, per-module O5/Oz policy, counter gates,
callback ordering and all original cache files. Re-emit into isolated
objects; receipt records original and transformed IR hashes. This
experiment intentionally does not apply a per-module size gate because
all callers must use the same shared definition. The actual link
and independent replay determine acceptance. Cross-module synthetic
MCJIT test verifies publication order and rejects changed/escaping
helpers. No production lowering changes.


### Checkpoint 4b — measured shared observation native link

Windows helper-plan.json, same metadata and full selective Oz
source manifest as the helper inventory. All 16,252 original modules
re-emitted, with one shared publisher owner (sub_0x100008_0x100008.ll).
Original source files unchanged; 48,882,996 bytes of IR text removed
in memory. Stage wall time 269.781 s. Exactly 16,251 modules shrink,
with the owner growing by five executable bytes. All 2,238 O5
modules save 751,968 executable bytes; 14,013/14,014 Oz modules
shrink, net saving 2,971,151 executable bytes for the Oz group.
Total executable object bytes 88,371,416 -> 84,648,297
(-3,723,119, -4.21%); aggregate object bytes 129,443,174 ->
124,402,583 (-5,040,591). This is a net code saving, not a
direct measurement of the exact remaining helper bodies.
Linked DLL 100,830,208 -> 96,549,888 bytes (92.08 MiB,
-4,280,320, -4.25%); linked executable raw bytes 94,864,896 ->
91,141,120. Original O5 DLL reduction 14,921,216 bytes (13.39%).
DLL SHA-256 a6ca18fc5c16b60976da94b6c39b6acaa9fe3aea74dc41da4cc8dd5bf81fe9b5.
All five CI workflows on a368d39 passed. Retail acceptance still
requires independent synchronous matched replay. Observation-enabled
behavior is covered by the synthetic cross-module test, but retail
dispatch replay alone does not establish exhaustive observation or
callback correctness. Production policy unchanged.


### Checkpoint 4c — shared observation retail acceptance

ee-helper-comparison-sync.zip: all three runs exit 4 with 40M
dispatches, both replays consume 3,018/3,018 controller polls,
191,606 common sampled transitions each, zero differences or
unpaired samples. No error, unchanged libraries. Candidate hash
matches 4b link receipt. Byte-identical terminal JSON SHA-256
caaead2afaf90f003a14888fa651a3eb049e5029bcf7d83e02f476bf0e974c22
and GPU PPM SHA-256
45f163039b259060340900df33859eb1459d66b064f6b4b5a77a86a7455f517b.
All arms: 21,359,291,879 EE cycles; 21,126,347,677 guest instruction
attempts; 21,128,382,756 reference slots; 4,340 fields; 4,816,523
draws; 3,111 presents; 398,539 native VU1 runs / 447,493,790 pairs,
zero fallbacks; final PC 0x10aff8. Wall seconds 42.812/42.750/42.500.
Candidate -0.58% versus O5 control, within measurement noise.
Validated on this synchronous workload only.

### Checkpoint 4d — isolate the reached-instruction counter

Extend the existing experiment with --share-counter, taking the
previous observation experiment as its source. Reconstruct its
transformed IR from the original files, validate the prior per-module
transformed IR and object digests, exact original counter body,
dependencies, ABI and direct symbol uses. Retain the counter's exact
body in the existing shared publisher owner and declare a single
external noinline counter in all others. Preserve the null guard,
attempt/reference counters and observation gate. Leave ALU batching,
annulled slots and all other helpers unchanged. Retain the previous
O5/Oz policy and original guest metadata. Re-emit into a fresh
isolated output. Synthetic MCJIT coverage exercises null, observed
and quiet paths and rejects changed or escaping counters. This is
a hot helper: size savings alone cannot justify promotion. Compare
retail runtime against the previous shared-observation image as
well as O5 before considering production policy.


### Checkpoint 4d — measured shared counter link

Windows helper-plan(1).json, same 16,252 modules and native target.
Prior observation library SHA-256 matches checkpoint 4b. Incremental
IR text reduction 11,809,640 bytes; stage wall 256.672 seconds.
Executable object bytes 84,648,297 -> 83,418,296 (-1,230,001);
aggregate object bytes 124,402,583 -> 121,516,151 (-2,886,432).
O5: 2,238/2,238 shrink, 179,040 executable bytes saved; Oz:
14,013/14,014 shrink, 1,050,961 bytes saved. One owner unchanged
in executable size, none grow. DLL 96,549,888 -> 94,994,432
(-1,555,456 bytes, 90.59 MiB); raw linked executable bytes
91,141,120 -> 89,910,784. Cumulative DLL saving versus O5
16,476,672 bytes (14.78%). Candidate DLL SHA-256
158753088be4d03da0845a6ce6b4d1f8d095a350af7b079d63445652e0bcfc48.
All five CI workflows passed on 86aa7fe. Runtime still unvalidated.

### Checkpoint 4e — matched incremental counter comparison

Extend replay wrapper with optional --prior-experiment for the
verified immediate shared-observation parent of a counter candidate.
Verify prior library hash, size, metadata, native target and source
object manifest hash against both receipts. Require --control --sync.
Record on O5, replay on O5 control, prior and new candidate with
identical input. Require matched PAD0, terminal dispatch budgets and
sampled transitions on all three replay arms; check all library hashes
again. Compare terminal reports and screenshots offline, and compare
candidate timing directly against the prior image. One run per image
cannot establish a statistically significant throughput change.
No native rebuild or cache modifications.


### Checkpoint 4e — shared reached-instruction counter replay accepted

ee-counter-comparison-sync.zip: O5 baseline/control, shared observation
and shared counter all exit 4 at 40M dispatches. Each replay consumes
2,982/2,982 PAD0 polls, 190,938 matching sampled transitions, zero
mismatches or unpaired samples. All library hashes unchanged, no error.
All four terminal JSON SHA-256
2519c77284829b3abbde064ef8701d747f0bc60789aef7f08f1a2d26e07e78c1
and GPU PPM SHA-256
6fe4bd5cb09b443cf239d42c3ca13bb6180c95d724411968d95daaaaba30391c
match byte-for-byte. 21,025,084,821 EE cycles; 20,801,549,032 attempts;
20,803,529,723 reference slots; 2,831 scheduler yields; 4,272 fields;
4,685,750 GPU draws; 3,071 presents; 393,671 native VU1 runs /
434,435,124 pairs; zero fallbacks; final PC 0x1a0aa8.
Wall seconds 42.015/42.109/41.860/41.938, candidate +0.19% relative
to immediate parent and -0.41% relative to O5 control. Within run
noise; synchronous workload only. The 90.59 MiB candidate is
accepted for this workload; production remains unchanged.

### Checkpoint 4f — shared quadword slow-path experiment

The original inventory contains 6,947 identical ee_store_quadword
definitions (20,070 static direct guest calls), with 2,150 named
copies and 1,056,789 attributed executable span bytes in the Oz mix.
Share only this helper, retaining the existing store trace guard
body and exact external memory ABI. Keep FIFO detection, two trace
calls, and FIFO/split-scalar write ordering. Reconstruct previous
observation/counter transforms for the affected modules and verify
their prior transformed IR hashes. Verify all original source and
parent object hashes; reuse the 9,305 unaffected objects directly.
Choose a deterministic owner among modules defining the helper.
The old alwaysinline helper becomes an external noinline definition
or declaration; this may increase costs at previously inlined
call sites, so actual size and matched runtime evidence are required.
Test cross-module FIFO/scalar ordering, data and dependency rejection.
Do not change normal lowering, cache policy or dispatch metadata.


### Checkpoint 4f — measured shared quadword native link

Windows helper-plan(2).json, LLVM 22.1.0 / znver2, same 16,252
guest modules and metadata. Immediate parent library SHA-256 matches
checkpoint 4d. Re-emitted 6,947 modules, reused 9,305. Quadword
and observation/counter owner is sub_0x100008_0x100008.ll.
Stage wall time 207.640 s, excluding link. Removed 9,049,074
bytes of IR text in memory. Of affected modules, 6,945 shrink
and two grow in executable size. O5: 2,150/2,150 shrink, saving
1,066,834 executable bytes; Oz: 4,795/4,797 shrink, net saving
2,487,674 executable bytes. Total incremental executable object
saving 3,554,508 bytes and aggregate object saving 4,732,324 bytes.
Executable object bytes 83,418,296 -> 79,863,788; aggregate
object bytes 121,516,151 -> 116,783,827.
DLL 94,994,432 -> 91,351,040 bytes (87.12 MiB), saving
3,643,392 bytes (3.84%) versus immediate parent and 20,120,064
bytes (18.05%) versus original O5. Linked raw executable bytes
89,910,784 -> 86,354,432. DLL SHA-256
74559ead7c638172176c08aa6440ba4ec5f7e7ccdc60b8f56b833f2b28a39b3e.
Net executable savings exceed the old named helper footprint;
the changed inlining and code generation contribute to the
difference, but this is not a direct measurement of inlined bytes.
All five CI workflows passed on 9d804e9. Runtime remains unvalidated.

### Checkpoint 4g — matched incremental quadword comparison

Use the existing four-arm synchronous replay with original O5
record/control, shared-counter immediate parent, and shared-quadword
candidate. Verify parent provenance and all library hashes, matched
input, exact terminal budget, sampled transitions, terminal report
and GPU screenshot. Compare candidate runtime against the shared
counter parent. The previous alwaysinline policy was changed, so
acceptance requires runtime evidence as well as size savings.
Production policy and original caches remain unchanged.


### Checkpoint 4g — shared quadword replay accepted

ee-quadword-comparison-sync.zip: all four runs exit 4 at 40M
dispatches, each replay consumes 3,196/3,196 PAD0 polls with
192,752 identical sampled transitions, zero differences/unpaired.
No error, all DLL hashes unchanged and match the link receipts.
Terminal report SHA-256
902606cadfc619ed21ac2db91412c2a572a9ab4692cfbc93104b74ad6d1a90bb
and GPU screenshot SHA-256
b24f1bab576e738260f77445c762699f1a317f7b717b230b77deadcd4461242c
match byte-for-byte in all four arms. EE cycles 22,565,867,672;
attempts 22,342,246,283; reference slots 22,344,315,309; scheduler
yields 3,047; fields 4,586; GPU draws 4,924,226; presents 3,285;
VU1 native runs 402,205 / 455,160,950 pairs, zero interpreter
fallbacks; final PC 0x1c3c68. Wall seconds 44.266/43.985/43.859/
43.734; candidate -0.29% vs parent and -0.57% vs O5 control.
Within measurement noise; one synchronous workload only.
The 87.12 MiB DLL is accepted for this workload. All five CI
workflows passed on d4823a5. Production remains unchanged.

### Checkpoint 4h — COP1 multiplication family experiment

The original inventory contains 1,134 identical definitions of
each of mul_bits, mul_correct, booth_row and mul_csa, with total
799,502 named mixed executable span bytes. mul_bits has 8,265
direct guest and 694 helper calls; mul_correct is also used by VU0
helpers. Share both roots with exact scalar/aggregate signatures,
keeping the two leaf definitions internal to the selected owner.
Validate the original audited body hashes, direct dependencies and
all symbol references; reject leaf escapes. Keep operand order,
correction gating, rounding/truncation and result flags unchanged.
Reconstruct the observation/counter/quadword transformations for
affected modules and verify the exact parent transformed IR hash.
Verify all parent objects against receipts, using the earlier
counter receipt for the 9,305 modules not rewritten by quadword
sharing. Recompile only 1,134 affected modules, retain the other
15,118 objects by absolute path. Synthetic MCJIT differential uses
the actual generated multiplier header, edge values and 10,000
deterministic random pairs, comparing all result bits and flags.
A separate native link and matched retail replay remain required.


### Checkpoint 4h — measured shared COP1 multiplication native link

Windows helper-plan(3).json, LLVM 22.1.0 / znver2, same metadata
and 16,252 guest modules. Parent library SHA-256 matches checkpoint
4f. Re-emitted 1,134 modules, reused 15,118. Shared multiplier owner
sub_0x101490_0x101490.ll. Stage wall time 106.782 s, excluding link.
Removed 7,814,240 bytes of IR text in memory. 1,130 affected modules
shrink in executable size, four unchanged and none grow.
O5: 586/587 shrink, saving 445,073 executable bytes; Oz:
544/547 shrink, saving 382,518 executable bytes. Incremental
executable object saving 827,591 bytes and aggregate object saving
1,107,018 bytes. Executable object bytes 79,863,788 -> 79,036,197;
aggregate object bytes 116,783,827 -> 115,676,809.
DLL 91,351,040 -> 90,495,488 bytes (86.30 MiB), saving 855,552
bytes versus the immediate parent and 20,975,616 bytes (18.82%)
versus original O5. Linked executable raw bytes 86,354,432 ->
85,526,528. DLL SHA-256
5442d6c4ab264c125bc1132209ac347424f57f1b9c28dbf89a0680b51f605593.
Retail runtime subsequently accepted in checkpoint 4i.

### Checkpoint 4i — matched incremental multiplication comparison

Use the existing four-arm synchronous replay: original O5 record
and control, shared-quadword immediate parent, shared-COP1-multiply
candidate. Require matching controller input, terminal budgets,
sampled transitions, terminal reports and screenshots, and unchanged
library hashes. Compare candidate runtime with the immediate parent.
The shared multiplication roots use the original aggregate/scalar
ABI but changed inlining/linkage. A successful native link alone
does not prove cross-module execution equivalence. Production
lowering and cache policy remain unchanged.

### Checkpoint 4i — shared COP1 multiplication replay accepted

ee-cop1-mul-comparison-sync.zip: all four arms stop at 40M dispatches
with exit 4. All three replays consume 2,887/2,887 PAD0 polls;
193,289 common sampled transitions each, zero differences or unpaired
samples. No replay error; library hashes unchanged and parent/candidate
hashes match checkpoints 4f/4h. Terminal JSON SHA-256
aac20dad151a2b340aff000c78d4b2318849515dcc315116021de155752feb8d
and GPU PPM SHA-256
f445bbffb38171c0497539b48bf7f4daa74518440baced07b7c623423a29adcf
are byte-identical across all four arms. EE cycles 20,388,302,085;
attempts 20,149,002,811; reference slots 20,150,984,826; scheduler
yields 2,738; fields 4,143; GPU draws 4,880,260; presents 2,966;
VU1 native runs 412,371 / 460,273,899 pairs, zero interpreter
fallbacks; no IOP guest fault or timing observer failure. Wall seconds
44.282/43.641/43.687/43.406; candidate -0.64% versus immediate
parent and -0.54% versus O5 control, within measurement noise.
All five CI workflows passed on 8a8159d. The 90,495,488-byte
(86.30 MiB) candidate is accepted for this synchronous workload.
Production lowering and cache policy remain unchanged.

Use exact bytes and identify the comparison metric: original O5 DLL
111,471,104; immediate quadword parent DLL 91,351,040; candidate DLL
90,495,488. Incremental DLL saving 855,552 bytes (0.94% of parent),
cumulative saving 20,975,616 bytes (18.82% of O5). The 827,591-byte
executable-object saving and 799,502-byte original mixed named helper
span are different metrics; the latter is not a current marginal
saving prediction. Do not add independent ownership-coalescing savings.

### Checkpoint 4j — exact COP1 addition experiment

The immediate parent is the validated 90,495,488-byte (86.30 MiB)
shared multiplier DLL, not original O5 or the earlier quadword DLL.
The original mixed add-helper span is an attribution estimate, not
an incremental size baseline. Share only ee_cop1_add_bits, preserving
its exact aggregate ABI, signed-zero, guard-bit alignment, range
flags and llvm.ctlz.i32 dependency. Leave MAC and VU0 FMAC bodies
unchanged; redirect their calls without altering operands or flags.
Verify the counter/quadword/multiplier receipt and manifest chain,
reconstruct earlier transforms for selected modules, require their
parent IR digests, and retain unaffected verified objects by absolute
path. Select one deterministic addition owner. Test real emitted
addition, COP1 MAC and VU0 FMAC bodies with cross-module ownership,
edge operands, both latch states and deterministic random inputs.
Measure incremental executable/object/DLL size against the multiplier
parent, then independently run the four-arm synchronous replay.
No production lowering or cache policy changes.

### Checkpoint 4j — measured shared COP1 addition native link

Windows helper-plan(4).json, LLVM 22.1.0 / znver2, same metadata
and all 16,252 guest modules. Immediate parent library SHA-256
matches checkpoint 4h. Re-emitted 1,499 modules, reused 14,753.
Addition owner sub_0x100008_0x100008.ll; multiplier owner unchanged.
Stage wall time 142.062 s, excluding link. Removed 4,029,865 bytes
of IR text in memory. Recomputed all per-module totals: 1,478 shrink
in executable size, 19 unchanged, two grow. O5: 692/692 shrink,
saving 189,635 executable bytes; Oz: 786/807 shrink, net saving
195,869 executable bytes. Incremental executable object saving
385,504 bytes and aggregate object saving 398,956 bytes.
Executable object bytes 79,036,197 -> 78,650,693; aggregate object
bytes 115,676,809 -> 115,277,853. Linked raw executable bytes
85,526,528 -> 85,140,480.
DLL 90,495,488 -> 90,095,104 bytes (85.92 MiB), saving 400,384
bytes (0.44%) versus the immediate multiplier parent and 21,376,000
bytes (19.18%) versus original O5. Candidate DLL SHA-256
0ba02590afb4806fc979a3b6fdc113566f5be41f0d02036f192f71b5a378fbed.
The receipt confirms the native link, not runtime equivalence.
Four CI workflows passed on 3785324 at review; Build and test
was still running. Production remains unchanged.

### Checkpoint 4j — matched incremental addition comparison

Run the existing four-arm synchronous comparison with original O5
record/control, shared multiplication immediate parent and shared
addition candidate. Require matching controller consumption, terminal
budgets, sampled transitions, terminal reports and GPU screenshots;
verify library hashes before and after. Compare timings directly with
the multiplier parent. Keep the 86.30 MiB image as the validated
baseline until acceptance. After this replay, investigate store and
V0 trace guards while keeping disabled diagnostic paths inexpensive.

### Checkpoint 4j — shared COP1 addition replay accepted

ee-cop1-add-comparison-sync.zip: all four arms stop at 40M dispatches
with exit 4. All three replays consume 2,838/2,838 PAD0 polls;
182,384 sampled transitions each, zero differences or unpaired samples.
No replay error; libraries unchanged and parent/candidate hashes match
4h/4j link receipts. Terminal JSON SHA-256
c9fba9f24569160916b07ffc02dfaab7e64c763d30323414344246ca91b86882
and GPU PPM SHA-256
8e8b96cb81e8d54be0c807878c638e383ac525b19bc896f6f1b630948ddafc30
are byte-identical across all four arms. EE cycles 20,070,022,629;
attempts 19,862,797,506; reference slots 19,864,714,689; scheduler
yields 2,697; fields 4,078; GPU draws 4,887,086; presents 2,925;
VU1 native runs 407,359 / 460,705,640 pairs, zero interpreter
fallbacks; no IOP guest fault or timing observer failure. Final PC
0x1a9f00. Wall seconds 45.703/45.312/43.891/43.656; candidate
-0.54% versus immediate parent, -3.65% versus O5 control, single
measurement only. All five CI workflows passed on 3785324.
The 90,095,104-byte (85.92 MiB) image is accepted for this synchronous
workload. Production lowering and cache policy remain unchanged.

### Checkpoint 4k — trace guard review

The current module builder already implements the preferred architecture:
inline null and enable/remaining checks, followed by shared external
publication (ee_trace_guest_store and ee_trace_guest_v0_write). The
runtime callbacks own the remaining filtering and budgets; the checks
are reloaded at each guest write. A shared quadword owner also retains
the exact local guard. Do not unconditionally outline these hot gates
or hoist them across callbacks; that would add calls to disabled trace
paths or risk stale enable/budget state. Retain the current bodies and
measure their residual footprint before reconsidering full outlining.

### Checkpoint 4l / 5a — remaining accounting and lowering cost audit

The annulled-slot helper increments only reference slots and publishes
observation kind 1. The ALU-run helper conditionally advances attempts
and slots together only when both observer and trace are absent; its
caller emits quiet and observed paths. Do not combine these semantics
or outline hot batching without measurements. Implement a read-only
audit of original guest IR instruction lines, annotations, count-only
PCs, cloned annotations, LLVM blocks, opcode-region costs, register
state GEPs/accesses, accounting and memory/exception helper calls,
fastmem regions and entry dispatch costs. Verify original and current
addition-overlay native object identities, report module-level
executable bytes and approximate COFF named spans of the four remaining
helpers. Report lexical attribution separately from exact static
instruction counts; no native per-opcode mapping is claimed. Include
size bands, all module summaries, largest blocks, and optionally
caller-supplied hot PCs. This does not recompile, mutate caches or
change runtime semantics. Use the results to select the next isolated
optimization rather than guessing at the cost of small hot helpers.

### Checkpoint 5a — measured lowering inventory

All 16,252 modules, 845,746 function-scoped PCs (780,826 unique),
15,337,469 original guest LLVM instructions (18.13 per scoped PC).
Guest body text 807,337,002 bytes. Current executable objects total
78,650,693 bytes, of which 77,586,186 are attributed to named guest
symbols (98.65%; includes inlined code/padding). Memory mnemonic
regions account for approximately 10.60M IR instructions (69.1%);
SW 2,396,997, LW 2,020,680, SWC1 1,493,075, LWC1 1,029,796.
Fastmem-labelled regions contain 8,222,941 instructions (53.61%),
not exclusively gate operations. 3,113,728 GEPs, 2,100,317 loads,
1,901,983 branches, 1,094,827 stores. Largest module 0x369520:
5,323 PCs, 117,469 IR instructions, 600,125 -> 574,814 executable
bytes. Remaining named spans: store guard 322,272, V0 guard 107,424,
annulled counter 204,007, ALU-run counter 107,424. These are approximate
attributions, not savings. Defer full outlining of these hot guards.
Prioritize repeated scalar memory lowering before speculative register
forwarding: the current memory boundary flush/reload is intentional.

### Checkpoint 5b — paired scalar-memory outlining pilot

Test module-local noinline per-width read/write helpers against the
validated addition parent (90,095,104-byte DLL). Match the canonical
null, physical span, base, widened end-address, bounds, direct and
slow arms exactly with SSA backreferences; reject escaping internal
values/labels and additional merge PHIs. Preserve the guest alignment,
state publication, diagnostic trace, address calculation and external
memory callback ABI. No hoisting or weakened range checks. Reconstruct
all previous transforms with their digests, verify selected parent
objects, require byte-identical parent re-emission, and compare native
object and executable bytes with unchanged O5/Oz policy. Use eight
modules per populated size band plus eight largest memory-call counts
by default. The pilot does not produce a DLL or update any cache.
MCJIT tests exercise all widths, reads/writes, null runtime/base,
zero-size, unaligned access, boundary and high unsigned addresses.
Outlining introduces a potential hot-path call cost; do not promote
based on size alone. Noncanonical patterns remain untouched.

### Checkpoint 5b — measured scalar pilot

55 modules sampled, 54 transformed, 38 executable wins, 16 losses,
one unchanged. Original sampled executable bytes 2,385,069;
candidate 1,967,663 (417,406 bytes / 17.50% saved). Aggregate object
saving 401,487; IR text saving 6,553,307 bytes. O5 saves 190,126
of 1,164,677 bytes; Oz saves 227,280 of 1,220,392 bytes. The largest
module 0x369520 saves 111,936 of 574,814 executable bytes. These are
sampled results only; small modules frequently grow.

### Checkpoint 5c — link measured scalar winners

Promote only the 38 positive executable-size trials, reproducing each
candidate object byte-for-byte against the pilot receipt and checking
all parent hashes. Reuse all other parent objects, preserve external
symbols and guest metadata, and link a separate experimental DLL.
Do not change the original lowering or production cache. Run matched
synchronous O5 record/control, addition parent, scalar candidate.
This small linked pilot is a runtime risk gate, not evidence that all
scalar memory outlining is beneficial. Expand only after acceptance.

### Checkpoint 5c — selective scalar memory replay accepted

helper-plan(5).json: 38 reproduced wins, 16,214 reused modules; all
16,252 retained. Executable saving 417,644 bytes, object saving
403,319 bytes, IR text saving 6,552,836 bytes; stage 58.75 s before
link. DLL 90,095,104 -> 89,680,384 bytes (85.53 MiB), incremental
414,720 bytes (0.46%), cumulative 21,790,720 bytes (19.55%) vs O5.
Linked executable raw bytes 84,723,200. Candidate SHA-256
1fc04e70ecbea0f0ea8a9f17bfdf24d3c0db5f8da72b81c2bebc9611dfbd51a9.
Four-arm synchronous replay: exit 4 at 40M dispatches, 2,979/2,979
PAD0 polls on each replay, 184,260 common samples each, no mismatches
or unpaired samples. Library hashes match receipts and remain unchanged.
All terminal reports SHA-256
5a160465f8e4a0a8ec8e30191d72cf706b7d61019a80958f4e3604c291e3141f
and GPU screenshots SHA-256
51edbc81a843cc4018d5efe3798b84fedc4bc1bd335584f867e4160cfb526721
are identical. EE cycles 21,061,784,673; attempts 20,857,111,836;
reference slots 20,859,060,982; fields 4,280; GPU draws 4,735,265;
presents 3,066; VU1 native runs 398,254 / 440,220,978 pairs;
zero fallbacks or IOP guest fault. Final PC 0x1082a0. Wall seconds
43.516 / 43.406 / 43.344 / 43.157: -0.43% vs immediate parent,
within noise. Accept the 38-module image for this workload only.
Four CI workflows passed; Build and test fast-2 reported a SEGFAULT
in ee_scalar_memory_pilot. Correct the MCJIT fixture target-machine
ownership (one fresh machine per engine); CI must confirm the fix.

### Checkpoint 5d — expand selective scalar outlining

Scan all original memory-bearing modules not already accepted by 5c.
Reconstruct the COP1 addition parent for each trial, require exact
paired native baseline reproduction, retain only positive executable
size changes, delete rejected trial objects, and reuse the accepted 38
scalar pilot objects by verified absolute path. Validate the complete
pilot manifest, metadata, DLL hash, object identity and receipt chain.
Keep a separate all-trial report with rejection and coverage counts;
link the full retained guest inventory once. Measure incremental savings
against the 85.53 MiB scalar pilot, not the addition parent. A four-arm
retail replay must independently validate expanded correctness and
performance. No generated IR, production cache or lowering changes.

### Checkpoint 5d — expanded scalar retail acceptance

The all-trial receipt SHA-256 is
 d8a2bc65e8ea2b79d152471fa36f11cea03e0d25d5a7469efd6c1068d178344d.
13,695 examined, 13,669 transformed, 9,644 positive executable wins,
4,051 rejected/unchanged, 38 previous wins retained, 2,519 non-memory
modules. O5 2,031/2,213 wins save 4,976,003 bytes; Oz 7,613/11,482
wins save 4,020,009 bytes. Incremental executable saving 8,996,012,
object saving 6,369,267, removed IR text 143,136,676 bytes. All trial
rows, sums, and winner inventory checked. Stage 409.875 s before link.
DLL 89,680,384 -> 80,889,856 bytes (77.14 MiB), reduction 8,790,528
bytes / 9.80% from the scalar pilot; 30,581,248 bytes / 27.43% from
O5. Linked executable raw bytes 75,732,992; cumulative scalar executable
saving 9,413,656 bytes; 9,682 scalar-optimized of 16,252 retained modules.
Candidate SHA-256 89498563cb33c701988e1c1a8e15a534e78ac2a7b09c4cdc91898d097e1c6fdc.
All four synchronous replays exit 4 at 40M dispatches; 3,176/3,176 PAD0
polls, 185,345 matching transitions per replay, zero mismatches or
unpaired transitions. No IOP guest fault or failed timing observer.
Report SHA-256 c1be58fc952fac661b72bd42a79d6370a5c451ac056adf03c39b188914eea4ea,
screenshot SHA-256 05fa8aeed9c91b85b25a95549934c2dad6ab9f71c420b074725f8fcbf7987350
identical in all arms. EE cycles 22,277,448,204; attempts 22,068,845,060;
reference slots 22,070,897,648; yields 3,008; fields 4,527; GPU draws
4,626,795; presents 3,267. Wall seconds 41.797 / 41.375 / 40.922 /
40.906 (0.039% faster than scalar pilot; noise). All five PR CI workflows
passed on f2cfa77. Accept expanded scalar outlining for the matched retail
workload, retaining isolated cache status.

### Checkpoint 5e — cross-module scalar helper sharing

The 9,644 new scalar winners contain 36,164 module-local scalar helper
definitions (static IR, not native span sizes). The 38 pilot modules
also contain local helpers. Test one exact noinline definition per
width/direction across the 9,682 accepted scalar modules. Choose a
stable O5 owner when available. Reconstruct and hash-check every local
outline, verify the complete two-layer scalar object/manifest chain,
require identical helper bodies, fallback declarations, direct call
counts and no escaping symbol uses. Preserve all other guest and shared
symbols. Recompile only existing scalar modules with original O5/Oz
policy, retain unchanged non-scalar objects, require aggregate executable
savings before linking. Unlike 5d, do not independently reject individual
module losses: the shared owner graph must remain coherent. Compare the
new DLL against the 77.14 MiB expanded scalar parent in a matched replay.
No production integration or claim of speedup until separately validated.

### Checkpoint 5e — shared scalar retail acceptance

9,682 unique recompiled modules; all eight shared definitions occur
once in sub_0x100420_0x100420.ll. O5 2,049 smaller / 1 unchanged
(800,150 executable bytes saved); Oz 7,632 smaller (1,483,491 saved).
Zero larger objects by executable-section size. Total executable saving
2,283,641, object saving 4,541,592, removed IR text 25,014,346 bytes.
6,570 reused, all 16,252 functions retained. Stage 219.954 s before link.
DLL 80,889,856 -> 78,397,952 bytes (74.77 MiB), incremental 2,491,904
bytes (3.08%), cumulative 33,073,152 bytes (29.67%) vs original O5.
Linked executable raw bytes 73,443,328. Candidate SHA-256
 d4842c017f4c98d1c55c0c9a5311aa324df9c4f2613e9a0962030435fb6ed3fe.
All four synchronous replays exit 4 at 40M dispatches; 3,475/3,475 PAD0
polls on each replay and 186,921 matching transitions per replay,
zero mismatches/unpaired samples. Reports SHA-256
109a086dc643d6927c68d685b0ba5d52b3b52fca4e3659c8d6f594210698f513;
screenshots SHA-256
24f1602e57a26d808cc40210e9e5e86bb512b729551df183152b5e6fdb4a4a13
identical across all arms. EE cycles 24,488,995,328; attempts
24,286,720,915; reference slots 24,288,772,056; fields 4,976;
VU1 runs 366,743 / 409,223,976 pairs, zero fallbacks or IOP faults;
final PC 0x185bd8, no timing observer failure. Wall seconds
40.437 / 40.422 / 40.312 / 40.547 (+0.58% vs parent, single run).
Accept for this workload only. Four CI workflows passed; fast-2
reported a test-fixture SEGFAULT after an error in the new sharing test.
Explicitly dispose linked ModuleRefs before their context and enable
verbose/fault diagnostics. CI must confirm this repair.

### Checkpoint 5f — revisit residual scalar candidates

Local outlining rejected or did not transform 4,051 modules. The
shared helpers eliminate local definition overhead, so retry exactly
this residual set from the hashed expanded trial receipt. Require all
existing shared owner and object identities to match, reconstruct the
original addition-parent IR and the exact attempted local outline,
replace local helpers with shared declarations only, and re-emit the
unoutlined parent to verify byte identity. Accept positive executable
size changes only; discard other candidate objects. Retain all 9,682
previous shared scalar objects, including the eight owners, unchanged.
Save the full trial receipt and link a new isolated candidate; compare
against the 74.77 MiB shared scalar parent in four-arm retail replay.

### Checkpoint 5f — residual shared scalar retail acceptance

All 4,051 residual trials verified against receipt SHA-256
81c4353a74dc88de8f549b6cd7be7369d6df3b0e7a067f527ec4351fed2f932c.
4,025 canonical rewrites, all executable wins; 26 unmodified.
O5 178 wins / 4 unchanged, 39,474 executable bytes saved; Oz 3,847
wins / 22 unchanged, 413,345 bytes saved. Total executable saving
452,819, object saving 359,749, removed IR text 6,537,936 bytes.
Stage 49.437 s before link. 13,707 cumulative scalar modules, 16,252
retained functions. DLL 78,397,952 -> 77,945,856 bytes (74.33 MiB),
452,096 bytes smaller than parent; cumulative 33,525,248 bytes /
30.075% vs O5. Linked executable raw bytes 72,991,232. Candidate
SHA-256 be08d290a081efdd28f17b73a91742236ab0f6df7aa4eda4d6bc40708b964cc7.
Four arms exit 4 at 40M dispatches; 3,606/3,606 PAD0 polls, 225,607
matching samples per replay, no mismatches/unpaired samples, libraries
unchanged. Reports SHA-256
4133ab36f59ef02cd0a908043064a4f0d077ce5699ebad4cb41f55930c44df40;
screenshots SHA-256
b359b9f003f3fd3d96a5befb9842fa87ed184af60ebf105bf92ec04370eb2ff1.
All four arms byte-identical reports/screenshots. EE cycles
25,220,954,778; attempts 24,904,636,159; slots 24,907,078,219;
yields 3,410; fields 5,125; final PC 0x19d2d0. VU1 native runs
353,348 / 397,589,308 pairs; no fallbacks, IOP fault or timing observer
failure. Wall seconds 40.796 / 40.672 / 40.250 / 40.234 (-0.04%
vs parent; noise). All five CI workflows passed on aaa294e, including
LLVM fixture repair. Accept the 74.33 MiB residual overlay for this
matched workload, with production cache unchanged.

### Checkpoint 6a — cold alignment exception merge pilot

The alignment guard already publishes precise state on both paths and
skips proven-aligned constants. Do not change those checks or memory
callback boundaries. Instead merge only exact terminal AdEL/AdES arms
inside a guest definition, grouping by exception code and BD constant.
Retain each original guard, fault label and normal successor; select
PC/BadVAddr with PHIs in a common cold call/return block. Leave other
exceptions and singleton groups unchanged. Test all argument and exit
semantics with a mock exception publisher in MCJIT. Reconstruct the
accepted residual parent and verify its full object/manifest chain.
Scan original IR for actual mergeable arms and sample eight per site
count band plus eight largest candidates. Require byte-identical paired
parent re-emission. Record per-module size changes only; no linking,
cache mutation or runtime acceptance at this checkpoint. If the pilot
wins, select measured improvements for a separate retail gate.
