# COP0 performance event accuracy (#160)

Reference: EE Core User's Manual v6.0, chapter 7, particularly Table 7-1
(p. 146) and event descriptions (pp. 147–149):
https://docs.alexrp.com/mips/ee.pdf

The original 55-console-transcript baseline is exact, but the original COP0
performance test checks register aliases and whether selected counters change,
not calibrated cycle counts. The runtime uses a reference-compatibility clock
or a deliberately explicit validated-source test fixture. The offline timing
candidate in #159 is not a hardware cycle producer.

## Existing runtime contract

| Event family | Current handling |
| --- | --- |
| Processor cycles (both PCRs, selector 1) | Existing approximate compatibility budget, or explicitly supplied validated-source fixture delta |
| PCR0 selector 0; both selectors 17–31 | Reserved encodings accepted as inert by existing compatibility policy; hardware behavior unverified |
| Both selectors 16 | No event |
| PCR1 selector 0; selectors 2–15 | Active selection rejected; no invented event increments |
| CTE clear or no enabled mode | Configuration accepted; no count |
| ERL set | Counting inhibited |
| Overflow | Whole interval rejected before either counter mutates; Level 2 exception delivery not implemented |

The manual distinguishes single-issue cycles (PCR0 selector 2) from dual-issue
cycles (PCR1 selector 2), and issued branches from completed instructions.
An instruction attempt, host dispatch or offline candidate cycle must not be
substituted for those events.

## Batch 1: selector/mode coverage

The existing COP0 smoke test now exercises both counters, all 32 selectors,
all 16 privilege-enable combinations, and both CTE settings (2,048
configurations), then tests synthetic interval accounting for each supported
configuration in each of the four modes. It also covers a second-counter
overflow after a successful first-counter candidate increment: neither value
may change on failure. ERL and CTE gating must remain effective at the
overflow boundary. This protects the existing runtime policy, not measured
hardware behavior. No production source or ABI is changed.

Next: explicit event availability and delivery contract for authoritative
producers; ordered exception integration needs Level 2 semantics and independent
validation. Physical console unavailable; keep hardware calibration open.

## Batch 2: explicit event-delivery model

Added `ee_perf_events.h`, an isolated Table 7-1 selector map and transactional
interval evaluator. Identical event identities (cycles, completed instructions,
non-BDS completions) can feed both PCRs; counter-specific selectors remain
separate. Each interval explicitly marks which measurements are available;
zero is valid only when supplied. Reserved selectors, missing events and
counter overflow have distinct outcomes. ERL, EXL and privilege gating apply
before delivery. Synthetic regressions cover every selector, shared events,
missing second events, explicit zero and atomic overflow. This model is not
installed in the runtime. The compatibility counter behavior is unchanged;
reserved selectors are rejected in the strict model rather than silently
assigning hardware semantics. No event source or Level 2 exception is fabricated.

## Batch 3: explicitly authorized runtime event delivery

A validated-clock owner can opt in to typed event delivery by declaring the
available event mask while CTE is clear. PCCR writes reject active selectors
outside that declaration (including reserved selectors in typed mode).
Event intervals require the validated source, a nonzero capability mask,
and no undeclared measurements. Legacy cycle advance is disabled while typed
mode is active to avoid double counting. The owner must split intervals at
control/status boundaries. No production owner registers this mode. The default
compatibility path and its prior reserved-selector policy remain unchanged.
The scheduler owns authorization and revokes it on reset. A compile-time
size assertion protects existing runtime member offsets on x64; a native retail replay
is advisable before integrating runtime/ABI changes. Synthetic fixtures
exercise source gating, missing/undeclared events, selector rejection,
configuration lifecycle and the return to the legacy cycle-only mode.

## Batch 4: scheduler-owned capability lifetime

Reset automatically revokes the producer capability mask. Installing another
validated clock does not restore the earlier producer's authorization. The
existing CTE gate requires disabling counters before reconfiguration. A test
exercises reset/reinstall with both issue events active and verifies that
neither legacy cycles nor typed events can increment those counters. No
production clock is activated.

## Windows header portability

The first Windows Clang P40 build stopped before guest execution because the
UCRT `OVERFLOW` macro expanded the scoped result enumerator. Rename that
status to `COUNTER_OVERFLOW`; no numerical result or counter semantics
change. The smoke test deliberately exposes the collision macro before the
transitive runtime header include, and checks that the header preserves it.
No cache deletion or IR regeneration is required specifically for this rename.

The standalone ROM0 link also exposed a new external scheduler-symbol dependency.
The capability setter is now inline, with the same validation and reset contract,
so standalone exception builds need no additional graphics/device scheduler linkage.

## Bounded retail validation — 2026-11-25 19:38

Archive: `sdbz-graphics-pr148-neutral-20261125-193852896.zip`.
After the Windows fixes and explicit `--p40 --overwrite-ir` cache refresh,
the O5 cache contains 16,250 modules. The replay reached 40,000,000 dispatches
with exit DISPATCH, final PC 0x0019d2a0, reference_compatibility clock, no
timing observer failure and no IOP guest fault. Native process wall 49.485 s;
profiled runner wall 44.830745 s, 892244.82 dispatches/s. VU1: 84 AOT
programs, 397854 native runs, 458826701 pairs, zero interpreter fallbacks,
zero captures, zero worker rejects/discards/host failures; one job pending
at the bounded stop. 4707 guest fields, 3389/3389 window presents,
4711029 GPU draws. CPU/GPU display PPMs are byte-identical (same display
source, not independent raster validation). Host audio disabled.

Previous PR #170 bounded run: 49.234 s process, 44.5755921 s profiled,
323777403 native VU1 pairs, 3176982 GPU draws. Workloads differ: current
SIO2 reports zero Cross-pressed polls out of 3300; EE pad observer reports
113 Cross-bit-low candidates and 352 analogue samples outside deadzone.
Archive lacks an explicit Git HEAD. Do not claim input-matched timing,
performance gains or calibrated hardware counter behaviour from this replay.

All five final code-head workflows at `4dcc82a` passed: [ROM0](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45685928942),
[SIF DMA](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45685928837),
[Native WebGPU](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45685928922),
[Build/test](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45685928832),
[Original suite](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45685928899).
Original transcripts 55/55; direct EFU 208/208. Typed events remain inactive
in retail; this is bounded compatibility-path regression evidence only.

## Post-I17F rebase and batch 5: revoked cycle-only typed producers

PR #173 was squash-merged as `c023fe8`. The original eight COP0
commits were replayed onto that main revision as `8e77050`; the
CMake-independent COP0 regression retained the newer main's Cause/IP
rules and compatibility syscall timeline. The original diff has seven
files and 595 insertions / five deletions after rebasing.

A typed producer may authorize only the processor-cycle event. Reset
previously erased its mask, allowing a newly installed validated source
to deliver legacy cycle deltas against the still-enabled PCCR without
explicit reauthorization. Distinguish a revoked typed source from an
ordinary cycle-only source with a sticky scheduler flag, stored within
existing x64 alignment padding. Repeated reset retains revocation;
legacy cycle delivery, new enabled PCCR writes and installation of the
compatibility source are refused until explicit validated-source
configuration with CTE clear. Disabling counters remains permitted.
An explicit zero capability declaration restores ordinary cycle-only
delivery; no production typed source is installed by this change.

Directed tests cover typed processor-cycle retirement, repeated reset,
rejection of legacy/typed delivery and compatibility substitution,
counter preservation, and explicit recovery. No new guest lowering,
calibrated cycle model or Level 2 exception delivery is introduced.
The scheduler size assertion remains; final rebased CI is pending.
No standalone retail replay is required for this inactive typed-source
path, but new runtime-header provenance must be observed when next
building retail IR.

## Batch 6: per-counter overflow planning

The typed event interval evaluator now exposes a read-only plan for
both PCRs: selected event identity, individual result, prospective
value, and the positive event ordinal at which the existing overflow
policy rejects delivery. The existing first-failure order and
all-or-nothing commit are retained. Missing measurements for one PCR
no longer conceal diagnostic evidence for the other PCR.

The boundary is in event-count space, not guest cycle space. In
particular, aggregates cannot establish which of two different
events occurred first, or the precise PC/BD state at overflow.
Already-high counters retain the existing fail-closed policy (first
positive event rejected), not a new hardware overflow claim.
Zero measured events and ERL inhibition do not create a crossing.
Directed tests cover both counters crossing, missing evidence on
either PCR, UINT64_MAX deltas, zero deltas and transactional commit.
No guest exception is synthesized, no default clock is changed,
and no production typed producer is installed.

This is preparation for ordered Level 2 exception delivery; independent
reference semantics and precise execution-boundary evidence are still
required. No standalone retail replay is needed for this inactive
typed path. New-head CI is pending.

## Batch 7: classify provable aggregate overflow order

The isolated interval planner can distinguish a single crossing, two
crossings of the same event identity (by positive event ordinal), and
equal ordinals of the same event identity. It deliberately reports
indeterminate for crossings of different event identities or incomplete
or reserved measurements. This classification is separate from the
existing transactional first-failure result; neither that result nor
the ordinal classification is a guest exception-priority rule.

Directed cases cover each order, simultaneous shared ordinals, different
events, unavailable versus measured-zero deltas, inhibited modes and
reserved selectors. No runtime call site is changed; no calibrated clock,
PC/BD attribution, Level 2 delivery or production producer is added.
The original fail-closed boundary policy remains unchanged. CI pending.

## Batch 8: manual-derived recognized exception entry projection

EE Core User's Manual v6.0, pp. 73, 75, 92, 97 and 150–151:
the recognized performance exception sets ERL and EXC2=2, updates
BD2 and ErrorEPC from the cancelled instruction, and selects its vector
using DEV rather than BEV. Other Status and Cause bits are preserved.
The pure constexpr projection is tested across DEV/BEV, EXL, delay
slot, prior EXC2 and BD2 combinations (256 cases), with a compile-time
fixture. It has no CPU/runtime call site, does not mutate registers,
and is not a guest exception recognition or delivery mechanism.

At PCSX2 revision 34670c6151d28dee8284e90f7957e470fc03f9d3,
pcsx2/COP0.cpp detects a high counter bit but comments out the
proposed Level 2 transfer; R5900.cpp also marks Level 2 handling
unfinished. These paths cannot validate precise delivery.
https://github.com/PCSX2/pcsx2/blob/34670c6151d28dee8284e90f7957e470fc03f9d3/pcsx2/COP0.cpp
https://github.com/PCSX2/pcsx2/blob/34670c6151d28dee8284e90f7957e470fc03f9d3/pcsx2/R5900.cpp
The ps2-web-recomp branch-specific source audit remains incomplete
(search did not return the source). Do not infer absence of support.

The printed predicate in section 7.2.4 conflicts with ERL inhibition
described in sections 4.1.3 and 7.2.6; do not copy that predicate
into runtime recognition. Section 7.2.7 further distinguishes event
generation from the cancelled instruction and permits cancellation
of older in-flight instructions for non-completion events. Precise
restart PC/BD, rollback, priority, pending high-bit handling and
counter commitment therefore remain separate open tasks. No
compatibility-clock changes or live retail replay are required for
this projection; CI is pending.

## Batch 9: isolated counter recognition and priority candidate

Manual v6.0 sections 4.1.3, 7.1.2, 7.1.4 and 7.2.4–7.2.6
(pp. 92, 145, 150–151) distinguish overflow flags from eligibility.
At a supplied observation boundary, classify raw PCR0/PCR1 high bits,
CTE inhibition, ERL deferral, Reset supersession and NMI deferral.
The candidate uses the explicit Level 2 inhibition text rather than
the conflicting printed ERL predicate in 7.2.4. The classification
does not depend on Level 1 interrupt enables or current count-mode
enables. The raw flags are reported even when delivery is inhibited.
Eligibility is not a completed recognition/cancellation decision.
This is an uncalibrated interpretation, not a claim of measured hardware
behavior.

Tests cover all four overflow masks, sixteen control/priority combinations,
four KSU values and both EXL values, with Level 1 masks and mode controls
varied (1,024 checks), plus constexpr deferral/return fixtures. No
counter writes, reset/NMI producer, pending-state storage, ERET hook
or actual exception delivery is introduced. No production call site
or default clock change; no retail replay required. CI pending.

## Batch 10: exact crossing-prefix projection

Manual v6.0 sections 7.1.2 and 7.2.1 describe a 31-bit count and
overflow flag, and increments for selected enabled events. The new
read-only helper projects the first exact crossing from clear flags
to 0x80000000, using a complete producer-supplied prefix and preserving
the other counter's measured increment. A prefix must independently
be known to end at the first crossing. This function cannot establish
that execution boundary from counts alone. It refuses overshoot,
missing or reserved evidence, already-high input, and different-event
dual crossings whose order is unknown. Equal shared-event crossings
can project both flags. No crossing produces no commitment.

The tests exercise exact single and equal shared crossings, distinct
events, missing deltas, oversized deltas, disabled/ERL gating and
previous overflow. Existing interval delivery still rejects overflow
transactionally. This candidate is not wired to the runtime and does
not resolve cancellation, event multiplicity within a physical cycle,
or actual architectural counter commitment. Hardware calibration and
precise recognition boundary are outstanding. CI pending.

### Compile-time regression follow-up

The event setter, interval planner, crossing-prefix projection and
transactional interval application are constexpr. A static_assert
fixture exercises exact crossings, preserved input counters, rejected
transactional overflow, shared-event ordering, overshoot, simultaneous
shared crossings, missing evidence, ambiguous ordering, ERL inhibition
and already-high counters. Runtime regressions remain intact. This
changes no production behavior or runtime state layout.

## Batch 11: explicit restart-boundary contract

Compose pending flag recognition with the existing Level 2 register
projection only when an independent producer supplies the cancelled
instruction PC, ordinary/delay-slot classification, branch PC for a
delay slot, and confirmation of restored architectural state. Reject
unavailable evidence, unsupported boundary kinds, inconsistent branch
addresses and unaligned PCs. Aligned delay-slot adjacency uses unsigned
32-bit address arithmetic. Inhibited, deferred or superseded exceptions
never project a new entry. Rejected plans preserve Status, Cause and
ErrorEPC; the vector is absent (zero).

The constexpr helper and static_assert fixture exercise accepted ordinary
and delay-slot boundaries, address wrap, malformed evidence, missing
restoration, pending flags, CTE/ERL inhibition and Reset/NMI priority.
The same fixture is called by the existing COP0 regression executable.
This is a checked input contract, not a producer or rollback mechanism:
a supplied restoration flag cannot establish hardware correctness.
Native IR, HLE and dispatcher paths still have no authoritative
cancelled-instruction producer. No guest exception delivery, production
clock change or runtime ABI change; no retail replay required. CI pending.

## Batch 12: source-authorized runtime crossing preflight

A constexpr composition combines the exact crossing and restart-entry
plans. It rejects a changed PCCR, Status or either starting PCR before
projecting entry from the proposed counter flags. The host-only C++
preflight reads live CPU/runtime registers, requires the authorized
validated typed source, and rejects undeclared event measurements
(including zero) and unsupported active selectors. It cannot distinguish
an intervening change followed by restoration or prove producer identity,
cancellation or exact hardware timing. Never retain the result for
later commitment.

Static_assert and runtime cases cover successful composition, stale
snapshots, rejected prefixes, incomplete restoration, higher-priority
NMI, compatibility/unavailable/cycle-only sources, undeclared events,
selector authorization, reset revocation and register preservation.

The runner's generic EE_EXIT_EXCEPTION route identifies HLE syscalls
and INTC using Level 1 Cause.ExcCode/EPC; using it for Level 2 would
risk confusing the preserved Level 1 code with a newly taken exception.
A distinct delivery/dispatch contract and authoritative producer are
required before committing any architectural state. No production caller,
clock change, ABI layout change or exception delivery was introduced.
CI pending; no retail replay required for this read-only preflight.

## Batch 13: distinct Level 2 dispatch exit

Append EE_EXIT_LEVEL2_EXCEPTION=7 without renumbering any existing
exit reason or changing CPU/runtime layouts. Compile-time assertions
protect the numeric ABI and classifier. A synthetic dispatcher fixture
supplies an already committed Level 2 exit with a preserved Level 1
syscall code, EXL and EPC; even a mapped Level 2 vector must not be
executed in that dispatcher call.

The stock game runner labels this exit separately, stops before
post-dispatch device service and HLE callbacks, and reports EXC2,
BD2, ErrorEPC, Cause, Status and vector with failure status. It does
not interpret the preserved Level 1 ExcCode or execute a Level 2
handler. No new production emitter, entry commit, timing source or
exception producer is enabled. Other execution frontends require
separate audit before live delivery is introduced. The header change
may affect cache provenance on a subsequent retail launch; no standalone
retail replay is requested for this inactive route. CI pending.

## Batch 14: transactional entry for a supplied exact crossing

The host-only commit helper recomputes the authorized preflight against
live state. A constexpr gate requires an explicit DISPATCH exit and a
live PC equal to the projected ErrorEPC (the branch address for a
delay-slot cancellation). These checks are necessary but cannot prove
restoration. The caller must own the execution thread and establish the
precise cancellation, restore all affected state, account preceding
clock/device work and exclude intervening state changes. No cached plan
is accepted. Rejected requests change neither PCR nor CPU state.

On success the non-fallible write sequence commits both projected PCRs,
Status, Cause, ErrorEPC, vector PC and the distinct Level 2 exit reason.
It preserves Level 1 EPC/diagnostics, IRQ fields and Count/Compare; it
does not perform clock publication, acknowledge interrupts or execute
the vector. Compile-time boundary gates and synthetic runtime fixtures
cover ordinary/BD2 entry, DEV selection, EXL preservation, ERET return,
authorization, stale snapshots, priority, overshoot and repeated commit.
The existing fail-closed typed interval API is unchanged.

No production caller is installed. Pending flags not generated by this
exact crossing (including software-written or deferred overflow), precise
rollback, authoritative event producers and frontend handler execution
remain unimplemented. The stock runner still reports Level 2 exits as
unsupported. No ABI layout change; CI pending. No standalone retail
replay is requested for this inactive host-only transaction.
