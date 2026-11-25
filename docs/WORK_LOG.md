# Work Log

> **Record status (2026-11-25):** Chronological early work log. Its recovery branch instructions and validation counts are historical; do not treat them as current development gates. See [current status](README.md), [retail validation](SDBZ_RUNTIME_VALIDATION_20261125.md) and [accuracy plan](ACCURACY_PLAN.md). Any branch, PR, pending replay or feature-status claims below refer to the documented stage.


## 2026-11-11 — Recovery baseline

- Confirmed private GitHub repository access and created this dedicated recovery branch from the initial repository README.
- Recovered the latest source follow-up preserved in the Library (09:57 UTC).
- Inspected the separately named 14:30 worktree archive; despite its name it contains the older reviewed source and does not contain the newer four-test state.
- Reconstructed the source from the 09:57 follow-up over the reviewed baseline.
- Performed a clean CMake/Ninja build and ran CTest: 4/4 passing; decoder regressions: 269/269.
- The exact later 2/4 worktree and logs remain unrecovered because the previous chat never exported them.

Further changes should be committed to this branch in small, testable increments. Do not merge it into `main` until the source and test baseline have been reviewed.


## 2026-11-11 — MMI arithmetic and MAC pipeline follow-up

- Made CI install `llvmlite` in an isolated virtual environment so the LLVM JIT runtime test is included rather than silently skipped.
- Expanded the O0–O3 JIT fixture to check the exact enabled MMI0 arithmetic operations (PADD/PSUB/PMAX at word, halfword, and byte widths), alongside PAND/PXOR/POR/PNOR, HI1/LO1 transfers, aliasing, upper-GPR preservation, and cross-block writeback.
- Corrected MADD/MADDU and MADD1/MADDU1. They now accumulate the signed/unsigned 32×32 product into the appropriate 64-bit HI:LO or HI1:LO1 accumulator, write sign-extended result words back to the special registers, and return the low result word in rd.
- Updated decoder validity and lifter mapping for the four exact MADD/MADDU encodings; nonzero reserved `sa` forms remain unsupported.
- Verified against the EE Core Instruction Set Manual, Version 6.0, MADD/MADD1/MADDU/MADDU1 descriptions and operation pseudocode.
- GitHub Actions run [43089047322](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43089047322): clean configure/build and **4/4 CTest passed**, including the LLVM JIT runtime fixture.



## 2026-11-11 — Pipeline-1 multiply support

- Added exact decoder/lifter coverage for `MULT1` and `MULTU1` (funct 0x18/0x19, reserved `sa` must be zero).
- Implemented signed and unsigned 32×32 multiplication into HI1/LO1, including sign-extension of the resulting 32-bit HI1/LO1 words and the low-word result in rd.
- Extended the LLVM JIT fixture to verify MULT1/MULTU1, in addition to MADD/MADDU and the MMI arithmetic/logical operations.
- Verified against the EE Core Instruction Set Manual v6.0, MULT1 and MULTU1 operation definitions.
- GitHub Actions run [43089471611](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43089471611): clean configure/build and **4/4 CTest passed**, including the JIT runtime fixture at O0–O3.


## 2026-11-11 — Pipeline-1 division support

- Added exact decoder/lifter support for `DIV1` and `DIVU1` (funct 0x1a/0x1b; reserved `sa` must be zero).
- Implemented signed and unsigned 32-bit quotient/remainder lowering into LO1/HI1, respectively.
- Guarded signed `INT_MIN / -1` before LLVM `sdiv`/`srem`, then applied the EE-defined quotient/remainder result.
- The EE manual leaves divide-by-zero results undefined. The recompiler chooses a deterministic no-op for HI1/LO1 on a zero divisor, avoiding LLVM undefined behavior/poison.
- Added an independent JIT fixture covering signed normal division, signed overflow, signed divide-by-zero, unsigned division, and unsigned divide-by-zero; each case also verifies MFHI1/MFLO1 readback at O0–O3.
- GitHub Actions run [43090955314](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43090955314): clean configure/build and **5/5 CTest passed**.

## 2026-11-11 — Exact MMI1 packed operations

- Added exact decoder/lifter/LLVM lowering for 13 MMI1 operations:
  - `PABSW` and `PABSH`, including the defined clamp of signed minimum to signed maximum.
  - `PCEQW`, `PCEQH`, and `PCEQB`.
  - Signed `PMINW` and `PMINH`.
  - Unsigned saturating `PADDUW/H/B` and `PSUBUW/H/B`.
- Restricted unary absolute-value forms to their documented `rs=0` encodings.
- Added a separate O0–O3 LLVM JIT fixture that checks all 13 operations, lane ordering, saturation edges, and a destination/source alias.
- Updated the decoder regression for the game entry-point word `0x70000c28`: it is now correctly accepted as exact `PADDUW`, not rejected.
- Left the remaining MMI1 operations (`PADSBH`, `PEXTUW/UH/UB`, and `QFSRV`) unsupported pending their own exact implementations and tests.
- GitHub Actions run [43093331006](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43093331006): clean configure/build and **6/6 CTest passed**.

## 2026-11-11 — Complete the MMI1 operation table

- Extended the exact MMI1 implementation to cover the remaining defined sub-ops: `PADSBH`, `PEXTUW`, `PEXTUH`, `PEXTUB`, and `QFSRV`.
- Added exact `MTSAB` and `MTSAH` lowering so QFSRV obtains its byte/halfword shift amount from the architectural SA field rather than a test-only preset.
- Added the three-instruction SA read-hazard gap before the second SA-setting instruction in the runtime fixture.
- Extended decoder/lifter assertions and the LLVM JIT fixture to exercise all 18 non-reserved MMI1 sub-ops, plus MTSAB/MTSAH, at O0–O3 and multiple SA values.
- The source and test changes are committed on this branch. CI runs for the latest commits are currently queued; no final result is recorded yet.

## 2026-11-11 — Host dispatcher and cross-function control-flow validation

- Added end-to-end LLVM JIT coverage for dynamic `JALR` across separately translated caller and callee functions. The test checks that the target register is sampled before the delay slot overwrites it, `rd` receives the PC+8 link, the callee returns, and the caller resumes at its PC+8 continuation.
- Added `ee_dispatch_run()` as a bounded host dispatcher over `s_recompiled_elf::functions` plus a parallel array of native entry points.
- The dispatcher resolves the current guest PC to its owning translated function. Both `EE_EXIT_RETURN` and `EE_EXIT_DISPATCH` re-enter translated code when the next PC belongs to a registered block; an unmapped PC is returned to the outer host with its original exit reason.
- Added explicit invalid-table checks and a maximum-dispatch budget so a malformed guest loop cannot monopolise the host indefinitely.
- Added six native dispatcher tests: direct caller/callee/return continuation, dynamic JALR/callee/caller continuation, return to external code, unknown initial PC, dispatch-budget exhaustion, and metadata/native-entry array mismatch.
- Moved `find_recompiled_function()` into the core library so dispatcher clients and tests share the same implementation without linking the full ELF recompiler into the decoder test.
- GitHub Actions run [43109193936](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43109193936): clean configure/build and **9/9 CTest passed**, including the new `dispatcher` test and all LLVM JIT suites.
- Limitation: the dispatcher is now implemented and independently tested, but the real LLVM object emitter still returns false. The dispatcher therefore has not yet been wired to a complete set of generated native game functions.

## 2026-11-11 — Real-game exception continuation and full-image validation

- Ran the CI-built `ee_recomp` binary against the preserved USA `SLUS_214.42` ELF.
- The first run emitted only the initial bootstrap function. Inspection showed that startup code executes `SYSCALL` and then continues after the exception handler; static discovery treated `FLOW_EXCEPTION` as an unconditional end and never reached the later direct JAL targets.
- Updated `collect_successors()` to include the possible `EPC+4` resume PC for exception instructions. Updated the CFG builder to retain that continuation as a separate dispatchable block while keeping the syscall/break block itself as a runtime exception exit.
- Added regressions for successor discovery after both SYSCALL and BREAK, plus CFG retention of code after both exception blocks.
- Re-ran the CI-built recompiler on `SLUS_214.42` (entry `0x00100008`, machine 8, two segments): **7,849 functions discovered and emitted**, covering **417,738 unique instruction PCs**. The current lifter reports 409,530 supported and 8,208 unsupported instructions (98.0%).
- Parsed and LLVM-verified all **7,849/7,849** generated modules with LLVM 20.1.8; zero invalid modules.
- Also ran llvmlite native target code generation at `opt=0` over all **7,849/7,849** modules. All succeeded, producing **34,220,040 bytes** of aggregate in-memory host object code.
- The validation used the Linux `ee_recomp` and `verify_ir.py` from CI run [43110647060](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43110647060), commit `d76f7ec`.
- Limitation: llvmlite emitted host objects in memory for validation; the project `object_emitter::emit_object()` remains a stub, and these objects were not linked into a runnable game executable.

## 2026-11-11 — Enable exact 64-bit scalar comparisons

- Confirmed the existing LLVM emitter already contains exact lowerings for `SLT`, `SLTU`, `SLTI`, and `SLTIU`, and the CFG liveness analysis already accounts for their source and destination registers. The decoder was still marking those operations unsupported.
- Marked `SLTI` and `SLTIU` valid, and marked `SLT`/`SLTU` valid only when their reserved shift-amount field is zero. Non-zero reserved encodings remain unsupported.
- Added decoder coverage and a generated LLVM JIT fixture checking signed versus unsigned 64-bit ordering, sign extension of the `SLTIU` immediate, read/write aliases, destination `$zero`, and preservation of the upper 64-bit GPR halves.
- Fixed the JIT harness to register the native LLVM target and assert the expected sequential dispatcher exit/PC.
- GitHub Actions run [43112984141](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43112984141), commit `5aba8e1`: clean configure/build and **10/10 CTest passed**.
- Re-ran the CI-built recompiler against USA `SLUS_214.42`: still **7,849 functions / 417,738 PCs**, with **414,916 supported** and **2,822 unsupported** instructions (**99.3%**, up from 98.0%). The 5,386 newly supported instructions are exactly the four scalar comparison families.
- LLVM parse/verify: **7,849/7,849 passed**, zero invalid modules.
- Native host-object generation via llvmlite at `opt=0`: **7,849/7,849 passed**, total **34,756,008 bytes** of in-memory object code (30,660,224 bytes for the first 7,000 modules plus 4,095,784 bytes for the remaining 849).
- The project's own `object_emitter::emit_object()` remains a stub; object bytes were emitted in-memory by llvmlite for validation and were not linked into a runnable executable.
## 2026-11-11 — COP1 FCC0 comparisons and full-game native linking

- Added exact FCC0-only `C.F.S`, `C.EQ.S`, `C.LT.S`, and `C.LE.S` decoding/lowering. Other condition-code destinations remain unsupported because the current state model only exposes FCC0.
- Added LLVM JIT coverage for all four predicates, signed zero, denormal flushing, infinity/NaN saturation to signed FMAX, and preservation of non-FCC0 FCSR bits.
- Corrected the fixture to match the R5900 comparison operand normalisation rather than applying host IEEE unordered-NaN expectations directly.
- GitHub Actions run [43115526527](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43115526527): **12/12 CTest passed**.
- Extended `tools/link_native_objects.py` to compile its large generated dispatcher metadata translation unit separately at a configurable optimisation level (default O0). Fixed response-file quoting/newline emission for thousands of object inputs.
- GitHub Actions run [43117522952](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43117522952), commit `f583796`: clean build and **12/12 CTest passed**, including the object-emission/link/dispatcher integration test.
- Recompiled the preserved USA `SLUS_214.42` ELF with the matching CI build: **7,849 functions**, **417,738 unique instruction PCs**, **416,648 supported / 1,090 unsupported** (99.7% decoder/lifter classification).
- LLVM 20.1.8 parse/verify and native object generation at O0 passed for **7,849/7,849 modules**. The persisted objects total **35,026,424 bytes** and all 7,849 defined guest symbols were checked against the generated metadata.
- Ran the committed linker tool over all 7,849 objects and the matching runtime/dispatcher sources. It produced a **28,618,808-byte** x86-64 shared library exporting `ee_game_run` and `ee_fn_00100008`. The library loaded through `ctypes.CDLL`; `ldd -r` reported no unresolved dynamic symbols.
- Full validation details: `FULL_GAME_NATIVE_LINK_VALIDATION.md`.
- Limitation: the linked library is not a bootable PS2 game and the full game was not executed. The EE runtime still lacks BIOS/IOP, GS, DMA, TLB/MMIO devices, interrupts, and other console services. The in-tree `object_emitter::emit_object()` remains a stub; the persisted objects were emitted using the external llvmlite validation tool.


## 2026-11-11 — CVT.S.W JIT regression coverage

- Extended the existing COP1 LLVM fixture to emit the exact `fmt=W, funct=CVT.S.W` encoding.
- Added O0 JIT runtime cases for zero, positive/negative values, the 16,777,217 single-precision rounding boundary, and signed 32-bit extrema.
- The test verifies the low 32-bit signed source is used, the destination is written as a zero-extended FPR word, the source is preserved, and FCSR is unchanged.
- The lowering itself was already present (`sitofp i32 to float`); this change adds execution coverage for the only supported `fmt=W` operation.
- CI result for this follow-up is pending; do not treat it as verified until the Actions run completes.


## 2026-11-12 — Complete supported COP1-S coverage and record COP2 boundary

- Compared the COP1 decoder/lifter coverage against PCSX2's R5900 COP1 table. Kept non-table conversion forms unsupported; the R5900 table does not define the conventional ROUND.W.S/TRUNC.W.S/CEIL.W.S/FLOOR.W.S family.
- Corrected MAX.S/MIN.S to use the EE's raw signed-bit ordering instead of host floating-point comparisons. Added raw-bit ABS.S/NEG.S handling and exact FCSR cause-bit clearing.
- Added EE operand normalisation and result handling for ADD.S/SUB.S/MUL.S/DIV.S and SQRT.S/RSQRT.S, including denormal flushing, signed FMAX saturation, divide-by-zero classification, and cause/sticky flag behavior.
- Added distinct accumulator paths for ADDA.S/SUBA.S/MULA.S, MADD.S/MSUB.S and MADDA.S/MSUBA.S. Runtime tests distinguish the normalized accumulator/product path from the raw accumulator/product path using denormal and overflow cases.
- Added MFC1/MTC1/CFC1/CTC1 runtime tests. Fixed block read/write liveness so CFC1 marks its destination GPR as written and CTC1 marks its source GPR as read. A cross-block ADDIU-to-CTC1 test confirms the value reaches FCSR.
- Added BC1T condition-sampling coverage with a CTC1 delay slot, plus BC1FL not-taken delay-slot annulment coverage.
- The first BC1 JIT test attempt failed only because the Python test called the generated two-argument guest ABI with one argument. Passed the runtime pointer and reran.
- GitHub Actions run [43125195580](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43125195580), commit `ce5f3522`: clean configure/build and **12/12 CTest passed**, including all COP1 JIT tests.
- Added `COP2_RESEARCH.md` using PCSX2's VU0 interpreter and state structures as references. Documented QMFC2/QMTC2/CFC2/CTC2 synchronization and side effects, BC2/macro decode groups, and the current project state gap. No COP2 opcode was promoted to VALID; VU0 transfers require a scheduler/state model rather than a direct register-copy lowering.


## 2026-11-12 — COP2 transfer encoding validation

- Started the VU0 transfer target without prematurely enabling execution. QMFC2, CFC2, QMTC2 and CTC2 remain `UNSUPPORTED` because their EE/VU0 synchronization and register side effects are not implemented.
- Tightened COP2 decoding: all four transfer encodings require the low eleven `sa`/`funct` bits to be zero. Reserved-bit violations now classify as `INVALID`, rather than being conflated with valid-but-unimplemented transfers.
- Added positive and negative decoder cases for all four transfer forms.
- GitHub Actions runs [43170109202](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43170109202) and [43170132717](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43170132717) were queued/in progress at the time of this note; verify their final results before treating this change as tested.
- Next: build the VU0 runtime state and synchronization interface, then test the transfer helpers independently before allowing the lifter to emit them.

- CI caught a mnemonic-table index error in the first patch (the compiler build passed; the decoder test reported two failures). Corrected the selector-to-name mapping in commit `f8762ed`; that commit's Actions result is pending.


## 2026-11-12 — VU0 runtime state and scheduler boundary

- Added a separately aligned `s_ee_vu0_state` sidecar for the VU0 VI bank, ACC/Q/P, visible flags/control registers, micro-PC, EE/VU cycle counters and E/M/running state. Kept the existing VF vector bank in `s_ee_cpu_state::m_vu0[32]` to avoid shifting its established LLVM/native ABI offsets.
- Extended `s_ee_runtime` with an optional sidecar pointer and a dedicated VU0 scheduler callback/context; the existing `m_user` memory context remains first and unchanged.
- Added `ee_vu0_sync`, a fail-closed callback boundary. Missing VU0 state or scheduler returns UNAVAILABLE rather than pretending synchronization succeeded.
- Added the transfer synchronization policy selector: ordinary transfers catch up to the EE cycle; delay-slot reads finish to E; delay-slot writes wait to E or M.
- Added VI read/write helpers with VI0 hardwired to zero.
- Added `vu0_runtime_test` coverage for layout, VI0, all synchronization modes, callback state/PC propagation, callback failures, and missing-runtime fail-closed paths.
- This does not yet provide an actual VU0 scheduler. COP2 transfer decodes remain UNSUPPORTED until a host scheduler is connected and tested against PCSX2 timing semantics.


## 2026-11-12 — First synchronized COP2 transfer helpers

- Added runtime helpers for QMFC2, QMTC2 and CFC2. Each calls the VU0 synchronization callback before performing any architectural transfer; missing or failed synchronization leaves the transfer destination unchanged.
- QMFC2 copies the full 128-bit VF value to the full 128-bit GPR, respecting hardwired GPR0.
- QMTC2 copies the full 128-bit GPR value to VF, treats GPR0 as a zero source, and protects VF0 from writes.
- CFC2 sign-extends ordinary VI values into the low GPR doubleword, masks the R register (VI index 20) to 23 bits, and preserves the GPR upper 64-bit half.
- Expanded `vu0_runtime_test` to cover full-width copies, zero registers, CFC2 R handling, scheduler failures and destination preservation.
- The LLVM emitter and decoder are intentionally unchanged: these transfer helpers are not yet wired to generated guest code, and the corresponding instructions remain UNSUPPORTED. CTC2 is deferred pending explicit side-effect mapping.

- Follow-up semantic review against PCSX2 corrected two edge cases in the transfer helpers: QMFC2 reads VF0 as the constant (0,0,0,1.0f), and CFC2's R-register path replaces only GPR UL[0] while preserving UL[1].


## 2026-11-12 — CTC2 special-register runtime semantics

- Added a dedicated host control-action callback for CTC2 FBRST and CMSAR1. It is separate from the VU0 cycle-sync callback.
- Implemented VI0 write suppression, ordinary VI writes, read-only MAC_FLAG/TPC/VPU_STAT, the R-register exponent rule, CLIP_FLAG mirroring, FBRST masking and CMSAR1 low-16-bit dispatch.
- CFC2 now reads the dedicated tracked MAC/clip/status/TPC/FBRST/VPU_STAT values where those indices map to PCSX2's visible special-register state.
- Side-effectful FBRST/CMSAR1 writes fail closed when the host action callback is missing or fails; relevant register state is not committed on failure.
- Expanded `vu0_runtime_test` for all of those cases. COP2 decoder and LLVM emission remain disabled pending JIT integration.


## 2026-11-12 — Bounded VU0 scheduler adapter and COP2 integration status

- Added a host scheduler interface with an execution-slice callback, a VU1 catch-up callback, and an EE event-scheduling callback. The host micro-engine remains responsible for executing VU instructions and updating VF/VI/ACC/Q/P, micro-PC, cycle counters, and E/M status.
- Implemented `ee_vu0_scheduler_sync_callback` as the reusable orchestration layer:
  - Catch-up-only uses the exact positive EE/VU0 cycle difference, does not run when VU0 is already ahead, and does not account EE cycles or invoke VU1/event callbacks.
  - Finish-to-E ignores M-bit termination; wait-to-E-or-M requests both stop conditions and includes the already-ahead M-bit fast path.
  - Normal interlocked completion accounts the elapsed VU0 cycles on EE, catches VU1 up, and schedules a four-cycle EE event if VU0 remains active.
  - Missing callbacks, host failures, invalid results, non-progressing engines and execution-call budget exhaustion fail closed.
- Added mock-engine regression coverage for exact catch-up budget, VU0-ahead behaviour, finish/wait termination, M-bit fast path, VU1 catch-up, event scheduling, callback failures and execution limits.
- Updated `COP2_RESEARCH.md` and `RESUME.md` to reflect the later COP2 transfer/BC2 LLVM integration and to distinguish the scheduler adapter from a real VU0 micro-engine.
- GitHub Actions [run 43180405182](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43180405182): clean configure/build and **14/14 CTest passed**, including `vu0_runtime` and `vu0_transfer_ir_runtime` plus the existing suites.
- Documentation follow-up Actions [run 43180563505](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43180563505) also completed successfully.
- Remaining: supply a real host VU0 micro-engine and differential-test real running microprograms against PCSX2. VU0 SPECIAL1/SPECIAL2 macros remain UNSUPPORTED.


## 2026-11-12 — Correct VU0 interlocked EE cycle accounting

- Cross-checked the scheduler adapter against PCSX2 `VU0.cpp` `_vu0run`: the normal finish/wait epilogue advances EE cycle by the amount of VU0 execution consumed during that wait (`VU0.cycle - startcycle`); it does not assign EE cycle to the final VU0 cycle.
- Corrected `ee_vu0_scheduler_sync_callback` to preserve both start counters and add only the elapsed VU0 cycles to the original EE cycle. The special early M-bit/already-ahead path remains an assignment to the VU0 cycle, matching PCSX2's separate early return.
- Strengthened tests so finish-to-E starts with VU0 already ahead of EE, and wait-to-E-or-M verifies EE advances by the consumed amount while remaining offset from VU0.
- GitHub Actions [run 43182310781](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43182310781): clean configure/build and **14/14 CTest passed**.
- Next micro-engine work remains gated on the 4 KB VU0 micro-memory, paired 64-bit instruction format, upper/lower dual-issue ordering, delay/branch/E/M state, and pipeline hazards. Implementing isolated lower-ALU operations without those rules would not be a safe execution engine.


## 2026-11-12 — VU0 local memories and paired micro fetch

- Added the two architectural 4 KB VU0 memories as separately 16-byte-aligned arrays in the VU0 sidecar: data memory and micro-program memory. Existing CPU-state layout and offsets remain unchanged because the sidecar is separate and the arrays are appended after its existing fields.
- Added `ee_vu0_fetch_micro_instruction`: fetches the paired lower/upper 32-bit words from the byte-addressed micro store, explicitly decodes little-endian words, extracts the upper I/E/M/D/T flag bits, advances by eight bytes and wraps at `0x1000`.
- Invalid or unaligned fetches fail without changing the PC or output record. `m_tpc` is documented in instruction-slot units; `m_micro_pc` is the active byte fetch address.
- Added compile-time state layout checks plus runtime tests for pair ordering, all five control flags, PC increment, end-of-memory wrap, invalid alignment and null arguments.
- Expanded the Python JIT harness's `VU0State` mirror with the two memories and explicit 16-byte padding, including offset/size assertions (`data_memory=240`, `micro_memory=4336`, `sizeof=8432`).
- GitHub Actions [run 43183141780](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43183141780): clean configure/build and **14/14 CTest passed**.
- This is fetch/storage only. The VU0 dual-issue interpreter, instruction decoder, delayed VI/VF writes, branch pipeline, and FMAC/FDIV/EFU hazard machinery remain unimplemented. Macro instructions remain unsupported.


## 2026-11-12 — VU0 micro-op primary and secondary decode tables

- Added symbolic decode identities for the documented VU0 lower and upper micro-op table entries. Each fetched instruction now carries the decoded operation for its lower and upper word, alongside the raw primary opcode, FT/FS/FD, IT/IS/ID and XYZW fields.
- Lower primary decode uses `word >> 25`. Primary `0x3c` is the `LowerOP` dispatch; the low-six-bit secondary selector maps IADD/ISUB/IADDI/IAND/IOR and `0x3c..0x3f` to the four T3 subtables indexed by bits 10..6.
- Upper primary decode uses `word & 0x3f`. Upper primaries `0x3c..0x3f` select the four FD subtables indexed by bits 10..6. Other upper primary slots are decoded directly.
- Unassigned primary/secondary slots map to `UNKNOWN`; no micro-op execution has been enabled. This prevents an encoding being mistaken for an implemented operation.
- A first attempt misread the lower secondary-table selector as primary `0x60`; table recounting against PCSX2 `VUops.cpp` showed the correct primary is `0x3c`. Corrected implementation and test encodings. Tests also corrected sample-word expectations (`0x11223344` lower = IADDIU; `0xf8001234` upper = unassigned primary `0x34`). Added a separate upper ADDi (`0x22`) vector.
- GitHub Actions [run 43185095496](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43185095496): clean configure/build and **14/14 CTest passed**, covering the corrected lower IADD/T3 LQI paths, upper ADDx/ADDi/FD-NOP and unassigned table slots.
- Remaining: implement issue/retirement scheduling with delayed VI/VF writes, branch-delay state and the IALU/FMAC/FDIV/EFU pipeline interlocks before executing any nontrivial VU instruction pair.


## 2026-11-12 — First executable VU0 lower-IALU micro-op slice

- Added `ee_vu0_execute_ialu_callback` behind the existing scheduler execution callback ABI. The first slice accepts only lower IADD, ISUB, IAND, IOR, IADDI, IADDIU and ISUBIU paired with upper NOP. Valid but unsupported pairs return UNAVAILABLE and restore the micro-PC to the pair start.
- VI operations read/write the low 16-bit halfword; writes preserve the neighbouring upper halfword and VI0 remains hardwired. Immediate widths/sign rules and destination fields are exercised by runtime tests.
- Integrated the existing four-entry IALU reservation queue: execution retires matured entries before issuing an operation, schedules one-cycle destination reservations, and flushes pending IALU reservations when E terminates the microprogram.
- E flag uses the two-pair delay observed in PCSX2. M flag ends each engine execution slice independently of the scheduler's wait policy. FINISH_TO_E resumes with another slice after M; WAIT_TO_E_OR_M returns at M. The scheduler integration test executes an M-marked pair, resumes, then completes an E-marked pair plus its delay instruction.
- PCSX2 `_vuFlushAll` waits for outstanding IALU reservations at E. The test therefore expects one additional cycle for the final pending destination to retire; the EE cycle counter receives the same consumed-cycle increment.
- Added tests for all seven operations, VI0 and upper-half preservation, M slice boundaries, FINISH_TO_E resumption after M, E delay/flush, and rejection of decoded but unsupported LQ/upper-vector pairs.
- Actions initially caught a mistaken positive operand in the unsupported-lower test and then a one-cycle-too-low E-flush expectation. Both were corrected from the observed semantics; no execution code was weakened to make the tests pass.
- GitHub Actions [run 43189469757](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43189469757): clean configure/build and **14/14 CTest passed**.
- This is a deliberately narrow micro-engine foundation, not full VU0 support. Branches, data memory, I/D/T modes, upper vector ops, full dual-issue ordering, VF/flag forwarding and FMAC/FDIV/EFU pipelines remain unsupported.
- Next: implement lower integer branches with explicit branch-delay state and IALU read interlocks; keep vector/memory paths unsupported until separately validated.


## 2026-11-12 — Conservative static JALR target recovery

- Added `collect_static_jalr_targets`, a forward constant-propagation pass over each caller's basic-block CFG. It recognizes LUI/ORI/ANDI/XORI/ADDIU/DADDIU and selected SPECIAL integer arithmetic/shift operations. GPR0 is hardwired to zero; unmodelled writes and memory loads invalidate affected constants.
- A JALR target is collected only when its source register is known at the instruction, nonzero and four-byte aligned. The target is sampled before the JALR delay slot. Branch-likely CFG edges carry separate taken and annulled-fallthrough states, so a delay-slot write is not applied to a not-taken path.
- Integrated the pass into `ee_recomp`'s discovery worklist as a fixed-point process. Static JALR call targets are validated against executable ELF ranges, added as entries, decoded, and scanned again for further calls. Dynamic targets and register values derived from unmodelled memory/table loads remain unresolved.
- Function discovery now pre-collects leaders from all decoded conditional branches, branch-likely instructions and direct jumps. Linear block construction stops at those leaders even before they have been visited; a later-discovered target inside an already scanned block is split into its own block with a connecting predecessor edge. This fixes overlapping basic blocks that hid merge predecessors from the data-flow analysis.
- Added `function_discovery_test` with fixtures for LUI/ORI, signed LUI/ADDIU, $zero, JALR target sampling before its delay slot, unknown load clobbers, conflicting/equal CFG predecessor constants, and branch-likely delay-slot annulment.
- CI initially exposed C++ scoped-enum conversions in the new analysis, a missing parenthesis in REGIMM link handling, a fixture namespace issue, and then the overlapping-basic-block bug. Those were corrected from the compiler/test output. The new branch-likely fixture is included in the final passing run.
- GitHub Actions [run 43207855948](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43207855948): clean configure/build and **15/15 CTest passed**, including `function_discovery`.
- The preserved USA `SLUS_214.42` full-image validation (7,849 functions) predates this JALR recovery. Do not update or reuse that count as a result of the new pass until the full ELF is re-run. A fresh full-image native-link validation is the next check.
- Remaining limits: JALR targets that depend on GOT/table loads, runtime values, interprocedural register states, or unsupported transformations are not guessed. Function discovery is still conservative and is not proof that every stripped-image function is found.


## 2026-11-12 — VU0 integer branches, JR/JALR and VI backup timing

- Cross-checked lower branch control flow against PCSX2 `VU0microInterp.cpp` and `VUops.cpp`: TPC advances before branch target calculation; B/BAL use signed 11-bit encoded offsets with bit 10 as the sign bit; branch issue uses a two-pair delay pipeline; branch-in-delay-slot targets are deferred until after the first target instruction.
- Extended the executable upper-NOP/lower-integer slice to B, BAL, JR, JALR, IBEQ, IBNE, IBLTZ, IBGTZ, IBLEZ and IBGEZ. JR/JALR sample the low 16-bit VI target in instruction-slot units before any JALR link write; JALR supports the Is == It overlap.
- Added the PCSX2 VI-backup window to the sidecar. IALU writes preserve the pre-chain low halfword for two cycles; repeated writes to the same VI retain the original value and refresh the window; a write to another VI replaces the backup register. Conditional branch reads use the backup value while active. E termination clears the window.
- Appended the backup counter/register/value to the VU0 sidecar. C++ layout assertions and the Python ctypes mirror now agree on offsets 8524/8528/8532 and the 8544-byte, 16-byte-aligned state size; Python includes explicit tail padding because ctypes natural alignment is 8 bytes.
- Added direct runtime cases for B/BAL delay slots, JR, JALR including Is/It overlap, JALR in a branch delay pair, every conditional integer branch, negative relative offsets/wraparound, and both branch-read VI-backup cases (active backup and replacement/chain behaviour).
- GitHub Actions [run 43210588688](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43210588688): clean build and **15/15 CTest passed**.
- This remains a restricted lower-integer/branch engine, not a full VU0 interpreter. VU data-memory operations, I/D/T flags, upper vector operations, general dual-issue dependencies, VF forwarding and FMAC/FDIV/EFU pipelines remain unsupported. The new tests are direct runtime regressions, not yet a full differential execution harness against PCSX2.

## 2026-11-12 — JALR recovery full-image revalidation and native-link correction

- Re-ran the preserved USA `SLUS_214.42` ELF against the iterative static-JALR discovery path. The whole-image discovery/CFG pass now reports **7,901 entries**, **418,898 unique PCs**, **418,019 supported / 879 unsupported** instructions (**99.8% decoder/lifter classification**). This is +52 entries and +1,160 PCs versus the pre-JALR historical baseline (7,849 / 417,738). The previous entry-address list was not persisted, so the comparison is by count only.
- The first full discovery attempt exposed a pathological cost in CFG leader collection: every function analysis rescanned the complete decoded instruction map to rebuild the global branch/jump leader set. Added `collect_function_block_starts()` and passed the resulting index through JALR recovery and final per-function analysis. The index is recomputed once per decoded-map revision, rather than once per function.
- The shared-index change is committed in `b68232cf3a48f1219d294968fd893da999d2b693`, `f0d5ba0a43a5029cc5b7fa6e8e605489499cb85e` and `751060da3a3c90e1fd97e79b42f5a480ca173161`. GitHub Actions runs [#43212998565](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43212998565) and [#43213043628](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43213043628) passed.
- A bounded `--max-functions=100` validation emitted the first 100 of the 7,901 discovered entries. All **100/100** modules parsed and verified with llvmlite / LLVM 20.1.8, and all 100 host objects were emitted at `opt=0` (aggregate **601,056 bytes**).
- The first native link exposed a missing source in `tools/link_native_objects.py`: generated COP2 modules referenced VU0 runtime helpers, but `src/runtime/ee_vu0.cpp` was not part of the linker's required source list. Added it in commit `97be99aff512edc8ec5067a2a554388152c83c60`.
- The 100-function sample then linked successfully into a **576,080-byte** x86-64 shared library. `ctypes.CDLL` loaded it, `ldd -r` reported no unresolved dynamic symbols, and `ee_game_run` plus `ee_fn_00100008` were exported. The linker change's Actions run [#43214060713](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43214060713) passed **15/15 CTest**.
- The complete 7,901-module LLVM verification/object generation/native-link pass remains outstanding. Only the bounded 100-function sample has been verified and linked for this new discovery output. The old 7,849-function full-link measurements remain historical for the pre-JALR-recovery source; the game itself was not executed.
- Updated `RESUME.md`, `FULL_GAME_NATIVE_LINK_VALIDATION.md` and `COP2_RESEARCH.md` to record the measured result and preserve the distinction between whole-image discovery and bounded backend validation.

## 2026-11-12 — Persistent JALR manifest and complete CFG-index reuse

- Found one remaining copy of the original performance defect: the `emit_entry` lambda still called `function_analyzer::analyze()` without the shared branch/jump-leader index. This meant the all-functions IR-emission path still rebuilt the global leader set for each function, despite the earlier coverage and JALR passes having been fixed.
- Updated the emission lambda to reuse the same decoded-image index. The fixed index is now used by JALR recovery, function-scoped coverage and per-function IR emission.
- Extended `ee_functions.json` with sorted `discovered_entries`, `static_jalr_targets` and `jalr_only_entries` arrays. These describe the complete discovery result even when `--max-functions` caps emitted IR. The discovered-entry snapshot is captured before the `--only-function`/emission filters can modify the emission list.
- Commits: `bf686639db2bb9c0a0da36e1b4501efa8eb3d160` (persistent entry manifest plus emission-path index) and `140655649eb7ba82d8578736b4d8c1f0906877e8` (filter-independent discovery snapshot).
- Re-ran USA `SLUS_214.42` with `--max-functions=1`: whole-image discovery reports **7,901 entries**, **52 static JALR targets**, and **52 JALR-only entries**. All three arrays are sorted and unique.
- Inspected all 52 JALR-only target addresses against the ELF. All are in file-backed executable memory and all have a `JR $ra` within their first 24 words. Fifty begin with the same 16-byte stack-frame prologue and return at offset `+0x54`; the remaining two are compact leaf routines returning at `+0x0c`. This strongly supports their classification as code entries, though it is not a dynamic call-path proof.
- Repeated the `--max-functions=100` run after the emission-path fix: full discovery plus 100 IR emissions completed in **7.53 seconds**, compared with the earlier 15.82-second run before the last whole-image rescan was removed.
- LLVM 20.1.8 parsed and verified **100/100** modules and emitted **100/100** objects at `opt=0` (601,056 bytes). The 100-function sample linked into a **580,176-byte** shared library; `ldd -r` found no unresolved symbols and `ctypes.CDLL` loaded it with `ee_game_run` and `ee_fn_00100008` exported.
- GitHub Actions [run 43215631986](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43215631986): **15/15 CTest passed**.
- The complete 7,901-module LLVM/object/link pass remains the outstanding validation task. The full title has not been executed.


## 2026-11-12 — Complete 7,901-function backend pass and per-site JALR audit

- Completed the unrestricted USA `SLUS_214.42` backend validation after reusing the shared CFG leader index in the final per-function emission pass: 7,901/7,901 LLVM modules parsed and verified, 7,901/7,901 host objects emitted at `opt=0` (33,823,808 aggregate bytes), and all 7,901 objects linked into a 28,187,040-byte `libee_game.so`.
- The shared library loads through `ctypes.CDLL`; `ldd -r` reports no unresolved dynamic symbols; `ee_game_run` and `ee_fn_00100008` are exported. This is a translated-code backend result, not proof that the game executes; the full title was not run.
- Added per-site static JALR audit records to `ee_functions.json`. Each record captures caller entry, JALR PC, source register, whether the source is known at that instruction, sampled 64-bit value, 32-bit target candidate, alignment and executable-range status. Sampling occurs before the link write and delay slot.
- Audited the USA image: 3,535 caller/site records, 3,505 distinct JALR PCs; 3,475 unknown-source records, 56 resolved executable records (53 distinct PCs), and four known but invalid targets. The 56 resolved contexts identify 52 unique executable targets.
- The dominant unresolved source is `$t9` (`$25`): 2,955 records / 2,944 distinct PCs, with 2,890 of those distinct PCs having an immediately preceding load into `$t9`. The common pattern is runtime object/vtable dispatch. Most unknown `$v0` and `$v1` targets are also loaded from memory. A fixed-address function-pointer slot observed at `0x0043c948` is zero in the on-disk ELF.
- No additional arithmetic-only constant-propagation rule is justified by this audit. Memory-loaded function pointers remain unresolved until memory initialization, aliasing and possible writes are modelled; do not guess targets.
- Added regression tests for known, unknown and misaligned JALR site states plus conflicting CFG predecessors. GitHub Actions [run #43219107693](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43219107693): clean build and **15/15 CTest passed**.
- Full backend measurements: `FULL_GAME_NATIVE_LINK_VALIDATION.md`. Indirect-call analysis: `JALR_COVERAGE_AUDIT.md`.


## 2026-11-12 — VU0 branch issue ordering and VI-backup timing

- Rechecked _vu0Exec in PCSX2: the lower opcode handler samples branch inputs before _vuTestLowerStalls; only after the stall does the interpreter age VIBackupCycles by the total elapsed cycles since the current pair began.
- The previous host callback waited for IALU reads and aged VI backup before evaluating the branch. This could change a branch result when its source had a pending IALU reservation and the backup window expired during the stall.
- Split branch execution into a preparation/action phase and an apply phase. The action snapshots predicates, relative/VI targets and BAL/JALR link values from pre-stall state; the IALU read interlock then runs; on success the action commits; VI backup ages after the full elapsed issue/stall interval. Invalid interlock state cannot partially write a link or branch target.
- Moved IALU VI-backup ageing after the lower IALU op as well, so a new write ages from two cycles to one on its issue pair, matching PCSX2. A branch issued during its last visible backup cycle reads the old halfword, then the window expires.
- Updated the existing timing expectation and added a forced-stall regression: IBEQ samples the backed-up VI value before a matching three-cycle IALU reservation stalls the pair; after the stall the queue is retired and the backup expires, without retroactively changing the branch decision. A separate M-terminated IADDIU case verifies that a newly created two-cycle backup ages to one immediately after its issue pair.
- GitHub Actions initially caught the now-obsolete backup-cycle assertion; after updating that expectation and adding the forced-stall case, [run #43222672470](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43222672470) passed **15/15 CTest**. The extra one-pair write-age boundary passed in [run #43223543377](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43223543377), also **15/15 CTest**.
- Reference points: PCSX2 [VU0microInterp.cpp](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/VU0microInterp.cpp) lower dispatch / _vuTestLowerStalls / backup aging order, and [VUops.cpp](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/VUops.cpp) VI backup and branch operand semantics.
- Remaining: a broader automated differential harness is still needed for branch-delay, cycle-budget, nested-branch and E/M interactions.


## 2026-11-12 — Correct scalar VU0 IALU latency from PCSX2 metadata

- Rechecked the implemented lower scalar integer operation set against PCSX2's lower-op metadata. IADD/ISUB/IAND/IOR/IADDI/IADDIU/ISUBIU have zero IALU-pipeline latency; the two-cycle VI-backup visibility window is separate from that pipeline.
- Corrected `ee_vu0_execute_ialu_callback` so these immediate VI writes do not create one-cycle destination reservations. The generic four-entry IALU queue remains intact for future operations with nonzero latency, including the planned ILW/ILWR path.
- Updated M-bit, E termination and FINISH_TO_E expectations: no pending scalar ALU reservation remains at M, and E no longer adds an artificial queue-flush cycle. EE cycle accounting now reflects only cycles actually consumed by the current scalar microprogram.
- The first CI run exposed four stale assumptions in the tests (M queue occupancy and E/FINISH_TO_E flush cycles). Corrected those expectations rather than reverting the semantic fix.
- GitHub Actions [run #43222225891](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43222225891): clean build and **15/15 CTest passed**.
- This does not enable any additional VU0 instruction family. The reservation queue is retained for later nonzero-latency lower operations; memory/vector and full dual-issue execution remain unsupported.


## 2026-11-12 — Deterministic VU0 scalar reference-model sequence

- Added a test-side scalar oracle independent of the runtime micro-op decoder. It derives each of the seven supported lower integer operations from the raw instruction word and models VI0, signed 16-bit arithmetic, immediate decoding, low-halfword writes, and the PCSX2 VI-backup window.
- Generated a deterministic 256-pair program using mixed VI source/destination registers and immediate values. Each pair is executed through the real runtime callback with a one-cycle budget, then compared against the oracle for all 16 VI values, backup register/value/lifetime, micro-PC, TPC, cycle count and empty scalar IALU reservation state.
- The first run caught an error in the test oracle's timing assumption: PCSX2 samples branch state before stalls and ages VIBackupCycles after lower execution; a newly written VI backup therefore reads as one remaining cycle at the end of its issue pair. Corrected the oracle to age the window after the operation, consistent with the dedicated M-bit issue-age test.
- The final run passed the deterministic sequence and the complete suite. GitHub Actions [run #43224000835](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43224000835): clean build and **15/15 CTest passed**.
- This is test-side reference-model coverage derived from PCSX2 semantics; it is not a live instruction-by-instruction differential run against the PCSX2 executable/interpreter.


## 2026-11-12 — Mixed VU0 branch/control-flow reference model

- Added a deterministic 512-slot lower-NOP/upper-NOP microprogram containing all 17 supported lower scalar/branch encodings; a fixed program seed and fixed initial VI values make the control-flow path reproducible.
- Executed up to 384 pairs one at a time through the runtime callback and compared each state transition with an independent test-side decoder/reference model: VI contents, backup register/value/age, byte micro-PC/TPC, branch counter/target/deferred target, E/M state, VPU running status, cycle count and scalar reservation queue.
- The first comparison failed at a bitwise op because the oracle bypassed the hardwired VI0 rule. Corrected IAND/IOR source reads to use zero-aware VI values; also made the JR/JALR oracle targets treat VI0 as zero. No runtime behavior needed changing for either test-oracle correction.
- The previously corrected lower issue ordering is now checked in both the focused forced-stall regression and the mixed reference trace. The scalar and mixed oracles together cover deterministic data dependencies plus branch/delay/E/M state transitions.
- GitHub Actions [run #43225840963](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43225840963): clean build and **15/15 CTest passed**.
- This remains a test-side PCSX2-derived model. It is not a live differential run of the PCSX2 interpreter; the next validation step is to run comparable traces through a PCSX2 interpreter build and compare serialized architectural states.


## 2026-11-12 — VU0 mixed branch oracle and boundary regressions

- Added a deterministic 384-pair mixed lower-IALU/branch program with a test-side raw-word reference model. The oracle independently decodes the lower instruction families, models VI low-halfword updates, branch conditions/targets/link writes, branch-in-delay deferral, VI-backup age, E/M timing and PC/TPC state, and compares runtime state after every executed pair.
- Extended directed conditional-branch coverage with false-boundary cases for IBEQ, IBNE, IBLTZ, IBGTZ, IBLEZ and IBGEZ, complementing the existing taken-boundary cases.
- Added an E-marked B regression proving the engine executes the branch delay pair, applies the branch target, and then terminates without executing beyond the target.
- Tightened the forced IALU branch-read interlock case to a one-cycle exact budget. The pair is already issued when the dependency is discovered, so the runtime completes the stall and pair atomically, consuming three cycles rather than returning in a partially issued state.
- GitHub Actions [run #43226060509](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43226060509): clean configure/build and **15/15 CTest passed**, including vu0_runtime.
- The independent models are PCSX2-derived and exercise this runtime; they do not execute PCSX2 itself. A live PCSX2/interpreter differential harness remains the next VU0 validation step.


## 2026-11-12 — Guarantee mixed-oracle execution coverage

- Strengthened the 384-pair mixed VU0 reference trace with a deterministic 21-pair prefix before the randomized tail. The prefix executes all seven scalar integer operations, B/BAL/JR/JALR, and each of the six conditional branch predicates.
- The transfer prefix is laid out so B and BAL land immediately after their respective delay pairs; JR and JALR read known instruction-slot targets whose delay pairs are executed; all conditional branches use deliberately false predicates and fall through. VI1/VI2/VI4/VI13/VI15 are seeded to stable values while other state remains deterministic from the xorshift seed.
- The oracle now asserts the full 17-bit operation coverage mask in addition to checking architectural state after every pair. This removes the earlier weakness where unreachable encodings existed in the program image but might not have been executed by its random branch path.
- GitHub Actions [run #43227105337](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43227105337): clean build and **15/15 CTest passed**.
- Remaining VU0 validation: forced multi-cycle branch interlocks and finer execution-budget boundaries in the mixed trace, followed by a live PCSX2 interpreter trace comparison.


## 2026-11-12 — Forced branch interlock inside mixed VU0 oracle

- Extended the 384-pair mixed reference trace with a controlled dependency at its first IBEQ. After the directed prefix, VI1 is set to the post-write value while VIBackup holds the previous value for one remaining cycle; VI2 is set equal to that old value.
- Injected a three-cycle pending IALU reservation for VI1. The branch must snapshot operands before the stall, take the branch using the backup value, wait atomically from cycle 15 through cycle 18 despite a one-cycle exact execution budget, retire the reservation, and only then age the backup to zero.
- Extended the reference model to account for per-pair elapsed cycles (not merely one instruction per cycle) and backup ageing by the same interval. It continues comparing PC/TPC, branch delay state, VI contents, backup state, E/M and queue state after each callback.
- The directed 21-pair prefix still guarantees all 17 lower opcode classes are executed before the randomized tail.
- GitHub Actions [run #43227644333](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43227644333): clean build and **15/15 CTest passed**.
- Remaining: extend forced dependency coverage to more than one source register and exercise multiple exact budget boundaries; then compare trace dumps against a live PCSX2 interpreter build.


## 2026-11-12 — Correct VU0 branch interlock and VI-backup issue ordering

- Audited the implementation against PCSX2 interpreter source at commit `82cac38b2af19d593dedfd9ddaa63448e7ec493c`. In `_vu0Exec`, PCSX2 decodes lower-op metadata, applies `_vuTestLowerStalls`, retires matured pipes through `_vuTestPipes`, ages `VIBackupCycles` by the elapsed issue/stall cycles, and only then invokes `_vu0ExecLower`. Conditional predicates and JR/JALR targets are consequently sampled after the interlock and backup aging.
- This disproved the previous runtime comment and test assumption that branch operands were snapshotted before the stall. Corrected `ee_vu0_execute_ialu_callback` to age the existing VI-backup window after the stall/retirement phase and before executing the lower operation. IALU writes now create a fresh two-cycle backup after this pre-handler aging; branch predicates, jump targets and link values are prepared from the post-stall state.
- Corrected both test-side models to apply backup aging before lower-op execution. Updated boundary expectations so a newly issued IALU write leaves two backup cycles at the instruction boundary, and a one-cycle later branch observes one remaining cycle.
- Added post-stall semantics to the direct tests: a three-cycle branch interlock expires a one-cycle backup before IBEQ executes; a second test places different three- and five-cycle reservations on both IBEQ sources with a two-cycle exact budget and verifies that the branch waits for the latest dependency, then compares the current VI values after backup expiry.
- Updated the 384-pair mixed oracle's injected three-cycle IBEQ dependency to verify current-value sampling after the stall. The oracle still guarantees that all 17 supported lower operation classes execute before its randomized control-flow tail.
- GitHub Actions [run #43229844156](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43229844156): clean build, artifact upload, and **15/15 CTest passed**.
- Remaining semantic gap: the reference models are still test-side PCSX2-derived models, not a live PCSX2 interpreter differential. The next validation step is a reproducible harness that runs matching fixtures through PCSX2's interpreter and compares per-pair state/cycle traces.


## 2026-11-12 — Per-pair VU0 trace production and PCSX2 adapter

- Added a shared 37-pair VU0 microprogram fixture. Its executed path covers all seven lower scalar integer operations, B/BAL/JR/JALR, all six conditional branches, a JALR source/destination overlap, one M-marked pair, and an E-marked pair with its delay pair.
- Added the in-tree ee_vu0_trace_candidate executable. It executes one bounded VU0 runtime pair per exact-cycle callback and emits the JSONL schema for all 32 VI registers, cycle count, byte micro-PC, branch/delay state, VI backup, E/M status, and VPU running status.
- Extended the trace format and comparator to record and compare the executed pair PC in addition to the lower/upper instruction words and architectural state.
- Extended the pinned PCSX2 adapter at 82cac38b2af19d593dedfd9ddaa63448e7ec493c. Its opt-in VU0IssueOrder.EmitDeterministicPerPairTrace test calls vu0Exec(&VU0) once per pair and writes the same JSONL records when VU0_TRACE_OUTPUT is set. The existing three issue-order boundary probes remain in place.
- Updated the PCSX2 integration installer to copy both the adapter and shared fixture. Added tools/vu0_diff/run_differential.py to configure/build both projects, produce both traces, and compare them.
- Added CTest coverage for candidate trace length, executed pair PCs, all 17 lower opcode classes, M/E boundaries, final VI state, and comparator acceptance. The first CI attempts found and corrected a Python unittest argument-parsing error and an incorrect test expectation for the encoded ISUB operand order.
- GitHub Actions [run #458](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43240924661): clean build, artifact upload, and **17/17 CTest passed**.
- This verifies the candidate trace producer and comparator path. The pinned PCSX2 adapter has been authored and integrated by the installer, but the PCSX2 test binary has not yet been built and a real cross-engine trace comparison has **not** yet been run. Do not claim live differential equivalence until that comparison succeeds.


- Follow-up validation: GitHub Actions [run #465](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43241884674) passed **18/18 CTest**, including the new differential-driver help smoke test. Updated README/resume status and corrected the runner's PCSX2 help text.
- The candidate trace is reproducible and CI-validated. A live trace from the pinned PCSX2 test executable has still not been generated in this environment; the first cross-engine compare remains pending.


## 2026-11-12 — PCSX2 Linux X11 configure fix and static ELF load dataflow

- Reviewed Grok's [commit 269a0db](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/commit/269a0dbaf94e8240959f20532b30a17d8da20999). It correctly removes the X11_API=OFF option and adds libxi-dev. In the pinned PCSX2 revision, cmake/SearchForStuff.cmake calls find_package(X11 REQUIRED) only inside if(X11_API), while common/CMakeLists.txt unconditionally links X11::X11, X11::Xrandr and X11::Xi for Linux. The workflow's package set already includes libx11-dev and libxrandr-dev; libxi-dev supplies the third target.
- Made X11_API=ON explicit in the manual PCSX2 workflow. Updated tools/vu0_diff/run_differential.py to add -DX11_API=ON only on Linux (the option is defined only for Unix non-Apple builds), removing its stale -DX11_API=OFF. Updated the manual Linux configure instructions.
- Added tests/tools/test_vu0_differential_x11_config.py to guard the local driver, CI package list, workflow option and manual command. Registered it with CTest. The first run caught a quoting typo in that new test; fixed it immediately.
- Added an immediate actions/cache/save@v4 after the pinned PCSX2 dependency build. This preserves the expensive dependency tree even if a later configure/build/test/trace step fails.
- GitHub Actions [run #514](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43282168159): build and **20/20 CTest passed**. The later resume-note-only commit and dependency-cache workflow update are undergoing ordinary CI separately.
- The pinned manual differential workflow [run #2](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43275643702) is still reported in the PCSX2 third-party dependency build step. It has not yet reached PCSX2 CMake configure, so the X11 imported-target fix is analytically verified against the pinned CMake source but not yet confirmed by a successful workflow configure. The prior run #1 failed exactly at the expected missing-target error before Grok's change.
- Continued the memory-aware JALR work: LW/LWU constant propagation now consults elf_loader::read32_const_at. The loader only considers ET_EXEC, valid section-table metadata, no dynamic/interpreter segment, no supported/unsupported relocation affecting the word, and a unique full-word mapping in file-backed readable/non-writable PT_LOAD. Packed RELR/Android relocation formats, writable/BSS data, partial/overlapping mappings and uncertain cases are rejected. Caller-saved GPR values are invalidated across JAL/JALR calls.
- Synthetic function_discovery_test coverage includes eligible read-only words, writable globals, BSS, relocated words, overlapping writable mappings, and caller-saved t9 invalidation. CI runs #506 and #507 passed the intermediate loader/dataflow changes, and run #514 includes all current tests.
- Important limitation: ELF PF_W flags alone do not establish that the PS2 runtime enforces memory protection or that a nominally read-only segment cannot be modified through another alias. The full USA ELF audit has not yet been rerun with these new rules; keep the prior 7,901-entry/3,475-unresolved counts as the pre-change baseline and validate runtime write assumptions before treating any newly folded pointer as immutable.
