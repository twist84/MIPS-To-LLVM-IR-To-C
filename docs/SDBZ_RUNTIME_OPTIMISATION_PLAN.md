# SDBZ runtime optimisation follow-up

**Status:** historical PR #156 implementation and measurement log; the workstream was squash-merged as [`130d305`](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/commit/130d3054e5ef978e5dfd4408b00422fa43184eea). Further accuracy development is tracked in [ACCURACY_PLAN.md](ACCURACY_PLAN.md). The later original matrix reached 54/55; see [PS2AUTOTESTS.md](PS2AUTOTESTS.md).

Original base: squash-merged PR #155, `f16a660bedee560d2e4b8179b39202aefd3aee8f`.

Evidence: [2026-11-25 retail validation](SDBZ_RUNTIME_VALIDATION_20261125.md).
The listening replay reached training gameplay with audible PCM and zero
IOP guest faults or VU1 fallbacks. Pacing, live compilation and physical
input make it unsuitable as an uncapped comparison.

## Implementation order

1. Establish matched, warmed-cache, uncapped P40 measurements; separate
   compilation and pacing from exclusive execution costs.
2. Eliminate provably redundant SPU2 processing for disabled effects with
   zero inputs/returns. Preserve filter histories, RAM accesses and
   future enable transitions; test silent, nonzero and tail cases.
3. Profile EE native execution and the highest-cost guest PCs, including
   polling loops. Establish actual producers and architectural side effects
   before considering any scheduling shortcut.
4. Reduce redundant IOP scheduler/service and dispatch-diagnostic work.
   Preserve guest-cycle ownership, DMA, IRQs and ordering.
5. Measure GS/GIF/WebGPU submission and GPU timestamps where evidence
   justifies further work; retain CPU/GPU scanout equivalence.
6. Continue independent R5900/COP0/COP1/VU and SPU2 differential accuracy
   audits. Do not label compatibility assumptions as hardware results.

## Validation contract

- One coherent change at a time; run relevant synthetic regression tests
  and verify CI before declaring a change validated.
- Preserve compatible EE IR and VU1 AOT caches. Regenerate only when
  guest-lowering provenance or actual microcode misses require it.
- Compare matched dispatch budgets, input, guest workload, cache state
  and profiler settings. Report whole-run and exclusive subsystem costs.
- Request a retail replay only when runtime evidence is needed. Otherwise
  proceed to the next implementation item.
- Do not fabricate guest completion, interrupts, output or game-specific
  bypasses. Do not commit proprietary retail assets or generated caches.

## Local profiling presets

```bat
set "SDBZ_P40_LISTEN="
set "SDBZ_HOST_AUDIO=0"
set "SDBZ_RUNTIME_PROFILE=1"
set "SDBZ_PROFILE_PCS=1"
set "SDBZ_AUDIO_PROFILE=1"
scripts\run_sdbz_p40.bat
```

Listening control: set SDBZ_P40_LISTEN and SDBZ_HOST_AUDIO to 1.
The script performs a fast-forward-only pull and uses the existing
`build/vu1-native-v20` library. Keep the code baseline intact while
measuring; do not compare paced and uncapped wall times directly.

## Batch 1: disabled-effect arithmetic elimination

The disabled reverb path does not simply discard its return. It omits
only work that cannot affect output, RAM or IRQs in the existing model:
input FIR and reflection calculations (their results feed suppressed
writes); with APF2 gain zero, its delayed RAM tap alone determines the
return; with only APF1 gain zero, the two delay taps suffice. Otherwise
use the original comb and APF calculations. All input history, output
history, sample phase and enabled-effect reads/writes remain intact.
A maintained 64-bit occupancy mask per output history permits exact zero
FIR elision, never based on zero EVOL or instantaneous silence alone.

A forced-reference host-only test control compares PCM byte-for-byte,
RAM (both work areas alias the same physical range), counters and IRQ
latches/deadlines through disabled and enabled phases, signed gains,
nonzero DMA input, invalid areas and re-enable. The runner reports
`fast_ticks` and `zero_fir_skips` per core. No hardware behavior is changed
or newly claimed. Performance gain requires a matched retail measurement;
this batch does not alter EE lowering or VU1 AOT provenance.

### Matched reference/optimised replay

The host-only `SDBZ_SPU2_EFFECT_FAST_PATH` switch selects the full reference
path with value 0; unset or 1 selects the default optimisation. Both use
the same guest clock, source input, cache provenance and runtime code.
Warm the compatible cache first. Use the uncapped device-free preset above,
run once with `set "SDBZ_SPU2_EFFECT_FAST_PATH=0"`, preserve the archive, then
repeat with `set "SDBZ_SPU2_EFFECT_FAST_PATH=1"`. Keep physical controller
input neutral and compare actual input observations, dispatch and work
counts before interpreting timings. The log prints the selected mode.

### Retail results — 2026-11-25 11:09, 11:11, 11:13

Code HEAD `51356c26df9989cadf2b2375f4b65ea68b528d5b`: all four CI workflows passed. Three uncapped
40M-dispatch training replays (archives `sdbz-graphics-pr148-neutral-20261125-110912880.zip`,
`...-111137950.zip`, `...-111333140.zip`) confirmed nonzero PCM, no IOP
guest faults, and byte-identical CPU/GPU captures within each replay.
Host device disabled throughout.

| Metric | 11:09 fast | 11:11 reference | 11:13 fast |
| --- | ---: | ---: | ---: |
| Native process wall (s) | 48.734 | 92.594 | 78.531 |
| Compilation wait (s) | 0 | 44.705 | 32.143 |
| SPU2 mixer (ms) | 1928.025 | 2495.889 | 1582.432 |
| Effect processing (ms) | 353.634 | 1230.512 | 274.145 |
| Effect core ticks | 8716446 | 6288234 | 6740402 |
| Effect ns/core tick | 40.57 | 195.68 | 40.67 |
| Zero FIR skips | 17432892 | 0 | 13480804 |
| PCM nonzero frames | 1880791 | 1775296 | 1907791 |
| EE native exclusive (s) | 15.617 | 15.237 | 15.110 |
| IOP service exclusive (s) | 9.523 | 9.692 | 8.695 |
| Dispatch diagnostics exclusive (s) | 2.201 | 2.184 | 2.183 |
| Cached EE IR modules | 16219 | 16232 | 16250 |
| Native VU1 runs | 351651 | 430518 | 407260 |
| VU1 fallbacks | 0 | 0 | 864 |
| GS fields | 5444 | 3928 | 4211 |
| Window presentations | 3928 | 2840 | 3031 |
| Cross-low candidates | 192 | 119 | 122 |
| Analog samples outside deadzone | 60 | 88 | 216 |

All common IR hashes match; 13 then 18 new modules published. The two
fast replays used the shortcut on every core tick; both effects remained
disabled, with zero wet inputs/returns and effect IRQs. Per-tick cost
fell approximately 79.2% (4.82x), not a controlled whole-runtime speedup.
Inputs, cache waits, guest cycles and final combat states differ. The
11:09 replay is the cleanest uncapped baseline (zero compilation wait).
The 11:13 replay captured three new VU1 images (720 no-entry, 144
code-mismatch fallbacks; PCs 0x450, 0x50, 0x170). Compile only the
captured misses into the existing v20 AOT directory, retaining the EE
cache. No VU1 optimisation is necessary for this batch.

Next: EE native and IOP service dominate exclusive runner time. The
command-slot polling entry 0x00102560 costs 1.369–1.515 s; the current
idle analyzer rejects 0x001025a4 because a branch enters the loop body.
Retain this guard until the original CFG, timer and command-slot producer
are established. Dispatch diagnostics consume a repeatable ~2.18–2.20 s
across all three runs and are a separate safe host-side optimisation
candidate. Do not conflate GPU sampled compute time with exclusive
runner CPU time or sum overlapping worker durations.

## Batch 2: exact dispatch-diagnostic hot path

Pack the complete ordered PC pair into one unsigned 64-bit key for the
edge map and direct-mapped cache. Keep the existing slot hash, admission
capacity, cache miss accounting, exact visits, and lexicographic tie
ordering. Compare the state fields with a grouped XOR/OR reduction rather
than a serial short-circuit comparison; no padding is compared and all
16 fields remain significant. History and state-change observation still
run at every original boundary. The smoke test exercises full-width keys,
collision/rejection accounting, chronological wrap, and each state field.

This is host-only work, not EE lowering or a scheduling shortcut. No EE
or VU1 regeneration is required beyond independently captured VU1 misses.
Measure the exclusive dispatch-diagnostics cost against the 11:09 baseline
with equivalent profiler settings before claiming a gain.

### Batch 2 retail result — 2026-11-25 11:23

Archive `sdbz-graphics-pr148-neutral-20261125-112313454.zip`: 40M
observations and dispatches, 53.187 s native process wall, no compilation
wait, no IOP guest fault, byte-identical CPU/GPU scanout. VU1 v20 loaded
84 programs, executed 389797 native runs with no fallbacks or captures.
PCM: 2115275 nonzero frames; effect cost 354.207 ms / 8183060 core ticks
(43.29 ns/tick). EE native exclusive 16.440 s, IOP 10.863 s, dispatch
diagnostics 2.347 s (58.67 ns/observation), versus 2.201 s previously.
Workload differed (5111 fields, 3644 presentations, 137 Cross candidates,
zero analog samples outside deadzone). No demonstrated diagnostic speedup;
retain exact counters and do not claim a regression caused by the patch.
All four CI workflows for `4fc03fa` passed. No new VU1 build needed.

## Batch 3: inline the empty SIF RPC service path

The IOP service ran 40035980 times in the last replay. Inline the exact
existing two-queue empty check and pending guest-call count, keeping
nonempty module-start/reply processing in the same out-of-line implementation.
Do not skip a pending initializer, change the pump cadence, or combine
IOP cycle budgets. Existing tests exercise pending starts, blocked and
accepted DMA5 replies; add a repeated empty-pump regression. This is a
small host call-overhead reduction, not a measured 10-second saving.

## Batch 4: prune redundant IOP idle alarm deadline scans

All four workflows passed for Batch 3 (`60543ee`). During idle
scheduling, resolve the existing thread, HLE and SPU2 device deadlines
before considering alarms. The existing conservative alarm hint is a
lower bound: if it cannot beat the already selected deadline, the full
alarm map scan cannot change the result. Otherwise retain the exact scan
of noncancelled future alarms. Never advance to a stale hint. This also
avoids the map iteration when no alarm exists. Preserve budget-exhausted
versus idle reporting, wake ordering and all hardware cycle advancement.
Extend the alarm cancellation test with a budget ending before the stale
hint. Existing earlier-insertion and HLE deadline tests cover the case
where an intervening real event changes the earliest alarm.
No lowering or AOT cache change. No standalone retail replay required;
performance remains to be measured with the next combined run.

## Batch 5: command-slot loop and VIF1 completion audit

The 11:23 replay records 3461 entries to 0x00102560, 273065681
native instructions, and 1563.086 ms of per-PC wall time. It contains
39 HOST_YIELD edges to 0x001025a4, 39 DMAC1 callback returns to that
header, 227 DMAC5 returns, and 48 INTC callback returns. All 3447
logged DMAC1 callback entries use handler 0x00102a60. No red-loop
snapshot was reached. The earlier ELF-derived
[M2.4 audit](M2_4_SDBZ_COMMAND_SLOT_TIMEOUT.md) identifies a loop
counter threshold of 3001, Timer0 COUNT (0x10000000) threshold 60001,
and the DMAC1 handler's pending-count decrement at 0x00102b08..10.
The latest ZIP has no ELF or IR instruction bytes; the static facts are
from that earlier exact-ELF audit, not newly reconstructed from logs.

This is not the pure idempotent VBlank poll. Do not relax internal
branch, loop-carried register or timer-read guards. The native code
still has to execute the counter and timeout path correctly. The
observed channel-1 returns are consistent with the expected producer,
but do not by themselves establish which pending count changed.

Record at most 16 before/after slot-selector and flag pairs around
actual completed DMAC1 callbacks at 0x00102a60 interrupting 0x001025a4.
The log includes the return dispatch and result; capture before entering
each matching handler and before restoring the interrupted context or
chaining the next callback. No new MMIO reads, guest writes, fabricated
interrupts, or timer changes. The smoke test checks filter selection
and immutable backed-RAM observations. Callback-boundary correlation
is not an instruction-level writer trace. No EE or VU1 cache changes;
collect the new diagnostic in the next combined retail replay rather
than request a standalone run.

The callback helper lives in `src/tools/ee_sdbz_slot_callback_probe.h`;
restore the original `include/ee/ee_sdbz_dma_wait_probe.h` byte-for-byte
because that header is included in the EE translation-cache input hash.

## Batch 6: exact per-PC native profiling cache

All four CI workflows passed for Batch 5 (`06aba24`). The 11:23
replay executed 40,000,000 direct dispatches with 39,903,773 native
target cache hits and 100,584 misses (99.75% hit rate). Increasing
the native target cache is not the priority. The same replay recorded
17,289 distinct entry PCs, and per-PC profiling currently performs
an unordered-map access after every dispatch. Cache stable references
to the exact per-PC counters in a 16K direct-mapped host table, retaining
the unbounded owning map, full 32-bit PC equality, all five counters
and the existing final sort and text format. No allocation of cache
slots when PC profiling is disabled. Tests cover collisions, zero and
unaligned PCs, full-width keys, rehash, zero-dispatch records and disabled
collection. No guest clock or native resolver behavior changes.

This reduces outer-runner profiling overhead, not the instruction
execution inside the measured native scope. Measure with PC profiling
still enabled; do not claim a reduction in EE native exclusive time
from this change alone. No EE/VU1 cache regeneration required.

## Batch 7: avoid repeated GS worker image discovery

All four CI workflows passed for Batch 6 (`69804d3`). The 11:23
replay's runner profile recorded 977.002 ms GIF consume exclusive,
1209.749 ms GS transaction exclusive, 1593.706 ms WebGPU submit CPU
and 1270.590 ms present CPU. The asynchronous GS observer completed
6915 batches (55,772,996 events / 1,784,735,872 event bytes and
1,752,776,640 image bytes); submit preparation/copy was 664.620 ms,
worker execution 1631.513 ms, fence 222.558 ms, no backpressure.
These worker statistics are not additive to exclusive runner totals.

During existing IMAGE preflight validation, retain up to 128 nonempty
IMAGE event indices in a bounded stack list. The owned event array
is still copied in full; then compact only the recorded IMAGE payloads
in their original order. A dense batch above the index capacity uses
the original full scan. No-image batches avoid the second scan entirely.
All original validation, byte limits, event offsets, failure propagation,
fences, publication and copy ownership remain unchanged. Test zero,
sparse, exactly 128 and 129 nonempty images, interleaved registers,
zero-QWC image events, out-of-order source offsets and exact byte counts.

No guest lowering or VU1 provenance changes. Host runtime provenance
may require the normal native relink, not IR regeneration. This is a
candidate host CPU improvement, not a measured GPU speedup. Batch 7
completes the current optimisation pass; run a combined warmed-cache
P40 replay to measure Batches 3–7 and collect Batch 5 callback evidence
before claiming retail validation.

### Combined Batches 3–7 retail validation — 2026-11-25 11:52

Archive `sdbz-graphics-pr148-neutral-20261125-115241025.zip`.
40M dispatches and observations, zero timing-observer failure or IOP
guest fault, CPU/GPU display PPMs byte-identical within the run, 5192/5192
GS worker batches completed with no failures or backpressure. VU1 v20:
84 programs, 423320 native runs, 475652611 native pairs, no fallbacks
or captures. All 16250 EE IR names and SHA-256 hashes match the 11:23
cache exactly; only the native provenance input ee_gs_worker.h changed.

| Metric | 11:23 | 11:52 |
| --- | ---: | ---: |
| Native process wall (s) | 53.187 | 49.062 |
| Compilation wait (s) | 0 | 0 |
| EE native exclusive (s) | 16.440 | 16.110 |
| IOP service exclusive (s) | 10.863 | 8.786 |
| Dispatch diagnostics exclusive (s) | 2.347 | 2.238 |
| GS worker copy/preflight (ms) | 664.620 | 488.428 |
| GS worker event bytes | 1784735872 | 1863763168 |
| GS worker image bytes | 1752776640 | 1239653952 |
| Copy ns per combined byte | 0.18788 | 0.15738 |
| GS transaction exclusive (ms) | 1209.749 | 1006.235 |
| GS transaction events | 55772996 | 58242599 |
| GS transaction ns/event | 21.69 | 17.28 |
| SPU2 effect ns/core tick | 43.29 | 43.05 |
| Nonzero PCM frames | 2115275 | 1745464 |
| EE reference cycles | 25152636707 | 19132270071 |
| Guest fields | 5111 | 3888 |
| Window presentations | 3644 | 2813 |
| Cross-low candidates | 137 | 137 |
| Analog samples outside deadzone | 0 | 358 |

The process wall is 7.76% shorter, but 23.9% fewer guest cycles
and different live controller input preclude a controlled whole-run
speedup claim. IOP cost per EE reference cycle actually rose from
0.432 to 0.459 ns. Worker copy per combined byte fell about 16.2%,
and GS transaction cost per event fell about 20.4%; batch composition
and overlap differ, so these are encouraging observational comparisons,
not isolated causal benchmarks. The 11:09 baseline wall was 48.734 s.
No standalone replay is required to continue hardware differential work.

### Batch 5 producer correlation closed for this replay

All 16 captured completed real DMAC1 callbacks at 0x00102a60 returning
to 0x001025a4 left the selector unchanged, and changed the opposite
slot from 1 to 0 with the active slot unchanged at 1. Nine observations
had selector 1 and seven selector 0. The first was at dispatch
10346560 and the last at 14317732. Each returned result 0. This agrees
with the earlier original ELF pending-count decrement audit, and
demonstrates the callback-level completion producer for these samples.
No red-loop snapshot was observed. The command wait still executed
273049227 native guest instructions (1540.488 ms), and the idle-loop
analyser correctly continues to reject internal branches. Do not skip
the counter/timer path or synthesize any VIF1 completion.

Next: independent hardware accuracy coverage, starting with the
existing R5900/COP0/COP1/VU and SPU2 reference/differential tests.
Keep the proven compatibility behavior separate from hardware claims.

## Batch 8: hardware-backed COP1 control-register coverage

All four workflows passed on `facadd76`. Reference source:
[ps2autotests fcr.cpp](https://github.com/unknownbrackets/ps2autotests/blob/97469ffbed8631277b94e28d01dabd702aa97ef3/tests/cpu/ee_fpu/fcr.cpp)
and [fcr.expected](https://github.com/unknownbrackets/ps2autotests/blob/97469ffbed8631277b94e28d01dabd702aa97ef3/tests/cpu/ee_fpu/fcr.expected).
The existing JIT test sampled read aliases 0, 15, 16, 31 and an all-ones
write. Add literal console observations for all 32 read aliases in
initial and flags-written states, plus all eleven FCR31 write groups.
Exercise the actual decoded/translated instructions at O0/O3, including
source GPR upper-half preservation, unrelated GPRs, FCSR read-only
behavior and ACC/latch preservation. The alternate initial FCSR for
write groups is an additional software control, not a separate console
measurement. This is test-only coverage; no production lowering,
runtime ABI or VU1 changes. CTC1 alias writes and physical timing remain
separate questions. Passing these fixtures will not validate R5900 timing.

Clarification to the combined replay: GPU-authoritative VRAM mode reads
back GPU state for display diagnostics. Matching PPM bytes are not
independent CPU-versus-GPU rasterizer proof; the existing independent
CPU reference and differential fixtures remain necessary.

## Batch 9: literal console COP1 arithmetic flag observations

Extend the same pinned fcr.cpp/fcr.expected reference with all seven
arithmetic flag situations: negative root, zero/zero, nonzero/zero,
extended-operand addition overflow, multiplier overflow, divide
underflow and inexact divide. Check exact reported FCR31 words after
real translated instructions at O0/O3, including unchanged source
FPRs and ACC/latch. The source prints decimal values rather than raw
result bits, so these are FCSR assertions only; the existing independent
raw-result fixtures retain that coverage. No extrapolation to all
flag sequences or hardware timing. Test-only; no EE/VU1 cache changes
or standalone retail replay required.

## Batch 10: R5900 hardware interrupt vector correction

The EE Core User's Manual v6.0, section 4.2.5, printed p.99
(https://docs.alexrp.com/mips/ee.pdf), specifies V_INTERRUPT at
0x80000200 (BEV clear) and 0xBFC00400 (BEV set). The pinned
PCSX2 interpreter's [cpuException](https://github.com/PCSX2/pcsx2/blob/82cac38b2af19d593dedfd9ddaa63448e7ec493c/pcsx2/R5900.cpp)
agrees. The runtime previously selected V_COMMON for EX_Int. Correct
normal interrupt vector selection in the shared runtime helper; the
legacy exception metadata class retains the common fallback for nested
entry. EXL/ERL masking, interrupt eligibility, Cause.IP, EPC, ordinary
exceptions and TLB refill behavior are unchanged. Do not change the
public lowering header solely to add two vector names; preserve shared
EE IR provenance. The HLE runner already resolves the IRQ vector via
that same helper, before restoring the interrupted guest context.

Update actual dispatch and compatibility scanout IRQ fixtures to use
literal hardware vector addresses, and test BEV/EXL selection and
unmodified syscall/refill vectors. This is an architectural correction,
not calibrated timing. Nested TLB BD preservation, COP0 writable
masks and level-2 handling are separate future changes. A warmed P40
retail replay is required after CI because real INTC delivery now
enters a different architectural PC; verify HLE callback delivery,
VBlank, DMA, audio, no guest fault and unchanged IR/VU1 inventories.
No generated IR or VU1 regeneration needed; allow normal native relink.
Stop before Batch 11 until this retail gate is satisfied.

### Batch 10 retail validation — 2026-11-25 12:06

Archive `sdbz-graphics-pr148-neutral-20261125-120640867.zip`:
40M dispatches, 49.406 s process wall, 44.451 s profiled, 0 compile
wait, no IOP fault, 4006 fields and 2886/2886 successful presents.
3419 explicitly logged transitions to the corrected 0x80000200 IRQ
vector and 3474 HLE INTC entries/returns; general exceptions still
use 0x80000180. All 16 sampled DMAC1 slot callbacks completed, no red
loop. VU1 v20: 84 programs, 411642/411642 worker jobs, zero fallback,
capture or host failure. PCM 1858699 nonzero frames. Both display PPMs
are byte-identical in GPU-authoritative mode (not independent raster
validation). EE cache contains 16250 modules; no compilation wait or
VU1 rebuild needed. EE native exclusive 16.208 s, IOP 8.912 s.
Retail gate passed for normal BEV=0 interrupt delivery; BEV=1 is
covered by synthetic regression rather than this retail replay.

## Batch 11: literal R5900 scalar division hardware vectors

Add all 26 literal console records for each of DIV/DIVU/DIV1/DIVU1
from [ps2autotests muldiv.expected](https://github.com/unknownbrackets/ps2autotests/blob/97469ffbed8631277b94e28d01dabd702aa97ef3/tests/cpu/ee/muldiv.expected),
expanding symbolic memory operands from the pinned shared.h/muldiv.cpp.
Execute 104 records at each existing O0–O3 emitter level, checking all
four HI/LO halves, MFHI/MFLO results and untouched 128-bit GPR halves.
Immediate cases define only the low 32 operand bits; chosen high bits
are test controls. No production changes, new timing claim or replay.

## Batch 12: expand shared VU0 differential sign branches

Extend the shared PCSX2/candidate program with four complementary
sign-branch predicates on stable negative and zero operands, checking
taken targets, delay-slot writes, fallthrough and exact executed PCs.
The 72-pair program has 63 executed pairs; its hash is pinned in CTest.
The existing manually dispatched VU0 PCSX2 differential will exercise
the same fixture. Previous 53-pair reference evidence is not a result
for the new fixture. No production lowering, cache or retail changes.

## Batch 13: SPU2 ADPCM predictor differential coverage

Add a byte-oriented reference recurrence in the IOP hardware smoke test
for all five defined ADPCM predictors and 16 encoded shifts, with four
initial histories and two chained blocks (640 blocks, 17920 samples).
The pinned PCSX2 source and SDBZ compatibility source agree in this
range. Reserved filter IDs 5–15 differ between references; retain the
existing SDBZ behavior until independently verified. No production
SPU2 behavior or timing is changed. This is source-level differential
coverage, not physical PCM validation; active reverb, decode FIFO timing,
reserved filters and mute polarity remain open. No standalone retail
replay required for Batches 11–13. The expanded VU0 trace still needs
its independent PCSX2 run and the combined PR head needs all four CI
workflows before acceptance.

## Next milestone: complete ps2autotests ELFs

All four CI workflows passed on `da448d4`. Begin the independent 55-test console-output baseline using the existing native ELF runtime; see [PS2AUTOTESTS.md](PS2AUTOTESTS.md). This infrastructure-only commit does not change retail execution or cache provenance. The first pilot selects CPU ALU, integer mul/div and FCR, with explicit observational CI and complete captured stop evidence.
