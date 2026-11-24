# EE R5900 architecture fidelity — deterministic alignment/COP1 batch

## Baseline and scope

Base: `feature/sdbz-deferred-arkd-rpc-binds` PR #145 at `90e9cfd74342a1d4082d1bcd82df443ce5c6fd67`. The original review inspected `main` at `4939867` and the earlier PR commit `a58816d`. The batch changes shared EE lowering only. It does **not** claim a new retail boot replay or resolve ARKD method `0x500:1`.

Primary manuals: [EE Core Instruction Set Manual v6.0, LQ p.141 and LW p.79](https://docs.alexrp.com/mips/ee_insns.pdf), [EE Core User's Manual v6.0, signed-zero Table 8-5 p.163](https://docs.alexrp.com/mips/ee.pdf). The `ps2-web-recomp/SDBZ` runtime is the implementation reference.

## Implemented

- LQ/SQ mask the four low bits of the 32-bit effective address before both 64-bit bus accesses. SQ trace records the unrounded guest virtual address separately from its aligned affected address.
- LH/LHU, LW/LWU, LD, SH, SW, SD, LWC1 and SWC1 guard their natural address alignment before calling any memory helper or modifying a destination. AdEL (4) is raised for loads; AdES (5) for stores. The existing guest exception helper preserves EPC, Cause.BD, BadVAddr, EXL, and flushes prior dirty GPRs.
- LQC2/SQC2 retain their **different** 16-byte alignment exception semantics, using the shared precise guard; LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR remain intentionally unaligned.
- DIV.S with zero-exponent divisor now returns signed `0x7fffffff` / `0xffffffff` for the architecturally specified exceptional result; existing D/SD and I/SI behavior remains unchanged.
- Add a synthetic real LLVM/MCJIT regression for unaligned LQ/SQ address masking, scalar/COP1 exception effects (including branch-delay BD/EPC), zero memory side effects, and an aligned LW control case. Extend the existing COP1 bit-pattern tests for both signed-zero combinations and exceptional DIV results.

## Deliberately open

- **Full COP1 bit-exact arithmetic remains incomplete.** Host IEEE float arithmetic, exponent-all-ones normalization, non-zero divide overflow, accumulator FMAC/rounding, FCR0 and FCR31 hardwired bits require independent verified batches. Do not change those by blindly substituting the EE FMAX word into native IEEE float operations; `0x7fffffff` is a NaN in the host format.
- Translated TLB-backed memory translation, memory/bus faults beyond alignment, CACHE behavior, COP0 Count/Compare, dual issue and cache stall timing are not implemented by this batch.
- VU0 micro-mode upper/lower dual issue and VU1 GIF PATH1 back-pressure / MSCALF remain open.
- Native SDBZ retail ARKD DMA4 IRQ and semaphore wakeup are separate from this EE architectural batch.

Required acceptance: all seven CTest shards, OpenPS2BIOS ROM0 execution, SIF DMA acceptance, and Native WebGPU Linux/Windows workflows at one exact final commit SHA. PR #145 remains draft/open/unmerged; eventual merge requires explicit authorization and must be squash.

## Follow-up: SQRT.S extended range

The hardware words in [ps2autotests sqrt.expected](https://github.com/unknownbrackets/ps2autotests/blob/master/tests/cpu/ee_fpu/sqrt.expected) and [PCSX2 #14794](https://github.com/PCSX2/pcsx2/issues/14794) establish that exponent 255 is finite and SQRT.S uses nearest rounding. For this binade the lowering scales the absolute input by 1/4 before the single-precision square root, then restores the result exponent. This avoids interpreting extended-range operands as host NaNs or losing mantissa bits through clamping. Exponent-zero results and existing FCSR handling are unchanged. Tests execute literal console result vectors at backend O0 and O3. RSQRT.S is deliberately left separate because its rounding and two-operand extended-range treatment must be validated together. No claim of bit-exact general COP1 arithmetic follows from this change.

## Follow-up: RSQRT.S result arithmetic

The [physical-console vectors](https://github.com/unknownbrackets/ps2autotests/blob/master/tests/cpu/ee_fpu/sqrt.expected) distinguish host SQRT/DIV rounding from the R5900 result (e.g. 3/sqrt(3), 0x3fddb3d7; maximum/sqrt(maximum), 0x5fb504f2). Replace the host floating-point pair with an integer radix-2 signed-digit recurrence, informed by the arithmetic algorithm DOI 10.1109/ARITH.1995.465363 and the hardware-derived model in [PCSX2 PR #12001](https://github.com/PCSX2/pcsx2/pull/12001). Separate root and quotient iterations preserve extended exponent 255, signed zero and result saturation. The helper is internal to each LLVM module that uses RSQRT.S, with no new runtime ABI. Tests cover 34 distinct console operand/result vectors and 13 additional controls at backend O0/O3. Existing FCSR cause/sticky policy is deliberately retained; overflow/underflow flag accuracy and exhaustive hardware parity remain open. This does not yet change standalone DIV.S or SQRT.S to use the recurrence.

## Follow-up: DIV.S result arithmetic

Standalone DIV.S now uses the same internal signed-digit quotient recurrence as RSQRT.S. The input words are never clamped to IEEE FMAX: exponent 255 retains its significand, zero-exponent operands preserve the XOR sign, quotient exponent overflow saturates to signed 0x7fffffff, and underflow flushes to signed zero. All 31 distinct operand/result pairs in [ps2autotests muldiv.expected](https://github.com/unknownbrackets/ps2autotests/blob/master/tests/cpu/ee_fpu/muldiv.expected) match an independent execution of the recurrence. The JIT regression runs these literal results at backend O0/O3 with both clear and populated cause/sticky flags, plus existing architectural controls. The legacy FCSR update policy is retained deliberately, pending hardware-backed cause/sticky audit; the result fixture does not measure FCSR. No runtime ABI or guest-specific exception was introduced.

## Follow-up: ADD/SUB and accumulator destinations

ADD.S, SUB.S, ADDA.S and SUBA.S now share a private integer LLVM helper using one retained alignment guard bit, sign/magnitude accumulation, normalization and truncation. It handles exponent 255 as finite, flushes exponent-zero operands, preserves signed zero, and detects real extended-range overflow and cancellation underflow separately from the result word. All 31 distinct operand pairs for each of the four instructions in [arithmetic.expected](https://github.com/unknownbrackets/ps2autotests/blob/master/tests/cpu/ee_fpu/arithmetic.expected) match the one-guard-bit model. The JIT tests use literal results at backend O0/O3 with two FCSR initial values, plus guard-bit and cancellation controls. The previous O/U cause/sticky update policy is maintained; full hardware flag sequencing remains a separate audit. MUL and MAC arithmetic are unchanged.

## Follow-up: MUL.S / MULA.S

Both instructions now use an integer significand multiplier with radix-4 Booth rows and the truncated carry-save low-product correction, following the hardware-derived model in [PCSX2 PR #12001](https://github.com/PCSX2/pcsx2/pull/12001). Operand order is significant: 0x3fffffff * 0x3f800000 produces 0x3fffffff while the reversed operands produce 0x3ffffffe. All 31 distinct operand/result pairs for each instruction in [muldiv.expected](https://github.com/unknownbrackets/ps2autotests/blob/master/tests/cpu/ee_fpu/muldiv.expected) match the integer model. Added literal O0/O3 JIT cases with two FCSR initial states and independent range/underflow controls. Shared result/exception handling keeps FPR and ACC destinations consistent. Existing FCSR update ordering is retained, pending hardware flag sequencing work. MAC instructions remain on their earlier path pending an accumulator exception audit.

## Follow-up: MADD.S / MSUB.S / MADDA.S / MSUBA.S

The LLVM path uses a truncated Booth product followed by a guard-bit integer sum. Product overflow takes precedence over the prior ACC overflow and ordinary accumulation; MSUB inverts the product sign even on overflow. Finite exponent-255 values alone do not imply overflow. The separate ACC latch occupies the existing aligned state tail padding; ADDA/SUBA/MULA and ACC-writing MAC update it, whereas FPR destinations preserve it. Existing field offsets and C++ state size are unchanged, and the LLVM type and full ctypes mirrors are updated. O0/O3 regressions use literal console result words from [muldiv.expected](https://github.com/unknownbrackets/ps2autotests/blob/master/tests/cpu/ee_fpu/muldiv.expected). Overflow precedence follows the behavior described by the [PCSX2 accurate recompiler](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/x86/iFPUd.cpp), while intermediate underflow sticky behavior is motivated by the EE Core Instruction Set Manual pp. 359–370. These flag/latch controls are model-derived, not physical-console flag traces; full FCSR sequencing and alternative execution-path parity remain open.

## Follow-up: standalone SQRT.S signed-digit consistency

SQRT.S now calls the private integer root recurrence already used by RSQRT.S. It retains the original sign-based invalid cause handling and does not reinterpret exponent 255 as IEEE infinity or NaN. The existing literal console result vectors agree with the recurrence; a separate sampled comparison over exponents 1–255 (mantissa step 4093) found 133,641 differences between the recurrence and host nearest rounding. See [PCSX2 PR #12001](https://github.com/PCSX2/pcsx2/pull/12001) (August 2026 square-root hardware discussion). Added explicit model-derived last-bit cases at O0/O3; they are not new physical-console captures. The FCSR policy is unchanged in this batch.

## Follow-up: cancellation underflow result encoding and flag evidence

Physical SCPH-900xx traces in [PCSX2 PR #12001, July 24 2026](https://github.com/PCSX2/pcsx2/pull/12001) show that ADD/SUB underflow retains the normalized fraction with exponent zero (e.g. 0x0080008b minus 0x008000ea = 0x803e0000, U/SU). The existing guard-bit model generates all four reported fraction words before its final flush; retain that fraction for true cancellation underflow, while exact zero and multiplier underflow still produce signed zero. Added four literal hardware result cases with U/SU checks and corresponding ACC model cases, plus a MAC cancellation model case and reported MAC sticky/overflow-latch and division range flag controls. The divide-unit range flag evidence is for late hardware; the research discussion reports possible differences on early revisions, so do not claim uniform behavior across revisions. Full FCSR cause sequencing remains open.

## Follow-up: FCSR same-unit cause replacement

The EE Core User's Manual v6.0 pp. 158–159 describes cause flags as current-result indicators and sticky flags as accumulated events. DIV.S now replaces I/D rather than indefinitely retaining both cause bits; ADD/SUB/MUL and ACC variants update both O and U even when the result overflows, rather than leaving a previous underflow cause set. Existing MAC and SQRT/RSQRT already replace their same-unit causes. Tests preseed conflicting cause/sticky states at O0/O3 and verify replacement with preservation of accumulated bits. This is a manual-derived policy; cross-unit cause retention is intentionally unchanged pending physical-console sequence traces, including early-hardware divide-unit differences.

## Follow-up: COP1 execution paths and VU0 FMAC

Both LLVM function builders share emit_insn, including control-transfer delay slots, and emit the required private COP1 helpers. The legacy native C++ emitter and RAM callback interpreter do not currently implement COP1 arithmetic. Their unsupported operations remain open rather than silently using approximations.

VU0 macro-mode VADD/VSUB and accumulator variants now use a guard-bit integer adder with extended exponent range, explicit O/U status, and cancellation fractions. Physical-console VU0 observations in [PCSX2 PR #12001](https://github.com/PCSX2/pcsx2/pull/12001) support the fraction and Z/U behavior. Standalone VMUL/VMULA explicitly detect true product underflow even when host arithmetic would already return zero. Added directed VU0 tests for cancellation, product underflow, ACC destinations and finite exponent-255 addition. Normal multiply precision, MAC intermediates, Q arithmetic, VU0 micro-mode and VU1 interpreter/AOT parity remain open; this batch does not change VU1 generated arithmetic.
