# M1-A — EE timing / scheduler implementation status

> **Current status:** The default HLE runtime now uses the approximate reference-compatibility clock, delivered real guest fields, and passes the original COP0 performance transcript. The manual-cache-hit profile is offline only; a validated R5900 clock is still outstanding. See [current status](README.md) and [accuracy plan](ACCURACY_PLAN.md). All branch/status claims below refer to the recorded development stages.

**Historical branch:** feature/sdbz-hle-first-bootstrap  
**PR:** #142 (draft, do not merge without authorization)  
**Checkpoint:** 18 November 2026

## Actual baseline

The existing native translation path counts committed guest instruction attempts and simplified reference interpreter slots. The diagnostic issue-timing probe is explicitly experimental. Neither is calibrated R5900 cycle time. The original SDBZ US boot reached the VSync polling loop at PC 0x00175210 with no delivered fields.

The GS rational NTSC/PAL scanout, INTC latch/mask and SetVSyncFlag one-shot delivery already exist, but no validated cycle producer is wired into native execution.

## This batch

- Added a runtime-owned monotonic EE cycle timeline. It is fail-closed until an independently validated R5900 timing source is explicitly installed. It rejects decreasing timestamps and does **not** derive cycles from native dispatches, loop visits, instruction attempts, interpreter slots or host wall clock.
- Wired scheduler servicing to native dispatch boundaries. With the current unavailable source, it cannot deliver VBlank. A synthetic, explicitly enabled test clock verifies delivery at a real deadline, no early/duplicate fields, one-shot VSync registration, independent INTC/CSR behavior, monotonic timestamps and reset gating.
- Added an explicit scheduler yield diagnostic separate from cycle time.
- Added interpreted RAM execution retirement to the existing native diagnostic counters, without claiming it represents hardware cycles.
- Expanded first-boot JSON snapshots with timing source status, EE cycles, instruction slots, scheduler yields, next GS field, delivered fields, CSR, INTC status/mask and registered guest VSync addresses.
- Made SetGsCrt configure the field epoch at the supplied cycle; unsupported CRT modes no longer retain a stale NTSC/PAL schedule. It still cannot itself deliver VBlank.

## Incomplete implementation gates

M1-A is **partial**, not complete. In particular:

1. Native and interpreted instruction execution do not yet emit a calibrated unified R5900 issue/retirement timeline.
2. Pipeline stalls, HI/LO, COP0/1/2, branch/annulled-slot timing and HLE overhead require calibration against trusted reference instruction traces before hardware activation.
3. No validated timing source is installed by the game runner; the clock source is deliberately reported as \`unavailable\`. The new synthetic scheduler fixture tests scheduling semantics, **not** R5900 timing accuracy.
4. Timing-driven guest VBlank and advancement beyond PC 0x00175210 are unverified. Those are M1-B real-game validation criteria, not assumptions.

## Validation

The first scheduler commit (\`9ec3bd3\`) passed all three GitHub workflows. Subsequent diagnostics and CRT-epoch changes require the new head's CI and real-game validation. No new local SDBZ log batch has been used to claim boot progress.

Do not mark M1-A or M1-B complete until their separate acceptance gates are satisfied; do not request repeated local logs while the implementation prerequisite remains outstanding.


## Ordered issue-observation stream (next M1-A batch)

- The existing bounded `--trace-instructions=PATH` capture now represents
  native-reached instructions, RAM-interpreted commits and annulled native
  branch-likely slots in one ordered sequence, preserving per-event
  `reference_slot` and `kind`. This is suitable for comparison with
  reference execution traces when calibrating the R5900 timing model.
- Source kinds: 0 = reached in native translation; 1 = annulled delay slot;
  2 = committed by the RAM fallback. Every event includes its original
  guest PC and opcode. Annulled slots increment the slot ordinal but not
  executed instruction attempts. Unsupported RAM instructions are not
  counted or traced.
- These are **execution observations**, not calibrated issue timestamps.
  Native observations occur before execution of the lifted instruction,
  while RAM fallback records after a successful interpreter step.
  Effects must not be inferred from a native observation alone.
- This batch intentionally does **not** authorize or publish EE hardware
  cycles and cannot unblock VBlank without separately validated timing.

## Trace-driven, uncalibrated issue replay

An explicit-latency candidate consumes complete, ordered native/interpreted/annulled trace prefixes, and uses the existing two-wide issue probe to track scalar integer and load RAW dependencies across execution boundaries. Non-covered instructions, gaps, malformed delay slots and unknown latencies fail closed. This replay cannot publish hardware EE cycles or activate GS scanout. It is a calibration aid, **not** a validated R5900 timing source.

### Explicit candidate replay CLI

Run `ee_issue_trace_candidate <trace.csv> <integer-result-latency> <load-result-latency>` on an unfiltered, non-truncated trace. The analyzer fails at the first unsupported opcode, malformed/gapped reference slot or truncated stream and reports no absolute hardware time. Its output labels candidate cycles as experimental and cannot generate VBlank. A synthetic fixture and CTest cover its CSV/parser/issue path.

## Issue scoreboard resource extension

The experimental issue probe now tracks independent GPR, HI0/LO0/HI1/LO1, COP1 FPR and FPU accumulator read/write reservations. A producer may declare separate special-register/FPR/ACC latencies from its GPR latency. Validation is atomic, so invalid masks and overflow cannot partially mutate a timing state. Dedicated tests exercise scalar HI/LO multiply-to-MFLO, FPR producer/consumer, accumulator and same-cycle pairing dependencies. No new hardware-clock source is installed; these latencies remain caller supplied and uncalibrated.

### Explicit HI/LO timing categories

The offline issue candidate now classifies scalar `MULT/MULTU/DIV/DIVU`, `MFHI/MFLO/MTHI/MTLO` and their second-pipeline `*1` equivalents, plus `MADD/MADDU/MADD1/MADDU1`, using separate HI/LO read/write dependency masks. Their four latency categories default to **unconfigured**: multiply, divide, HI/LO transfer and accumulate. Existing candidate traces remain backward compatible. The CLI accepts the four optional latency values as one group; these values are diagnostic inputs, not hardware validation. Other MMI operations remain unmodeled and fail closed.

### Scalar COP1 issue classes

The offline candidate can now replay supported MFC1/MTC1, COP1.S scalar arithmetic, DIV/SQRT/RSQRT, ACC operations and CVT.S.W when **four separate caller-provided COP1 latencies** are available: transfer, arithmetic, divide/sqrt and accumulate. FPR0 is modeled as writable, independently from GPR0. Other FCR, COP0, VU0 and packed-MMI hazards still fail closed (FCR31 control/condition tracking has since been added), so these cases cannot be used to authorize GS timing. CTests exercise FPR and ACC cross-instruction dependency sequencing.

### Explicit host HLE entry boundaries

Native and RAM instructions, annulled slots and recognized EE startup/registered syscall HLE handoffs now share one bounded observation stream. `kind=3` identifies an HLE boundary, carries `hle_service`, and records the *current* instruction/slot ordinals without consuming any new slot or time. The runner also exposes a cumulative `hle_timing_boundaries` diagnostic. The offline replay rejects HLE boundaries as unmodeled rather than silently assigning guessed kernel time. This is not a calibrated HLE cost and does not enable GS/INTC events.

### Per-instruction candidate issue-cycle output

The offline analyzer now optionally writes `--candidate-events=<path>` after all profile arguments, producing `reference_slot,pc,opcode_word,kind,candidate_issue_cycle`. Each event is stamped from the candidate probe's own issue/reservation state, not from a host dispatch or guest instruction count. The CSV is created only for a complete, supported, non-truncated trace; replay failures produce no candidate timeline. These are experimental relative cycles and cannot drive VBlank.

### Independent reference differential (fail-closed)

`tools/ee_timing_differential.py` compares a **complete** exported candidate stream with an independently captured `reference_slot,pc,opcode_word,kind,reference_issue_cycle` CSV. It verifies every event identity, contiguous slots, nondecreasing issue times and exact relative issue deltas after removing the reference's arbitrary epoch. It rejects differences, missing rows, malformed/out-of-order records and host HLE events. Exact matching of one fixture **does not** prove all game opcodes or R5900 pipelines are calibrated, and it never installs the validated scheduler source. The companion exporter and comparator CTests are synthetic acceptance fixtures only.

### Optional live timing-observation sink

The native LLVM emitter now invokes an **internal** function-pointer observer (no extra external JIT helper symbol) for reached instructions and annulled slots. The RAM interpreter invokes the same callback signature after successful committed steps; host HLE entry records also reach it with the normalized service. The sink is unset by default. Rejections latch a host-only failure (not a guest exception, cycle tick or interrupt). The stock runner does not install any timing sink or validated hardware clock; it fails closed if an installed observer later rejects a native instruction. These hooks permit a separately calibrated issue-time producer to consume one unified ordered stream without promoting the current experimental candidate to hardware time.

### Rejection safepoints

An installed observer that refuses a RAM-interpreted committed step now terminates that interpreter immediately with `TIMING_OBSERVER_REJECTED`; the runner checks the same failure latch at the start of each host dispatch iteration so HLE continuations cannot silently proceed. Native translated code checks at the dispatch boundary. The default observer is null, preserving the baseline boot. No issue model or observer installs an EE timing source implicitly.


### Optional functional-unit initiation-interval probe

A new isolated scoreboard accepts explicit per-instruction unit identities
and positive initiation intervals, reserving MAC0, MAC1 and shared scalar
COP1 next-issue timestamps without changing the runtime clock. A zero
interval for a named unit, an interval for `NONE`, or an unknown unit
fails atomically. `c_ee_issue_trace_replay` exposes
`m_require_throughput` to reject timed unit operations that lack an
explicit interval. The CLI accepts one optional seven-value
`--throughput=...` group and marks the stream `throughput_mode=explicit`.
Without it the older latency-only profile remains an **unvalidated,
incomplete** offline replay (diagnosed as `unconfigured`).

The shared COP1 scalar unit, cross-cycle WAW deferrals and incomplete
pipeline occupancy are conservative **candidate** choices, not evidence
that these values reproduce R5900 hardware. No automatic enablement of
GS/INTC, no new local game boot and no calibrated scheduler source.

### Optional WAW-overlap comparison and candidate result deadlines

An explicitly selected counterfactual WAW policy can issue consecutive
MAC/COP1 writers at caller-configured initiation intervals without
automatically stalling until the previous register result is ready.
RAW dependencies continue to wait. Any newer result that would finish
before **or in the same cycle as** an older write is rejected without
mutating probe state; this does not silently manufacture a serialization
delay. Default behavior remains conservative. The host runner does not
select this policy.

The offline CLI accepts `--ordered-waw-overlap` only with an explicit
seven-value `--throughput` profile. The optional
`--candidate-completions=PATH` export records issue and result deadlines
separately, with no completion event for annulled slots.
`tools/ee_timing_differential.py --compare-completions` compares both
cycle fields against an independent reference, preserving instruction
identity and a single source epoch. Test vectors are **synthetic**.
These timestamps are **not** EE retirement, cache, HLE or scanout cycles.
No device timing source was installed and M1-B remains blocked.

### COP0 and VU0 transfer architectural-resource coverage

Decoder-recognized MFC0/MTC0, QMFC2/QMTC2 and CFC2/CTC2 now
retain separate COP0/VU0 VF/VU0 VI *instruction masks*, but the
macro-mode issue scoreboard now aliases VF[n]/VI[n] by register number
for n=1..31 and excludes VF00/VI00 from ordinary dependencies
(VU User's Manual v6.0 section 5.4.2). Every transfer class
still requires an individual
positive latency supplied explicitly by the caller; the analyzer's
optional last profile group is
`<cop0-transfer> <vu0-vf-transfer> <vu0-vi-transfer>`.
These conservative classes do not pair with other instructions
until independently verified issue rules become available.
They preserve dependencies across native/interpreted observation
boundaries without introducing any runtime EE clock source.

The coverage **does not** model COP0 implicit updates or
EI/DI/ERET, VU0 macro operations, BC2, VU0 memory traffic,
packed MMI or cache pipeline behavior. All continue to fail
closed. Synthetic COP0/VU0 transfer regressions are not physical
R5900 reference timing evidence. No M1-B game-boot validation.

### EE Core manual audit (18 November 2026)

See [M1_A_R5900_REFERENCE_AUDIT.md](M1_A_R5900_REFERENCE_AUDIT.md) for
primary-manual provenance, Table 1-2/1-3 logical-pipe combinations,
Table 2-12 latency/throughput values, the FCR31 scoreboard extension,
separate DIV.S/SQRT.S/RSQRT.S profile fields and explicit outstanding
pipeline/cache/throughput/HLE timing gaps. The offline probe now supports
documented ALU/branch, load/store, COP1 move/operate and MAC0/MAC1
logical-pipe combinations with strict dependency gating. A manual Y
resource-hazard pair still does **not** qualify as a modeled same-cycle
pair. Annulled delay slots cannot accept the next issued instruction.

All new tests continue to use synthetic candidate profiles. No observed
R5900 hardware issue-cycle trace has been validated, and no GS/INTC
timing source is installed. Do not treat the manual's latency list as a
complete guest-cycle or boot-validation timeline.


### Native JIT observer regression

`ee_timing_observer_ir_runtime` runs a generated BLTZALL function with a real C-compatible callback pointer installed in a properly sized runtime instance. It verifies exact reached/annulled event PCs, opcodes, attempt/slot ordinals, absence of guest delay-slot effects and first-rejection latching. This fixture does not publish EE cycles. Native and RAM rejection now have separate test coverage.


## Reference-compatible HLE boot timing (pragmatic compatibility mode)

In `--hle-startup` mode, the native runner now defaults to a
`reference_compatibility` event clock. `--ee-clock=off` disables it.
This follows `twist84/ps2-web-recomp:SDBZ`'s guest-execution-budget and
event-deadline architecture, not its R5900 instruction pipeline accuracy.
The native runner currently charges one approximate budget unit per reached
guest instruction at dispatch safepoints. It does not calibrate real EE
pipelines, HLE syscall time, cache stalls, DMA contention or VU timing.
If a loop-header watchdog is configured, compatibility mode defaults to
up to 256 bounded safepoint resumes when no resume budget was supplied.
It never converts host dispatches, loop visits or elapsed wall time to cycles.

Compatibility time advances **the existing** scanout/CSR/INTC/VSync path;
no flag, frame or interrupt is injected to skip guest code. The boot JSON
and console explicitly identify `reference_compatibility`, distinct from
`validated_r5900`. Tests verify its monotonic event deadlines and real
hardware-delivery route. This is a practical boot mode, **not** evidence of
hardware-accurate EE timing or completion of M1-A calibration. A new matched
real-game boot is needed to check whether SDBZ passes `0x00175210` and
which actual next system blocker appears. Offline issue-timing tooling remains
available for later accuracy work without blocking HLE-first boot progress.


### Matched 23:24 boot — first reference-compatible field and INTC HLE handoff

Matched USA diagnostic batch `20261118-232435` confirms the reference
compatibility clock delivered the first GS field: `ee_cycles=9001660`
(approximate), `gs_fields_started=1`, `gs_csr=0x2008`,
`INTC_STAT=4`, and `INTC_MASK=0x804`. The guest registered a VBlank
INTC handler at `0x00104b30`, cause 2, then took genuine exception code 0
at vector `0x80000180` with `EPC=0x00175210`. The missing capability
is HLE-first INTC interrupt dispatch, not missing VBlank timing.

The existing registered guest IRQ callback owner now dispatches both DMAC
and INTC: it acknowledges only a genuinely masked pending INTC bit through
the hardware W1C register, runs ordered registered guest handlers on the
existing kernel callback stack, stops on negative handler return, restores
full interrupted EE/VU0 context, clears EXL and retries EPC (not EPC+4).
The bridge excludes branch-delay interrupts and all other exceptions,
which remain unsupported. No synthetic timer, VBlank or handler result.
A focused regression checks real scanout->COP0 IP2 exception, INTC W1C
acknowledgement and ERET resume. A subsequent matched real-game run must
verify actual guest INTC callback execution and progression beyond the
previous exception at `0x80000180`.

## Accuracy direction (PR #156, 25 November 2026)

Baseline: 54/55 original exact transcripts, all five CI workflows green
at `4aa9cdf2afffe0a5b8a0b706841b33d1a0456946` ([run 45662090326](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45662090326)).
EFU remains the sole transcript mismatch. Passing COP0 changed/unchanged
counter checks does not validate exact processor-cycle measurements.
The offline issue replay now exposes a manual cache-hit profile with
explicit provenance and distinct LWC1 FPR latency; see the reference
audit. Next accuracy work: documented pipeline resource stalls, measured
cache behavior, precise retirement/exception accounting, and independent
hardware counter/trace comparisons. Maintain original ELF and retail
regression evidence separately from timing-model validation.
