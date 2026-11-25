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
| P1-B | DMAC, INTC, deferred VIF1, VU0/VU1 | Pending IRQ/DMA, running VUs and deferred work prevent optimistic lookahead; directed tests cover start, completion, cancellation, error and state changes inside native execution. Prove no unobserved publication path. | Started-channel entry observation added; transition audit open |
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
