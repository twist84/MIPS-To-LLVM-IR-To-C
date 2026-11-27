# R5900 physical-console issue microbenchmarks

This is an independent, optional PS2SDK program; it does not modify the pinned
55-test suite. It does not ship a synthetic reference or claim measured cycles.
The original console performance test's PCCR cycle event programming is the
basis for raw PCR0 measurement:
https://github.com/unknownbrackets/ps2autotests/blob/97469ffbed8631277b94e28d01dabd702aa97ef3/tests/cpu/ee_cop0/performance.cpp

Generate 27 straight-line bodies (nine pairs, three repeat counts) with matching
offline observation traces. Each body begins 16-byte aligned, has fixed counter
read/fence boundaries, no measured branch loop, and initialized GPR/FPR/data
operands. The .word instructions are also the exact words in each synthetic
candidate trace. Traces contain the body only and use a synthetic aligned PC;
they are NOT hardware instruction issue traces. Warm each case three times,
measure eleven raw counter deltas without printing inside the timed region.
Interrupts are not disabled; repeated measurements and size slopes help expose
jitter and fixed measurement overhead. Counter reads do not prove retirement.

From a PS2SDK shell at the repository root:

    python3 tools/generate_ee_issue_bench.py --out build/ee-issue-hardware
    cp tests/hardware/ee_issue/bench_main.c tests/hardware/ee_issue/Makefile build/ee-issue-hardware/
    make -C build/ee-issue-hardware

Run ee_issue_counter.elf on an actual PS2 via your normal homebrew loader and
save the complete EE_ISSUE_COUNTER_V1 through EE_ISSUE_COUNTER_END output.
Record console model, BIOS, SDK/toolchain revision and loader; do not substitute
emulator timing. The SDK build and physical execution are not yet verified.
The host CI checks identical source/trace words and candidate decode for all
27 cases. No retail replay or change to the guest clock is involved.

The first set distinguishes independent and dependent integer pairs, cache-hit
load/use, repeated MAC and MFLO dependencies, and both COP1 operation/move
orders. Compare size-dependent slopes instead of subtracting an assumed
counter-read cost. Broader cases and direct cycle traces are still required.

## Counter capture comparison

    python3 tools/compare_ee_issue_bench.py --log console.log \\
        --candidate build/ee_issue_trace_candidate \\
        --console SCPH-model --bios version --toolchain sdk-revision --loader loader-version

The importer requires all 297 positive counter deltas exactly once in one
complete capture. It reports raw min/median/max for each of the three body
sizes and exact adjacent size slopes in cycles per two-instruction block.
Candidate issue and result-completion slopes remain separate. Fixed counter
and fence overhead is not assigned to an instruction. Optional
`--ordered-waw-overlap` compares the existing counterfactual WAW model.
The report records the capture hash and caller-declared provenance (not
independently authenticated). No automatic calibration or clock activation
is permitted, even if slopes match. Synthetic importer tests are not console
evidence.

## COP1 A-stage resource-hazard candidate

EE Core User's Manual v6.0 tables 1-1 to 1-3 (pp. 23–26) classify COP1
loads/stores as COP1 Move and permit COP1 Operate/Move to enter R together,
with a one-cycle resource hazard delaying the second instruction in A.
The default probe keeps conservative serialization. The optional
`--cop1-a-stage-pairing` candidate records the shared R issue, shifts the
second result and its unit deadline by one, and reports the number of such
pairs separately from disjoint issue-window counters. LWC1 is correctly
classified as COP1 Move even in the default policy. The option does NOT
model subsequent pipeline backpressure or prove a hardware issue timeline.
The comparator can evaluate both policies without activating guest timing.

https://docs.alexrp.com/mips/ee.pdf

## Build artifact

The separate R5900 console timing ELF workflow compiles the optional program
using the immutable PS2DEV v2.0.0 image index digest documented at
https://github.com/ps2dev/ps2dev/pkgs/container/ps2dev/ . It verifies each
linked executable body's alignment, length and exact instruction bytes
against the generator before uploading the ELF, traces and provenance.
No SDK is required for ordinary host builds. Successful cross-compilation
and opcode verification still do not constitute hardware execution.

Strict candidate CSV export now rejects an empty stream, a missing ordinary or
likely delay slot, an illegal executed nested branch, and a trace ending with an
unresolved delay slot even if its footer says non-truncated. Partial valid
prefixes remain available to offline analysis but are not complete timelines.

## Single-issue and dual-issue capture (COP0 #160)

The benchmark now makes two separate passes over the same 27 aligned
bodies. The first retains the cycle-only V1 log; the second programs
PCCR 0x8001785e (PCR0 selector 2: single issue; PCR1 selector 2: dual
issue, all modes) and records 297 pairs of raw deltas between
EE_ISSUE_PAIR_COUNTER_V1 and EE_ISSUE_PAIR_COUNTER_END. Zero is a valid
event count. Both counters and the original control word are restored
after each case. PCR reads are adjacent, not simultaneous; additional
fixed read/fence overhead differs from the older cycle-only executable.
Do not claim identical absolute measurements across the two versions.

The comparator accepts historical cycle-only logs and additionally
requires complete, unique paired samples when pair markers appear.
It reports separate raw min/median/max and exact length slopes for each
event, compared with the candidate's disjoint single/dual issue counts.
No counter value is assigned to a particular instruction, cancellation
or retirement, and no candidate source is authorized. Generated body
words and trace identity remain unchanged. Console execution and
independent timing measurements remain outstanding.
