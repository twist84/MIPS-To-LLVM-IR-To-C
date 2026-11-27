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
