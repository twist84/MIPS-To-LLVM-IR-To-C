# SPU2 guest-clock foundation

Reference: [ps2-web-recomp/SDBZ SPU2](https://github.com/twist84/ps2-web-recomp/blob/SDBZ/runtime/src/iop/spu2.cpp), `kCyclesPerSample = 768`. The nominal sample rate is 48 kHz at 36.864 MHz IOP clock. The reference batches 32 samples per event; this first batch tracks the elapsed sample count and fractional phase, not rendered samples or a hardware-cycle-validated timeline.

The existing attached R3000 interpreter/AOT already advances IOP hardware on retirement. The HLE kernel previously advanced the same hardware a second time after each slice. Attached CPU retirement now owns that advancement; the kernel still advances idle time, handled HLE syscall cycles and detached fixtures. This affects existing DMA and Timer 5 timing, so a fresh retail replay is required.

Clock activation follows the reference's first SPU2 access. Zero elapsed samples before activation; exact quotient/remainder after activation; reset clears both. Ordinary DMA behavior, register latches and IRQ semantics otherwise remain unchanged. No synthetic voice status, periodic IRQ, AutoDMA completion, sample output or host playback is implemented. The sample clock does not yet introduce a device wake deadline, because there are no sample-triggered events. The next stage must integrate real voice fetch/advance and playback-event deadlines before exposing dynamic voice readback.

The runner prints the final IOP/SPU2 elapsed cycles, sample count and phase. These are diagnostic time-base values, not output frames. No change to EE LLVM lowering or VU1 AOT is required.

Tests cover first access activation, sub-sample remainder, large advances, reset, active R3000 and idle kernel cycle ownership and absence of invented IRQs.
