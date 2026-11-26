# EE execution horizon: barrier audit and acceptance gates

The horizon is advisory only. An observed quiet native boundary is not a
pre-execution guarantee. No deferral or chaining is enabled by this audit.
All batches require final-head CI (Build and test, original ps2autotests,
native WebGPU, SIF DMA, ROM0), unchanged shadow-on/off architectural state
and compatibility timestamps in directed fixtures, and no new EE IR
provenance changes unless explicitly necessary.

| Batch | Audit scope | Explicit success criteria | Status |
| --- | --- | --- | --- |
| P1-A | Timer/delegated EE MMIO | Accepted and rejected accesses advance the generation; passive queries do not; MMIO-free plus touched equals lookahead candidates. Enumerate direct helpers separately. | Observation implemented; direct helpers open |
| P1-B | DMAC, INTC, deferred VIF1, VU0/VU1 | Pending IRQ/DMA, running VUs and deferred work prevent optimistic lookahead; directed tests cover start, completion, cancellation, error and state changes inside native execution. Prove no unobserved publication path. | Started-channel observation and narrow passive HLE SIF0 exception added; transition audit open |
| P1-C | Asynchronous GS worker | Idle healthy worker is quiet; queued or executing batch and failed worker are not quiet; drained healthy worker is quiet again; no new fence or guest event. Headless shadow reports zero GS barriers. Any submission after the snapshot requires an independent pre-execution guard. | Entry observation implemented; CI pending |
| P1-D | Direct runtime helpers, COP0, exceptions | Enumerate memory/device callbacks and COP0 writes that bypass timer MMIO; prove a barrier before guest-visible access, trap or interrupt changes; test Count/Compare, Status, ERET and both delay-slot states. | VU callback and COP0 endpoint observations added; pre-access guards open |
| P1-E | SIF DMA/RPC, HLE and shared memory | Pending calls/replies/binds/module starts and runnable IOP prevent extended idle lookahead; new requests flush debt before observing shared state; differential tests cover both directions, ordering, faults and callback re-entry. | RPC ownership observation added; debt flush and shared-memory audit open |
| P1-F | Clock, timer/scanout reprogramming, host continuation | Verify fractional 8:1 accounting and preserved outstanding debt; unknown/overflow/overdue deadlines fail closed; test equal-time events, deadline replacement and continuation before service. | Aligned checkpoint guard implemented; pre-access ordering open |

## GS worker observation (P1-C)

The runner already treats pending DMAC/INTC, deferred VIF1 and running
VU0/CMSAR1 as entry barriers. GS worker publication was not included.
The WebGPU mirror is a committed-event observer, not a guest DMA
transaction; its worker can nevertheless finish or fail asynchronously,
and the existing service checkpoint checks its health.

The new worker quiescence query observes job count and failure under the
existing mutex. The job count includes the executing batch, not merely
queued batches. It is consulted only during opt-in shadow profiling
when no previous entry barrier is active and asynchronous GS is enabled.
The counter `gs_worker_barriers` counts additional entry barriers from
this check (not all busy observations). The mutex does not wait for GPU
completion or drain the worker, but shadow profiling overhead can increase.
The existing device service and failure handling are unchanged.
A worker can receive new work later in the same native boundary; this
snapshot cannot authorize execution deferral. GPU command completion is
also distinct from renderer-worker quiescence.

Directed worker regression holds consumer work until the producer has
observed nonquiescence, adds a queued batch, releases the consumer and
checks drain, plus failure rejection. The existing headless timer
shadow-on/off fixture requires zero GS worker barriers.

## DMAC started-channel observation (P1-B)

`pending()` is masked completion IRQ status, not channel activity.
`started_channels()` reads CHCR.STR for all nine modeled channels,
including IPU, SPR and both SIF directions; it does not invent channel 7.
The shadow profiler counts otherwise-unblocked started channels in
`dmac_started_barriers`, before the GS worker check. Tests distinguish
unmasked/no-IRQ VIF1 and SIF0 starts, clearing an individual STR bit,
and reset. The query changes no guest register, timer or service cadence.
A permanently armed receive channel can be inactive in practice, so this
is a deliberately conservative upper bound on required barriers, not an
assertion that each started DMA will progress immediately. Before enabling
deferral, distinguish dormant channels using proved producer/consumer
dependencies, and cover transitions/completion/faults and native accesses.
The new classifications can reduce the earlier shadow candidate counts;
do not compare them as performance measurements.

## Retail validation and passive receive refinement

The 40,000,000-dispatch replay at the coalescing validation stage
completed at PC 0x0019d2a0 (DISPATCH, status 1), with zero final debt.
44,481,558 logical IOP service turns became 43,527,167 physical
kernel runs: 954,391 saved calls in 942,867 combined batches.
This is a 2.15% call-count reduction, not a matched wall-time speedup.
The replay had 39,993,844 barrier samples, of which 23,839,190 were
attributed to the blanket started-DMA check. Only 6,044 projected
normal exits were MMIO-free and observationally direct-helper-quiet.

The HLE SIF0 channel can remain armed in receive-chain mode with
zero QWC, no bound FIFO start callback, no latched chain end and no
raw DMA5 completion. In HLE scheduling mode only, when SIF0 is the
sole started channel and RPC ownership/reboot are absent, the shadow
model now retains the normal IOP deadline rather than treating
readiness as an autonomous DMA event. IOP lookahead still requires
the kernel idle-event projection. SIFMAN guest calls can publish
directly to EE RAM during IOP execution, so no real scheduling is
changed by this exemption. Other started channels and genuine FIFO
transports remain barriers.

`dmac_str0` ... `dmac_str9` count started-channel observations after
the earlier entry/clock barriers; they can overlap.
`dmac_sif0_only` counts masks containing only channel 5;
`dmac_passive_sif0` counts the narrower exemption.
`entry_callback`, `entry_idle`, `entry_irq`, `entry_vu`, and
`entry_other` are independently counted entry states and may overlap.
Mask histograms are reduced only at report time to minimize hot-path
profiling overhead. `dmac_top_mask` and `dmac_top_mask_count` identify
the most frequent nonzero exact STR mask among otherwise eligible
entries (zero/count zero means no nonzero mask was observed).
Do not sum the overlapping counters as exclusive barrier counts. The prior retail log
did not contain channel attribution, so SIF0 dominance is a hypothesis
to test, not an established result.

Tests cover nonzero QWC, genuine FIFO binding, raw completion, stop
and wrong transfer mode. The native idle coalescing fixture now also
arms SIF0 and checks the shadow classification without changing the
A/B architectural or IOP clock comparison.

## Direct-helper observations (P1-D)

The translated VU synchronization/start/control callbacks bypass EE timer
MMIO. Install counting wrappers only for opt-in shadow runs; the original
callbacks and return values remain unchanged, including failed or no-op
calls. The existing host service call is not wrapped. The runner observes
the generation before and after each measured native boundary.

COP0 Count/Compare, Status, Cause, EPC, ErrorEPC, core clock anchor and
latch, and performance control/fault are compared at the same endpoints,
before subsequent device service. A changed clock anchor can expose a
Count synchronization even when Compare is unchanged. BC0 already
uses the instrumented MMIO reads of D_PCR/D_STAT. Endpoint equality is
not proof of no COP0 access: a write restored within a block, an unchanged
read, or an unaudited register can be invisible. VU operations that
never invoke these callbacks are also not covered by the generation.

`vu_callback_boundaries` and `cop0_changed_boundaries` count all valid
shadow observations, including exceptions; these can overlap.
`mmio_free_direct_touched` and `mmio_free_direct_quiet` partition only
MMIO-free projected normal exits, according to either observation.
Their sum must equal `lookahead_mmio_free`. Quiet is observational,
not a safety guarantee. No runtime ABI or translated IR changes.

Directed snapshot tests exercise register, clock and performance changes.
The native COP2 fixture compares shadow on/off architectural reports
and requires observed VU and COP0 activity; the timer fixture verifies
the partition and bounds. CI pending. Actual pre-access safepoints remain
open and are a prerequisite to deferral/chaining.

## SIF/RPC ownership (P1-E)

The HLE RPC pump can write module results and reply payloads directly
to EE RAM before raising DMA5; these stores bypass the timer MMIO
generation. A DMA5 completion bit remains significant regardless of
the interrupt mask. The receive handler can retain ownership even
after acknowledgment and after the reply queue becomes empty.

`c_sif_rpc_bridge::scheduler_barrier()` combines raw DMA5 completion,
EE receive-busy and the four pending RPC queues. The shadow IOP
projection retains the original cadence when this returns true or
the HLE IOP reboot bootstrap is pending. Directed tests cover a
pending bind, completion status with the IRQ mask disabled, acknowledged-but-busy
receive ownership and a fully released receiver with empty queues.
This changes no pump, IRQ, RAM store, or IOP service behavior.

The genuine hardware SIF0/SIF1 transports pump on start callbacks,
and are used by the ROM0 path, already excluded from lookahead.
For HLE, a syscall can initiate SIF1 directly and the IOP reply
pump can publish RAM asynchronously with respect to EE dispatch
boundaries (but synchronously on the runner thread). Do not enable
deferral until pre-access debt flushes and callback ordering are
proved. HLE reboot and all reply ownership states remain conservative.

## Clock and continuation checkpoint (P1-F)

A projection requires a reference-compatible source, a non-sentinel
published timestamp equal to the checked live instruction/idle/HLE
budget, and (when IOP scheduling is active) a matching IOP clock
anchor, zero debt and a representable next cycle. An otherwise
unblocked stale checkpoint increments `clock_barriers`. All three
shadow horizon estimates reject it. Tests cover initial anchoring,
unpublished slots, fractional cycles, a 300-unit HLE continuation,
partial consumption and overflow.

The existing runner publishes HLE/RAM continuation time at the outer
loop before IOP service. The ordinary post-native device checkpoint
publishes time before COP0/VU/DMA/timer/scanout processing. Deferred
VIF1 is also polled at outer-loop entry before publication; direct
HLE IOP runs and receive callbacks remain separate barriers. These
paths have not been converted to deferred execution.

## P2 preliminary optimisation: same-checkpoint idle IOP coalescing

`SDBZ_IOP_COALESCE_IDLE_TURNS=1` is experimental and off by default.
Within the existing scheduled IOP service loop, combine only complete
legacy turns (256 cycles each, except the final debt tail) strictly
before the known idle horizon. A batch never exceeds outstanding
debt or the remaining logical 64-turn cap. Unknown/zero horizon,
runnable IOP, pending IRQ, software event, DMA completion, SPU2
sample boundary, Timer5 edge, RPC ownership, pending HLE reboot and
unrepresentable cycle sums prevent a combined turn. The existing
idle horizon rejects ROM0, AutoDMA, CDVD and interrupt ownership.
On unexpected guest execution, timestamp divergence or stop reason
during a combined turn, abort rather than retry mutated guest state.

No IOP time is deferred across EE native functions or host services.
The combined interval has no modeled guest execution or event;
intermediate RPC pumps would have no work. Retain the original
single-turn path at or beyond any predicted edge. The ordinary
128-instruction quantum, logical turn accounting, fractional clock
and outstanding debt are retained.

Success criteria:
- Planner reproduces exactly the constituent legacy turns and refuses
  crossing or reaching the known event; test partial tails and cap.
- Partitioned/combined kernel comparison agrees before and after
  HLE callback and SPU2 sample delivery, including Timer5 phase.
- Native ELF integration compares final EE reports and IOP/SPU2 clock
  lines with opt-in disabled/enabled and must save actual service calls.
- Final-head CI passes; bounded retail A/B compares matched workload
  and checks `IOP clock coalesce:` counters and guest behavior.
- No default enablement or EE IR regeneration for these host-only changes.

`logical_turns = physical_runs + saved_turns` counts original service
turns vs kernel calls in the scheduled path. `combined_batches` counts
combined calls, `debt` reports remaining scheduled debt at stop.
Direct HLE service calls are not part of these counters. Saved turns
are not measured wall-time improvement.

## Second retail replay and source-level pre-execution bounds

All five CI workflows passed at 945d831. The subsequent 40M-boundary
replay confirmed the dominant STR mask was 0x020 (SIF0 only),
21,119,805 observations; 17,517,263 qualified as passive. Started-DMA
barriers fell to 4,045,121 and total barriers to 23,907,296.
14,556,200 normal exits preceded the projected IOP deadline;
14,175,254 were also MMIO-free and endpoint/direct-helper-quiet
(35.44% of sampled boundaries). Coalescing saved 813,587 of
42,296,753 logical turns (1.92%) with zero debt. These runs differ
in workload and no matched wall-clock speedup is established.

The dominant remaining aggregate entry barrier was VU-related
(18,223,091). Split it into `entry_vif1_pending`,
`entry_cmsar1`, and `entry_vu0_running`; the counts overlap. A
deferred VIF1 worker can complete at any host timestamp and publish
committed DMA/GS effects at a poll. CMSAR1 uses instruction-attempt
budgets, and VU0 has its own next-attempt scheduler. None is safely
interchangeable with an EE cycle deadline. Preserve all existing
service/poll barriers until a separate ordering proof exists.

P3 groundwork: source-level register-only paths are cached per
exact ELF PC in shadow mode. The bounded analysis accepts up to
32 slots (including a mandatory nontrapping ALU delay slot) ending
in JAL, canonical JR or JALR. It rejects conditional branches,
memory, COP instructions, arithmetic traps, unknown encodings,
missing bytes and PC overflow. The bound must fit strictly before
a finite projected deadline. The native outcome is compared against
the predicted slot count, attempted instructions, normal one-entry
exit and MMIO/VU/COP0 observations. `alu_bound_candidates =
alu_bound_matched + alu_bound_mismatch`. A mismatch is diagnostic;
no new execution or failure path is introduced.

These are source-level candidates, not native-entry certificates:
the ELF bytes do not independently establish the loaded object's
provenance, its exact CFG entry ownership, all runtime helper effects
or all host continuations. The proof must bind to the actual loaded
native owner and preserve pre-access debt/interrupt ordering before
enabling chaining. The synthetic native coalescing replay requires
a positive matched bound and zero mismatches; smoke tests reject
unsafe operations and equal deadlines. All changes are host-side.

## Native owner and linked IR certificate (P3 groundwork)

The optional `ee_game_resolve_owner` library export validates the
exact nonzero guest PC and callable pointer against the dispatch
table's original first-owner index. Both chunked/full and plain
data-dispatch overlay libraries expose it. A null/unknown target,
incorrect pointer, legacy module without the export, or sentinel
PC zero cannot certify an owner. Replacements refresh the exports
and clear overlay identity alongside native target invalidation.

`SDBZ_NATIVE_ALU_CERTIFICATES=1` enables certificate extraction at
link time. It reads the IR for each native object only when its SHA256
matches the verified object manifest. A narrow parser requires the
canonical entry PC switch and one unguarded straight-line LLVM block
with only exact instruction-count calls, integer ALU, and GPR/PC/exit
state accesses. The block must return normally, with 2..32 contiguous
guest instruction records ending in a JAL/JR/JALR and delay slot.
It rejects unknown IR, helper calls, branches, invalid CPU fields,
unavailable/mismatched IR, duplicate native owner VAs and entry aliases.
This relies on the existing verified emitter/object provenance, not
on treating arbitrary untrusted IR as safe.

The linked library optionally exports `ee_game_alu_words`, returning
the embedded exact opcode sequence only for the correct first owner
and callable target. The runner additionally checks the source-level
opcode whitelist, strict deadline, source words and native execution
observations. `alu_owner_known` / `alu_owner_matched` record loaded-owner
coverage; `alu_certified` / `alu_certified_matched` are narrower subsets.
These remain shadow-only; no host service or native boundary is skipped.
The instruction counter helper can invoke a timing observer or append
trace data, so the future live path must separately guard diagnostic
observers and all host continuations.

Certificate mode and script are host-link inputs, not guest lowering
inputs. Enabling it needs a DLL relink, not new EE IR/object emission.
Missing certificate export or absent per-entry certificate falls back
to ordinary dispatch. The parser intentionally rejects many legal
native paths (including multi-block CFGs and guarded loop headers);
false negatives are acceptable. CI tests exercise source/IR digest
mismatch, malformed/unsafe IR, first-owner overlap, stale pointers,
cache provenance split, and a real native synthetic certified exit.

## Certificate integration correction and direct quiet IOP service

The first complete native certificate regression at 5d0ab63 reached
one matching source-bound/native-owner candidate but zero certificates.
The emitter inserts standalone guest instruction comments (including
the delay slot), which the strict parser rejected. Standalone comments
and blank lines are now ignored; inline syntax and actual instructions
remain subject to the same grammar. The Python fixture includes the
emitter's comment form and the native integration still requires a
positive certified match.

`SDBZ_IOP_FAST_QUIET_TURNS=1` (off by default) is a separate
experimental host optimization. It tries to advance a scheduled
IOP turn directly only when the kernel is idle, the requested
nonzero budget is strictly before a finite idle-event deadline,
cycle arithmetic is representable and no RPC ownership, reboot or
timing observer is present. It invokes the same hardware
`advance_cycles` and increments the kernel clock by the same amount.
The existing RPC pump, EE clock observation, logical turn cap,
cycle debt consumption and all EE checkpoints remain in place.
Rejected turns use the unchanged general kernel run path, including
turns reaching exactly the next event. No IOP time is deferred.

`IOP clock quiet:` reports attempted shortcut calls, successful
advances, fallbacks and total directly advanced IOP cycles. If both
optimizations are enabled, `physical_runs` in the coalescing report
counts service invocations (ordinary runs plus direct quiet advances);
the actual number of general kernel runs is that value minus
`advanced`. The `saved_turns` identity still measures coalescing
alone. Neither counter alone establishes wall-time speedup.

The three-way native fixture compares ordinary scheduling, coalescing,
and coalescing plus quiet advancement using identical EE state and
IOP/SPU2 clock output. Directed kernel tests preserve the original
partitioned-versus-combined comparison and add the direct path,
including ready-thread rejection, event equality and subsequent
callback/sample delivery. No guest lowering changes.

## Cached native certificate identity

The shadow identity lookup now uses a bounded, exact-PC cache keyed
by guest PC, callable pointer and source slot bound. A cache miss
checks the original first owner and compares the linked certificate's
words against immutable ELF bytes. A hit reuses only that exact result.
Negative results for a known pointer are cached, but null targets
are never cached because a later overlay may resolve them. Full
replacement clears the proof cache before unloading any native
module. Appending an overlay cannot displace an already resolved
first owner. No allocation is made when horizon profiling or direct
dispatch is disabled.

`alu_identity_hits` and `alu_identity_misses` count proof lookups;
they do not imply an actual scheduling optimization. Directed tests
cover colliding PC keys, changed pointers and slot bounds, disabled
and null entries, failed owner identity, and replacement invalidation.

Before enabling a live chain, validate the linked certificate coverage
and direct quiet IOP fast-path counters. A useful chain must preserve
the runner's per-boundary host continuations (interrupt delivery,
thread scheduling, VU/GS worker publication, diagnostic and window
service), not merely the native exit reason. The compatibility clock
and IOP fractional remainder must account for every original boundary;
at the dispatch budget limit the last EE slice's IOP time is still
unserviced under the baseline cadence. A chain must not pay that final
interval early. Reject or fall back before executing a path that
cannot prove these requirements.

## Third retail horizon and quiet service replay (db10666)

All five CI workflows passed. The opt-in 40,000,000-dispatch
retail run stopped at 0x001082a0 with DISPATCH/status=1 and zero IOP
debt. There were 1,616,595 source-bound matches and zero mismatches;
261,268 also had matching linked IR certificates (0.653% of dispatches).
The identity cache hit 1,609,934 times and missed 6,661 times.
The projected quiet count was 13,003,240. VIF1 deferred transactions
accounted for all 19,001,061 VU entry barriers, with no CMSAR1 or
VU0-running barriers. These are overlapping observations, not a
wall-time profile.

IOP quiet service accepted 26,628,385 of 30,530,526 attempts,
advancing 514,464,166 IOP cycles. Coalescing saved 844,276
of 42,482,932 logical turns, leaving 41,638,656 service invocations.
After subtracting direct quiet advances, 15,010,271 ordinary kernel
runs remained (64.67% of logical turns avoided the general scheduler).
This is a reduction in invocations, not measured speedup.

## VIF1 host-only overlap timing opt-out

`SDBZ_VU1_NATIVE_OVERLAP_PROFILE=0` disables only the per-native
boundary overlap timing (default remains enabled). It bypasses
the controller's native interval start/end clock and worker timestamp
reads, including submission-time overlap capture. Worker execution
and wait timing, command counts, controller polling, DMA/IRQ
publication and explicit joins are unchanged. The worker JSON
`native_overlap_profile` field distinguishes unavailable overlap
metrics from a genuinely zero measured overlap. Directed VIF1
transaction tests run with both modes, including in-native job
submission, pending-state isolation, late rejection and reset.
This does not authorize removing any worker completion checkpoints.
No guest IR lowering changes. Measure paired retail wall time before
adopting the opt-out as a performance preset.

## IOP clock hot-path simplification

At 1e87910 all five workflows passed, including the VIF1
overlap-instrumentation mode regression. The SPU2 common clock-only
advance now inlines into its caller, reusing the before/after sample
quotients; only a sample transition calls the unchanged mixer/IRQ
edge operation out of line. Inactive clocks, deadline invalidation,
fractional phase, and multi-sample advances retain the original
semantics. The idle hardware horizon also reads the two ordinary
DMA countdowns directly after rejecting AutoDMA, avoiding the
general refill-aware query. Added partitioned versus multi-sample
clock coverage with exact sample phase, mixed frames and IRQ state.
These are host-only changes, independent of EE IR generation.
No measured wall-time improvement claimed.

## IOP host hot paths and exact checkpoint comparison

All five workflows passed at db9f4f42, including ordinary DMA4/DMA7
overlap, cancellation, rearm and the AutoDMA idle barrier. The mode-zero
Timer5 fast path now uses unit-rate unsigned wrap for advancement and
the same rollover deadline, with all prescaled/compare modes left on the
existing path. Directed tests cover wrap, target writes while disabled,
prescaler remainder, mode reset and compare IRQ delivery. The shared
ownership check for coalescing and quiet service is evaluated once per
physical turn and never reused across hardware advance or RPC pump.

`SDBZ_IOP_CHECKPOINT_TRACE=1` emits a deterministic stderr line after
each scheduled IOP checkpoint. It records EE/IOP timestamps, logical
turns, cumulative executed IOP instructions, debt/next deadline, masked
and raw IOP IRQ state, Timer5 count/mode/target/remainder/armed state,
ordinary DMA deadlines, AutoDMA/CDVD activity, SPU2 clocks and frames,
EE DMA/INTC, SIF flags and RPC pending state. It uses passive reads:
in particular, I_CTRL and Timer5 MODE are not read through guest MMIO.
A directed test verifies that masked and read-clear latches survive
the diagnostic. The bounded synthetic native fixture compares the
complete checkpoint lines and final architectural state for all four
combinations of quiet service and coalescing.

This is a checkpoint comparison, not a complete ordered event trace;
two different intermediate event orders can still yield the same
snapshot. It does not authorize cross-native deferral or chaining.
Tracing is disabled by default and should not be enabled in performance
benchmarks. No EE guest lowering or runtime ABI changes. Final CI
and matched retail wall time remain unverified.

## Matched controller-input retail experiments

All five workflows passed at b0c4fff, including the four-way checkpoint
comparison and replay provenance. Subsequent host-only changes avoid
sample-phase arithmetic for a valid absent SPU2 IRQ opportunity and
simplify the ordinary/AutoDMA two-channel deadline selection.

For P40, set `SDBZ_PAD0_RECORD=1` (and unset
`SDBZ_PAD0_REPLAY`) to log one raw active-high button mask and four
axis bytes per authentic connected port-0 SIO2 0x42 poll. The sequence
continues across IOP resets and records reset-local poll number, pad
mode/config flag and packet sizes. A complete recording ends with
`PAD0 input summary: record N` after the dispatch budget is exhausted.
A cancelled or incomplete run has no valid completion footer.

Copy the recorded boot.log outside the replay work directory before
launching again. Unset `SDBZ_PAD0_RECORD`, set
`SDBZ_PAD0_REPLAY` to the absolute path of that copy, and replay with
identical dispatch budget and initial memory-card state. Live input
is overridden at each actual poll; no extra packet, interrupt or poll
is manufactured. Cross/script probes and recording/replay cannot be
combined. A malformed, truncated or mismatched stream is rejected;
exhaustion is not silently filled with neutral input. A successful
replay reports `PAD0 input summary: replay consumed=N total=N valid=1`.
The parser accepts at most one million samples. Multiple concatenated
recording sessions are rejected.

Use the same recorded stream for both baseline and experimental
runs, restoring the initial memory card where necessary. Compare
final architectural state, guest fields, VU1 work and progress PCs
as well as wall time. Matching controller input alone does not prove
deterministic guest execution or event order. The provenance JSON
includes the replay source SHA-256, not its local path. Tests cover
stream validation, axes/buttons, reset-local numbering and actual
SIO2 digital response bytes. No guest lowering changes. Final CI and
Windows retail capture/playback remain to be validated.

## Remaining activation gates (not implemented)


P2: baseline-versus-experimental deterministic event traces must match
for EE/IOP debt, timer/scanout, interrupt delivery, DMA/RPC, shared
memory and fault paths. Any unresolved path selects baseline scheduling.
Keep the experimental path opt-in and validate retail before default use.

P3: prove per-block worst-case budget and pre-access barriers, preserve
PC/register/delay-slot/exception and indirect dispatch semantics, and
fall back before an unknown or due event. Compare against baseline and
P2; do not increase the existing dispatch slice as a substitute.

P4: DMA/GIF/GS optimization must preserve ordered packet and register
effects, framebuffer/readback results, IRQ and DMA completion semantics.

P5: carried counter/timer loop acceleration requires exact register
state, retired counts, delay slots, timer/RAM observations and safepoints.

P6: record comparable title and gameplay workloads separately, with
matched input, exact revision, opt level, clock, renderer and audio.
Minimum target title 1000%, sustained gameplay 100%; primary targets
1500% and 300% respectively. Shadow candidate counts are not speedups.
