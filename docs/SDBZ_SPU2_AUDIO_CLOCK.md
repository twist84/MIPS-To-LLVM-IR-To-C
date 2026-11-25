# SPU2 guest-clock foundation

Reference: [ps2-web-recomp/SDBZ SPU2](https://github.com/twist84/ps2-web-recomp/blob/SDBZ/runtime/src/iop/spu2.cpp), `kCyclesPerSample = 768`. The nominal sample rate is 48 kHz at 36.864 MHz IOP clock. The reference batches 32 samples per event; this first batch tracks the elapsed sample count and fractional phase, not rendered samples or a hardware-cycle-validated timeline.

The existing attached R3000 interpreter/AOT already advances IOP hardware on retirement. The HLE kernel previously advanced the same hardware a second time after each slice. Attached CPU retirement now owns that advancement; the kernel still advances idle time, handled HLE syscall cycles and detached fixtures. This affects existing DMA and Timer 5 timing, so a fresh retail replay is required.

Clock activation follows the reference's first SPU2 access. Zero elapsed samples before activation; exact quotient/remainder after activation; reset clears both. Ordinary DMA behavior, register latches and IRQ semantics otherwise remain unchanged. No synthetic voice status, periodic IRQ, AutoDMA completion, sample output or host playback is implemented. The sample clock does not yet introduce a device wake deadline, because there are no sample-triggered events. The next stage must integrate real voice fetch/advance and playback-event deadlines before exposing dynamic voice readback.

The runner prints the final IOP/SPU2 elapsed cycles, sample count and phase. These are diagnostic time-base values, not output frames. No change to EE LLVM lowering or VU1 AOT is required.

Tests cover first access activation, sub-sample remainder, large advances, reset, active R3000 and idle kernel cycle ownership and absence of invented IRQs.

## Batch 2: ADPCM voice cursor

The reference's 28-sample, 16-byte blocks, five predictors, signed nibble order, rounding, saturation and 12-bit fractional pitch accumulator are implemented for both cores. Two-block lookahead and loop-start/repeat/end flags follow the reference; ENDX is set on consumption rather than prefetch. NAX reflects the consumed position (with the prior SSA+1 idle convention before key-on). Sound RAM fetch wraps at 20 bits. The guest clock advances only complete sample periods; active voices advance in aggregate by pitch, not a host playback clock. Key-off still stops immediately pending the ADSR/release batch. ENVX/sweep reads still fail closed for active voices. Playback IRQ, pitch modulation, noise, interpolation/mixing, reverb, AutoDMA and host PCM remain unimplemented. No PCM samples are emitted by this stage. raudio (https://github.com/raysan5/raudio) is selected for the eventual independent host output layer.

Tests check known decoder vectors, signed rounding, sound RAM wrap, key-on sample boundaries, pitch, NAX, loop/ENDX consumption, core and voice isolation, and no fabricated IRQ. The runner reports voice starts, decoded blocks, end events and active masks. These are not mixed frame counts.
