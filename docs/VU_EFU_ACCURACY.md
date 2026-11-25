# VU EFU arithmetic investigation (issue #158)

The current original console baseline is 54/55 exact transcripts. The shared EFU
helper is used by both interpreted VU1 and the native AOT callback. The remaining
seven raw differences are recorded in [PS2AUTOTESTS.md](PS2AUTOTESTS.md).

## Direct arithmetic probe

Configure the normal CMake build, then explicitly build the diagnostic target:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target ee_vu1_efu_probe
python tools/probe_vu_efu.py --probe build/ee_vu1_efu_probe --report-only
```

On Windows use `build/ee_vu1_efu_probe.exe`. For offline operation, supply
`--reference-root` pointing at the pinned upstream ps2autotests checkout.
The driver verifies the original source and expected transcript Git blob hashes,
parses all 208 expected words and feeds the four raw VF lanes to the production
helper. It prints each difference and a per-opcode count. Without
`--report-only`, any arithmetic difference is a failing exit. Existing model
latencies are checked separately (not claimed as measured hardware cycles).
No original test results are edited or waived. This bypasses guest startup
and formatting only for arithmetic investigation, not for the actual ELF suite.

The probe also accepts arbitrary hexadecimal words on stdin: opcode index,
component, X, Y, Z and W. Its output is the hexadecimal result and decimal
model latency. This permits comparison against newly captured independent
console inputs without changing the harness or runtime.

## Initial cancellation investigation

EATAN's four mismatches reduce to the same argument, -1. The current sequential
FMAC polynomial produces `34000000` rather than the console's `33e60000`.
An exact mathematical sum of the stored coefficients and truncated pi/4 at -1
is approximately -7.91624e-9 (`b2080000`), not the console value
+1.07102e-7. A standalone alignment-guard experiment on those already-rounded
terms gave:

| Guard bits | Forward polynomial then pi/4 |
| ---: | --- |
| 0 | 00000000 |
| 1 (current) | 34000000 |
| 2–12 | 34400000 |

This experiment is not a hardware schedule or a proof about all inputs.
Reversing the accumulation order or adding pi/4 before the terms also did
not recover the console value. The pinned peer implementation
[ps2-native-recomp](https://github.com/exoteric33/ps2-native-recomp/blob/2a1cbfe7c355d5a3d6830810879b399868d760c1/runtime/include/ps2/vu_ops.hpp)
uses a direct reduced-argument replacement at -1 and acknowledges unresolved
sine arithmetic. Do not import that special case.

Next: obtain independent console word vectors near the cancellation and for
positive/negative ESIN across multiple magnitudes; distinguish intermediate
product rounding, retained accumulator bits and evaluation ordering before
changing the shared implementation. No arithmetic behavior or latencies
are changed in this diagnostic commit.

## Batch 2 — ESIN coefficient audit

The [Sony VU User's Manual v6.0, ESIN, p. 143](https://studylib.net/doc/25815876/vuusersmanual.158394566)
lists S4 as `b94fb21f`; the previous peer-derived implementation used
`b94fb222`. All other ESIN coefficients agree. A standalone integer
calculation transcribing the existing one-guard-bit adder and asymmetric
Booth multiplier reproduced the baseline 13/16 and, changing only S4
to the documented word, predicted 16/16. This is a source-based correction
rather than fitting a coefficient to the test cases.

| Input | Previous | Console / predicted |
| --- | --- | --- |
| 3 | 3e126228 | 3e126230 |
| pi | 3b565be0 | 3b565de0 |
| 3*pi/2 | bf1a9964 | bf1a9934 |

No evaluation order or latency changed. Add direct raw-word regression cases
for these three inputs, while retaining all original ELF and direct probe
checks. Expected overall arithmetic result is 204/208; actual CI and retail
validation are required before claiming that result in the runtime.

The same manual documents EATAN's pi/4 as `3f490fdb`, whereas the console
readback at reduced argument zero is `3f490fda`. Do not change the constant
blindly: the implementation's existing word matches the hardware at that
argument. Inserting pi/4 before the last polynomial term with the existing
one-guard-bit adder gives `33e70000` at reduced argument -1, close to but
not equal to the hardware's `33e60000`. It is not a justified fix.
