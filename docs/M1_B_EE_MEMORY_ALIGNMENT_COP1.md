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
