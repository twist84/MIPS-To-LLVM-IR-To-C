# SDBZ retail runtime validation — 2026-11-25

## Earlier listening replay (PR #155)

Code: [`1f6a12142bd7dfa34ace33fc23cf26db190df421`](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/commit/1f6a12142bd7dfa34ace33fc23cf26db190df421), PR [#155](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/pull/155).
All four workflows passed: Build and test, Native WebGPU build, SIF DMA
acceptance, OpenPS2BIOS ROM0 execution. Archive:
`sdbz-graphics-pr148-neutral-20261125-104855750.zip`; USA `SLUS_214.42`,
HLE-first P40 listening preset. User reported audible audio restored.

| Observed metric | Value |
| --- | ---: |
| Dispatches | 40,000,000 |
| Cached EE IR module hashes | 16,219 |
| Final PC | 0x001A9AF0 |
| IOP guest fault | none |
| Native VU1 runs | 396,717 |
| Native VU1 pairs | 457,133,821 |
| Interpreter fallbacks / worker host failures | 0 / 0 |
| Window presentations succeeded | 2,705 |
| CPU/GPU display capture bytes | identical |
| SPU2 mixed / nonzero frames | 2,939,857 / 1,791,290 |
| PCM peak / clipped samples | 32,766 / 10 |
| Host audio | ready, 48 kHz stereo signed 16-bit stream |

The final report stopped at the dispatch cap, not game completion.
Training gameplay was reached. The pad observer recorded 99 Cross-low
candidates and 101 analogue samples outside deadzone (2,410 samples);
this is not a controlled neutral comparison. One VU1 job was pending at
the report boundary. Cache inventory is dynamic, distinct from static
function discovery. No retail binaries, assets, captures or generated
objects are included.

## Later warmed retail replay (PR #156 workstream)

The latest fully documented warmed run is the 12:06 archive
`sdbz-graphics-pr148-neutral-20261125-120640867.zip`, recorded in
[the optimisation log, Batch 10](SDBZ_RUNTIME_OPTIMISATION_PLAN.md#batch-10-retail-validation--2026-11-25-1206).

| Observed metric | Value |
| --- | ---: |
| Dispatches | 40,000,000 |
| Process wall / profiled / compile wait | 49.406 s / 44.451 s / 0 s |
| Cached EE IR modules | 16,250 |
| IOP guest faults | 0 |
| Native VU1 jobs | 411,642 |
| VU1 interpreter fallback / host failure | 0 / 0 |
| Fields / successful presentations | 4,006 / 2,886 |
| Nonzero PCM frames | 1,858,699 |
| HLE INTC entries/returns | 3,474 |

This is the corrected BEV=0 interrupt-vector replay, not a measurement
of the final squash commit [`130d305`](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/commit/130d3054e5ef978e5dfd4408b00422fa43184eea). Both display PPMs were
byte-identical in GPU-authoritative mode, which is not independent
CPU-versus-GPU rasterizer validation. It was device-free profiling rather
than a second audible-audio confirmation. Input and workload must be
matched before comparing timing.

## Changes consolidated in PR #155

R5900/COP1 integer arithmetic and result-vector regressions, VU accuracy
and asynchronous native VU1 execution, transactional GS/VIF/GIF batching,
WebGPU presentation, IOP scheduling and direct EE dispatch improvements,
cache recovery/provenance, Windows launcher and P40 diagnostics, and
guest-clock SPU2 with host raudio PCM. See the detailed design and audit
documents for individual limitations.

SPU2 includes voice ADSR/volume, DMA and AutoDMA, IRQ deadlines, MMIX/VMIX
routing, mixer rings, core cascade, Gaussian interpolation, PMON/NON,
master sweep/readback and reverb stages. The original active-high ATTR
bit 14 mute gate caused a silent replay; it is now bypassed while the
register latch is preserved. Hardware mute semantics, inactive-voice
fetches, Gaussian FIFO startup, extreme pitch and reverb timing,
SPDIF and full hardware differential execution remain open. Both
effects were disabled throughout this replay with zero input/return;
a synthetic fixture pass does not prove audible retail reverb accuracy.

## Profile and follow-up

The listening replay is not an uncapped benchmark. Compilation waits,
frame pacing and user input differ across captures. The SPU2 mixer
consumed 2.324 s inclusive, including 1.124 s in reverb and 0.404 s in
voice processing. Both effect engines ticked despite disabled state and
zero effect input/return. This warrants a provably equivalent fast path,
not a global assumption that audio is the main bottleneck.

At the time of the listening replay, the next steps were to establish matched warmed-cache uncapped baselines; optimise
unnecessary SPU2 processing with history/IRQ equivalence tests, then
profile EE native execution, IOP service and dispatch diagnostics.
Preserve real guest events and cached translations. Do not skip polling
loops without establishing their producers and architectural effects.
Keep further R5900/COP0/COP1/VU accuracy and device differential audits
separate from claimed performance gains.

See [audio history](SDBZ_SPU2_AUDIO_CLOCK.md),
[native boot plan](SDBZ_NATIVE_BOOT_PLAN.md) and
[cache-miss recovery](SDBZ_LIVE_CACHE_MISS_RESUME.md).

PR #156 subsequently implemented and measured disabled-effect optimisation and other runtime changes. See the [optimisation log](SDBZ_RUNTIME_OPTIMISATION_PLAN.md) for those measurements and [current status](README.md) for the final pre-merge CI evidence. The post-merge accuracy priorities are tracked separately in [ACCURACY_PLAN.md](ACCURACY_PLAN.md).
