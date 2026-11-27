# EE practical timing compatibility (T1–T7)

## Objective and constraints

Match the practical timing behavior needed for games, using PCSX2 as the
hardware-behavior reference and ps2-web-recomp as the native/HLE reference.
Physical cycle-perfect R5900 issue, pipeline and bus simulation is not the
goal. Avoid per-instruction host callbacks, unproven constants, and forced
synchronization when a cheaper conservative boundary suffices. Keep the
current reference-compat clock and its caches available as the control.
Never label a PCSX2-derived heuristic as validated physical R5900 timing.

IOP AOT accuracy work G8D.2 accepted; execution micro-optimization is
paused, but function splitting and parallel compilation remain active in #174. Keep it optional and use interpreter for initial EE timing
comparisons so only one component changes. No further retail replay until
a timing candidate requires one.

## T1.0: verified baseline

Control runner SHA-256 6cb539ba63b4790d1e034eb041d04ce5c42eec7a614e7346057389bec40b977f;
G6A EE DLL and V30 VU1; fixed RTC and PAD; synchronous workers.
G8D.2 interpreter report matches the previous reference exactly:
40M dispatches, 21910026110 guest instruction attempts, 21912038396
reference slots, 138937 HLE boundaries, 22115080673 published EE cycles,
4494 fields, 42578752 IOP turns, zero debt. 3127 valid PAD polls.
Current formula (include/ee/ee_runtime.h):
slots + compatibility idle + 300 * recognized HLE boundaries.
Thus HLE cost 41681100, idle contribution (inferred by subtraction)
161361177, and slot count exceeds attempts by 2012286. Slots are not
instruction retirements and idle skips can synthesize attempted/slot
counts. Exact tests pin current formula, independent diagnostic attempt
count and overflow rejection.

### Pinned source comparison

* [PCSX2 opcode table](https://github.com/PCSX2/pcsx2/blob/34670c6151d28dee8284e90f7957e470fc03f9d3/pcsx2/R5900OpcodeTables.cpp):
  weights in eighths of a cycle, e.g. ordinary 9, conditional branch 11,
  load/store 14, MULT 16, DIV 112, COP default 7. Not all instructions
  within a family share a cost; J/JAL/JR/JALR use ordinary 9. MULT1/DIV1
  use ordinary multiply/divide weights rather than generic MMI weights.
  Decode exact leaf identity; do not extrapolate from major opcode.
* [PCSX2 interpreter](https://github.com/PCSX2/pcsx2/blob/34670c6151d28dee8284e90f7957e470fc03f9d3/pcsx2/Interpreter.cpp):
  weighted accumulation, factor 2 when COP0 Config bit 18 (DIE) clear,
  factor 1 when set, default rate shifts accumulator right 3 with
  minimum one published cycle per update, retains fractional residue.
* [PCSX2 EE recompiler](https://github.com/PCSX2/pcsx2/blob/34670c6151d28dee8284e90f7957e470fc03f9d3/pcsx2/x86/ix86-32/iR5900.cpp):
  similarly weights at compilation; NOP uses 9; accumulates block
  weights, scales and clears at defined boundaries. Inspect all
  block/branch/fault paths before choosing a comparable publication
  unit. PCSX2 rate hacks are not the initial target. Minimum-cycle
  and rounding semantics depend on publication boundaries; never
  charge minimum one for arbitrary host dispatches.
* [PCSX2 event test](https://github.com/PCSX2/pcsx2/blob/34670c6151d28dee8284e90f7957e470fc03f9d3/pcsx2/R5900.cpp):
  checks CPU-level INTC/DMAC exceptions first, synchronizes IOP using
  EE elapsed-cycle balance before timers, then timer and scheduled
  interrupt work, then VU service. Newly raised exceptions can be
  deferred to a later event check. Do not copy the order blindly
  into the HLE scheduler; test observable ordering first.
* [ps2-web-recomp SDBZ reference](https://github.com/twist84/ps2-web-recomp/tree/985c0ac15f2622199f90b206bac3db89268e2f22):
  existing local compatibility model is documented against that
  revision in ee_runtime.h (reserved branch slots including annulled
  likely slots, 300 syscall budget). Resolved pinned commit; direct
  re-audit of its timing producer and event ordering remains open.

### Current consumer/producer gaps

| Path | Current model | Required audit |
|---|---|---|
| Translated EE | diagnostic attempts + reference instruction slots | exact executed leaf, branch likely, delay/fault and exception paths |
| HLE | 300 per recognized transition | separate explicit compatibility overhead; no double count or arbitrary scaling |
| Idle skip | synthetic attempts and slots per verified loop iteration | weighted per-iteration cost or decline acceleration |
| COP0 Count/PERF | compatibility clock cursors | same selected timebase at MMIO/CP0 boundary; preserve distinct counters |
| VU0 micro/CMSAR1 | instruction-attempt deltas | explicit timebase conversion, not raw attempt count |
| EE hardware service | monotonic reference publication | bounded publication and event ordering, no host-wall-clock coupling |
| IOP | EE elapsed / 8, debt bounded per checkpoint | ratio/remainder/debt and service ordering after EE producer changes |
| DMAC/VIF/GIF/GS | existing event and scanout boundaries | compare pacing, stalls, IRQ and visibility, avoid global instant completion |
| Host yields | no synthetic timing from yield | preserve invariant |

Observed runtime anchors: service_devices publishes compatibility time
and syncs COP0, then CMSAR1/VU0; main loop publishes HLE budget before
IOP service. runner_vu0_sync consumes raw attempt delta; pure idle
loop acceleration adds both attempts and slots. Those are distinct
time domains today, so swapping a single formula is insufficient.

## Implementation order and gates

1. T1.1: inventory exact local opcode IDs and path accounting, including
   COP0 Config changes, branch-likely annul, faulting memory, HLE,
   callbacks and idle acceleration. Re-audit the pinned web reference.
   Build a constexpr leaf-cost table and fixed-point checked arithmetic
   with differential tests. Establish PCSX2 block/rounding semantics;
   avoid importing implementation code or assuming hardware accuracy.
2. T1.2: opt-in shadow producer, no device clock changes. Accumulate
   weights in translated IR using constant folding/straight-line
   aggregation only across safe boundaries. Measure actual path counts,
   fractional remainder, DIE epochs and idle accounting, preserve old
   runtime ABI prefix. Explicitly refuse unsupported paths. Keep
   reference report/trace and code-cache provenance intact.
3. T1.3: separately named PCSX2-style compatibility clock, not
   VALIDATED_R5900. Make publication/COP0/HLE/idle semantics coherent;
   initial diagnostic builds can disable idle skip if necessary.
   Keep reference-compat default. First local timing fixtures,
   all 55 exact original autotests and existing CI; then controlled
   SDBZ replay with new recording if timing intentionally changes poll
   positions. Do not relax same-model determinism tests; cross-model
   comparison uses event sequences, guest progress, field cadence,
   audio and input behavior, not equality of old absolute timestamps.
4. T2 EE/IOP synchronization; T3 DMA pacing and interrupt order;
   T4 VU timing; T5 VIF/GIF/GS FIFO and scanout; T6 timers/interrupts;
   T7 targeted performance recovery. Each independently testable
   checkpoint should preserve existing architectural tests and be
   committed before requiring new user validation.

T1.0 is audit and baseline regression only; no new timing producer
or changed game timing yet. New source changes must be reviewed
before generating any new EE image; no cache invalidation for T1.0.

## T1.1: standalone cost primitive

`include/ee/ee_pcsx2_timing.h` implements numerical timing categories
independently using raw architectural selector fields, gated by the local
decoder's VALID classification. A zero result means no established cost;
it is not a zero-cycle instruction. Decoder validity is required even for
COP2, where the pinned reference charges the major opcode only. The
reference also weights integer traps at 11, MADD/MADDU and their 1
variants at 16, and MFHI1/MTHI1/MFLO1/MTLO1 at 9.
The checked accumulator preserves fractional eighth-cycles and rejects
overflow transactionally. Its explicit DIE epoch is a primitive, not
a claim about exact PCSX2 Config-write or publication semantics.
No runtime or generated-IR consumer has been connected.

Provenance: numerical heuristic observations from the pinned PCSX2
opcode table and interpreter above. PCSX2 is GPL-licensed; no PCSX2
implementation routines or original table layout are imported. This
is independently expressed architectural selector/category logic; no
license change or legal determination is implied. The regression target
`ee_pcsx2_timing` checks constexpr arithmetic, selector distinctions and
the existing runtime decoder gate. T1.2 must separately audit actual
publication/exception paths and ps2-web-recomp timing. No retail replay
or EE IR refresh is required for this standalone checkpoint.
