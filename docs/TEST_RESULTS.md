# Test Results

## 2026-11-11 clean recovery build

Environment: GCC 14.2.0, CMake/Ninja, Python 3, llvmlite/LLVM 20.1.8.

Commands:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/ee_decoder_test
```

Results on the reconstructed source snapshot:

- Clean configure: PASS.
- Clean build: PASS.
- CTest: **4/4 PASS** (`decoder`, `multi_function`, `smoke`, `mmi_ir_runtime`).
- Decoder regressions: **269 passed, 0 failed**.
- The generated LLVM runtime fixture parsed, verified, and JIT-executed at module-builder optimization levels O0–O3.

The focused LLVM runtime fixture exercises HI1/LO1 transfers, supported MMI logical operations, same-block aliasing, upper-GPR preservation, and cross-block register writeback.

These results validate this recovered source snapshot. They do **not** establish that the inaccessible 2/4 worktree was repaired.

## Historical real-game validation

Preserved notes report 6,797 discovered functions from USA `SLUS_214.42`. Full-game LLVM parse/verify and O0 host-object emission were completed before the final narrow CFG liveness refinement. After that refinement, the real-game entry function at `0x00100008` was verified and emitted as an 800-byte host object. A complete 6,797-module re-verification after that final refinement was not recorded.

The in-project object emitter remains a stub; llvmlite JIT/object checks are validation harnesses, not a runnable static recompiler.


## 2026-11-11 MMI / MAC follow-up

Environment: GitHub Actions `ubuntu-latest`, GCC/CMake/Ninja, isolated Python virtual environment with `llvmlite`.

- Clean configure: PASS.
- Clean build: PASS.
- CTest: **4/4 PASS** (`decoder`, `multi_function`, `smoke`, `mmi_ir_runtime`).
- LLVM JIT runtime test: PASS at O0, O1, O2, and O3.
- Runtime assertions cover:
  - HI1/LO1 transfer ordering and preservation of upper GPR halves.
  - Signed MADD and unsigned MADDU accumulation into HI:LO.
  - Signed MADD1 and unsigned MADDU1 accumulation into HI1:LO1.
  - MFHI/MFLO and MFHI1/MFLO1 readback.
  - PADD/PSUB/PMAX word, halfword, and byte lane behavior.
  - PAND/PXOR/POR/PNOR, source/destination aliasing, and cross-CFG-block writeback.

The MADD/MADDU behavior is implemented from the EE Core Instruction Set Manual v6.0 operation definitions: the low 32-bit halves of the appropriate HI/LO pair form the 64-bit accumulator; the low and high result words are sign-extended into LO/HI, and the low result word is also written to rd.


## 2026-11-11 Pipeline-1 multiply follow-up

- Clean configure/build: PASS.
- CTest: **4/4 PASS** (`decoder`, `multi_function`, `smoke`, `mmi_ir_runtime`).
- LLVM JIT runtime: PASS at O0–O3.
- Added runtime coverage for signed `MULT1` and unsigned `MULTU1`, including HI1/LO1 writeback, sign-extension, and MFHI1/MFLO1 readback.

## 2026-11-11 Pipeline-1 division follow-up

Environment: GitHub Actions `ubuntu-latest`, GCC/CMake/Ninja, isolated Python virtual environment with `llvmlite`.

- Clean configure: PASS.
- Clean build: PASS.
- CTest: **5/5 PASS** (`decoder`, `multi_function`, `smoke`, `mmi_ir_runtime`, `div1_ir_runtime`).
- Existing MMI runtime fixture: PASS at O0–O3.
- New DIV1/DIVU1 LLVM JIT fixture: PASS at O0–O3 for all five cases:
  - Signed quotient/remainder (`-17 / 5`).
  - Signed overflow (`INT_MIN / -1`).
  - Signed divide-by-zero preservation.
  - Unsigned quotient/remainder (`0xfffffffe / 3`).
  - Unsigned divide-by-zero preservation.
- Each case verifies HI1/LO1 state and MFHI1/MFLO1 readback.
- GitHub Actions run: [43090955314](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43090955314).

## 2026-11-11 MMI1 packed-operation follow-up

Environment: GitHub Actions `ubuntu-latest`, GCC/CMake/Ninja, isolated Python virtual environment with `llvmlite`.

- Clean configure: PASS.
- Clean build: PASS.
- CTest: **6/6 PASS** (`decoder`, `multi_function`, `smoke`, `mmi_ir_runtime`, `div1_ir_runtime`, `mmi1_ir_runtime`).
- New MMI1 LLVM JIT runtime test: PASS at O0–O3.
- Runtime coverage:
  - PABSW/PABSH with signed-minimum saturation.
  - PCEQW/PCEQH/PCEQB all-ones equality masks.
  - Signed PMINW/PMINH.
  - PADDUW/PADDUH/PADDUB unsigned saturation.
  - PSUBUW/PSUBUH/PSUBUB unsigned floor-at-zero saturation.
  - In-place source/destination aliasing.
- The game entry-point encoding `0x70000c28` now decodes as supported PADDUW.
- Remaining MMI1 operations stay unsupported: PADSBH, PEXTUW, PEXTUH, PEXTUB, and QFSRV.
- GitHub Actions run: [43093331006](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43093331006).

## 2026-11-11 MMI1 completion — validation pending

- Implemented the remaining defined MMI1 operations: PADSBH, PEXTUW, PEXTUH, PEXTUB, and QFSRV.
- Added MTSAB/MTSAH decoding, lifting, and SA state updates used by QFSRV.
- Extended the MMI1 JIT fixture to cover all 18 non-reserved MMI1 sub-ops and multiple byte/halfword SA settings at O0–O3.
- **Validation status:** GitHub Actions runs for this latest source revision are queued and have not yet produced a result. The prior 6/6 green result applies to the earlier 13-operation MMI1 subset only.

## 2026-11-11 — Control-flow and host-dispatcher validation

Environment: GitHub Actions `ubuntu-latest`, GCC 15.2, CMake 4.4.5, Ninja 1.13.2, Python 3.14, llvmlite 0.50.0.

- Clean configure: PASS.
- Clean build: PASS.
- CTest: **9/9 PASS** (`decoder`, `dispatcher`, `multi_function`, `smoke`, `mmi_ops`, `control_flow_ir_runtime`, `mmi_ir_runtime`, `div1_ir_runtime`, `mmi1_ir_runtime`).
- Control-flow LLVM JIT coverage includes JAL/JALR/JR target and link ordering, mandatory delay-slot effects, ordinary branch condition sampling, branch-likely annulment, BLTZAL/BGEZAL and likely variants, direct caller/callee return continuation, one-level recursive JAL, and cross-function JALR with the target register overwritten by its delay slot.
- New native `dispatcher` test covers:
  - Direct caller dispatch into a translated callee.
  - `EE_EXIT_RETURN` back into a translated caller continuation.
  - Dynamic JALR dispatch and caller resume.
  - Return to an external guest address.
  - Unknown initial guest PC.
  - Bounded dispatch-loop exhaustion.
  - Rejection of mismatched metadata and native-entry arrays.
- The dispatcher uses guest block-PC ownership metadata and a parallel array of native entry pointers. It routes both RETURN and DISPATCH exits back into translated code when the new PC is owned by a registered function; otherwise it returns the exit to the outer host.
- GitHub Actions run: [43109193936](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43109193936).

The dispatcher API and native unit tests are complete, but the project still does **not** produce a runnable game: the in-project LLVM object emitter remains a stub, and the dispatcher has not yet been connected to an actual emitted/linked native function set.

## 2026-11-11 — Resumable exceptions and real-game validation

### CI regression results

GitHub Actions run [43110647060](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43110647060), commit `d76f7ec`:

- Clean configure: PASS.
- Clean build: PASS.
- CTest: **9/9 PASS**.
- New decoder/CFG coverage verifies that SYSCALL and BREAK expose a possible EPC+4 continuation. The exception blocks still terminate to the runtime host; code after them is emitted as a separate block that can be re-entered if the exception handler resumes execution.

### USA `SLUS_214.42` full-image pass

Inputs/tools:
- ELF: preserved USA `SLUS_214.42`.
- Recompiler: CI-built Linux `ee_recomp` from run [43110647060](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43110647060).
- Verifier: matching `tools/verify_ir.py`.
- LLVM: 20.1.8 via llvmlite.

Recompiler result:
- Entry point: `0x00100008`.
- Machine: 8 (MIPS).
- Segments: 2.
- Functions discovered/emitted: **7,849 / 7,849**.
- Unique function-scoped instruction PCs: **417,738**.
- Lifted instructions marked supported: **409,530**.
- Lifted instructions marked unsupported: **8,208**.
- Current reported support: **98.0%**.

LLVM validation:
- IR parse + verify: **7,849/7,849 PASS**, zero invalid modules.
- Native target object generation at `opt=0`: **7,849/7,849 PASS**.
- Aggregate in-memory host object bytes: **34,220,040**.

This confirms that the latest exception-continuation CFG change allows discovery through the game startup's syscall path and reaches the direct JAL call graph. The earlier run stopped after one bootstrap function; after retaining the possible EPC+4 resume path it discovered the full 7,849-function set.

Important limitation: `verify_ir.py --emit-opt=0` asks llvmlite to emit host object bytes in memory as a validation step. It does not write/link one object per game function. The project's own `object_emitter::emit_object()` remains a stub, so a standalone runnable game has **not** been produced.

## 2026-11-11 — Scalar compare enablement and updated game coverage

### CI regression results

GitHub Actions run [43112984141](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43112984141), commit `5aba8e1`:

- Clean configure: PASS.
- Clean build: PASS.
- CTest: **10/10 PASS**, including the new `compare_ir_runtime` test.
- New comparison tests verify `SLT`, `SLTU`, `SLTI`, and `SLTIU` against 64-bit signed/unsigned edge cases, sign-extended immediates, source/destination aliases, `$zero`, and upper-GPR preservation.

### USA `SLUS_214.42` updated full-image pass

- Functions discovered/emitted: **7,849 / 7,849**.
- Unique function-scoped instruction PCs: **417,738**.
- Supported: **414,916**.
- Unsupported: **2,822**.
- Reported support: **99.3%** (previous baseline: 98.0%).

The decoder now enables the four exact scalar compare families. This recovers **5,386 instructions** without changing the discovered function/PC set.

LLVM validation on the newly emitted 7,849 modules:
- IR parse + verify: **7,849/7,849 PASS**, zero invalid modules.
- Native target object generation at `opt=0`: **7,849/7,849 PASS**.
- Aggregate in-memory host object bytes: **34,756,008**.

As before, the in-memory llvmlite object-generation test does not implement the project's `object_emitter::emit_object()`, write/link a per-function object set, or produce a runnable game executable.


## 2026-11-12 — Supported COP1-S and transfer/control-flow JIT validation

GitHub Actions run [43125195580](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43125195580), commit `ce5f3522b515a3f5bd5689bd94fb53d30d789a75`:

- Clean configure: PASS.
- Clean build: PASS.
- CTest: **12/12 PASS**.
- `fpu_compare_ir_runtime` covers the supported R5900 COP1 scalar table: transfers/control transfers, FCC0 comparisons, arithmetic, square root/reciprocal square root, min/max, sign operations, conversions, and accumulator variants.
- Transfer coverage verifies MFC1 sign extension, MTC1 low-word transfer, CFC1 FCSR read/sign extension, CTC1 FCSR write, upper GPR preservation, and cross-block CTC1 source liveness.
- BC1 control-flow coverage verifies condition sampling before an ordinary delay slot changes FCSR and branch-likely annulment when BC1FL is not taken.
- Existing decoder, dispatcher, native object emission/link, MMI and delay-slot suites also passed.

The C++ implementation and JIT tests are validated for the covered instruction cases. This does not establish full EE/FPU equivalence or a complete bootable game. VU0/COP2 remains unsupported pending the state and synchronization foundation documented in `COP2_RESEARCH.md`.
