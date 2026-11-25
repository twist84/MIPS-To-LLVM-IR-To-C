# M2.7 — Strict bounded native VU1 and GIF PATH1, first batch

> **Record status (2026-11-25):** First bounded VU1 implementation; native VU1 AOT and asynchronous execution were later observed in gameplay. The remaining EFU and precise timing work is tracked separately. See [current status](README.md), [retail validation](SDBZ_RUNTIME_VALIDATION_20261125.md) and [accuracy plan](ACCURACY_PLAN.md). Branch names, open PR instructions, and pending gates in the historical text refer to the documented revision.


**Status:** partial implementation; real USA SDBZ game boot unverified.
**Branch:** feature/sdbz-hle-first-bootstrap; draft PR #142 (unmerged).
**Reference:** twist84/ps2-web-recomp:SDBZ runtime/src/vu_micro.cpp and runtime/include/ps2/vu_ops.hpp (Samir Nuri, MIT 2026); retained license attribution.

## Implemented

- 16 KiB resident VU1 code/data, microinstruction pairs, bounded E-bit delay-slot execution, staged upper VF writeback, I-bit, partial floating-point vector arithmetic, vector memory operations, VI integer operations, VI branch-delay view, TOP/ITOP, integer branches, TPC and XGKICK.
- XGKICK walks real GIFtags and payload in VU1 data (up to 1024 qwords, wraps data memory) and forwards only an EOP-complete packet to a separate GIF PATH1 parser sharing the original GS/WebGPU event sink.
- MSCAL launches the real MPG-uploaded VU1 microinstructions; MSCNT resumes at real TPC after a prior accepted program. FBRST retains resident code/data and VU1 registers.
- VIF1 stages VU1 state/data before GIF emission; rejected packets leave VIF1 DMA active, with micro-PC and upper/lower words logged.
- Mixed PATH1/PATH2 within one DMA transfer is explicitly rejected pending a transactional per-path arbiter.

## Deliberately unsupported

This is **not full VU1 parity**. Batch B provides reference-derived ACC/ADDA/MULA/MADD/MSUB, OPMSUB/OPMULA, FTOI/ITOF, CLIP, MAC/status/clip flag registers, four-issue flag visibility, and FCGET/FSEQ/FSSET/FSAND/FSOR/FMEQ/FMAND/FMOR/FCAND/FCOR/FCEQ/FCSET. Still unsupported: Q/P unit timing, precise VF/VI dependency stalls, lower EFU/RNG and related special instructions, MSCALF synchronization, GIF PATH1/2/3 arbitration, and VIF IRQ. These paths fail closed. There is no claim of complete PS2 VU correctness or original game progression. Complete the upstream interpreter in coherent tested batches before relaxing these gates.

The original 13:53 boot showed VIF1 source-chain CHCR=0x1c5 and TADR=0x00771400, but was captured before source-prefix diagnostics existed. No game VIF opcode is proved reached. Only a new timestamp-matched Windows boot batch can prove progression.

## Regression

The ee_vif1_device test adds a real 3-pair VU1 MPG program (IADDIU, XGKICK with E-bit and delay slot) and actual in-memory GIFtag/body, proves PATH1 events, and rejects an unsupported upper opcode without emitting GIF. A standalone direct VU1 arithmetic/E-bit test checks VF results and two executed pairs. Existing TTE, UNPACK, PATH2 and MMIO tests remain. CI completion must be verified on the final exact head; tests are not evidence of game boot.

## Next batch

Next port upstream Q/P scalar pipelines, lower DIV/SQRT/RSQRT, register-hazard stalls, EFU/RNG specials and remaining micro opcodes, with differential vectors. Then unify GIF PATH1/2/3 arbitration and VIF/DMAC stalls.


## Batch B — PS2 FMAC and delayed flags (validation pending)

- Ported ps2-web-recomp:SDBZ runtime/include/ps2/fpu.hpp to C++17-friendly src/runtime/ee_vu1_fp.h, replacing bit_cast/countl_zero without changing reference arithmetic.
- Implemented VU1 ACC operations, MADD/MSUB, FTOI/ITOF, CLIP, VF0 suppression, MAC/status/clip results and four-issued-pair flag visibility, with lower flag instructions.
- Corrected synthetic XGKICK GIFtag fixture: NREG=0 means sixteen payload registers; the fixture now encodes NREG=1 for its single payload.
- Added ACC/MADD, FTOI4 and delayed CLIP->FCGET regression vectors. No real-game boot progression is claimed.


## Batch C — Q/P, specials, and dependency scoreboard (new PR)

- **Integration:** PR #142 was squash-merged as `c576a0cec1732d21efb04889cc95a627f0883ece`. This batch lives on `feature/sdbz-m2-7-vu1-q-p-pipelines` based on that commit; do not add further Batch C changes to PR #142.
- Ported Q FDIV instruction effects (DIV, SQRT, RSQRT), seven- and thirteen-cycle pending Q results, WAITQ interlock, and upper MULq/ADDq/MADDq/SUBq/MSUBq plus accumulator variants. Early Q reads observe the previous value; a new FDIV producer stalls until the prior one completes.
- Added VU1 EFU families ESADD, ERSADD, ELENG, ERLENG, EATANxy/xz, ESUM, ESQRT, ERSQRT, ERCPR, ESIN, EATAN and EEXP using reference arithmetic. Pending P is visible only after its instruction-specific latency. MFP can observe old P; WAITP and a new EFU producer synchronize it.
- Added lower LQI/LQD/SQI/SQD, MTIR/MFIR, ILWR/ISWR, RINIT/RNEXT/RGET/RXOR and MFP. Random number recurrence is taken directly from the reference. VF/VI register dependency scoreboarding uses the reference four-cycle lane-specific vector availability and integer-load hazard model.
- The interpreter remains bounded; unknown opcodes, reserved EFU specials and D/T debug interrupt requests reject instead of silently completing. Existing VIF1 staging still gates GIF/DMA commit. No change to LLVM-IR EE execution, VIF DMA completion semantics or native rendering is asserted.
- The separate `ee_vu1_pipeline` CTest exercises Q/P old-to-new visibility and waits, Q/EFU computation, RNG, sign extension, memory update, VF dependency stalls and unsupported debug-bit rejection. The existing `ee_vif1_device` test retains VIF1 rollback and PATH1 integration checks.

**Remaining to close M2.7:** complete instruction/branch-delay parity against the original game VU programs, precise hardware flag/VI pipeline corner cases, reference differential vector sweeps, and confirmed GIF back-pressure semantics. The reference VU interpreter currently performs EFU effects immediately; this port adds manual-derived latencies without claiming a complete cycle-accurate reference differential. M2.8 GIF arbitration and M2.9 DMA scheduling are separate milestones. A green synthetic test is not a new matched Windows boot.
