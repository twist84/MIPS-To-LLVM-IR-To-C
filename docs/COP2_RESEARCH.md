# VU0 / COP2 implementation research

## Decision

The decoder and LLVM lifter support the COP2 transfer/control forms QMFC2, QMTC2, CFC2, CTC2 and the four BC2 branches through runtime helpers. The in-tree runtime also has a deliberately narrow VU0 micro-instruction slice: lower IADD, ISUB, IAND, IOR, IADDI, IADDIU and ISUBIU, plus B, BAL, JR, JALR, IBEQ, IBNE, IBLTZ, IBGTZ, IBLEZ and IBGEZ, each paired with upper NOP. It models zero-latency lower integer writes, branch delay/nested-branch state, the conditional-branch VI-backup window and source interlocks. The generic four-entry IALU reservation queue remains available for future lower operations with nonzero latency; it is not used by this scalar integer subset. The scheduler remains host-integrated: the host supplies VU1 catch-up and EE event services, while unsupported VU0 pairs fail closed.

The current test boundary is stronger than isolated opcode checks: a 256-pair scalar IALU reference program and a 384-pair mixed integer/branch control-flow program compare architectural state after every pair against test-side PCSX2-derived models. Directed regressions exercise taken and not-taken boundaries for all six conditional branches, nested branch-delay handling, E termination from a branch pair, VI-backup lifetime/replacement and a branch read stall that completes beyond a one-cycle budget. [Actions run #43226060509](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43226060509) passed 15/15 CTest. These models are not a live comparison against PCSX2's interpreter, so the remaining next step is an external differential harness.

This is not a general VU0 micro-engine. VU data-memory operations, I/D/T modes, upper vector operations, full dual-issue ordering and FMAC/FDIV/EFU pipelines remain unsupported. The implemented branch subset is limited to lower integer control flow paired with upper NOP; it does not establish complete VU0 branch/pipeline equivalence.

VU0 macro instructions remain unsupported. Do not promote SPECIAL1/SPECIAL2 forms until the complete micro-engine and the required FMAC/FDIV/EFU pipeline semantics are available.

This note records the PCSX2 interpreter reference, implemented integration boundary and remaining semantic gaps. The runtime helper tests are not proof of a complete VU0 execution engine.

## R5900 COP2 top-level decode

The top-level COP2 opcode is `0x12`. The `rs` field selects the instruction family:

| rs | Instruction family | Behaviour |
|---:|---|---|
| `0x01` | QMFC2 | Move a 128-bit VU0 vector register to a GPR |
| `0x02` | CFC2 | Read a VU0 integer/control register into a GPR |
| `0x05` | QMTC2 | Move a 128-bit GPR to a VU0 vector register |
| `0x06` | CTC2 | Write a GPR value to a VU0 integer/control register |
| `0x08` | BC2F/T/FL/TL | VU0 condition branch; `rt` selects one of four defined table entries |
| `0x10..` | VU0 macro operation | Macro instruction groups; SPECIAL1/SPECIAL2 decode and VU pipeline semantics apply |

PCSX2's R5900 opcode table exposes QMFC2, CFC2, QMTC2, CTC2, BC2 and COP2_SPECIAL at these positions. Its COP2 SPECIAL1/SPECIAL2 dispatch is separate from the main SPECIAL and MMI tables.

The decoder now classifies the four transfer forms and BC2F/T/FL/TL as VALID only for their documented encodings. The LLVM path invokes the synchronized runtime helpers; BC2 reads VPU_STAT bit 8 and retains ordinary/likely delay-slot behaviour. If the runtime has no VU0 sidecar/scheduler where required, generated code exits at the guest instruction instead of guessing.

The VU0 SPECIAL1/SPECIAL2 macro forms are still classified by opcode table but remain unsupported by the lifter.

## Transfer semantics from PCSX2's interpreter

Reference: [PCSX2 VU0.cpp](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/VU0.cpp).

All four transfer/control-transfer handlers first call `vu0Sync()`. That synchronizes the VU0 micro engine with EE time; it is not equivalent to reading or writing a host-side array immediately.

There is an additional synchronization distinction when the current EE instruction is in a branch delay slot (PCSX2 checks `cpuRegs.code & 1`):

- QMFC2 and CFC2 call `_vu0FinishMicro()`: run VU0 until its E-bit termination.
- QMTC2 and CTC2 call `_vu0WaitMicro()`: run VU0 until E-bit or M-bit termination.

The synchronization helpers have materially different run policies:

- `vu0Sync()` is catch-up-only: if VU0 is active, it runs for the cycle distance `EE_cycle - VU0_cycle`. If that distance is negative, VU0 is already ahead and no VU instruction is run.
- `_vu0FinishMicro()` runs until E-bit termination, ignoring M-bit as an early stop.
- `_vu0WaitMicro()` may stop at E-bit or M-bit termination.
- The finish/wait variants add elapsed VU0 cycles to the EE, execute a VU1 catch-up block, and schedule a near-term event again if VU0 remains active.

PCSX2's `_vu0run` also checks whether VU0 is already ahead with M-bit set before entering the main loop. A host recompiler that does not model the EE cycle counter, VU0 cycle counter, E/M status and VU1 catch-up cannot faithfully replace these calls with no-ops or immediate array access.

The data movement and register-zero rules are:

### QMFC2

- Source: VU0 vector register `VF[fs]`, where `fs` is the encoded vector-register index.
- Destination: GPR `rt`.
- Copies both 64-bit halves of the 128-bit vector to the GPR.
- If `rt == 0`, no GPR write occurs.
- It performs the additional finish-micro synchronization in a delay slot.

### QMTC2

- Source: GPR `rt`.
- Destination: VU0 vector register `VF[fs]`.
- Copies both 64-bit halves.
- If `fs == 0`, no vector-register write occurs.
- It performs the additional wait-micro synchronization in a delay slot.

### CFC2

- Source index: `fs`, selecting a VU integer/control register.
- Destination: GPR `rt`; if `rt == 0`, no GPR write occurs.
- For `fs == REG_R (20)`, the returned value is `VI[R] & 0x007fffff`.
- For other indices in this handler, the 32-bit VI value is sign-extended into the low 64-bit GPR word.
- It performs the additional finish-micro synchronization in a delay slot.

### CTC2

- Source: low 32 bits of GPR `rt`.
- Destination index: `fs`; if `fs == 0`, no write occurs.
- It performs the additional wait-micro synchronization in a delay slot.
- MAC flag, TPC and VPU status are read-only in this handler.
- R is special: the write is `(value & 0x007fffff) | 0x3f800000`.
- FBRST masks the stored value and has reset/force-break side effects.
- CMSAR1 starts VU1 microcode; it is not just a VI register write.
- CLIP_FLAG updates the clip flag as well as following the current interpreter's VI write path.
- Other indices write the 32-bit VI register.

These operations therefore need a VU0-aware runtime helper or dispatcher boundary, not only a new LLVM GEP into a larger state struct.

## State required by the reference model

Reference: [PCSX2 VU.h](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/VU.h).

PCSX2's VU register set contains substantially more than the 32 vector registers:

- `VF[32]`: 128-bit vector registers.
- `VI[32]`: integer/control registers, each represented in a 128-bit slot with only the low 32 bits meaningful.
- `ACC`: a separate 128-bit vector accumulator.
- `q` and `p`: divide/EFU pipeline result registers.
- VU index, cycle and flags.
- Current instruction/micro-PC state, branch-delay state, E-bit state and pending pipeline state.
- FMAC, FDIV and EFU pipeline bookkeeping and flags.

The project retains `s_ee_cpu_state::m_vu0[32]` as the VF bank and adds a separate aligned `s_ee_vu0_state` sidecar. The sidecar provides VI[32], ACC, Q/P, visible control/flag values, micro-PC, EE/VU cycle counters, E/M/running state, and separate 4 KB VU data and micro-program memories without shifting the established CPU-state offsets.

The sidecar is not a full `VURegs` equivalent. It now includes the restricted lower-branch pipeline state (including deferred nested branches), a generic IALU destination-reservation queue and VI-backup state for conditional branch reads. The supported scalar IADD-family operations bypass the queue because their lower-op metadata specifies zero IALU latency. It still does not contain a complete dual-issue pipeline scoreboard, VF write/forwarding state, upper-pipeline dependencies, FMAC/FDIV/EFU internal state, or a VU1 execution engine. Those remain unimplemented. A helper-only mock can validate ABI, fetch and transfer rules, but cannot establish correct behaviour while a real VU microprogram is executing.

## Implementation path

1. **Lower integer execution:** the implemented subset covers IADD/ISUB/IAND/IOR/IADDI/IADDIU/ISUBIU and B/BAL/JR/JALR/IBEQ/IBNE/IBLTZ/IBGTZ/IBLEZ/IBGEZ, paired with upper NOP. Keep every other lower operation unsupported until independently specified and tested.
2. **VU0 memory operations:** add LQ/SQ/ILW/ISW with the distinct VU0 data-memory mapping, address wrap rules and interlocks.
3. **Upper/lower issue:** add upper vector execution with explicit dual-issue register hazards, delayed VF writes and flag forwarding.
4. **Pipeline completion:** model FMAC/FDIV/EFU scheduling and result/flag forwarding before adding macro instruction coverage.
5. **Semantic differential testing:** compare the existing lower integer/branch subset against PCSX2 over randomized and directed fixtures, especially nested branch timing, VI backup, stalls, cycle budgets and E/M interaction.
6. **Scheduler validation:** exercise real VU0 microprograms while EE transfers occur in ordinary code and delay slots; differential-test against PCSX2.
7. **Control actions:** implement FBRST reset/force-break and CMSAR1 VU1 startup in the host control callback. The current runtime only routes these actions; it does not emulate them.
8. **VU0 macro instructions:** enable incrementally only after the engine models dual-issue scheduling, flags, ACC/Q/P forwarding and branch timing.

## Current guardrail

QMFC2/QMTC2/CFC2/CTC2 and BC2 are enabled only through the fail-closed runtime boundary and only when the required host callbacks/state exist. The in-tree micro-engine executes only the explicitly listed lower integer/branch subset paired with upper NOP. Its direct runtime regression tests passed as part of [Actions run #43214060713](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43214060713) (15/15 CTest). These tests are not yet a full differential execution harness against PCSX2. The scalar ALU cases now explicitly check zero IALU-pipeline latency. This is integration and a limited execution slice, not a general VU0 engine. All VU0 macro instructions remain unsupported.


## Runtime state and synchronization interface

The runtime sidecar and COP2 integration are now represented in source:

- The existing `s_ee_cpu_state::m_vu0[32]` remains the VF vector-register bank, preserving its existing LLVM/native offsets.
- The 16-byte-aligned `s_ee_vu0_state` contains VI[32], ACC, Q/P, MAC/clip/status/control registers, TPC/micro-PC, EE/VU cycle counters, E/M/running state, and separate 4 KB data and micro-program memories.
- `m_tpc` is the visible VI[TPC] value in 8-byte instruction slots; `m_micro_pc` is the byte offset used by the active fetch loop.
- `ee_vu0_fetch_micro_instruction` fetches an aligned pair of little-endian 32-bit lower/upper words, decodes upper I/E/M/D/T flags, advances the byte PC by eight, and wraps at the 4 KB program boundary. Invalid/misaligned fetches leave both the PC and output untouched.
- `s_ee_runtime` can point to that sidecar and carry a host VU0 synchronization callback. The callback is separate from `m_user`, which remains the EE memory context.
- `ee_vu0_sync` validates its arguments and fails closed with UNAVAILABLE when either state or callback is missing. A missing scheduler is not silently treated as a completed synchronization.
- The transfer policy selector maps non-delay-slot transfers to catch-up-to-EE-cycle; delay-slot reads to finish-to-E; and delay-slot writes to wait-to-E-or-M.
- VI0 is exposed as a hardwired zero register by the new read/write helpers.

The fetch path now also performs a table-driven **classification** of both micro-op halves. The lower half uses the 7-bit primary opcode (`word >> 25`); primary `0x3c` selects the lower secondary table, whose low-six-bit selector includes the IADD/ISUB/IADDI/IAND/IOR entries and four T3 sub-tables. Each T3 sub-table selects its operation with bits 10..6. The upper half uses the low-six-bit primary opcode; `0x3c..0x3f` select the four FD sub-tables, again indexed by bits 10..6. All other primary entries are decoded directly. Unassigned table slots return `UNKNOWN`.

The operation identities are based on PCSX2's VU0 tables in [`VUops.cpp`](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/VUops.cpp). This is encoding classification only, not an execution implementation. The host engine must still resolve upper/lower dual-issue dependencies, maintain delayed VI/VF writes and FMAC/FDIV/EFU pipelines, process branches and end flags, and update architectural state. Transfer forms are integrated into the LLVM path and remain guarded by missing/failed scheduler callbacks. VU0 SPECIAL1/SPECIAL2 macro instructions remain unsupported.

The `vu0_runtime_test` suite checks sidecar layout, VI0 behavior, micro fetch and wrap, I/E/M/D/T flags, lower/upper operand extraction, primary and secondary opcode classification, transfer helpers, synchronization modes, callback state identity, error propagation, and fail-closed behaviour. Its mock-engine cases validate orchestration and decode identities, not instruction semantics.


## Initial synchronized transfer helpers

The runtime now has first-pass implementations for QMFC2, QMTC2 and CFC2. They all call `ee_vu0_sync` before touching their destination:

- QMFC2 copies both 64-bit halves of VF[fs] into GPR[rt]. GPR0 remains unchanged, but synchronization still occurs.
- QMTC2 copies both halves of GPR[rt] into VF[fs]. GPR0 is treated as a zero source and VF0 remains unchanged; synchronization still occurs for both cases.
- CFC2 reads the VI bank after synchronization, sign-extends ordinary 32-bit VI values into the low GPR doubleword, and applies the 23-bit mask for R (VI index 20). It preserves the GPR's upper 64-bit half, consistent with the current scalar write helpers.
- All three helpers leave architectural destinations untouched when synchronization is unavailable or reports failure.

These runtime-callable helpers are now emitted by the LLVM lifter for QMFC2, QMTC2, CFC2 and CTC2, and the decoder validates their reserved bits before classifying them as VALID. CTC2's read-only, R, FBRST, CMSAR1 and CLIP_FLAG cases are handled explicitly; side-effectful FBRST/CMSAR1 operations still depend on a host control callback. JIT integration tests exercise transfers, cache barriers, failure exits and delay-slot synchronization.

### Transfer edge cases verified against PCSX2

- VF0 is not an all-zero quadword. It is the immutable vector `(0.0f, 0.0f, 0.0f, 1.0f)`; QMFC2 must return `m_hi = 0x3f80000000000000` and a zero low half even if a backing VF0 slot contains stale data.
- QMTC2's VF0 destination is discarded.
- CFC2's R path writes only GPR `UL[0]` with the 23-bit masked value. Unlike ordinary VI reads, it does not update `UL[1]`; that pre-existing upper word must be preserved. Ordinary reads sign-extend bit 31 into `UL[1]`.


## CTC2 control-register handling

The first CTC2 runtime helper now implements the interpreter's explicit register cases:

- VI0 writes are ignored; ordinary writable registers update VI.
- MAC_FLAG, TPC and VPU_STAT are read-only.
- R stores `(value & 0x007fffff) | 0x3f800000`.
- CLIP_FLAG updates both the VI slot and the separately tracked clip flag.
- FBRST stores `value & 0x0c0c`; when any reset/break action bits are present, a host control callback must handle them.
- CMSAR1 is routed through the host control callback, which is responsible for finishing/starting the VU1 micro subroutine using the low 16-bit address.
- Missing/failed host control callbacks fail closed before the helper commits the relevant register state.

The helper does not emulate VU1 or execute FBRST hardware actions itself. These remain responsibilities of the host control callback. QMFC2/QMTC2/CFC2/CTC2 and BC2F/T/FL/TL are now decoded and lowered through the runtime boundary; VU0 SPECIAL1/SPECIAL2 macro instructions remain UNSUPPORTED.
## Bounded scheduler policy adapter

`ee_vu0_scheduler_sync_callback` now implements the orchestration around a host-provided micro-engine slice callback:

- Catch-up-only calls the engine with the exact positive difference between EE and VU0 cycle counts; when VU0 is already ahead it does no execution. It does not account cycles on the EE, catch VU1 up, or schedule an EE event.
- Each host micro-engine slice returns at an M-flagged pair, matching PCSX2's `Execute()` boundary. Finish-to-E invokes another slice after M and continues until E; wait-to-E-or-M returns when that slice reports M. The early M-bit/already-ahead path advances EE time to the VU0 cycle and returns without the normal epilogue.
- Finish/wait account the VU0 cycles consumed on EE, call the host VU1 catch-up hook, and schedule a four-cycle EE event if VU0 remains active.
- Every callback result is validated. Missing VU1/event services are detected before interlocked execution; non-progressing engines and excessive run-call counts fail rather than looping indefinitely.

The scheduler adapter itself does not execute VU micro-instructions; it orchestrates the host engine callback. The in-tree `ee_vu0_execute_ialu_callback` currently supplies a restricted lower-integer execution slice, with every supported pair required to have upper NOP:

- Lower IADD/ISUB/IAND/IOR/IADDI/IADDIU/ISUBIU perform immediate 16-bit VI writes, preserve the upper halfword and keep VI0 hardwired. PCSX2 lower-op metadata assigns these scalar integer operations zero IALU-pipeline latency, so they bypass the reservation queue. The four-entry queue remains a generic facility for future nonzero-latency operations such as ILW/ILWR.
- Lower B/BAL/IBEQ/IBNE/IBLTZ/IBGTZ/IBLEZ/IBGEZ/JR/JALR execute through the same callback. Relative branch offsets use the already-advanced TPC plus the signed 11-bit immediate field (bit 10 is the sign bit). JR/JALR targets are the VI low halfword multiplied by eight and wrapped to the 4 KB micro store.
- The branch state applies a one-pair delay after branch issue. A second branch in that delay pair is deferred until after the first target instruction. BAL/JALR link values account for this nested-delay case; JALR samples its target before writing the link, including when Is == It.
- Conditional branch reads observe PCSX2's VI-backup window. An IALU write saves the old low halfword for a two-cycle window; repeated writes to the same VI preserve the value from before the write chain, while another VI write replaces the backup register. The execution order is important: at PCSX2 commit 82cac38b2af19d593dedfd9ddaa63448e7ec493c (https://github.com/PCSX2/pcsx2/blob/82cac38b2af19d593dedfd9ddaa63448e7ec493c/pcsx2/VU0microInterp.cpp#L924-L1020), lower-op metadata is decoded, _vuTestLowerStalls runs, matured pipes are retired, VIBackupCycles is aged by the elapsed issue/stall cycles, and only then _vu0ExecLower executes the operation. Branch predicates, JR/JALR targets and link values are therefore sampled after the stall and backup aging. The in-tree runtime now follows that ordering; a lower IALU write creates a fresh two-cycle backup after the pre-handler aging.
- E sets a two-pair delay and then clears VI backup state. With the current scalar-only subset there are no outstanding IALU reservations to flush, so E adds no spurious extra cycle. M returns from one engine slice regardless of wait policy; the outer scheduler resumes for FINISH_TO_E or stops for WAIT_TO_E_OR_M.

The runtime regression suite covers the supported lower integer ops, all six conditional branch predicates, B/BAL/JR/JALR, ordinary and nested delay slots, JALR source/destination overlap, signed backward offsets, VI-backup windows and E/M scheduling. A deterministic 256-pair scalar IALU sequence and a 384-pair mixed scalar/branch control-flow program are compared after every pair with separate test-side reference models. The mixed program begins with a directed linear prefix that guarantees execution of all 17 supported lower-op classes (seven scalar operations, four unconditional transfers and six conditional branches), then continues into a randomized control-flow tail. The mixed model checks all VI words, backup state, PC/TPC, branch countdown/targets/deferred targets, E/M state and queue state. At the first IBEQ it injects a three-cycle reservation on VI1; the stall advances cycle 15 to 18, expires the one-cycle backup before the lower handler, and makes IBEQ compare the current VI values despite a one-cycle exact execution budget. A separate directed test gives IBEQ two matching reservations completing at cycles 3 and 5 and a two-cycle budget; it verifies that the latest dependency controls the stall and that the expired backup is not used. The test-side models remain PCSX2-derived reference models, not execution of the PCSX2 interpreter itself. GitHub Actions [run 43229844156](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43229844156) passed **15/15 CTest** after the issue-order correction, including the 256-pair scalar oracle, 384-pair mixed oracle, all-17-op execution, and one-/two-budget multi-source interlock regressions.

This is still **not** a general VU0 interpreter. VU data-memory operations, I/D/T flags, upper vector operations, general upper/lower dual-issue dependencies, VF and flag forwarding, and FMAC/FDIV/EFU pipelines remain unsupported. Unsupported pairs fail closed without consuming the micro-PC. The regression suite includes a PCSX2-derived issue-order/forced-stall check and mixed control-flow reference model, but is not yet a live differential harness against the PCSX2 interpreter.
