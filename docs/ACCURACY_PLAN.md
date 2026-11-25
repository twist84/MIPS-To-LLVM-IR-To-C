# Hardware accuracy follow-up (after PR #156)

**Starting point:** [current verified baseline](README.md). No additional runtime changes are part of the repository cleanup PR. This is the proposed order for a separate accuracy PR or series of PRs; completion is evidence-based, not inferred from implementation.

| Priority | Work | Acceptance evidence |
| --- | --- | --- |
| 1 | [VU EFU arithmetic (#158)](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/issues/158): original transcript target achieved in PR #170; independent vectors and retail regression pending | 55/55 exact transcripts and 208/208 direct words verified; independent vectors and no fitted input-specific results |
| 2 | [R5900 issue model (#159)](https://github.com/twist84/MIPS-To-LLVM-IR-To-C/issues/159): documented pairing/resource stalls, dependencies, forwarding and completion/retirement | Measured microbenchmarks and independent cycle traces; keep manual-cache-hit profile distinct from hardware-validated timing |
| 3 | COP0 performance accounting: cycle events, instruction/branch events and overflow/exception semantics | Counter deltas and exception state validated against hardware; no invented timing or counters |
| 4 | EE memory and TLB/cache behavior | Measured access/cache/translation behavior, exception paths and ordering |
| 5 | Precise exceptions and interrupts, including nested delay-slot EPC/BD and retirement | Original and directed exception vectors with exact state and resume behavior |
| 6 | VU0/VU1 timing, asynchronous launches and flags | Independent interpreter/console state and timing comparisons; native AOT parity |
| 7 | DMA/VIF/GIF bus contention, partial-payload progress and arbitration | Real FIFO, DMA progress, interrupts and ordering under stalls |
| 8 | GS rasterisation and framebuffer accuracy | Independent CPU/GPU/console comparisons, not two reads of the same GPU VRAM |
| 9 | IOP/SPU2 timing, audio and DMA | Measured scheduling, IRQ, sample/mixer and reverb behavior |
| 10 | HLE services and device coverage | Original guest-observable results; keep unknown services explicit |

Retain the pinned unmodified 55-test suite and all CTest/CI regressions. Keep timing source labels explicit. No test-specific output corrections, fake interrupts, fabricated completion or arbitrary syscall costs. Prefer Sony manuals and console observations for behavior, the SDBZ fork of ps2-web-recomp for runtime architecture, and pinned PCSX2 for functional differential checks. Run matched retail replays when a change affects a live retail path; otherwise continue with independent regression coverage. Preserve warmed cache provenance and report actual limitations.
