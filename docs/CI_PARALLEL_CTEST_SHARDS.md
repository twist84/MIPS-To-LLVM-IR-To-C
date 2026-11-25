# Parallel CTest CI shards

The [Build and test workflow](../.github/workflows/ci.yml) builds once on Linux, retaining its release configuration, LLVM Python binding and `ee-recomp-linux` tool artifact. It uploads the configured build and Python virtual environment in a tarball preserving executable bits and symlinks.

Seven independent test jobs with `needs: build` use the same archive:

- `game-0` through `game-3`: one native integration test each (`game_runner_end_to_end_0` through `game_runner_end_to_end_3`).
- `fast-0`, `fast-1`, `fast-2`: all other configured CTest tests, sorted and partitioned by index modulo three.

Each job checks out source, restores the build at the same workspace path and invokes CTest without rebuilding. [The selector](../tools/ci/ctest_shard.py) uses the JSON listing, checks mandatory game tests and exhaustive nonoverlapping coverage, and selects exact names. The matrix uses `fail-fast: false`; every shard uploads JUnit and LastTest.log. The workflow passes only if all seven shards pass.

Verified pre-merge [Build and test 45663177622](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/actions/runs/45663177622): 57 + 57 + 56 fast tests and four game tests = **174/174 passed**. Native WebGPU, SIF DMA and OpenPS2BIOS ROM0 have separate workflows. The [Original ps2autotests workflow](PS2AUTOTESTS.md) independently builds its two tools and runs six observational groups covering all 55 original ELFs; its score is not part of the CTest count.

Run a shard locally after building:

```bash
python3 tools/ci/ctest_shard.py --build-dir build --shard fast-0
python3 tools/ci/ctest_shard.py --build-dir build --shard game-0
```
