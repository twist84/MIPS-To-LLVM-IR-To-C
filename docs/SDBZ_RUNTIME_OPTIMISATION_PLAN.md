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

### Retail results — 2026-11-25 11:09, 11:11, 11:13

Code HEAD `51356c26df9989cadf2b2375f4b65ea68b528d5b`: all four CI workflows passed. Three uncapped
40M-dispatch training replays (archives `sdbz-graphics-pr148-neutral-20261125-110912880.zip`,
`...-111137950.zip`, `...-111333140.zip`) confirmed nonzero PCM, no IOP
guest faults, and byte-identical CPU/GPU captures within each replay.
Host device disabled throughout.

| Metric | 11:09 fast | 11:11 reference | 11:13 fast |
| --- | ---: | ---: | ---: |
| Native process wall (s) | 48.734 | 92.594 | 78.531 |
| Compilation wait (s) | 0 | 44.705 | 32.143 |
| SPU2 mixer (ms) | 1928.025 | 2495.889 | 1582.432 |
| Effect processing (ms) | 353.634 | 1230.512 | 274.145 |
| Effect core ticks | 8716446 | 6288234 | 6740402 |
| Effect ns/core tick | 40.57 | 195.68 | 40.67 |
| Zero FIR skips | 17432892 | 0 | 13480804 |
| PCM nonzero frames | 1880791 | 1775296 | 1907791 |
| EE native exclusive (s) | 15.617 | 15.237 | 15.110 |
| IOP service exclusive (s) | 9.523 | 9.692 | 8.695 |
| Dispatch diagnostics exclusive (s) | 2.201 | 2.184 | 2.183 |
| Cached EE IR modules | 16219 | 16232 | 16250 |
| Native VU1 runs | 351651 | 430518 | 407260 |
| VU1 fallbacks | 0 | 0 | 864 |
| GS fields | 5444 | 3928 | 4211 |
| Window presentations | 3928 | 2840 | 3031 |
| Cross-low candidates | 192 | 119 | 122 |
| Analog samples outside deadzone | 60 | 88 | 216 |

All common IR hashes match; 13 then 18 new modules published. The two
fast replays used the shortcut on every core tick; both effects remained
disabled, with zero wet inputs/returns and effect IRQs. Per-tick cost
fell approximately 79.2% (4.82x), not a controlled whole-runtime speedup.
Inputs, cache waits, guest cycles and final combat states differ. The
11:09 replay is the cleanest uncapped baseline (zero compilation wait).
The 11:13 replay captured three new VU1 images (720 no-entry, 144
code-mismatch fallbacks; PCs 0x450, 0x50, 0x170). Compile only the
captured misses into the existing v20 AOT directory, retaining the EE
cache. No VU1 optimisation is necessary for this batch.

Next: EE native and IOP service dominate exclusive runner time. The
command-slot polling entry 0x00102560 costs 1.369–1.515 s; the current
idle analyzer rejects 0x001025a4 because a branch enters the loop body.
Retain this guard until the original CFG, timer and command-slot producer
are established. Dispatch diagnostics consume a repeatable ~2.18–2.20 s
across all three runs and are a separate safe host-side optimisation
candidate. Do not conflate GPU sampled compute time with exclusive
runner CPU time or sum overlapping worker durations.
