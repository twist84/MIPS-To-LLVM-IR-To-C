# Resume Point

## Current baseline — 2026-11-25

Main [`130d305`](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/commit/130d3054e5ef978e5dfd4408b00422fa43184eea) contains PR #156, with 54/55 original exact console transcripts, 174/174 CTest and all five pre-merge workflows passing. The sole remaining transcript is EFU (201/208 words). Earlier retail SDBZ USA replays reached interactive training with audible audio and native presentation; no fresh retail replay on the squash commit is recorded. See [current status](README.md), [retail validation](SDBZ_RUNTIME_VALIDATION_20261125.md), [original tests](PS2AUTOTESTS.md) and [accuracy plan](ACCURACY_PLAN.md).

For the relocated launchers, use `scripts\run_ps2_game.bat --full --neutral` or `scripts\run_ps2_game.bat --p40` from the repository root. Retain compatible caches; request a matched retail replay when required by changed runtime behavior.

## Historical recovery baseline (November 11–12)

The following sections preserve the original recovery-era checkpoint. Their “current” and “latest” statements apply to that historical revision, not the merged baseline above.

The current working baseline is main. It contains the reconstructed 2026-11-11 09:57 MMI/HI1/LO1 source snapshot plus the subsequent decoder, lifting, control-flow, dispatcher, and validation work.

Latest fully verified CI: [Actions run #43282168159](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43282168159), **20/20 CTest passed**. This includes the 256-pair scalar and 384-pair mixed VU0 control-flow oracles, corrected PCSX2 VI-backup/branch issue ordering, one- and two-source forced-interlock regressions, per-site JALR audit, immutable-ELF load propagation regressions, COP1/COP2 JIT tests, native object emission/linking, dispatcher execution, MMI/HI1/LO1 suites, VU0 trace comparator tests, candidate per-pair trace production, differential-driver smoke tests, and the X11 CMake configuration guard.

Latest USA `SLUS_214.42` full-image revalidation:
- **7,901 function entries discovered**; **418,898 unique instruction PCs** in the function-scoped CFG set.
- **418,019 supported / 879 unsupported** instructions (**99.8% decoder/lifter classification**).
- LLVM parse/verify: **7,901/7,901 passed**, zero invalid modules.
- Native host objects at `opt=0`: **7,901/7,901 passed**, **33,823,808 aggregate bytes**.
- Native link: **7,901/7,901** guest objects linked with dispatcher and runtime helpers; `libee_game.so` is **28,187,040 bytes**.
- The library loads with `ctypes.CDLL`; `ldd -r` reports no unresolved dynamic symbols; `ee_game_run` and `ee_fn_00100008` are exported.

See `FULL_GAME_NATIVE_LINK_VALIDATION.md` for the complete backend measurements and `JALR_COVERAGE_AUDIT.md` for the whole-image indirect-call analysis. The linked result is a translated-code library, not a complete runnable PS2 title: the full title was not executed, and BIOS/IOP, GS, DMA, TLB/MMIO, interrupts, and other console services remain incomplete. The in-tree `object_emitter::emit_object()` is still a stub; llvmlite generated the per-function objects.

## Function discovery and indirect calls

- Entry discovery seeds the ELF entry point and defined `STT_FUNC` symbols in executable segments, then harvests direct `JAL` and REGIMM branch-and-link targets from reachable decoded code.
- Discovery now iterates to a fixed point: every newly decoded callee is scanned for further direct calls and statically resolvable `JALR` calls.
- `collect_static_jalr_targets` runs forward constant propagation over each caller CFG. It recognizes common address-building sequences (`LUI`, `ORI`, `ANDI`, `XORI`, `ADDIU`, `DADDIU`, and selected SPECIAL arithmetic/shift operations). A target is accepted only when the target register remains a known, aligned, nonzero constant at the `JALR` itself.
- The dataflow can now fold aligned `LW`/`LWU` reads from the ELF image, but only through a deliberately narrow `ET_EXEC` reader: valid section-table coverage, no `PT_DYNAMIC`/`PT_INTERP`, no REL/RELA or unsupported packed relocation metadata affecting the word, and one unique, fully covering, file-backed, readable/non-writable `PT_LOAD` mapping. Writable segments, BSS, overlapping mappings and uncertain relocation cases remain unknown. This is bounded static-image evidence, not yet proof that the running EE cannot write to a nominally read-only segment; preserve that caveat until the USA audit and runtime memory assumptions are checked.
- Calls invalidate caller-saved GPR constants before propagating the link value, so a loaded `$t9` or argument register cannot survive an intervening `JAL`/`JALR`.
- CFG joins discard a register constant when reachable predecessors disagree. JALR samples its target before the delay-slot instruction executes. Branch-likely propagation uses separate taken/fallthrough states so an annulled delay slot cannot contaminate the not-taken path.
- CFG construction pre-collects decoded branch/jump leaders and splits blocks at targets even when a target is found inside a block already scanned. This avoids overlapping blocks and preserves all predecessors for data-flow. The global leader set is indexed once per decoded-image revision and reused by JALR analysis, coverage analysis and per-function IR emission.
- `ee_functions.json` persists sorted `discovered_entries`, `static_jalr_targets` and `jalr_only_entries` independently of emission filters, plus `jalr_sites` records showing the source state at each reachable JALR. The current USA image has 7,901 entries, 52 statically resolved JALR-only additions, 3,505 distinct JALR PCs and 3,475 unresolved caller/site records.
- `function_discovery_test` covers constant address construction, `$zero`, delay-slot sampling, caller-saved register clobbers, equal/different predecessor values, branch-likely annulment, and synthetic ELF reads from eligible immutable segments versus writable, BSS, relocated and overlapping mappings. The fresh USA ELF pass discovers 7,901 entries, 52 more than the preserved pre-JALR baseline; the old entry-address list was not persisted, so this is a count comparison rather than a per-address diff. All 52 newly isolated JALR-only targets were inspected in the ELF: every target is file-backed executable code and has a `JR $ra` within its first 24 words; 50 share a normal stack-frame wrapper shape and two are short leaf routines. This is a strong structural sanity check, not proof of every dynamic call path.
- Dynamic function pointers loaded from writable/runtime memory, object/vtable state, GOT/table lookups, runtime-computed targets, and unsupported register-value transformations remain unresolved. The 7,901-entry/3,475-unresolved audit predates the new read-only `LW`/`LWU` propagation and must be regenerated before quoting updated coverage.

The 7,849-function full-image figures in the historical report are the pre-JALR-recovery baseline. The complete 7,901-module LLVM/object/link pass is now complete; see `FULL_GAME_NATIVE_LINK_VALIDATION.md`. The preserved JALR audit predates read-only data-load propagation; regenerate it before assessing the remaining memory/object/table calls. See `JALR_COVERAGE_AUDIT.md` for the baseline.

Control-flow regressions cover JAL/JALR/JR target and link ordering, delay-slot effects, ordinary and likely branch semantics, branch-and-link, caller/callee continuations, recursive JAL, dynamic cross-function JALR, PC-zero entry, exception resume PCs, and host dispatcher budget/unknown-target handling.

COP1 JIT coverage now includes the supported R5900 instruction-table forms:
- MFC1/MTC1 and CFC1/CTC1, including cross-block GPR liveness for CTC1.
- C.F.S/C.EQ.S/C.LT.S/C.LE.S, signed zero, denormal and NaN normalisation, and FCC0/FCSR preservation.
- ADD.S/SUB.S/MUL.S/DIV.S, SQRT.S/RSQRT.S, MAX.S/MIN.S, ABS.S/NEG.S.
- ADDA.S/SUBA.S/MULA.S, MADD.S/MSUB.S, MADDA.S/MSUBA.S, and CVT.W.S/CVT.S.W.
- FCSR cause/sticky semantics, denormal handling, signed infinities/NaNs, saturation and accumulator-path distinctions.
- BC1 condition sampling before an ordinary delay slot and BC1FL delay-slot annulment.

## Latest COP2/VU0 status

See [`COP2_RESEARCH.md`](COP2_RESEARCH.md) for the PCSX2 reference and semantic caveats.

- QMFC2/QMTC2/CFC2/CTC2 and BC2F/T/FL/TL are now decoder-VALID for their documented encodings and lowered through the generated LLVM runtime-helper calls.
- The aligned `s_ee_vu0_state` sidecar carries VI[32], ACC, Q/P, visible control/flag values, TPC/micro-PC, cycle counters and E/M/running state, plus separate 4 KB VU data and micro-program memories. The established `s_ee_cpu_state::m_vu0[32]` remains the VF bank, so the previous CPU/LLVM ABI offsets are preserved.
- `ee_vu0_fetch_micro_instruction` fetches paired lower/upper words, decodes I/E/M/D/T flags, advances the byte PC by eight and wraps at 4 KB. C++ and Python ABI layout checks cover the appended memories.
- Fetch also classifies lower and upper operations using the PCSX2 primary/secondary tables. Lower primary `0x3c` dispatches through the lower sub-op/T3 tables; upper primary `0x3c..0x3f` dispatches through the four FD tables. Unassigned entries return `UNKNOWN`; this is classification, not execution.
- Runtime transfer helpers apply synchronization before reads/writes; cover VF0, GPR0, CFC2 R and CTC2 special-register cases; and fail closed when the required scheduler/control callback is missing or fails.
- BC2 reads VPU_STAT bit 8 and has ordinary/likely branch JIT coverage, including delay-slot execution and annulment.
- `ee_vu0_scheduler_sync_callback` provides bounded catch-up, finish-to-E, and wait-to-E-or-M orchestration, including VU1 catch-up and EE event callbacks. Interlocked waits add only the VU0 cycles consumed to the original EE cycle counter; they do not collapse EE time to the VU0 counter unless taking PCSX2's separate early M-bit/already-ahead path.
- `ee_vu0_execute_ialu_callback` executes lower IADD/ISUB/IAND/IOR/IADDI/IADDIU/ISUBIU and lower B/BAL/JR/JALR/IBEQ/IBNE/IBLTZ/IBGTZ/IBLEZ/IBGEZ when paired with upper NOP. The IALU path performs 16-bit VI writes, keeps VI0 hardwired, writes destinations immediately with zero IALU-pipeline latency (as specified by PCSX2's lower-op metadata). The generic four-entry queue is retained for future nonzero-latency lower operations such as ILW/ILWR; VI-backup timing is modelled separately. The branch path implements signed relative targets, VI-slot JR/JALR targets, two-pair delay handling, nested-branch deferral, BAL/JALR links, source interlocks, and PCSX2's two-cycle VI-backup behaviour for conditional reads.
- Each engine execution slice returns at an M-flagged pair. The outer scheduler decides whether M ends the wait: WAIT_TO_E_OR_M returns, while FINISH_TO_E invokes another slice and continues toward E. The integration tests cover resuming after M and E delay execution; the scalar integer subset does not incur an extra IALU flush cycle at E.
- This is **not** a general VU0 interpreter. The direct runtime suite covers B/BAL, JR/JALR, all six conditional integer branches on taken and not-taken boundaries, delay-slot execution, nested JALR, target/link overlap, negative relative offsets, VI-backup/interlock timing, E on a branch pair, and a branch stall that overruns a one-cycle execution budget to finish its issued pair. A deterministic 256-pair scalar IALU program and a separate 384-pair mixed IALU/branch control-flow program are checked against independent test-side models after every pair. The shared 37-pair differential fixture, candidate trace producer, JSONL comparator and pinned PCSX2 trace adapter are now implemented. Candidate trace generation passes CTest, but the PCSX2 test binary has not yet been built and a real interpreter trace has not yet been compared. VU data-memory operations, I/D/T flags, upper vector operations, general dual-issue dependencies, VF forwarding and FMAC/FDIV/EFU pipelines remain unsupported. Unsupported pairs fail closed without consuming their micro-PC.
- VU0 SPECIAL1/SPECIAL2 macro instructions remain UNSUPPORTED. Do not enable them until dual-issue scheduling and the FMAC/FDIV/EFU pipelines, flags, ACC/Q/P forwarding and branch timing are implemented.

The earlier VU0 integer/branch, scalar/mixed oracle, branch-boundary and E-delay, COP2 JIT, function-discovery/data-flow, linker, dispatcher and MMI suites passed **15/15** in [Actions run #43229844156](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43229844156). The current test suite now passes 20/20 on main. Forced three-cycle and dual-source three-/five-cycle interlocks verify that VI-backup aging occurs after the stall/pipe-retirement phase and before lower-op execution; the exact-cycle budget completes an already-issued pair atomically.

## Remaining priorities

1. **Run the live VU0 differential comparison:** the manual GitHub Actions run [#2](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/43275643702) is currently building the pinned PCSX2 third-party dependencies. It has not yet reached the PCSX2 CMake configure, core-test build or trace comparison. Resolve any first-divergence mismatches before treating the supported lower integer/branch slice as PCSX2-equivalent.
2. **Re-audit USA JALR recovery:** bounded `LW`/`LWU` propagation from carefully filtered ELF data is implemented and covered by synthetic fixtures. Re-run the full USA ELF analysis, compare per-site targets against the 3,475-unresolved baseline, and validate the EE loader's actual write/protection assumptions before counting a read-only segment load as immutable. Keep writable globals, runtime object/vtable values and unknown aliases dynamic.
3. **VU0 memory and vector issue:** add LQ/SQ/ILW/ISW and a carefully tested dual-issue model before enabling vector arithmetic or macro instructions.
4. **Runtime completeness:** EE memory/TLB/MMIO, scratchpad/ROM aliases, alignment exceptions, DMA, interrupts, plus BIOS/IOP/GS host services.
5. **Semantic validation:** differential instruction and function testing against PCSX2's interpreter. LLVM verification and native linking do not establish R5900 equivalence.
6. **Production backend:** replace the current external llvmlite object-emission path with an integrated LLVM backend or supported `llc`/LLVM library path; keep the current script as a validation/reference path until then.

The exact later temporary worktree associated with the historical 2/4 CTest report was never exported. Main remains a reconstructed source baseline, not recovery of that inaccessible working directory.

## Reproduction

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/ee_decoder_test
```

Full-image validation (after building `ee_recomp` and installing `llvmlite`):

```sh
./build/ee_recomp SLUS_214.42 build/recompiled
python3 tools/verify_ir.py 'build/recompiled/*.ll' --progress=250
python3 tools/verify_ir.py 'build/recompiled/*.ll' --emit-opt=0 --progress=250
```
