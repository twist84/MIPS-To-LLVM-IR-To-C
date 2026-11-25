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

## Batch 6: Stereo PCM mixer

Generate interleaved signed 16-bit stereo PCM at 48 kHz from the guest clock, with the reference's linear fractional ADPCM interpolation, per-voice ADSR and signed voice-volume gains, planar AutoDMA input/B-volume gains, per-core master gain and final saturated sum. Use wide intermediates for master mixing. All state advances with or without an optional nonblocking PCM sink; the sink receives full 32-frame batches, with at most 31 pending frames. Reset clears pending samples and counters while preserving the host binding. Headless inactive silence advances its frame count without a per-frame loop. Diagnostics report mixed/nonzero frames and peak magnitude. No host playback, mixer-ring writeback/IRQs, reverb, noise, pitch modulation or hardware-exact Gaussian interpolation yet. Master sweep uses the reference's full-scale fallback. Keep mixer-ring writeback separate because it changes shared guest RAM and IRQ timing; the next batch will address this before the raudio host sink. Tests cover known voice/ADSR samples, signed planar input gain, silence, clock boundaries, reset and sink batching.

## Batch 7: Guest mixer-ring writeback and IRQs

Write the reference's 16 sound RAM mixer rings (eight per core, 512 halfwords each) at completed 32-frame guest sample boundaries. Core 0 input taps hold raw AutoDMA samples; core 1 input taps hold post-master core 0 output. Voice 1/3 taps hold enveloped pre-volume samples. Dry taps hold the pre-input voice mix; wet taps remain zero until reverb is implemented. First completed tick writes ring offsets 32..63, then wraps at 512. Ring IRQA comparison is cross-core and uses the existing edge latches, with idle deadlines even when no voices play. A whole silent ring revolution may be collapsed only inside an uninterrupted headless advance, when no source reads or host callbacks exist; partial batches and IRQ deadline phase are retained. Final stereo clipping is counted separately from peak magnitude. No raudio device yet, no reverb, noise or pitch modulation. The reference ring positions are modeled against the existing guest sample clock, not asserted hardware-exact. Tests cover actual voice tap values, dry/core transfer taps, silent wet tap, batch boundary, silent ring IRQ and wrap.


## Batch 8: raudio host playback

Standalone raudio is downloaded by CMake FetchContent into build/_deps/raudio-src at a pinned upstream revision and statically linked from a patched copy in build/_deps/raudio-build (see docs/RAUDIO_UPSTREAM.md). The fetched checkout and its license remain intact. Raylib's own audio module is disabled to avoid duplicate symbols. The sink copies full 32-frame SPU2 batches to a 4096-frame interleaved stereo SPSC queue. The guest is the sole producer; raudio's 48 kHz signed-16 stereo stream callback is the sole consumer. Release/acquire publication protects the samples; the guest never waits for the device and the callback never touches guest state, allocates or logs. Overflow drops the newest frames; underrun writes silence. Queue capacity is about 85 ms, and actual device scheduling adds latency. Pausing for hot expansion or IOP reset detaches the sink, synchronizes with the callback and discards stale queued frames; resume rebinds it. Shutdown detaches the sink before stopping the callback and closing the device. Device/stream initialization failure leaves guest emulation active.

Interactive paced execution enables playback by default, while uncapped or headless runs leave it disabled. Override with SDBZ_HOST_AUDIO=0 or 1. SDBZ_P40_LISTEN=1 makes the P40 script run at 100% target speed with audio. Uncapped audio is intentionally lossy when the producer outruns the device; it never changes the guest sample clock. The final host diagnostic records produced/queued/consumed/dropped/underrun/discarded frames, callbacks, queue high-water and depth before shutdown, stream format and negotiated device format. The existing SPU2 diagnostic supplies clipping counts. Queue tests use no audio hardware and cover partial reads/writes, wraparound, overflow, underrun, stereo order, quiescent discard/reuse and concurrent publication. Actual audible output and physical device latency require retail validation.

## Batch 9: Shared raylib checkout

Use the already pinned raylib 5.5 source for standalone raudio, including headless builds. Only configure the graphics library when the window is requested. Generate an audio-only header and patched translation unit in build/_deps/raylib-build/ee_raudio. The pinned raylib revision already synchronizes stream control and callback assignment and untracks buffers before converter teardown. Keep the local null-backend rejection, shutdown ordering, startup failure cleanup and negotiated-format diagnostics. The public stream validation name changes to IsAudioStreamValid. No SPU2 guest timing or mixer changes in this dependency-only batch. A cached retail listening replay is required before tuning queue behaviour.

## Batch 10: PCM startup and underrun recovery

The shared-raylib retail replay reached 40M dispatches and initialized a 48 kHz stereo device, with 3,684,352 queued PCM frames, zero dropped frames, 783,008 underrun frames and one 15.102-second compilation wait. Guest fields advanced at 42.57/s overall (49.48/s excluding compilation). The runner spent 48.956 seconds in 2,445 frame-pacing sleeps. This suggests a host pacing/throughput issue, not an audio clock change. Workloads were not matched across the two listening replays.

Keep the 4096-frame SPSC queue and add a consumer-owned 2048-frame startup/recovery threshold (42.67 ms at 48 kHz). The callback outputs silence while buffering without consuming queued samples, waits for at least the requested callback size when larger (capped at queue capacity), and re-enters buffering after a short read. Quiescent discard rearms prebuffering for IOP reset/hot expansion. Track prebuffer_silence separately from actual underrun frames and count rebuffer_events; both silence totals must be considered in comparisons. No producer blocking, clock changes, resampling, adaptive pacing or new host allocations. This mitigates short-term starvation but cannot compensate for a sustained production rate below real time. Tests cover startup hold, stereo order, partial read, larger callback requests, restart and conservation of queued frames.

## Batch 11: Windows frame-pacing wait precision

Batch 10 reached 40M dispatches with 4812 fields, 3,851,521 PCM frames, 94,816 underrun frames plus 778,080 prebuffer-silence frames and 312 rebuffer events. Total inserted silence was higher than Batch 9. It spent 47.788 seconds across 2357 pacing sleeps and 15.549 seconds in one live compilation wait. Workloads differ; the measured sleeps do not establish overshoot without requested-deadline telemetry.

The paced Windows window runner now uses a reusable high-resolution waitable timer, falling back to steady-clock sleep_until if unsupported or failed. Round relative intervals up to 100 ns units; preserve the absolute steady-clock deadline, original cycle-to-time conversion and re-anchor policy. No spin loop, process-wide timer-resolution change, guest clock changes or effect on uncapped/headless execution. Report requested and elapsed sleep time, total and maximum overshoot, high-resolution/fallback calls and timer failures. A portable regression checks expired deadlines and no early return; actual Windows timer precision requires retail validation. Keep the Batch 10 prebuffer unchanged for this comparison.

## Batch 12: Profile-overhead control

Batch 11 completed 40M dispatches without a cache miss. All 2345 pacing waits used the high-resolution timer, with 46.743 s requested, 47.390 s measured and 0.647 s overshoot (maximum 1.438 ms). The run generated 3,534,628 PCM frames; 52,352 underrun and 684,000 prebuffer-silence frames remain. Across the 45 completed profiling intervals, the 26 intervals at >=55 fields/s covered 3121 fields in 52.174 s, while the 19 slower intervals covered 1295 fields in 36.727 s with only 3.575 s pacing sleep. EE native exclusive time was 13.444 s in those slower intervals, followed by 6.784 s uninstrumented and 3.714 s IOP service. This does not establish that profiler overhead is responsible, nor that a larger PCM queue can fix sustained insufficient throughput.

Allow SDBZ_RUNTIME_PROFILE=0 to override the P40 default of 1. Keep all other P40 settings, host pacing, cache and audio queue unchanged. For the next control set SDBZ_PROFILE_PCS=0 and SDBZ_RUNTIME_PROFILE=0 with SDBZ_P40_LISTEN=1. This retains final guest/audio counters and captures but deliberately omits detailed timing scopes and per-PC timing. Compare phase speed, guest fields and total inserted silence, not raw dispatch count alone. No guest code changes or additional IR regeneration. Workloads remain input-dependent; repeat matched phases if needed.

## Batch 13: Independent audio profiling and playback control

Batch 12 disabled the general runtime and per-PC profiler. The 40M dispatch replay completed with no compilation waits, 4296 guest fields in 85.288 s runner elapsed time, 3,438,168 mixed frames, 59,232 underrun and 600,960 prebuffer-silence frames. Late speed dips persisted. This was not a matched workload and does not isolate SPU2 cost.

SDBZ_AUDIO_PROFILE=1 now enables thread-confined wall-time counters for SPU2 mixing, voice advance, ADPCM block decode, AutoDMA input mixing and pushes, mixer-ring writes, silent ring revolutions, PCM sink calls and the host queue callback. Mixer stages are nested; do not add their inclusive times. AutoDMA push is outside the mixer; host callback time is on a separate thread and measures only queue reads, not the whole backend converter/device thread. Disabled by default, independent of SDBZ_RUNTIME_PROFILE. Profiling counters accumulate across guest resets, are not guest state, and are read only after host shutdown. Timer overhead can affect measurements; SPU2 register accesses, IRQ deadline queries, DMA service outside push_input and device backend work are not separately timed by these counters. P40_LISTEN still selects 100% pacing but respects an explicit SDBZ_HOST_AUDIO=0; with P40_LISTEN unset, explicit 0/1 selects uncapped playback-off/on. Guest audio remains active in both cases; sinkless silent-ring acceleration may change host work, not guest timing. Compare matched game phases, and do not infer accuracy from playback performance.

## Batch 14: Dry/wet voice routing (effect input prerequisite)

Batch 13 uncapped audio-off/on: 40M dispatches each, 4765134/4794692 mixed frames, mixer wall 514.329/971.340 ms, PCM sink 0/5.414 ms and host queue callback 0/0.898 ms. Audio-off had two live expansion waits totaling 32.156 s; audio-on had none. VU1 native pairs differed substantially (739357588/353367740). These are not matched workloads. Audio timing excludes backend/device work, guest register access and IRQ deadline queries; it does not establish total audio overhead, but measured mixer/queue cost is small. Continue SPU2 accuracy independently of general runtime performance.

Implement per-core, per-channel 24-bit voice routing at VMIXL/VMIXEL/VMIXR/VMIXER (0x188/0x18c/0x190/0x194 relative to core). Low 16 bits at the first halfword, upper eight at +2, consistent with PCSX2 SPU2 register handling; reset routes all voices to all four paths following its compatibility initialization. References: https://akuhak.github.io/ps2sdk-doxy/iop/spu2regs_8h_source.html and https://github.com/PCSX2/pcsx2/blob/master/pcsx2/SPU2/spu2sys.cpp (register semantics, not copied implementation). Voice generation, ADSR and IRQ progression do not depend on the routing masks. Accumulate signed post-volume contributions separately for dry and wet; clamp both for existing guest mixer-ring writeback. The existing dry mix feeds PCM; wet rings are pre-effect voice taps, not reverb returns. MMIX input/external gating, effects work-area handling, reverb processing and EVOL return remain separate batches. Tests exercise reset masks, independent stereo dry/wet routing on both cores, upper voice bits and the existing mixer-ring fixture. No EE/VU1 changes. Retail replay required because guest-visible wet ring contents and dry routing now differ.

## Batch 15: MMIX and core cascade

Batch 14 reached training gameplay (Goku/Vegeta), 40M dispatches, zero IOP faults, 3,184,130 PCM frames, 9 clipped samples, 764.820 ms mixer time. Four live expansions took 58.301 s and added five modules. The run does not establish hardware fidelity or matched performance.

Implement the MMIX source gates (0x198 per core), with separate dry/wet bits for voice, AutoDMA input and external input, right/left bit order and no external input on core 0. Compatibility reset masks are 0xff0/0xffc. The voice rings retain their pre-MMIX values. Apply B-volume to AutoDMA as before; route core 0 post-master output through core 1 AVOL, then mix core 1 and apply its master gain. Clamp voice sums and pre-master mix to signed 16-bit range. The existing clipped_samples diagnostic now counts core 1 pre-master saturation rather than saturation of the old sum of two post-master outputs; comparisons across this boundary are not equivalent. Count nonzero frames of the routed effect input per core. Do not yet add the wet signal to the dry output: reverb return is unimplemented. Core mute, SPDIF/CDDA special paths, master sweeps, exact timing and effect work RAM/filter remain pending. Reference: PCSX2 SPU2 Mixer.cpp and spu2sys.cpp for register semantics and signal order; no implementation copied. Regression checks include default masks, gate independence, AVOL, external tap, core 1 input, voice taps, and revised PCM expectations. No EE lowering/VU1 changes; retail replay needed.

## Batch 16: Effect register and work-area foundation

Batch 15 completed 40M dispatches, 3707 fields and training gameplay without cache expansion or guest faults. PCM frames 2,966,958, mixer 796.872 ms, effect input nonzero 0 on both cores. A zero routed input is not proof that effects were never enabled. Both captures match and the 16,208-module translation manifest is unchanged.

Implement the effect coefficient banks at 0x774..0x786 and 0x79c..0x7ae, leaving current master-volume readback as previously modelled. Preserve high-halfword-first address parameter latches. ESA (0x2e0/0x6e0) and EEA (0x33c/0x73c) use 22-bit logical halfword addresses; EEA low half reads 0xffff and ignores writes. Compatibility reset areas follow PCSX2. Add a bounded effect_address helper wrapping positive/negative offsets and large guest step positions in the inclusive logical work area, then masking to physical 2 MiB sound RAM. Invalid/nonpositive-length areas fail without access. The helper uses mathematical signed modulo; hardware boundary quirks need differential validation. Report ATTR bit 7 and enable-write counts plus area endpoints per core. No actual reverb RAM reads/writes, filter or effect IRQ scheduling yet; address/coefficients tests exercise both cores, reset, high page aliasing, wrap and invalid areas. References: PCSX2 SPU2 regs.h, RegTable.cpp, Reverb.cpp, spu2sys.cpp. No EE/VU1 changes.

### Effect RAM transactions (Batch 17)

Effect tap reads resolve the wrapped work-area address to shared sound RAM and
interpret a signed halfword. Enabled writes saturate to signed 16-bit before
storing. The current compatibility policy permits disabled reads but suppresses
writeback and effect IRQ comparisons while ATTR bit 7 is clear. This follows the
observed distinction in PCSX2's reverb model; disabled-read IRQ behavior remains
a hardware-validation item. Each enabled actual access compares both cores'
physical IRQA through the existing playback latch path, with a separate effect
IRQ edge counter. Repeated hits do not create new edges until acknowledged.

Synthetic coverage seeds RAM through DMA, checks logical aliasing, positive and
negative wrapping, saturated samples, disabled effects, both IRQ latches,
re-arming one core, invalid addresses and reset. These helpers are not called
by the mixer yet: no dummy tap reads or writes, effect deadlines or synthetic
IRQ events are introduced by this batch. Guest-clock activation, silent-tail
handling and scheduler deadlines must be added with real filter processing.

### IIR reflections (Batch 18)

The IIR stage accepts one channel's already-downsampled signed input and a
half-rate work-area step. Native SPU2 tap registers are assembled high halfword
first, interpreted as signed 32-bit halfword offsets; hardware behavior of
extreme register values remains to be verified. The IIR uses signed Q15 input,
wall and alpha gains with explicit floor rounding through the existing wide
multiply helper. The previous destination is one word earlier, and the DIFF
source is selected from the opposite channel. Both reflection values are staged
before writeback, so future comb and APF reads can observe the old RAM contents.
Disabled effects calculate but do not write back or latch effect IRQs.

The synthetic fixture seeds known RAM via DMA and verifies exact signed left and
right values, cross-channel selection, wraparound, negative coefficient and tap,
read-before-write ordering, disabled writeback and cross-core read IRQ. This is
not yet a bit-exact hardware claim. Filter scheduling remains inactive until the
complete filter pipeline and sample-clock integration are ready.

Register reference: https://akuhak.github.io/ps2sdk-doxy/iop/spu2regs_8h_source.html
Algorithm reference: https://psx-spx.consoledev.net/soundprocessingunitspu/

### Four-tap comb accumulator (Batch 19)

Each channel reads four independent wrapped RAM taps selected by the native
SPU2 address register pairs (left 0x2f4, 0x2fc, 0x314, 0x31c; right +4),
weighted by that core's four signed Q15 COMB coefficients. Each product is
floor-rounded independently and accumulated in signed 32-bit precision,
without premature saturation. All four reads occur even for zero gain; IRQ
comparisons use the existing enabled-effect policy. The comb stage does not
write RAM and must run before the staged reflection/APF writebacks.

The DMA-seeded fixture checks both channels, independent coefficient banks,
negative fractional rounding, wide accumulation (131072), signed offset and
step wrap, disabled effects, cross-core IRQ on a zero-gain tap, invalid
requests and unchanged RAM. Filter clock integration is still deferred.

References: PS2SDK libspu2 register mapping
(https://ps2dev.github.io/ps2sdk/s__sra_8c_source.html) and the PCSX2 SPU2
reverb model (https://github.com/PCSX2/pcsx2/blob/master/pcsx2/SPU2/Reverb.cpp).
The rounding choice follows that compatibility model, not a new bit-exact
hardware validation.
