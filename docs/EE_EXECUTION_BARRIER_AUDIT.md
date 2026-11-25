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
