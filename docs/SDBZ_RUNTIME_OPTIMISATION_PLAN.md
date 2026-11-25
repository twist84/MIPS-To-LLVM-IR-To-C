# SDBZ runtime optimisation follow-up

Base: squash-merged PR #155, `f16a660bedee560d2e4b8179b39202aefd3aee8f`.

Evidence: [2026-11-25 retail validation](SDBZ_RUNTIME_VALIDATION_20261125.md).
The listening replay reached training gameplay with audible PCM and zero
IOP guest faults or VU1 fallbacks. Pacing, live compilation and physical
input make it unsuitable as an uncapped comparison.

## Implementation order

1. Establish matched, warmed-cache, uncapped P40 measurements; separate
   compilation and pacing from exclusive execution costs.
2. Eliminate provably redundant SPU2 processing for disabled effects with
   zero inputs/returns. Preserve filter histories, RAM accesses and
   future enable transitions; test silent, nonzero and tail cases.
3. Profile EE native execution and the highest-cost guest PCs, including
   polling loops. Establish actual producers and architectural side effects
   before considering any scheduling shortcut.
4. Reduce redundant IOP scheduler/service and dispatch-diagnostic work.
   Preserve guest-cycle ownership, DMA, IRQs and ordering.
5. Measure GS/GIF/WebGPU submission and GPU timestamps where evidence
   justifies further work; retain CPU/GPU scanout equivalence.
6. Continue independent R5900/COP0/COP1/VU and SPU2 differential accuracy
   audits. Do not label compatibility assumptions as hardware results.

## Validation contract

- One coherent change at a time; run relevant synthetic regression tests
  and verify CI before declaring a change validated.
- Preserve compatible EE IR and VU1 AOT caches. Regenerate only when
  guest-lowering provenance or actual microcode misses require it.
- Compare matched dispatch budgets, input, guest workload, cache state
  and profiler settings. Report whole-run and exclusive subsystem costs.
- Request a retail replay only when runtime evidence is needed. Otherwise
  proceed to the next implementation item.
- Do not fabricate guest completion, interrupts, output or game-specific
  bypasses. Do not commit proprietary retail assets or generated caches.

## Local profiling presets

```bat
set "SDBZ_P40_LISTEN="
set "SDBZ_HOST_AUDIO=0"
set "SDBZ_RUNTIME_PROFILE=1"
set "SDBZ_PROFILE_PCS=1"
set "SDBZ_AUDIO_PROFILE=1"
run_sdbz_p40.bat
```

Listening control: set SDBZ_P40_LISTEN and SDBZ_HOST_AUDIO to 1.
The script performs a fast-forward-only pull and uses the existing
`build/vu1-native-v20` library. Keep the code baseline intact while
measuring; do not compare paced and uncapped wall times directly.
