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

## Batch 1: disabled-effect arithmetic elimination

The disabled reverb path does not simply discard its return. It omits
only work that cannot affect output, RAM or IRQs in the existing model:
input FIR and reflection calculations (their results feed suppressed
writes); with APF2 gain zero, its delayed RAM tap alone determines the
return; with only APF1 gain zero, the two delay taps suffice. Otherwise
use the original comb and APF calculations. All input history, output
history, sample phase and enabled-effect reads/writes remain intact.
A maintained 64-bit occupancy mask per output history permits exact zero
FIR elision, never based on zero EVOL or instantaneous silence alone.

A forced-reference host-only test control compares PCM byte-for-byte,
RAM (both work areas alias the same physical range), counters and IRQ
latches/deadlines through disabled and enabled phases, signed gains,
nonzero DMA input, invalid areas and re-enable. The runner reports
`fast_ticks` and `zero_fir_skips` per core. No hardware behavior is changed
or newly claimed. Performance gain requires a matched retail measurement;
this batch does not alter EE lowering or VU1 AOT provenance.

### Matched reference/optimised replay

The host-only `SDBZ_SPU2_EFFECT_FAST_PATH` switch selects the full reference
path with value 0; unset or 1 selects the default optimisation. Both use
the same guest clock, source input, cache provenance and runtime code.
Warm the compatible cache first. Use the uncapped device-free preset above,
run once with `set "SDBZ_SPU2_EFFECT_FAST_PATH=0"`, preserve the archive, then
repeat with `set "SDBZ_SPU2_EFFECT_FAST_PATH=1"`. Keep physical controller
input neutral and compare actual input observations, dispatch and work
counts before interpreting timings. The log prints the selected mode.
