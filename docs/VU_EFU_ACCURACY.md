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

### Batch 2 verification

[CI run 45675443215](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45675443215)
at PR head `34b19e7` confirms 204/208 direct EFU words, ESIN 16/16,
zero existing-model latency differences and 54/55 exact original ELF
transcripts. All five workflows passed.

## Batch 3 — documented EATAN nanocode order

The [VU User's Manual v6.0](https://studylib.net/doc/25815876/vuusersmanual.158394566),
p. 355, describes the EATAN internal operation order. Use coefficient-first
for the initial product, square-first for the cube, then power-first for
subsequent odd terms. The constant term is a multiply by one accumulated
before the final odd term, not a literal addition after the polynomial.
The existing asymmetric Booth model computes `mul(1, 3f490fdb)` as
`3f490fda` and `mul(-1, bb84d7e7)` as `3b84d7e6`, explaining the
previous constant and cancellation discrepancies without any input-specific
replacement.

A standalone integer transcription of the actual FMAC/quotient algorithms
reproduced the previous 44/48 arctangent words and predicts 48/48 using
this documented sequence (including EATANxy and EATANxz). Combined with
the verified ESIN correction this predicts 208/208 direct words and
55/55 original transcripts; actual native CI results remain pending.
The implementation still uses the existing separate truncated multiply
and add primitives, and this is not a claim of independent validation
of internal accumulator precision or hardware timing. The existing
54-cycle model latency is unchanged. Add the four cancellation words to
the VU1 pipeline regression. Independent console inputs and a matched
retail replay remain required for broader validation.

## Batch 3 verification — original transcript target achieved

[PR #170](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/pull/170) head `e2870d8` verified **55/55 exact original transcripts (100%)**, zero DIFF/INCOMPLETE/NO_OUTPUT/ERROR, and **208/208 direct EFU words** in [run 45676297801](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45676297801). All five workflows passed; [CTest](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45676297753) passed 174/174 across seven shards. This is the tested PR revision, not a claim of a new main or retail baseline. Independent EFU vectors, clean guest termination and calibrated timing remain separate validation objectives.

Artifacts: [combined original suite summary](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45676297801/artifacts/15051667296), [direct EFU report](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45676297801/artifacts/15052516676). All thirteen EFU operations matched 16/16 raw words. The direct probe reported zero existing-model latency differences. The seven historical discrepancies are resolved without per-input replacement. A matched retail replay on this runtime and additional independent console vectors are still outstanding.

## Retail replay preflight recovery

The first post-EFU Windows P40 attempt verified all 16,250 shared EE IR modules but refused the O5 native profile because its saved lowering fingerprint for `include/ee/ee_cycle_scheduler.h` differed from the current source. No retail dispatch occurred. The canonical shared source was already current; this is a stale native profile, not an EFU execution failure. After authenticating the canonical source and locking the profile, rebuild the native library through the full incremental object verifier, which checks IR and object hashes and target/LLVM identity. Publish fresh native provenance only after a successful link. Never bypass stale canonical-source validation or overwrite guest IR. A regression simulates a stale profile fingerprint with an unchanged verified source; the previous modified-IR rejection remains in place. The user's actual replay and new CI are pending.

### Cache recovery and bounded retail verification (2026-11-25, 18:53)

User archive: `sdbz-graphics-pr148-neutral-20261125-185358362.zip`.
The O5 cache preflight succeeded, preserving 16,250 EE modules. The native
process reached its configured 40,000,000-dispatch cap, final PC
`0x00398db0`, reason DISPATCH, in 49.234 s process wall (44.576 s
profiled runner wall). The archive contains no explicit Git HEAD; the
user supplied it following the cache recovery patch. Do not claim a
cryptographically revision-pinned retail run from the filename alone.

| Metric | Earlier warmed baseline | New bounded replay |
| --- | ---: | ---: |
| Native VU1 invocations | 411642 | 390362 |
| VU1 interpreter fallbacks | 0 | 0 |
| IOP guest faults | 0 | 0 |
| Successful window presents | 2886 | 3541 |
| Nonzero guest PCM frames | 1858699 | 1587046 |
| Process wall (s) | 49.406 | 49.234 |

Spawn boundaries were reached; 512x448 display and WebGPU PPM files are
byte-identical, but both read GPU-authoritative VRAM. 3,176,982 GPU draws,
4,947 guest fields, no host audio device, 3,960,368 guest PCM frames.
One VU1 job was pending at the dispatch cap; no worker failures/rejections.
SIO2 recorded zero Cross-pressed polls out of 3,444, but the EE pad-return
observer recorded 190 Cross-bit-low candidates out of 3,152. Therefore
this is a bounded retail regression pass, not a strictly input-matched
neutral comparison or proof of performance improvement or audible sound.
The old and new workloads differ. Timing remains reference_compatibility.

At PR head `a2aece6`, all five workflows passed again, including
[original suite 45678885192](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45678885192)
(55/55 exact, 208/208 direct EFU, zero existing-model latency differences)
and [build/test 45678885324](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45678885324).
Independent hardware vectors and fully matched replay evidence remain open.
