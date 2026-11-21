# PR #148 — matched USA retail graphics replay (21 November 2026)

Source archive: **sdbz-graphics-pr148.zip**, containing boot.log,
expansion.log, first-boot.json, translation-cache.json,
iop-disc-catalog.json, and all three CPU/GPU/upload PPMs.

Guest ELF: Super Dragon Ball Z USA SLUS_214.42, SHA-256
1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16.

## Authoritative final replay

Use the final stopped first-boot.json. Do not sum replay counters.

| Metric | Matched final result |
|---|---|
| EE dispatches | 3,656,670 |
| Guest instruction attempts | 94,621,123 |
| Final EE PC | 0x00175210 (host observation site) |
| EE report exit | UNSUPPORTED (4) |
| GS fields started | 18 |
| Committed GIF/GS batches and tags | 47 / 51 |
| VU1 XGKICK / GIF PATH1 | 0 committed batches |
| VIF1 DIRECT / GIF PATH2 | 7 batches, 11 tags, 133 input QWC |
| GIF DMA / PATH3 | 40 batches, 40 tags, 262,312 input QWC |
| PATH3 image payload | 16 events, 262,144 image QWC |
| Committed sprites | 6, all black RGB, all untextured |
| Native GPU draw count | 6 |
| Supported scanout / successful WebGPU presents | 16 / 16 |
| Host-poll skipped GS fields | 2 |
| CPU display | 512x448, zero nonblack pixels |
| Native WebGPU display | 512x448, zero nonblack pixels |
| CPU/GPU display same source and equal digest | true / true |
| Upload display | 1024x64, zero nonblack pixels |
| Runtime IOP module loads / RPC commits | 7 / 46 |

All three PPM images were inspected: all are entirely black.
Six valid GPU draws and sixteen successful native present calls
are real, but show only the *current black game output*, not the
first visible rendered frame.

The new execution advanced beyond the earlier unsupported VU0
VRSQRT at 0x00106988 and VMULq at 0x00106990. That proves
code-reach progression, not bit-exact vector arithmetic.

## True first blocker: guest IOP R3000 thread 81

The final boot.log recorded, before the EE unsupported stop:

    IOP clock guest execution stopped: turn=3470595
      thread=81 reason=3 pc=0x001cdf78 opcode=0x6e736168

IOP e_kernel_stop reason 3 is unsupported_instruction.
Little-endian word 0x6e736168 contains ASCII-like bytes "hasn".
It is not evidence of a missing legitimate R3000 instruction.
A corrupted guest function pointer, invalid return, callback entry,
or overwritten RAM are possibilities, but the supplied archive has
no last-eight-PC/thread-origin/stack window proving any one cause.

**Do not misattribute this to the EE INTC load at 0x00175210.**
The host sets EE UNSUPPORTED when the *IOP* stops; the EE's next
LW would have read already implemented INTC_STAT (0x1000f000).
No fake interrupt, no skipped opcode, and no synthetic device
completion is justified.

ARKD RPC server 0x503 method 257, real guest semaphores, and IOP
thread scheduling progressed before the failed R3000 instruction.

## Expansion and cache provenance

Ten outer expansion pass footers appear in expansion.log.
Passes 1–9 each report new_functions=128; pass 10 reports
new_functions=14, replay=8 and final stop 0x00175210.
A per-pass footer is not a final cache inventory delta.
Final translation-cache.json includes **1,167 IR hashes**.
Never sum final boot metrics across the expansion replays.

## Implemented follow-up batch

In the same draft PR #148, new *host-only* stopped-thread diagnostics
are being added without guest-visible changes:

- Bounded recent fetched R3000 PC/opcode ring per saved IOP thread,
  including the actual unsupported word, preserving context switches.
- Host-only thread creation origin, source/entry/module/IRQ/alarm
  classification, saved GPR/RA/SP, stack allocation and syscall history.
- Read-only RAM code windows around the invalid PC, thread entry, RA,
  and SP, with no diagnostic MMIO access.
- A one-time IOP stop report and an m2_iop_guest_fault JSON member
  for matched replay attribution.
- A focused regression executing exactly 0x6e736168, preserving its
  fail-closed unsupported outcome and verifying snapshot immutability.

The next matched replay should distinguish a bad initial thread entry
from an actual jump/return into data, enabling a causal kernel/IRX fix.
No first rendered game frame has been claimed.

The PR must stay draft and unmerged. Eventual merge is squash-only
after the user explicitly authorizes it.

## Follow-up matched archive: callback registration source

The user's second full graphics archive, `sdbz-graphics-pr148(1).zip`,
repeated the same final stopped replay: EE PC `0x00175210`,
3,656,670 dispatches, 94,621,123 guest instruction attempts,
18 GS fields, 16 successful native presents and six untextured
black sprite draws. All three captures remain black.

The previously implemented `m2_iop_guest_fault` report identifies
**thread 81 as a newly scheduled hosted callback**, not a routine
that branched into text after executing legitimate code.
Original entry `0x001cdf70`, module ownership `7`, module
load `PADMAN.IRX`, GP `0x001d5ff0`, SP `0x001d6cf0`,
callback-return RA `0x00013fe0`. Its entire fetched
history is:

```text
0x001CDF70  0x20746579  "yet "
0x001CDF74  0x20666973  "sif "
0x001CDF78  0x6E736168  "hasn"  <-- unsupported R3000
```

This string is in the loaded PADMAN allocation, but is not code.
No corrupt return is needed to explain the direct entry. The
registration producer is not yet identified in the supplied archive.

The PADMAN static import table includes VBLANK ordinals 8 and 9,
making a bad VBLANK registration plausible, but source attribution
requires registration evidence. The existing VBLANK handler uses
`a2` as the callback pointer, which matches both PS2SDK and the
primary `ps2-web-recomp/SDBZ` reference. Do not swap arguments
without a guest-side explanation.

### New coherent host-only provenance batch

- Preserve the original callback registration source: VBLANK start,
  VBLANK end, or CDVDMAN read; registration thread, syscall PC,
  RA/GP and original A0–A3.
- Preserve the last eight fetched R3000 instructions **from the
  registering thread** in a bounded ring, copied into the deferred
  callback thread without affecting its guest registers or execution.
- Record bounded registration events in the boot log, and include the
  carried callback registration origin in `m2_iop_guest_fault`
  when a guest callback actually fails.
- Add a regression for deferred callback provenance alongside the
  existing exact-unsupported-opcode failure test.

The IOP guest still executes its provided callback pointer.
A bad pointer still stops fail-closed. This batch does not change
IRX relocations, import ordinals, guest scheduling, DMA, VBLANK,
register values, or fabricate work. The next matched replay can
identify the original registration call, after which an evidence-backed
fix can be selected.

## Third matched archive: definitive stub collision and fix

The third uploaded full replay, `sdbz-graphics-pr148(2).zip`, reproduces
the **same authoritative stopped counters** (3,656,670 EE dispatches,
94,621,123 reached attempts, 18 GS fields, 16 successful native
presents, six black untextured sprite draws, and black CPU/GPU/upload
captures). The new callback registration evidence is decisive:

- `m2_iop_guest_fault.callback_registration.source = 3`:
  `cdvdman` `sceCdCallback`, registered by thread 54 at the guest
  service syscall PC `0x000187a8`, RA `0x001cd888`, A0
  `0x001cdf70` (ASCII `"yet sif hasn't..."`).
- The boot log has an **earlier** registration by PADMAN thread 53,
  using the **same** string address and the same HLE import stub,
  with RA `0x001cd7c4`. No guest fault occurred until the host
  subsequently completed a genuine disc read and dispatched this
  mistakenly registered address.
- The static `iop-disc-catalog.json` lists PADMAN's actual imports:
  **`sifman` #5 is present; `cdvdman` is absent**.
  The guest's real call went through a resolved SIFMAN import stub.
- Inspecting the HLE sources shows an exact memory collision:
  `sifman` #5 = `0x18700 + 5 * 0x20 = 0x187a0`, while
  old `cdvdman` #37 = `0x18300 + 37 * 0x20 = 0x187a0`.
  Similarly `sifman` #6 and `cdvdman` #38 both used `0x187c0`.
  CDVDMAN installation overwrote the actual R3000 `sceSifInit`
  service stub with a `sceCdCallback` syscall stub. As a result,
  PADMAN's legitimate SIF initialization was routed to CDVDMAN,
  with its ordinary diagnostic-string pointer in A0 being mistaken
  for a callable guest address.

**Causal fix:** relocate every CDVDMAN ordinal except the explicitly
fixed `sceCdSearchFile` #10 to the disjoint low IOP reserved range
`0x19000 + ordinal * 0x20`. The highest currently exported
CDVDMAN stub (#75) ends below `0x19980`, outside the existing
SIFMAN `0x18700` range, SIFCMD `0x18b00` range and the
`0x18f80` RPC-return trampoline; the guest module allocator
starts at `0x20000`. The old `sceCdSearchFile` #10
address remains `0x18200`.

**Regression:** compose KERNEL HLE + SIFMAN + SIFCMD + CDVDMAN in
both orders, check uniqueness and actual five-word R3000 service
selectors for all currently exported stubs, and execute a synthetic
non-copyrighted IRX whose original imported `sifman` #5 function
must initialize real IOP SIF DMA register state rather than invoke
CDVDMAN. This covers both the original `0x187a0` bug and the
previously latent `0x187c0` collision.

This fix is neither an exception bypass nor a fabricated interrupt,
RPC reply, DMA event, or graphical frame. It restores existing guest
SIFMAN service dispatch to the previously linked correct function.
A fresh **post-fix** retail replay is required before claiming
progress beyond thread 81 or a visible image.

## Fourth matched archive: after SIFMAN/CDVDMAN stub fix

Uploaded `sdbz-graphics-pr148(3).zip` is the first post-fix Windows
full graphics replay. Its `first-boot.json`, `expansion.log`,
`boot.log`, translation cache, catalogue and all three PPMs match.

| Metric | Previous IOP-stopped replay | Post-stub-fix replay |
|---|---:|---:|
| EE dispatches | 3,656,670 | 3,981,062 |
| Guest-instruction attempts | 94,621,123 | 1,847,424,953 |
| Guest GS fields | 18 | 375 |
| Native WebGPU successful presents | 16 | 268 |
| GPU committed GS draws | 6 | 255 |
| Committed GIF tags | 51 | 1,040 |
| VIF1 DIRECT / PATH2 batches | 7 | 256 |
| GIF DMA / PATH3 batches | 40 | 289 |
| VU1 XGKICK / PATH1 batches | 0 | 0 |
| IOP RPC commits | 46 | 292 |
| Cached native guest IR modules | 1,167 | 1,285 |
| Guest IOP failure | Thread 81 invalid text opcode | **None** |

The final `m2_iop_guest_fault` is `null`, and the boot log contains
no new guest unsupported IOP instruction, load/relocation failure or
fatal callback fault. The PADMAN misroute is resolved: the new run
enters genuine SIFMAN and survives to hundreds of IOP RPC and GS
field events.

**Visible image remains unverified.** CPU and WebGPU 512x448 captures
match byte-for-byte and have zero nonblack RGB pixels; the 1024x64
upload capture is also black. The GS committed 255 sprite pairs, all
untextured with zero RGB. The absence of a visible frame after 375
fields is not itself proof of missing GPU functionality, especially
because PATH1 has not committed any event.

### Exact stop classification

The authoritative final report ended with `HOST_YIELD (6)`, not an
unsupported opcode or crash, at EE PC `0x00175210`. The 4,000,000
configured dispatch limit was not reached: dispatch count was
3,981,062. The native runner had a 1,000,000-guest-instruction
loop-header watchdog but the `--loop-yield-resumes` option was not
forwarded by `run_ps2_game.bat`, so its CLI default was **zero**.
Thus the first translated-loop HOST_YIELD ended execution. It must
not be described as a fresh EE INTC MMIO blocker: the current EE PC
is again adjacent to the modeled `INTC_STAT` read.

`m2_runtime` pinpoints two dominant native dispatch entry PCs:
`0x00178de8` (1,815,980 visits) and `0x001bd58c`
(1,813,916 visits), with 1,813,916 cycles through each
direction of their edge. These two entrypoints account for
approximately 91% of all observed dispatches. The guest is
nonetheless continuing to run real IOP RPC, DMA/GIF and VBlank work:
a hot loop is **not** a demonstrated deadlock.

The one **outer** expansion pass printed `replay=78`,
`new_functions=118` and elapsed=1,221.55s. This is a per-pass
footer, not 78 outer passes. The final cache includes 1,285
`ir_hashes`. Preserve it; no more IR reconstruction is required
for these host-side diagnostic improvements.

### Host-only improvements for next long replay

- Runner supports optional environment
  `SDBZ_DISPATCH_TRACE_EVERY=N` (positive integer). Only the
  *verbose per-dispatch stdout text* is sampled. The first 64,
  every Nth and all non-ordinary or non-successful dispatches
  remain. CTest, all actual guest dispatches, recent JSON history,
  hotspots, hardware state and timing boundaries remain unsampled.
- `run_ps2_game.bat --full` defaults this cadence to **1024**;
  set `SDBZ_DISPATCH_TRACE_EVERY=1` to restore the old full trace.
  The previous 4-million-line `boot.log` was 293 MB uncompressed.
- The batch script now forwards the existing
  `--loop-yield-resumes` option via `LOOP_YIELD_RESUMES`,
  default **16**, preserving a hard bounded watchdog and
  allowing the already-committed guest state to resume at native
  safepoints. Set it to zero for the original immediate stop.
  It creates no synthetic cycle, IRQ, RPC result, DMA completion,
  GS field or frame.

These options are entirely host/launcher-side; do not delete the
existing translation cache or regenerate guest LLVM IR just to
change logging or native-loop resume counts. Use a larger explicit
`MAX_DISPATCHES` on the next run if the goal is to observe activity
beyond the old four-million-dispatch limit.

**Next evidence gate:** a new matched Windows full graphics replay
after these bounded knobs, with the existing IR cache retained.
Do not claim that boot reached a visible screen until a real,
nonblack CPU/GPU capture is verified.

## Fifth matched archive: cumulative native HOST_YIELD cap

The full archive `sdbz-graphics-pr148(4).zip` is the post-change
retail replay with `MAX_DISPATCHES=6000000`,
`LOOP_YIELD_RESUMES=16`, native watchdog 1,000,000,
`SDBZ_DISPATCH_TRACE_EVERY=1024` and cached translation intact.

**The stop is a host watchdog configuration limitation, not a newly
reached missing game opcode.** The final report shows:

| Metric | Previous replay (4) | Fifth replay |
|---|---:|---:|
| EE dispatches | 3,981,062 | 3,667,736 |
| Guest instruction attempts | 1,847,424,953 | 157,999,642 |
| EE GS fields | 375 | 31 |
| Window presents succeeded | 268 | 25 |
| Native GPU GS draws | 255 | 15 |
| Committed IOP RPC events | 292 | 51 |
| Guest IOP fault | null | null |
| EE exit | HOST_YIELD | HOST_YIELD |
| Cached LLVM IR modules | 1,285 | 1,285 |

`first-boot.json` and the final replay `boot.log` agree on
`dispatches=3667736`, `ee_cycles=157999642`,
`scheduler_yields=16`, final EE PC `0x00175210`,
`INTC_STAT=0x4`, `INTC_MASK=0x804` and
`m2_iop_guest_fault=null`. The actual 6-million-dispatch
budget is **not** reached. All three PPM captures remain black;
CPU and native-GPU display captures are pixel-for-pixel identical,
while the upload PPM is also black. PATH1/VU1 XGKICK still has
zero accepted batches; PATH2 committed 16 batches and PATH3
committed 49. The final `translation-cache.json` still contains
1,285 `ir_hashes`; the one outer expansion pass appended no
functions.

The boot log has seventeen actual
`0x00175204 -> 0x00175210 HOST_YIELD` transitions,
of which the first sixteen were resumed. The first two happened
at dispatch 10,426 and 10,514. There was then substantial
genuine EE/IOP/GS activity until the next native loop watchdog
at dispatch 3,639,873, followed by later watchdogs while
fields and GIF register writes continued. The previous runner
counted all sixteen resumes cumulatively across the *entire*
boot, so the seventeenth was rejected unconditionally at
dispatch 3,667,736, regardless of intervening progress.
This explains the new early stop; the smaller field/graphics
counts must not be described as a rendering regression.

### Causal host-only correction

New `ee::c_host_loop_yield_budget` preserves a strict finite
`LOOP_YIELD_RESUMES` allowance of sixteen **while no newly
observed real progress occurs**, but reopens a fresh bounded
window when at least one independently recorded GS field,
committed IOP RPC sequence, or GIF DMA register write
changes between watchdog events. Guest instruction counts,
ordinary dispatches, host wall time, and the host's own
yield counter do *not* qualify as progress. An unchanged
loop still stops after the configured number of resumes,
while setting `LOOP_YIELD_RESUMES=0` stops immediately.

This is observer-only host scheduling: no guest-visible PC,
GPR, timer, INTc state, DMA completion, callback, RPC result,
GS packet or frame is fabricated. CTest
`ee_host_loop_yield_budget` tests both fail-closed stationary
loops and genuine GS, GIF and RPC progress, including
the previous cumulative-cap failure.

`run_ps2_game.bat --full` retains the native watchdog,
the explicit user's 6-million-dispatch budget and
the low-volume verbose text cadence. The guest IR cache,
`run_game.py` cache schema and fingerprints remain
unchanged.

**Next gate:** a matched retail replay at the corrected
branch head to determine whether later boot traffic and
nonblack graphics appear. Neither a visible first frame
nor an EE/IOP timing-parity result is yet established.

## Sixth matched archive: test the memory-card/controller hypothesis

The screenshot provided with `sdbz-graphics-pr148(5).zip` shows
**PCSX2 running SDBZ USA** at the original startup warning:

> No memory card (PS2) inserted in MEMORY CARD slot 1 or MEMORY
> CARD slot 2. A memory card (PS2) with at least 110KB of free
> space is required to create a Super Dragon Ball Z save data.

The screenshot is a **reference presentation**, not an image of
the native LLVM-IR runner. It makes a controller-confirmation or
memory-card-init gate a worthwhile hypothesis, **not yet proof** of
which path the native guest is executing. The native runner does
not have a persistent virtual memory card, and its SIO2
controller starts with a neutral, unpressed virtual pad in port 0.

### Exact evidence in sixth matched replay

All eight members are from the current batch. The authoritative
final `first-boot.json` reaches the full configured 6,000,000
native dispatches, **12,737,447,273 EE guest instruction
attempts**, 2,588 GS fields, 1,826 successful native WebGPU
presents, 1,803 committed GPU draws and 1,804 progress-aware
HOST_YIELD resumes. **Exit `RETURN(0)` at the dispatch budget**
is not a fatal IOP/EE opcode. The final IOP fault object remains
`null`. The existing translation cache retains **1,285
guest IR modules**; the new outer expansion pass did not add any
functions.

GPU/CPU captures of 512x448 and the 1024x64 upload remain all
black. Both display captures match. All 1,803 committed sprite
kicks are untextured with zero nonblack RGBA/RGB provenance,
and VU1 PATH1 has zero accepted XGKICK batches. This is not a
verified rendered memory-card warning or main menu.

Crucially, the actual RPC timeline separates controller, memory
card and Arika file-service work:

- `SIO2MAN.IRX` loads at RPC sequence 13;
  `MCMAN.IRX` at 14; `MCSERV.IRX` at 15;
  `PADMAN.IRX` at 33. Loaded modules are evidence of module
  thread creation, not actual controller or memory-card bus I/O.
- PADMAN RPC client binds `0x80000100` and `0x80000101`
  and issues **four method-1 calls to server `0x80000100`**
  at sequences 36-39. The original PADMAN service runs via
  native guest IOP threads, and no invalid callback recurs.
- The main recurring operation is **1,796 RPC calls to server
  `0x503`, method 2**. This server belongs to the
  ARKD_DVD / Arika guest IOP RPC family `0x500..0x503`,
  as proven by repo's prior HLE parity audit and the server
  registration/guest handler. It is **not** a controller or
  memory-card server. Do not interpret its repeated calls as
  waiting for a user's X press without guest-code evidence.
- No `0x80000400` or `0x80000480` MCSERV RPC appears
  among the final 1,840 committed SIFRPC observations; the
  memory-card driver has not been proved to service a request.
- The current log has SIO2 hardware snapshots only through
  2,000,000 EE dispatches; all seven report **zero CTRL.start
  events, zero successfully processed SIO2 packets, and zero
  completed SIO2 transfers**. There is no final SIO2 counter
  in this archive, so we cannot assert no controller poll
  happened later in the 6-million-dispatch session.

### Read-only diagnostic added

`first-boot.json` now includes optional
`m2_iop_sio2` containing an actual `ctrl_starts`,
`packets`, `completed_transfers`,
`transfer_requested` and `pending_tx_bytes`
snapshot, or `null` when the guest IOP is absent.
This is written at both periodic and final reports, using
the existing IOP hardware's *actual observed transactions*.
It does not create a controller read, supply a fake
button press, simulate a virtual memory card, claim a
successful SIO2 transfer, or change any guest cycles.

**Next acceptance gate:** determine whether SIO2 ever
transfers a real controller packet in a post-change
six-million-dispatch replay. If counters remain zero,
investigate original PADMAN/SIO2MAN guest scheduling,
input task state and why no controller poll is produced;
adding host input cannot influence a guest that never
reads the device. If transfers are present but neutral,
implement a deliberate, testable host gamepad/input mapping
against the genuine SIO2 protocol and check whether an
actual button edge advances the native guest. Separately
compare MCSERV RPC traffic and rendered GS primitives;
do not assume a UI prompt is running when captures
remain black.

Everything is a host-only reporting change. The existing
1,285 cached LLVM modules must be preserved.

## Seventh matched archive: first final SIO2 input-bus evidence

The resent `sdbz-graphics-pr148(7).zip` contains the eight
normal replay artifacts and is byte-for-byte identical to
`sdbz-graphics-pr148(6).zip`. It is the **earlier
post-initial-SIO2-telemetry replay**, not a replay of the
new original-USA-ELF xref analysis.

The stopped `first-boot.json.m2_iop_sio2` confirms:

| Observed original IOP bus event | Actual count / state |
| --- | --- |
| SIO2 CTRL.start writes | **1** |
| SEND3 protocol packets processed | **2** |
| Completed SIO2 transfers | **0** |
| Host SIO2 request still outstanding | **true** |
| Queued unprocessed TX bytes | **0** |

All SIO2 checkpoints through 2,000,000 EE dispatches were
zero. This **final** capture proves later genuine IOP
SIO2 traffic. It does **not** establish that the caller
has already received pad button states or that a
memory-card warning is displayed. The virtual pad starts
connected but neutral; no virtual memory card is present.

No further game progress is established relative to the
sixth replay: exactly 6,000,000 EE dispatches,
12,737,447,273 attempted guest instructions,
2,588 GS fields, 1,826 successful native WebGPU
presents, 1,803 untextured **black** sprite draws,
zero PATH1 XGKICK, no IOP guest fault and all three
PPM captures still black. `translation-cache.json`
is identical, with 1,285 IR modules. The run terminates
at the configured dispatch limit, not at a new IOP fault.

### Source audit: the exact incomplete SIO2 event

`src/iop/iop_hw.cpp` processes SEND3 packets through the
actual IOP TX FIFO or DMA11. The packet handler can
queue replies and advance `m_sio2_index` even before
a subsequent `CTRL.start`. However
`run_sio2_transfer()` returns false on several
distinct hardware conditions, including:

- No actual new packet and no buffered reply.
- Invalid or unbacked DMA11 source, or DMA12
  destination/geometry.
- A required packet or reply still missing.
- Too few DMA12 output words or failed guest RAM write.

On such a rejection it leaves request pending, does
not clear DMA11/12 STR, and does not raise IRQ17.
That is consistent with the observed counters,
but the **specific failing predicate cannot be
recovered** from the old five SIO2 report fields.
Two processed packets plus no pending TX bytes
do not prove that the RX queue was empty.

Compared with primary reference
`twist84/ps2-web-recomp:SDBZ/runtime/src/iop/sio2.cpp`,
the reference `run_transfer()` raises IRQ17 at
`CTRL.start`, including empty-PIO cases, whereas
the current stricter native HLE rejects empty or
incomplete requests. This is a specific parity
difference to examine; do not equate that alone
with permission to fabricate an interrupt or
make a false DMA completion.

### Read-only rejection snapshot added after this replay

- The genuine SIO2 preflight now records
  `last_rejection` and `rejected_starts` for
  exact failed CTRL.start conditions without
  changing guest state.
- The stopped and periodic `m2_iop_sio2`
  objects additionally include the actual
  `queued_rx_bytes`, `packet_index`,
  `ctrl_word`, `stat_word`,
  first four `send3` descriptors,
  DMA11/DMA12 MADR-BCR-CHCR triplets,
  the actual guest IOP PC/RA of CTRL.start,
  packet count and TX/RX queue sizes **at the moment
  CTRL.start was issued**, to establish whether
  the first packets were sent before or after
  the start request, and the first four *real decoded packet*
  port numbers, input/output byte counts,
  leading TX bytes, command IDs and
  connected-pad decisions. These records
  distinguish controller port 0/1 from
  memory-card port 2 without generating
  any new peripheral response.
- The original no-completion/no-IRQ invariant
  remains intact until sufficient authentic
  guest evidence identifies the actual mismatch.
- `iop_hw_test` extends the synthetic PIO/DMA
  fixtures to verify incomplete packets,
  empty-CTRL request, and invalid DMA11 source
  are correctly diagnosed, are fail-closed,
  and successful transactions reset the
  last rejection.

The next completed replay will distinguish whether
the original guest exhausted the PIO RX FIFO
before CTRL.start or attempted a genuinely
incomplete DMA11/12 transfer. Then make a
targeted parity correction with an exact guest
transaction regression if warranted.

Only host IOP hardware diagnostics, runner JSON,
smoke tests and this document are changed. No
original executable, guest LLVM IR, function
discovery metadata, translation cache or guest
input is modified.

## Eighth matched archive: genuine controller polling, missing empty-START IRQ17

The matched archive `sdbz-graphics-pr148(8).zip` is a
replay **with the additional SIO2 rejection and packet-origin
diagnostics**, not a replay of this latest corrective change.
Compared with `(7).zip`, the ELF/translation cache,
IOP disc catalogue and all three black PPMs are byte-identical.
The deterministic guest execution counters are unchanged:
6,000,000 EE dispatches, 12,737,447,297 instruction attempts,
2,588 GS fields, 1,826 successful GPU presents, 1,803 black
sprite draws, no IOP guest fault, and 1,285 cached IR modules.

### Exact original IOP hardware evidence

The final `first-boot.json.m2_iop_sio2` reports:

| Field | Recorded value |
| --- | --- |
| CTRL.start writes | 1 |
| Decoded PIO packets | 2 |
| Transfer completions | 0 |
| Last rejection | `no_processed_packet` (code 7) |
| Rejected start count | 1 |
| CTRL value | `0x000003BD` |
| Guest IOP CTRL.start PC / RA | `0x00020008` / `0x00020314` |
| Packets at CTRL.start | **2** |
| TX / RX queue bytes at CTRL.start | **0 / 0** |
| DMA11 registers (MADR/BCR/CHCR) | **0/0/0** |
| DMA12 registers (MADR/BCR/CHCR) | **0/0/0** |
| Pending transfer at report | true |
| Packet index after start | 0 |

The *actual* first two packets target **controller**
ports, not the memory-card port:

1. Port 0, `SEND3=0x00140500`, in/out=5/5,
   TX first byte `0x01`, command `0x42`
   (controller poll), `pad_present=true`.
2. Port 1, `SEND3=0x00140501`, in/out=5/5,
   TX `0x01 0x42 ...`, `pad_present=false`.

The first controller is a neutral connected emulated pad;
there is no injected button press. The second controller
is absent. These observations **do not** establish that
the native game is displaying the original memory-card
warning seen on PCSX2.

### Root cause, confirmed against primary reference

The guest's exact `CTRL=0x3BD` has *both* FIFO
reset bits (`0x0C`) **and** START (`0x01`) set.
In the existing native `c_hw::write(k_sio2_ctrl)` it
therefore **clears the already processed PIO replies and
packet index first**, then calls `run_sio2_transfer()`.
With no DMA11/12 armed and no queued PIO bytes remaining,
the native code rejects with `no_processed_packet`
and never raises **IRQ17**. This explains *all* SIO2
diagnostic fields without assuming an unsent packet or
missing controller input.

The primary implementation
`twist84/ps2-web-recomp:SDBZ/runtime/src/iop/sio2.cpp`
also clears FIFO/index when bits `0x0C` are set, but
on START always calls `run_transfer()`. That routine
raises IRQ17 after servicing any armed DMA channels,
**including an empty/control-only PIO start**.

### Evidence-backed correction

The native `run_sio2_transfer()` now treats an actual
START as completed when **neither DMA11 nor DMA12 is
armed and there are no incomplete PIO TX bytes**, even
when there are no newly queued RX bytes. It preserves
all existing unsupported/incomplete packet rejection
and DMA buffer preflight behaviour:

- No artificial SIO2 response bytes or pad key presses.
- No invented DMA11/12 completion; unarmed DMA remains
  unarmed and only genuine DMA still clears STR.
- Genuine SIO2 START signals IRQ17 and records one
  control transfer completion, as in the primary
  reference, even if the guest already read or reset
  PIO reply queues.
- PIO TX with insufficient bytes remains fail-closed.
  Invalid DMA11/12 geometry or RAM stays fail-closed.

The `iop_hw_test` smoke suite now models the **exact
retail port0+port1 0x42 poll followed by CTRL=0x3BD**
and asserts a real IRQ17 without fabricated device
data or DMA completion. Earlier tests for partial PIO
and invalid DMA source remain active. An empty
ordinary CTRL.start also asserts IRQ17 to match the
primary reference.

This is an IOP host/HLE hardware change only, not an
EE LLVM IR source change or translated object change.
Preserve the existing **1,285-module translation cache**.
A **new post-fix retail replay** is required before
claiming improved PADMAN behaviour, new game graphics
or a visible memory-card warning.

## Ninth matched archive: SIO2 IRQ17 reaches missing interrupt-status register

The uploaded `sdbz-graphics-pr148(9).zip` contains the normal
eight matched replay members. It is the first post-`CTRL=0x3BD`
empty-PIO completion fix retail run; it **predates the new
SIO2 interrupt-status MMIO fix** below.

### Exact verified changes versus the eighth replay

The SIO2 fix is effective:

| Actual SIO2 observation | Previous (8) | Ninth (9) |
| --- | ---: | ---: |
| CTRL.start writes | 1 | 1 |
| Decoded controller polls | 2 | 2 |
| Successful SIO2 completions | **0** | **1** |
| Rejected starts | 1 | 0 |
| Transfer still requested | true | false |

Both real PIO packets still target controller ports 0 and 1,
using command `0x42`; port 0's pad is connected and neutral,
port 1 disconnected. The combined `CTRL=0x3BD` start at
original IOP PC `0x00020008` now completes and raises the
original IOP **IRQ17**, without inventing a controller button
or DMA operation.

The ninth full attempt stops earlier than the 6m budget:
**3,657,851 EE dispatches**, 101,646,685 guest
instruction attempts, **20 GS fields**, 17 successful
WebGPU presents, seven GPU draws, and no nonblack pixels.
All 512x448 CPU/GPU display captures and the 1024x64
upload capture are black. The existing translation cache
still records 1,285 guest LLVM modules and the expansion
pass appended **zero** new functions.

The final stopped report calls the EE exit `UNSUPPORTED`,
but this is a **propagated IOP guest MMIO fault**,
not a new R5900 opcode. The provenance object proves:

| Fault field | Original guest event |
| --- | --- |
| IOP thread | 90 |
| Original IRX | **SIO2MAN.IRX** (loaded module 1) |
| Callback | genuine SIO2 **IRQ17** |
| Callback entry | `0x00020584` |
| Faulting PC | `0x00020180` |
| Faulting word | `0x8C420000` = `LW v0,0(v0)` |
| Read address | **`0xBF808280`**, physical `0x1F808280` |
| Read width | 4 bytes |
| Return address | `0x00020598` |
| Guest instructions executed in handler | 7 |

The original callback calls a three-word address-load/read
helper at `0x00020178..0x00020180` and then returns to
an adjacent helper call at `0x00020598` which writes the
same value back. This exactly matches the authentic
SIO2MAN interrupt-status acknowledgement pattern.

### Register identity and primary-reference cross-check

The original PS2SDK SIO2 register definitions
(`ps2dev/ps2sdk/iop/sio/mx4sio_bd/src/sio2regs.h`)
name `0x1F808280` `SIO2_A_INTR_STAT`. Its documented
bits are:

- bit 0: transfer complete
- bit 1: transfer error
- **write-one-to-clear** for the interrupt status

The actual `ps2dev/ps2sdk/iop/sio/sio2man/src/sio2man.c`
interrupt handler does
`sio2_stat_set(sio2_stat_get())`, followed by
`iSignalSema`, and returns 1. PCSX2 also labels
this hardware word `SIO2_INTR` and treats it
as a distinct device register from the IOP INTC.

These sources supplement the project's **primary**
`twist84/ps2-web-recomp:SDBZ` reference. The
reference's `sio2.cpp` models genuine PIO/DMA
transfers and raises IRQ17 on completion; the
additional register is the next original IRX access
newly exposed by our previous fix.

Source reference links:
- https://ps2dev.github.io/ps2sdk/sio2regs_8h_source.html
- https://github.com/ps2dev/ps2sdk/blob/master/iop/sio/sio2man/src/sio2man.c
- https://github.com/twist84/ps2-web-recomp/blob/SDBZ/runtime/src/iop/sio2.cpp

### Minimal host IOP correction and regression

`iop::c_hw` now implements **only the proven**
32-bit physical MMIO word `k_sio2_intr=0x1F808280`:

- Device interrupt status resets to zero on IOP reset.
- A **genuine completed SIO2 transfer** latches
  bit 0 **before** raising the existing IRQ17.
- Original guest KSEG1 `LW` at `0xBF808280`
  reads the device status.
- Original guest `SW` write-one-to-clear
  acknowledges bits 0/1; writing bit 1 does
  not clear an existing transfer-complete bit.
- Clearing the device status does **not**
  acknowledge the independent IOP `I_STAT`
  interrupt-controller bit. INTRMAN still owns
  that acknowledgement.
- The stopped and periodic `m2_iop_sio2`
  report adds `intr_word` to observe whether
  original SIO2MAN completed its acknowledgement.

`iop_hw_test` now verifies the KSEG1 read/write
through the actual R3000 memory interface,
a real PIO transfer, the already fixed
`CTRL=0x3BD` two-pad case, write-one-to-clear,
interrupt-controller independence, failed-transfer
non-completion, and reset behaviour.

The implementation does **not** add a memory card,
press a pad button, change the DMA completion
rules, or fabricate additional IRQ17 events.
No guest ELF, LLVM IR generation inputs,
cached translated objects or cache fingerprint
were changed. Preserve the **1,285** existing
guest translation modules.

The next evidence gate is a **post-fix retail replay**:
verify that the SIO2MAN IRQ17 callback executes the
`0xBF808280` read/acknowledgement successfully,
then assess original pad/memory-card traffic,
IOP RPC completions and native GS rendering.
No original visible frame is yet established.

## Tenth matched replay: SIO2MAN IRQ17 acknowledged; RX FIFO empty read fault

Uploaded `sdbz-graphics-pr148(10).zip` contains eight matched
artifacts, written around 15:56 UTC 2026-11-21. It is the first
retail replay **after** the SIO2 interrupt-status register
`0x1F808280` was implemented, and it **predates** the empty
RX FIFO correction below.

### Verified hardware progress

| Observation | Ninth replay | Tenth replay |
| --- | --- | --- |
| SIO2 CTRL.start writes | 1 | 1 |
| Original controller PIO packets | 2 | 2 |
| SIO2 completed transfers | 1 | 1 |
| Transfer rejected | 0 | 0 |
| SIO2 interrupt status at final report | unsupported read | **0** |
| IOP faulting guest PC | `0x00020180` | **`0x00020158`** |
| Faulting guest IOP thread | 90 IRQ17 callback | **2 SIO2MAN worker** |
| Failing MMIO | `0xBF808280` status LW | **`0xBF808264` RX FIFO LBU** |

The original IRQ17 handler now executes the previously unsupported
`0xBF808280` read and write-one-to-clear acknowledgement.
`first-boot.json.m2_iop_sio2.intr_word=0` confirms its device
status is clear at the final checkpoint. The next fault occurs
in the original SIO2MAN worker thread 2, not in an interrupt
callback:

- PC `0x00020154`: `LUI v0,0xBF81`
- PC `0x00020158`: opcode `0x90428264` =
  `LBU v0,0x8264(v0)`, targeting physical
  **SIO2 RX FIFO `0x1F808264`** through KSEG1
  **`0xBF808264`**, width one byte.
- Caller at `0x0002035C` invokes the RX-byte helper
  (return `0x00020364`); SIO2MAN worker entry
  `0x0002039C`, original module 1.
- `m2_iop_sio2.queued_rx_bytes=0`,
  `pending_tx_bytes=0`, two genuine controller
  `0x42` polls already processed (connected port 0,
  disconnected port 1), and DMA11/12 all inactive.

The original `CTRL=0x3BD` both reset the PIO
queues and started the completed transfer. The
worker subsequently attempts an ordinary **empty RX
FIFO read**, which the native implementation had
classified as an unsupported MMIO operation. This
is **not** an R5900 unsupported opcode, input-button
wait, invalid SIF RPC response, or new IRQ17
delivery failure.

The sixth-to-tenth replay's visual baseline remains
unproven. This attempt stops because of the IOP
fault at **3,657,851 EE dispatches**,
**101,646,685 attempted guest instructions**,
20 genuine GS fields, 17 successful native WebGPU
presents and seven black GPU draws. CPU/GPU
display captures match and are black; the upload
capture is black. The matched translation cache
still contains **1,285** original game IR modules;
the expansion pass appended **zero** functions.

### Primary-reference implementation: register-specific 0xFF

The project's primary reference, original
`twist84/ps2-web-recomp:SDBZ/runtime/src/iop/sio2.cpp`,
implements `read(0x1F808264)` as follows:

- When a reply byte is queued, return and consume
  that **actual device** byte.
- When the RX deque is empty, return
  **`0xFF`**, *not* a MMIO fault, without
  allocating a pad response, starting DMA or
  raising an interrupt.

The native `iop::c_hw::read()` previously
returned `false` in the second case. Its exact
register-specific correction now returns `0xFF`
for supported 8- and 32-bit reads, matching
the reference. A diagnostic
`m2_iop_sio2.empty_rx_reads` counts genuine
empty RX reads and resets with IOP reset. It is
observation only.

### Test and cache acceptance

The `iop_hw` smoke test now executes an original-style
guest KSEG1 `read8(0xBF808264)` against an empty
FIFO and asserts `0xFF` with no fabricated
controller packet, transfer, DMA completion or IRQ17.
The existing tests retain correct FIFO consumption
for a genuine neutral controller reply. Further tests
verify empty byte and word reads return `0xFF`
even with a nonempty TX queue, and that an emptied
reply FIFO subsequently reads `0xFF`.
The original IRQ17 W1C and dual-pad `0x3BD`
regressions remain in place.

The fix only touches host IOP hardware, host
diagnostics, tests and this document. No game ELF,
guest LLVM IR source, translated object, cache
fingerprint or function map changed. The 1,285
cached guest IR modules must be **reused**.

**Next gate:** a new post-fix retail replay confirming
original SIO2MAN worker continues past
`0x00020158`, no fresh original IOP MMIO fault
appears, and actual PADMAN/IOP RPC/GS behaviour
is recorded. Do not claim the memory-card warning
or visible scene is reproduced while captures remain
black.

## Eleventh matched archive: SIO2MAN runs without a guest fault, black frame persists

The exact attachment `sdbz-graphics-pr148(20261121-161638).zip`,
SHA-256
`bb06cee793d122eceba9498d87462739f93a16e71c202998908ed693adffbb61`,
contains the eight expected matched members, including a
fresh 16:16 UTC stopped `first-boot.json`.
This report was produced **before** the optional
controller-input probe added below, and must be evaluated
as the normal neutral-controller baseline.

### Original guest IOP hardware progress versus the tenth archive

| Verified measurement | Tenth replay | 16:16 replay |
| --- | ---: | ---: |
| EE native dispatch boundaries | 3,657,851 | **6,000,000** |
| Original guest instruction attempts | 101,646,685 | **12,779,550,806** |
| SIO2 CTRL.start writes | 1 | **3,613** |
| SIO2 decoded original packets | 2 | **3,614** |
| SIO2 completed transfers | 1 | **3,613** |
| SIO2 empty RX reads handled | worker MMIO fault | **18,070** |
| SIO2 rejected starts | 0 | **0** |
| SIO2 completion status at final report | 0 | **0** |
| IOP guest fault | `0x00020158` | **null** |
| Original guest GS fields | 20 | **2,597** |
| Successful WebGPU window presents | 17 | **1,830** |
| Native GPU draws | 7 | **1,809** |

The `0xBF808264` empty-FIFO instruction is now
supported, and original SIO2MAN continues for
thousands of *real* port-0/port-1 poll exchanges.
The final IOP controller request is not armed,
`pending_tx_bytes=0`, `queued_rx_bytes=0`,
`last_rejection=none`, `intr_word=0`.
The full replay ends at the configured 6m
**dispatch budget** (last EE PC `0x002F401C`),
not a new IOP or R5900 instruction fault.

The existing game translation cache is
**byte-for-byte identical** to the tenth archive:
1,285 cached original guest LLVM modules.
The expansion log says `new_functions=0` in
the first pass, so this is not guest-translation
cache invalidation.

### No visible game frame: GS provenance is now decisive

The guest submitted 3,653 accepted GIF
batches, including 1,810 VIF1 DIRECT PATH2
batches and 1,843 GIF DMA PATH3 batches;
VU1 PATH1 has **zero accepted XGKICK batches**.
The original GS parsed **16 IMAGE transfers**
totalling **262,144 QWC**.
There are 1,809 sprite-pair kicks and 1,809
native GPU committed draws, but:

- `sprite_textured_kicks_total=0`
- `sprite_nonblack_rgb_kicks_total=0`
- The bounded first and last samples record
  untextured `PRIM=0x6` sprites and
  **`RGBAQ=0x3F80000000000000`**, with
  the colour bytes all zero (Q=1.0).
- Both 512x448 CPU and GPU display images,
  plus the 1024x64 upload image,
  have **zero nonblack pixels**.
  The exact PPM files are unchanged from
  the tenth replay.

This proves genuine black/untextured sprite
commands dominate the recorded committed draws;
it does **not** prove that WebGPU is failing to
render coloured/textured commands, nor that
the original game has reached the rendered
memory-card warning. Do not fabricate
coloured geometry or infer original UI
progress from the PCSX2 reference screenshot.

Original EE execution still heavily visits the
SIF RPC client-status checker `0x00178DE8`
and return continuation `0x001BD58C`,
but the runtime log independently shows
many successful original ARKD_DVD `0x503`
method-2 guest IOP RPC worker returns.
The guest also executes many original
animation/VU0/vector routines. Therefore
counting dispatch edges is not sufficient
to call this a stuck input loop.

### Reproducible opt-in controller confirmation experiment

The native IOP SIO2 pad state is **neutral**
by default and has **no host keyboard or gamepad
input binding**. The original guest is now
polling that state thousands of times, so a
bounded, user-authorized button pulse is a
concrete experiment for the screenshot's
possible controller-confirmation gate.

An opt-in host-only setting is now available
**only on later PR commits**, through these
two environment variables:

- `SDBZ_PAD0_CROSS_FIRST_POLL`: positive,
  one-based count of original connected
  port-0 command-`0x42` controller reads
  at which to first press Cross.
- `SDBZ_PAD0_CROSS_POLL_COUNT`: number
  of those real polls to hold the input
  pressed, from 1 to 120.

Both variables must be set together. When
unset, all original pad responses remain
**bit-for-bit unchanged**. When enabled,
only the authentic port-0 `0x42`
response button high byte is returned as
`0xBF` instead of `0xFF` during
the selected range. This is the active-low
PS2 Cross bit (`0x4000`); the low byte,
device identity, commands, other port
and all non-controller SIO2 interactions
remain original. No guest hardware start,
DMA, interrupt, memory-card reply,
RPC return or other event is synthesized.

The new `m2_iop_sio2` diagnostic fields
`pad0_polls`, `pad0_cross_pressed_polls`,
`pad0_cross_first_poll` and
`pad0_cross_poll_count` prove how many
actual guest controller replies were
delivered with Cross held.

Example for a deliberate comparative
Windows full replay:

```bat
set "SDBZ_PAD0_CROSS_FIRST_POLL=200"
set "SDBZ_PAD0_CROSS_POLL_COUNT=8"
run_ps2_game.bat --full
```

Then **clear** both settings before a
neutral control replay:

```bat
set "SDBZ_PAD0_CROSS_FIRST_POLL="
set "SDBZ_PAD0_CROSS_POLL_COUNT="
```

The probe is an *input stimulus* for testing
the game, not evidence that the game
actually presented a warning or accepted
a button. Compare the **same** replay
budget with/without stimulus: native
GS coloured/textured command counts,
new MCSERV requests, original EE
execution hotspots, SIO2 poll response
counts and CPU/GPU PPM captures. If nothing
changes, do not conclude a game-visible
input gate from the neutral baseline.

The control and report changes only touch
host-side IOP hardware and runner,
synthetic-IOP tests and docs. They do
not fingerprint guest retranslation.


## Twelfth matched archive: eight authentic Cross replies, no GS change

Archive: `sdbz-graphics-pr148(20261121-163925).zip`, SHA-256
`e561b474ff83447ab693ecc05c521dc655e78c186d10464b0ba426872dd2cc49`.
All eight members are coherent with one 16:31–16:34 UTC Windows
full replay; the pre-existing translation cache was last written
at 14:13 UTC. The USA retail ELF catalogue has SHA-256
`1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`.
The boot log records the explicit 200/8 Cross configuration,
but **does not record the local Git commit SHA**.

### Original IOP input and replay comparison

`m2_iop_sio2` confirms 1,807 genuine connected port-0 0x42
polls and exactly **eight Cross-marked responses on polls
200–207**. There were 3,614 decoded port-0/1 packets,
3,613 completed transfers from 3,613 CTRL starts,
18,070 empty RX FIFO reads, zero rejected starts,
zero outstanding request, and final SIO2 interrupt
status zero. There is no IOP guest fault.

At the full 6,000,000 EE dispatch limit, the report gives
12,779,550,806 attempted instructions, final PC 0x002F401C,
2,597 GS fields, 1,830 successful WebGPU presents and 1,809
committed GPU sprite draws: **all of these match the supplied
neutral 16:16 replay**. The accepted paths are 1,810 VIF1
DIRECT/PATH2 batches, 1,843 GIF DMA/PATH3 batches, and
zero accepted PATH1 XGKICK. No textured or nonblack RGB
sprite was recorded. All actual PPMs are black: 229,376
black pixels each in the 512x448 CPU/WebGPU displays and
65,536 black pixels in the 1024x64 upload capture. The
display digests match. No visible image is proven.

The original IOP log contains 1,846 committed RPC events,
including 1,802 ARKD_DVD 0x503 method-2 calls and **no
MCSERV 0x80000400/0x80000480 RPC calls**. The previous
neutral replay also recorded 1,802 ARKD method-2 calls.
The expansion footer reports `new_functions=0` in its
one outer pass; there are still 1,285 cached guest IR
modules. Preserve the cache.

**Conclusion:** eight authentic emulated button responses
were produced; no original-game branch, memory-card request,
coloured/textured draw or visible scene change was established.
In particular, `pad0_cross_pressed_polls` previously proved
SIO2 *reply generation*, not that the guest had consumed
the fifth byte. The PCSX2 memory-card warning remains an
external reference, not native output.

### Narrow host-only FIFO provenance added after this archive

The next diagnostic tags only the active-low Cross byte
from each authentic port-0 0x42 reply and tracks it through
the pre-existing RX FIFO. The JSON adds:

- `pad0_cross_bytes_pio_read`: genuine IOP CPU reads of that byte;
- `pad0_cross_bytes_dma_written`: successful original DMA12
  writes of that byte;
- `pad0_cross_bytes_fifo_discarded`: original CTRL FIFO resets
  discarding that byte before consumption.

All tags are host-only, enqueued and dequeued alongside the
original RX bytes. No button state, packet, interrupt, DMA
completion, RPC result or GS command changes. The focused
test reads exactly two Cross bytes through the PIO port;
a separate pulse leaves its fifth reply byte unread and
proves that CTRL FIFO reset discards it rather than counting
it as received. Kernel reset clears these counters.

**Next evidence gate:** a new matched 200/8 Cross replay
after rebuilding the host runtime should identify whether
the original SIO2MAN worker *read* the generated button
byte or the guest discarded it. Even a positive FIFO-read
count does not prove the PADMAN-to-EE application handoff
or a native UI screen. PR #148 remains draft/unmerged.


## Thirteenth matched archive: Cross bytes discarded by SIO2 CTRL readback mismatch

Archive: `sdbz-graphics-pr148(20261121-165442).zip`, SHA-256
`e877433368632f3855464302c2e06551ba59a60521f3b7a70e4884ebc5f3c68f`.
The eight ZIP members are coherent with the final 16:54:26 replay:
stopped `first-boot.json`, `boot.log`, one outer `expansion.log`
pass, static IOP catalogue, old retained translation cache, and all
three PPM captures. The game ELF SHA-256 remains
`1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`.
The log explicitly records the opt-in 200/8 Cross experiment. It does
not prove the exact local Git commit SHA; new JSON keys prove the
read-through diagnostic was built.

### Definitive response consumption diagnosis

`m2_iop_sio2` gives:

| Counter | Result |
| --- | ---: |
| Original port-0 0x42 polls | 1,807 |
| Cross-marked port-0 replies generated | **8** |
| Cross bytes read by IOP CPU | **0** |
| Cross bytes delivered by DMA12 | **0** |
| Cross bytes discarded by original CTRL FIFO reset | **8** |
| SIO2 packets/CTRL starts/completed | 3,614 / 3,613 / 3,613 |
| Empty FIFO reads | **18,070** |
| Failed starts / outstanding transfer | 0 / false |
| IOP guest faults | 0 |

For comparison, 3,614 five-byte reply packets nominally contain
18,070 response bytes, matching the observed empty RX reads. The
per-byte Cross provenance proves that the actual Cross bytes were
discarded by the guest's CTRL reset before SIO2MAN read them; they
were not presented to EE game input. Do not misdescribe the input
experiment as a failed original-game button-gate test: the actual
button value never reached the guest's SIO2 RX reader.

All control/graphics milestones match the previous Cross and neutral
replays: 6,000,000 EE dispatches; 12,779,550,806 instruction attempts;
EE final PC 0x002F401C and budget exit RETURN; 2,597 GS fields,
1,830 successful WebGPU presents and 1,809 untextured black sprite
draws. PATH1 XGKICK still commits zero, PATH2 commits 1,810 batches,
PATH3 commits 1,843 batches, and all three PPMs are genuinely black.
1,802 ARKD_DVD 0x503:2 RPC commits; no MCSERV 0x80000400/0x80000480
commit, and no IOP guest fault. The one expansion pass reports
new_functions=0; the same 1,285-module translation cache is retained.

### Original read-modify-write sequence and reference divergence

The original SIO2MAN interface uses separate control operations:
read CTRL, OR 0x0c, write/reset FIFO; transmit SEND3-described
PIO bytes; read CTRL, OR bit 0x01, write/START; wait IRQ17;
then read the actual RX response bytes. This is also visible in
PS2SDK's `sio2_set_ctrl_c` and `sio2_set_ctrl_1` helpers.

The primary `ps2-web-recomp/SDBZ/runtime/src/iop/sio2.cpp`
implements CTRL read (`0x1F808268`) as **0**, while write preserves
a write-side FIFO reset/start action. Native `iop::c_hw::read()`
previously echoed `m_sio2_ctrl`. After the guest reset to
`0x3BC`, the next read therefore returned `0x3BC`; its START
OR 1 became **0x3BD**, which contains reset bits 2 and 3.
Native `c_hw::write(k_sio2_ctrl)` consequently cleared the
already-queued PIO replies and their Cross bytes immediately
before completion/IRQ17. The empty reads then returned
the intentionally supported 0xFF default. This accounts for
all eight provenance discards and the repeated empty reads.

**Causal native-host correction:** CTRL read returns zero as in
`ps2-web-recomp`; the last written value remains separately
stored for diagnostics. Guest reset now writes 0x0c, transmits
the two authentic controller polls, then writes 0x01 for START.
The reply is preserved for the original SIO2MAN FIFO reader,
rather than mistakenly reset at START. An explicit direct
write of 0x3BD still retains the existing combined reset/start
behaviour; no FIFO semantics, IRQ/DMA, guest instruction, or
peripheral byte is faked.

The focused `iop_hw` regression executes this exact
read-modify-write/reset/send/two-pad/START flow and reads all ten
bytes through the **original R3000 KSEG1 address 0xBF808264**.
It asserts the connected port-0 Cross reply appears as fifth
byte 0xBF, the disconnected port-1 reply remains all 0xFF,
one real completed transfer, zero discarded Cross bytes,
zero empty RX reads and zero rejected starts. Previous tests
for explicit 0x3BD reset/start and incomplete-packet
failure remain intact.

**Next acceptance gate:** CI for the host-only readback fix,
then a matched original USA retail replay with the optional
200/8 Cross experiment. Verify that
`pad0_cross_bytes_pio_read` is positive, discard count
is zero, and original guest RPC/GS execution is compared
with neutral baseline before concluding whether it
reacts to the button. Do not claim an EE input branch or a
nonblack frame until the new original-game archive proves it.
No new translation or 1,285-module cache invalidation is needed.
Keep draft PR #148 open and unmerged.


## Fourteenth matched archive: authentic Cross bytes read; original EE pad code newly reached

Archive: `sdbz-graphics-pr148(20261121-171353).zip`, SHA-256
`714b40311bc1af73f9a345fe50f97d2de325115732b4095fb6dcd3b67e84242d`.
The eight archive members have coherent 17:07–17:13 UTC provenance:
the old cache was loaded, expansion generated eleven missing functions,
then the stopped first-boot report/boot log and three PPMs were written.
The archive does not independently record local Git HEAD. The actual
USA retail ELF's SHA-256 is
`1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`.

### Accepted genuine SIO2 data path

The retail original guest now completes both controller packets per
one original SIO2 START, no longer issuing combined FIFO reset/start
`CTRL=0x3BD`. Final `m2_iop_sio2` records:

| Measurement | Previous discarded-Cross replay | 17:13 replay |
| --- | ---: | ---: |
| Original connected port-0 0x42 polls | 1,807 | 1,712 |
| Opt-in Cross responses generated | 8 | **8** |
| Cross bytes consumed by original IOP PIO reader | 0 | **8** |
| Cross bytes delivered via DMA12 | 0 | 0 |
| Cross bytes discarded by CTRL FIFO reset | 8 | **0** |
| Empty RX FIFO reads | 18,070 | **0** |
| Total decoded controller packets | 3,614 | 3,520 |
| CTRL START/completed transfers | 3,613/3,613 | **1,760/1,760** |
| Rejected START / IOP guest fault | 0 / null | **0 / null** |

The first decoded controller commands additionally include genuine
port-0 command 0x43 after the first port-0 0x42 poll, confirming
post-initialization controller-command traffic. The final START
had **26 real reply bytes queued before START**, and `CTRL=1`
retained them until the guest read them. No new response,
IRQ17, MCSERV action, or emulated key event is being fabricated.

This is authentic original IOP RX delivery only. The
`pad0_cross_bytes_pio_read=8` counter cannot alone establish that
PADMAN placed those specific bytes in its RPC buffer or that
the original EE application took a Cross-triggered branch.

### Original EE code reachability and cache append

The initial expansion replay reached a previously untranslated
EE function at `0x00188970` after approximately 3.67m native
dispatch boundaries. Sequential auto-expansion discovered
**eleven** real executable function entries:

- `0x00188970`, `0x00188AA8`, `0x001885F8`
- `0x00175598`, `0x001754F0`
- `0x001886B0`, `0x00188E60`, `0x00188D00`
- `0x00188EC0`, `0x00188DB0`, `0x001884D0`

All eleven emitted functions have **zero unsupported instructions**
per their function-scoped lift summaries. The expansion log records
one **outer** pass, `replay=11 new_functions=11`, followed by an
authoritative full final run; the first eleven intermediate
`guest left compiled code` observations are cache-expansion gates,
not final runtime IOP faults. The 1,285 old IR hashes are
**all unchanged**; 11 new hashes are appended for **1,296** total.
The source/ELF fingerprint inputs match the previous cache;
manifest metadata/library digests changed because its inventory
expanded. No existing game IR modules were regenerated or replaced.

The recovered original USA executable (verified SHA above)
supplies direct JAL evidence for those functions in the
**original EE controller state machine** at 0x001099xx.
At original 0x00109B98, JAL invokes function 0x001884D0
with its destination at the caller's `sp+0x40`;
return continuation 0x00109BA0 checks the status and
returned bytes. At 0x00109BC8/0x00109BD0 the original
caller reads bytes `sp+0x42` and `sp+0x43`,
combines them with the former as high byte, and
inverts the raw active-low word at 0x00109BE4.
The nearby original 0x00109BF4/0x00109BF8
tests it against the game's stored mask table.
This is a **verified static original ELF input-processing
path**, not proof of a particular executed branch or a new
visible native frame.

### Matched final guest outcome

The stopped final `first-boot.json` is authoritative:
6,000,000 EE dispatches, RETURN on the configured budget
at original EE continuation PC `0x001BA584`,
12,431,808,881 guest instruction attempts, 1,760
HOST_YIELD resumes, **no IOP guest fault**.
The previous Cross/neutral pre-fix replay used
12,779,550,806 guest attempts and ended at 0x002F401C;
the comparison does **not** isolate Cross because the
reference-compatible CTRL readback repair also changed.
These are genuine different guest execution paths.

The new run has 2,526 GS fields, 1,786 successful WebGPU
presents, 1,759 untextured **black** GPU sprite kicks/draws,
zero nonblack RGB/textured sprites, no committed VU1 PATH1
XGKICK packets, 1,760 VIF1 PATH2 batches and 1,793 GIF
PATH3 batches. All three real capture files are **pixel-black**
and the CPU/GPU 512x448 outputs agree byte-for-byte; the
1024x64 upload image also remains black.

Actual IOP HLE RPC commit count is 1,798, with 1,752
genuine ARKD_DVD server 0x503 method 2 commits, and
PADMAN 0x80000100 method 1 commits increased from
the previous 4 to **6**. No 0x80000400/0x80000480
MCSERV service RPC was committed, and merely loading
MCMAN/MCSERV does not prove card I/O.
The original-game PCSX2 memory-card-warning screenshot
is still only an external reference.

### Evidence-gated next step: EE-side passive sample and matched neutral control

The current log does not record the original EE
return-state bytes at 0x00109BA0. The branch now includes a
narrow **host-only, read-only** observer at that exact
ELF-verified return continuation. It samples four backed
EE RAM bytes at original `sp+0x40` and calculates the same
16-bit active-low button candidate that the guest later uses.
The report key `m2_sdbz_ee_pad_return` includes bounded
first/last sample, candidate Cross sample, a reached-site
counter and read-only status/length context. It does not
override the guest return value, controller data, input
processing, branch, memory, IRQ, DMA, RPC, or GS state.
A synthetic CTest checks byte order, neutral and Cross
candidate, rejecting unsuccessful status and preserving
all guest RAM bytes.

**Only a neutral replay at this same post-fix branch HEAD**
can isolate the effect of the opt-in Cross stimulus from
the CTRL readback repair. A new opt-in replay with the
EE-side return probe can then establish whether the
Cross word reaches the original EE caller; the probe
does not itself establish that the game took the button
branch. Compare both replays' cache, IOP pads,
PADMAN/MCSERV/ARKD RPC, translated PC paths, GS
and real PPMs before changing game or graphics code.
Use the existing 1,296-module append-only translation cache.

No new game-visible code change is justified by the
fully black GS evidence. Keep PR #148 draft/open
and unmerged; eventual merge remains squash-only after
explicit authorization.
