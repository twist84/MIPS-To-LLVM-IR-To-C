# M1-A R5900 timing reference audit (implementation, NOT clock validation)

**Branch:** `feature/sdbz-hle-first-bootstrap` · **PR:** #142 (draft)
**Scope:** offline instruction issue replay. The original November 18 audit predates the runtime's reference-compatibility clock. The current HLE runner uses that approximate clock; the manual-cache-hit profile below is still offline only. Neither establishes calibrated R5900 timing. See [current status](README.md).

## Primary architectural reference

- Sony Computer Entertainment, *EE Core User's Manual*, version 6.0:
  https://docs.alexrp.com/mips/ee.pdf
- Table 1-2 (printed p.25): logical Pipe 0/1 instruction categories.
- Table 1-3 (printed p.26): concurrent issue combinations, including X
  (forbidden) and Y (issue together but one-cycle A-stage resource stall).
- Table 2-12 (printed p.58): instruction execution latency and throughput.
- Section 8.3.2 (printed pp.158–159): FCR31 condition bit and arithmetic
  cause/sticky status flags.
- PCSX2 opcode decoder/implementation is used only as a cross-check for
  *architectural instruction behavior*, **not** as a hardware cycle oracle:
  https://github.com/PCSX2/pcsx2

The EE manual explicitly notes that a complete program's cycle count also
depends on dynamic pairing and cache misses. A table of *instruction*
latencies is not a validated whole-program cycle producer.

## Manual-derived reference constants (offline profile)

| Instruction category | Latency (cycles) | Throughput (cycles) |
|---|---:|---:|
| Integer add/sub/logic, shift, branch | 1 | 1 |
| MULT/MULTU, MULT1/MULTU1 | 4 | 2 |
| DIV/DIVU, DIV1/DIVU1 | 37 | 37 |
| MADD/MADDU, MADD1/MADDU1 | 4 | 2 |
| Load with no cache miss | 1 | 1 |
| MTC1, MFC1 | 2 | 1 |
| COP1 scalar add/sub/compare, multiply, conversion | 4 | 1 |
| DIV.S | 8 | 7 |
| SQRT.S | 8 | 7 |
| RSQRT.S | 14 | 13 |

These were initially reference-only values. PR #156 exposes the explicitly selected `--manual-cache-hit` profile; they are not installed as the runtime clock. The existing explicit profile remains opt-in and synthetic. The experimental probe can now accept separately supplied **initiation
intervals** for MAC0, MAC1 and a conservatively shared scalar COP1 unit.
These values are **not defaulted from the table**; the CLI's explicit
throughput mode requires all seven positive intervals. The table is still
insufficient to infer stalls, cache behavior, WAW semantics, or validated
total EE cycles.

## Implemented in this branch

1. Separate logical issue classes for ALU/branch (either pipe),
   load/store and COP1 move (Pipe 1), scalar COP1 operate and MAC0
   (Pipe 0), and MAC1 (Pipe 1). Pair only if two logical pipes can accept
   the instruction categories, there are no tracked dependencies, and
   Table 1-3 does not mark the pair as X or unmodeled Y.
2. Preserve RAW/WAR/WAW pairing restrictions and conservatively defer an
   overwriting instruction until the preceding result is available. This
   conservative WAW policy must still be checked against real pipeline
   behavior; it is not claimed as calibrated hardware stall time.
3. Do not let an instruction pair into an *annulled* branch-likely slot.
   Such slots consume no committed guest instruction and remain explicit
   in the observation stream; their candidate reservation policy is
   experimental.
4. Model FCR31 as a shared condition/status scoreboard resource.
   CFC1/CTC1 to FCR31, BC1F/T/FL/TL, scalar comparisons and FCSR-writing
   arithmetic need separately configured candidate latencies. FCR0 and
   other unsupported control-register operations continue to fail closed.
5. Match this project's guest decoder's actual supported COP1 comparison
   encodings (C.F.S=0x30, C.EQ.S=0x32, C.LT.S=0x34, C.LE.S=0x36).
   ADDI/DADDI remain decoder-unsupported because overflow exceptions are
   not modeled, rather than being silently priced as harmless integer ALU.
6. Preserve explicit multiply-result availability for both the GPR result
   and HI/LO. Do not assume the generic integer ALU result latency applies.
7. Give DIV.S, SQRT.S and RSQRT.S separate optional timing fields rather
   than reusing a single latency despite the manual's differing results.
8. The probe now reserves unit next-issue deadlines independently of
   each instruction's register-result availability. MAC0 and MAC1 are
   separate reservation domains; scalar COP1 divide/square-root and ordinary
   arithmetic currently share one **conservative, unvalidated** domain.
   Caller-selected intervals never become GS or INTC cycles.
9. `ee_issue_trace_candidate` accepts an optional final throughput group
   (before `--candidate-events`):
   `--throughput=2,37,2,1,7,7,13`. This *example* merely exposes the
   manual's listed initiation intervals for offline experimentation.
   The group is optional to preserve earlier latency-only candidate traces;
   when supplied, strict replay rejects any modeled MAC/COP1 operation
   whose unit interval is absent. Mode is reported as `explicit` or
   `unconfigured` in diagnostics.
10. Keep missing profile entries, unsupported instructions and HLE timing
    boundaries non-replayable in strict mode. No scheduler source is installed.

## Explicit WAW overlap counterfactual and result-deadline differential

The default candidate scoreboard **still waits** for outstanding writes to
HI0/LO0, HI1/LO1, FCR31, GPR, FPR and the FPU accumulator before reusing
the architectural destination. That conservative policy may mask the
manual's separate functional-unit initiation intervals.

The new `e_ee_probe_waw_policy::ORDERED_OVERLAP` experiment, available
only through explicit opt-in, allows a later same-resource writer to issue
after the configured **initiation interval**, rather than waiting until
the prior result is ready. It **rejects** the candidate atomically if
the later result would become ready **before or in the same cycle** as
the earlier result. Equal deadlines are ambiguous without independently
verified writeback ordering. RAW remains enforced; pending unit
occupancy and result readiness survive explicit HLE overhead boundaries.
`reset()` restores the default conservative WAW policy.

This is a **counterfactual comparison**, not a reconstruction of EE
register forwarding, bypass networks, completion ports or retirement.
The simple scoreboard stores the latest per-resource result deadline,
but cannot prove the behavior of actual overlapped R5900 instructions.

Offline CLI syntax (after explicit profile numbers):

- `--throughput=<seven-positive-intervals> --ordered-waw-overlap`
  enables the optional WAW comparison; overlap without a throughput
  profile is rejected.
- `--candidate-events=PATH` retains the original five-column *issue*
  export for existing differential tests.
- `--candidate-completions=PATH`, alternatively, exports both
  `candidate_issue_cycle` and `candidate_completion_cycle`. An
  annulled delay slot has an **empty completion field**, never a
  fabricated result timestamp. Issue and completion remain distinct.

`tools/ee_timing_differential.py --compare-completions --candidate PATH
--reference REFERENCE_PATH` strictly checks an independently supplied
six-column `reference_issue_cycle,reference_completion_cycle`
stream. It checks event identity, issue ordering, nonnegative result
latency and both issue/completion *relative deltas* after one reference
epoch offset. It permits independent functional units to have
non-monotonic completion order; it does not infer a global retirement
ordering. Synthetic fixtures exercise mismatched deadlines, malformed
rows, nonexistent completion for annulment, ambiguous ordering and
missing events. **No independent physical-console completion trace has
been supplied**, so passing these synthetic tests does not calibrate
the R5900.

## Decoder-grounded COP0 and VU0 transfer coverage

The shared decoder (`src/ee/decoder.cpp`) already admits exact
`MFC0/MTC0`, `QMFC2/QMTC2` (VU0 VF), and `CFC2/CTC2`
(VU0 VI) encodings. The offline probe keeps distinct **functional**
VU0 VF and VI instruction masks and an independent COP0 register bank,
but now uses **one shared VU0 macro-mode dependency identity** for
VF[n]/VI[n] of matching index n=1..31. This follows the VU User's
Manual v6.0 section 5.4.2, which also exempts VF00 and VI00 from
ordinary macro-register hazards. The issue model covers RAW,
conservative WAW, same-cycle exclusions and opt-in atomic
ordered-overlap checks in this shared hazard bank. It does **not**
merge actual VF and VI register values, establish transfer forwarding
latency, or authorize whole-system timing.

**No new hardware timing constants were introduced.** The three
transfer classes require individually supplied candidate result
latencies (`m_cop0_transfer_latency`,
`m_vu0_vf_transfer_latency`, `m_vu0_vi_transfer_latency`).
The CLI adds an all-or-nothing final three-number optional latency
group after the existing 14 input values. The single-issue
`SYSTEM_CONTROL` and `VU0_TRANSFER` categories explicitly
avoid claiming unverified mixed-pipeline pairing.

**Remaining hard stop:** A COP0 register scoreboard cannot model
implicit changes to Count/Compare/Status/Cause, interrupts,
TLB operations or `EI/DI/ERET` transitions. The VU0 VF/VI register
scoreboard cannot model vector lanes, VU0 ACC/flags, asynchronous
VU0 execution, macro FMAC, `BC2` flag dependencies, VU0 memory
transfers (`LQC2/SQC2`) or memory/cache stalls. These instructions
still produce `UNMODELED_INSTRUCTION`, even when all three new
transfer latency fields are supplied. Packed MMI and CACHE also
remain rejected. Thus the expanded candidate stream **cannot**
drive the EE cycle scheduler, GS scanout or INTC events.

The new regression fixture covers decoder-supported transfers,
unconfigured-latency rejection, isolated COP0/VU0 hazards,
VF/VI same-number alias dependencies, VF00/VI00 exemptions,
overlap ordering and deliberately unmodeled system/vector/MMI
operations. Synthetic test vectors prove software behavior
only, not R5900 cycle calibration.

## Outstanding correctness gates

- The Table 1-3 Y combinations (e.g. COP1 operate plus COP1 move)
  require modeling the A-stage resource stall, not a guess.
- The isolated initiation-interval reservation is implemented and tested
  using caller-supplied values; it is **not** a complete hardware occupancy
  model. Prior conservative cross-cycle WAW waiting can still dominate MAC
  HI/LO or COP1 FCR31 throughput, especially when latency exceeds interval.
  To establish real back-to-back MAC/FPU timings, independent evidence must
  resolve overlapping destination writes and retirement order.
- The current shared scalar COP1 occupancy and independent MAC0/MAC1
  resource identities are candidate assumptions, not independent timing
  validation. Short-loop hazards, execution/retirement distinctions,
  cached/uncached access and cache-miss stalls remain unmodeled.
- COP0 transfers and VU0 VF/VI transfers now have explicit, isolated
  offline COP0 versus shared-numbered VU0 macro hazard resources.
  COP0 system state (ERET/EI/DI,
  interrupts, TLB, implicit register changes), VU0 macros, vector
  flags/ACC, packed MMI, cache and memory-side hazards remain unavailable.
  Reject these rather than assigning default costs.
- HLE entry/exit overhead and scheduler handoffs have no independently
  measured instruction/cycle trace. Do not add arbitrary costs.
- The candidate replay's synthetic regressions are **not** independent
  real-console timing traces. A trustworthy full-instruction reference
  with cycle correlation is still required before enabling the clock.
- M1-A hardware clock validation **has not passed**. M1-B first-field,
  VBlank, CSR/INTC and real boot progression **must not** be inferred
  from offline test results.

## Regressions and evidence separation

`ee_issue_timing_probe_test` covers logical pipe combinations,
branch-likely annulment and register/FCR31 ordering.
`ee_issue_trace_replay_test` covers COP1 compare/control paths,
unsupported signed traps, separate DIV/SQRT/RSQRT inputs and MULT
GPR availability. CLI and differential tests remain explicitly
synthetic until a provenance-tagged hardware/reference trace is supplied.

There are no ELF-derived timing constants or new local SDBZ boot findings
in this batch. **No GS/INTC event generation and no simulated VBlank** are
authorized by this work.

## PR #156: manual cache-hit profile (25 November 2026)

The manual's section 2.12 is now available as an explicit offline
`--manual-cache-hit` profile. It supplies documented integer, cache-hit
load, HI/LO, scalar COP1 and functional-unit initiation intervals, rather
than requiring the user to reenter the reference table. LWC1 has its own
2-cycle FPR result latency and participates in the FPR dependency bank
(including FPR0); it is not priced as a 1-cycle integer load. The profile
keeps unspecified COP0, VU0, FCR control and packed-MMI timing absent.
Some shared classes are conservative approximations (e.g. ACC arithmetic
and FCR31 reservations); the manual does not fully characterize internal
forwarding. A cache-hit profile does not predict cache misses, contention,
Y-pair A-stage stalls, HLE cost or actual program retirement. The runner
still uses the separately labelled reference compatibility clock; this
profile is never installed as validated timing.

Example: `ee_issue_trace_candidate trace.csv --manual-cache-hit --candidate-completions=manual.csv`.
The output reports the profile provenance, and tests check load/FPR
hazards, preservation of unknown categories and explicit CLI selection.
Physical-console cycle comparisons remain required.
