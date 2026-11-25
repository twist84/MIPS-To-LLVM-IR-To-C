# SPU2 guest-clock foundation

Reference: [ps2-web-recomp/SDBZ SPU2](https://github.com/twist84/ps2-web-recomp/blob/SDBZ/runtime/src/iop/spu2.cpp), `kCyclesPerSample = 768`. The nominal sample rate is 48 kHz at 36.864 MHz IOP clock. The reference batches 32 samples per event; this first batch tracks the elapsed sample count and fractional phase, not rendered samples or a hardware-cycle-validated timeline.

The existing attached R3000 interpreter/AOT already advances IOP hardware on retirement. The HLE kernel previously advanced the same hardware a second time after each slice. Attached CPU retirement now owns that advancement; the kernel still advances idle time, handled HLE syscall cycles and detached fixtures. This affects existing DMA and Timer 5 timing, so a fresh retail replay is required.

Clock activation follows the reference's first SPU2 access. Zero elapsed samples before activation; exact quotient/remainder after activation; reset clears both. Ordinary DMA behavior, register latches and IRQ semantics otherwise remain unchanged. No synthetic voice status, periodic IRQ, AutoDMA completion, sample output or host playback is implemented. The sample clock does not yet introduce a device wake deadline, because there are no sample-triggered events. The next stage must integrate real voice fetch/advance and playback-event deadlines before exposing dynamic voice readback.

The runner prints the final IOP/SPU2 elapsed cycles, sample count and phase. These are diagnostic time-base values, not output frames. No change to EE LLVM lowering or VU1 AOT is required.

Tests cover first access activation, sub-sample remainder, large advances, reset, active R3000 and idle kernel cycle ownership and absence of invented IRQs.

## Batch 2: ADPCM voice cursor

The reference's 28-sample, 16-byte blocks, five predictors, signed nibble order, rounding, saturation and 12-bit fractional pitch accumulator are implemented for both cores. Two-block lookahead and loop-start/repeat/end flags follow the reference; ENDX is set on consumption rather than prefetch. NAX reflects the consumed position (with the prior SSA+1 idle convention before key-on). Sound RAM fetch wraps at 20 bits. The guest clock advances only complete sample periods; active voices advance in aggregate by pitch, not a host playback clock. Key-off still stops immediately pending the ADSR/release batch. ENVX/sweep reads still fail closed for active voices. Playback IRQ, pitch modulation, noise, interpolation/mixing, reverb, AutoDMA and host PCM remain unimplemented. No PCM samples are emitted by this stage. raudio (https://github.com/raysan5/raudio) is selected for the eventual independent host output layer.

Tests check known decoder vectors, signed rounding, sound RAM wrap, key-on sample boundaries, pitch, NAX, loop/ENDX consumption, core and voice isolation, and no fabricated IRQ. The runner reports voice starts, decoded blocks, end events and active masks. These are not mixed frame counts.

## Batch 3: Voice envelopes

Implement the SDBZ reference attack/decay/sustain/release sample-rate envelope and signed fixed/swept voice volumes. ENVX and current volume reads expose the computed state. Key-off cancels a pending key-on or enters release, retaining voice activity and cursor progression until level reaches zero. ADPCM end without repeat stops immediately. Cursor/ENDX and envelope updates are ordered per sample, replacing aggregate pitch advancement so release cannot consume extra blocks. No host PCM, playback IRQ, noise, pitch modulation, AutoDMA or reverb yet. Reference envelope behavior is not claimed to be bit-exact hardware verification. Tests cover attack, decay, sustain, release, signed fixed volume, sweeps, pending key-on cancellation and sample boundaries.

## Batch 4: Voice playback interrupts

Compare IRQA against each eight-halfword consumed voice block, including sound RAM wraparound and either core's IRQ enable/latch. On the first sample after key-on and subsequent consumed block transitions (including the reference's end transition), newly latched core bits assert IOP IRQ9. Speculative lookahead alone does not assert IRQ. Clearing I_STAT alone does not clear the SPU2 latch; disabling the core IRQ bit rearms it. Idle kernel deadlines now include the earliest possible voice fetch while a core can latch an IRQ; the actual address match remains conditional. No host clock, synthetic periodic interrupt, mixer-ring access, AutoDMA completion or PCM is generated. AOT still advances devices at its existing retirement boundaries, not a claimed per-instruction hardware timing model. Tests cover sample boundaries, lookahead isolation, shared RAM wrap/cross-core edges, acknowledgement, rearm and idle progression.

## Batch 5: AutoDMA input streaming

Implement reference planar PCM input DMA on each core: 1024-byte chunks (256 stereo frames), 512-frame input rings at 0x2000/0x2400 (left) and +0x200 (right), immediate initial fill and refill at 32-sample boundaries when a full half is free. Read live IOP RAM on each refill rather than snapshotting future data. DMA4/7 complete 32 IOP cycles after the final chunk enters the ring; CHCR cancellation stops pending refill/completion without discarding already buffered data. ADMAS zero resets the input cursor/count as in the reference. TSA remains independent. Input consumption checks the reference left-ring IRQA range, including after DMA completion. Idle deadlines include pending refill and completion. No actual PCM mixing or host raudio yet. Tests cover planar data, delayed live RAM fetch, refill boundary, completion, independent TSA and cancellation. Nominal timing follows the existing sample clock and reference batch length; not hardware-cycle validated.
