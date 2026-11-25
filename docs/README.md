# Documentation and current development status

**Baseline (2026-11-25):** [`130d305`](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/commit/130d3054e5ef978e5dfd4408b00422fa43184eea), squash merge of [PR #156](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/pull/156). The repository layout cleanup in PR #157 is separate from this runtime baseline.

## Verified evidence

| Boundary | Result |
| --- | --- |
| Pre-merge original console transcripts | 54/55 exact (98.18%), 1 DIFF, 0 INCOMPLETE/NO_OUTPUT/ERROR |
| Pre-merge CTest | 174/174 across seven Linux shards |
| Other pre-merge workflows | Native WebGPU, SIF DMA and OpenPS2BIOS ROM0 passed |
| Remaining original transcript | `vu/lower/efu`: 201/208 numerical words match; seven differences |
| Retail SDBZ USA | Bounded interactive training gameplay with native graphics and audible PCM observed on earlier revisions |
| Latest documented warmed retail replay | 40M dispatches, 49.406 s process wall, 16,250 cached EE modules, 411,642 native VU1 jobs, 2,886 successful presents, 1,858,699 nonzero PCM frames |

Sources: [original transcript run 45663177559](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45663177559), [Build and test 45663177622](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45663177622), [VU artifact 15044679435](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45663177559/artifacts/15044679435), and [retail replay evidence](SDBZ_RUNTIME_VALIDATION_20261125.md). The tested PR merge was `0e48f9a42d1eb4696f01bc1fafaeb95e76a8ced7`, PR head `8bbc80d8f6d639265ea8a9dae27ecac815584148`. No fresh retail replay on the squash commit is claimed. A passing observational autotest workflow means the matrix completed and aggregated correctly, not that all guest transcripts matched. Clean guest exit is separately recorded.

## Common entry points

From the root, use `scripts\build.bat` (host build), `scripts\build.bat --test` (build and CTest), `scripts\run_ps2_game.bat --full` (normal retail replay), `scripts\run_ps2_game.bat --p40` (40M profiling preset), or `scripts\run_ps2_game.bat --p40-listen` (paced audio). See [project README](../README.md) for focused build modes and launcher options.

## Current guides

- [Project README and launch commands](../README.md)
- [Original ps2autotests: execution, provenance and results](PS2AUTOTESTS.md)
- [Latest retail evidence and limitations](SDBZ_RUNTIME_VALIDATION_20261125.md)
- [Parallel CTest workflow](CI_PARALLEL_CTEST_SHARDS.md)
- [Live translation-cache recovery](SDBZ_LIVE_CACHE_MISS_RESUME.md)
- [Matched CPU-only replays](SDBZ_KNOWN_IR_REPLAY.md)
- [Native WebGPU and optional Win32 window](NATIVE_WEBGPU.md)
- [VU0 differential trace contract](VU0_DIFFERENTIAL_TRACE.md) and [adapter](../tools/vu0_diff/README.md)
- [raudio provenance](RAUDIO_UPSTREAM.md)
- [Accuracy work plan](ACCURACY_PLAN.md)
- [Resume point](RESUME.md) and [test results](TEST_RESULTS.md)

## Architecture and development records

These files retain the evidence, decisions and limitations of their named revisions. Older uses of “current”, “pending”, “unsupported” and branch/PR instructions refer to the recorded checkpoint, not today's runtime.

- EE and translation: [COP2 research](COP2_RESEARCH.md), [full-image native link](FULL_GAME_NATIVE_LINK_VALIDATION.md), [JALR audit](JALR_COVERAGE_AUDIT.md), [alignment and COP1](M1_B_EE_MEMORY_ALIGNMENT_COP1.md), [EE timing history](M1_A_EE_TIMING_STATUS.md), [manual timing audit](M1_A_R5900_REFERENCE_AUDIT.md), [reference architecture audit](M1_A_PS2_WEB_RECOMP_REFERENCE_AUDIT.md).
- Boot and HLE: [native boot plan](SDBZ_NATIVE_BOOT_PLAN.md), [HLE contract](HLE_FIRST_RUNTIME_PLAN.md), [boot log](GAME_BOOT_PROGRESS.md), [IOP integration](IOP_BOOT_INTEGRATION.md), [IOP parity audit](SDBZ_IOP_HLE_REFERENCE_PARITY.md), [M2 runtime/ISA plan](M2_SDBZ_NATIVE_RUNTIME_AND_ISA_PLAN.md).
- Graphics and audio: [native graphics integration](SDBZ_NATIVE_GRAPHICS_INTEGRATION.md), [PR #148 graphics replay](SDBZ_PR148_GRAPHICS_REPLAY_20261121.md), [initial WebGPU port](NATIVE_WEBGPU_PORT.md), [SPU2 history](SDBZ_SPU2_AUDIO_CLOCK.md).
- Optimisation: [PR #156 workstream](SDBZ_RUNTIME_OPTIMISATION_PLAN.md), [ELF hotspot audit](SDBZ_US_ELF_HOTSPOT_XREF_AUDIT_20261121.md).
- Additional M2 and SDBZ IOP investigation files in this directory retain the detailed original captures. [Work log](WORK_LOG.md) records early recovery and subsequent work. [Recovery metadata](../recovery/metadata/) contains historical snapshot records, not instructions for the current tree.

## Accuracy boundary

LLVM IR/native EE execution, a bounded RAM callback interpreter, HLE EE/IOP services, original IOP IRX execution, VU0/VU1, GS and SPU2 are implemented to differing extents. The default HLE time domain is `reference_compatibility`, not a calibrated R5900 clock. The manual-cache-hit issue model is offline only. COP0 cycle-event counting is functional on the compatibility budget; full pipeline timing, event counters and precise overflow remain outstanding. A successful test vector, transcript, or bounded retail replay is not a general hardware-equivalence guarantee.
