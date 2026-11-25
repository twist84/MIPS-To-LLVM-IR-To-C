# Original ps2autotests ELF validation

This milestone runs the unmodified upstream ELF binaries through the
existing EE LLVM translation / native HLE runtime. Reference revision:
[97469ffbed8631277b94e28d01dabd702aa97ef3](https://github.com/unknownbrackets/ps2autotests/tree/97469ffbed8631277b94e28d01dabd702aa97ef3).
The manifest pins the Git blob IDs of all 55 ELF/expected pairs and the
license. The script verifies every download and cache hit. No commercial
game, BIOS, ISO or SDK build is required; the open test binaries remain
under the ignored build directory.

Configure and build the existing tools with Python/llvmlite installed:

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build --target ee_recomp ee_game_runner
    python tools/run_ps2autotests.py --list
    python tools/run_ps2autotests.py cpu/ee/alu cpu/ee/muldiv cpu/ee_fpu/fcr
    python tools/run_ps2autotests.py "cpu/ee/*"
    python tools/run_ps2autotests.py

Each test uses an isolated translation cache and a fresh results folder;
the normal native live expansion and bounded guest execution are used.
The script extracts only actual EE KPUTS guest bytes, reverses the
runtime's byte escapes, joins fragmented messages and compares the full
marker-delimited transcript with the console reference. It normalizes
CRLF and trailing empty lines only. A PASS means exact test output,
not necessarily a clean guest process exit; the runner exit, stop
reason and report are kept separately, including failures after the
test end marker. Missing/partial output, differences and infrastructure
errors are separate statuses, not passing tests. No known-difference
exemptions. The denominator is always 55; selected runs also report
their selected count.

Results: build/autotests/runs/<UTC timestamp>/{summary.json,summary.md}
and per-test run.log, guest.raw, test.out, optional test.diff, runner.json
and result.json. Default exit is nonzero unless every selected output
matches. --report-only explicitly makes the initial pilot observational,
rather than turning unimplemented services into an all-PR gate. The CI
pilot executes alu, muldiv and fcr via the same built runner; inspect the
uploaded ps2autotests-pilot artifact for actual outcomes. Guest timeout
excludes compilation; CI job time bounds the total.

The first pilot may stop in PS2SDK startup, TLB setup, scheduling or
console transport. Do not bypass the original startup, patch expected
output, or count an unexecuted test as passed. Implement missing services
from independent references, rerun the smallest failing test, expand
the selection, then establish the full-suite baseline. No score is
claimed before actual execution. Peer 54/55 is a reported output
comparison, not a matched timing benchmark.
