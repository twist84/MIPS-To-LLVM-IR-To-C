# M2.5 — VIF1 source-chain DMAC transport (bounded, fail-closed)

> **Record status (2026-11-25):** Historical VIF1 source-chain transport batch; subsequent VIF and DMA work is recorded in the original-test log. See [current status](README.md), [retail validation](SDBZ_RUNTIME_VALIDATION_20261125.md) and [accuracy plan](ACCURACY_PLAN.md). Branch names, open PR instructions, and pending gates in the historical text refer to the documented revision.


## Evidence: 19 November 2026, 13:53 matched USA boot

Sources: `boot(20261119-135356).log`,
`first-boot(20261119-135354).json`, matching expansion,
catalog, translation-cache and uploaded GS PPMs. Input ELF
SHA-256 `1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`.

| Probe | Dispatch | Pending flag | VIF1 CHCR | VIF1 TADR | D_STAT pending |
|---|---:|---:|---|---|---|
| First command wait | 10,411 | 0 | 0 | 0 | 0 |
| First pending wait | 10,495 | 1 | `0x000001c5` | `0x00771400` | 0 |
| First red fallback | 36,205 | 1 | `0x000001c5` | `0x00771400` | 0 |

`0x1c5` denotes STR=1, DIR=1, MOD=source-chain, TTE=1
and TIE=1. The source-chain tag pointer never advances.
A DMAC channel-1 callback is registered to
`0x00102a60` but no channel-1 completion is seen.
The same exact 100,000-dispatch end PC and 2,275
red sprites persist. This is a **confirmed missing
VIF1 chain execution path**, not a reason to force
an IRQ or change the GS scissor.

The reference architecture is
`twist84/ps2-web-recomp:SDBZ`,
`runtime/src/dmac.cpp::run_chain(1)` and
`runtime/src/vif.cpp::transfer(1)`.
The current native runner's single device transfer callback
goes to `c_ee_gif_path3`; it correctly refuses channel
1 because VIF1 commands are **not GIF PATH3**.

## Batch delivered in this commit

`c_ee_dmac::transfer_vif1_source_chain()` now
starts only when the *guest* starts VIF1 with CHCR.MOD=1.
It implements bounded, transactional physical source
tag walking, including:

- REFE, CNT, NEXT, REF, END and balanced two-deep CALL/RET.
- TTE tag transfer: high 64 tag bits as a complete
  16-byte VIF quadword with the low 8 bytes zeroed,
  before the associated data.
- Guest TIE/IRQ termination and architectural
  CHCR/MADR/TADR/QWC/ASR commit only on full acceptance.
- Prevalidated, bounded EE RAM/scratchpad tag and payload
  reads; no invented source bytes or silent wraps.
- Refusal of unsupported REFS/STADR stalls, unbalanced
  tag stacks, missing memory, oversize chains, invalid
  spans or consumer rejection **without** retiring STR,
  advancing channel registers or raising D_STAT.
- Read-only `vif1_chain_last_reject()` reason, and
  a 64-byte raw EE-RAM `source_prefix_qwords` snapshot
  alongside the actual VIF1 start/wait/red-loop
  register states. The prefix is null unless its
  physical source span is backed; it is never a
  speculative packet assembled from static ELF data.

The new `ee_vif1_chain_test` covers accepted
CNT+END TTE, REF/TIE IRQ termination, sink rejection,
missing sink, unsupported REFS and invalid external
payload. The previously introduced command-wait probe
also validates raw source-prefix and rejection-state
snapshots. No synthetic HLE completion is permitted.

## What this does *not* fix yet

**The game will not boot solely from this change.**
There is still no fully connected VIF1 command
processor/VU1 execution/GIF PATH2 transport. The
existing GIF PATH3 callback refuses channel 1, so
this newly implemented chain walker cannot claim
completion in the production runner. That refusal
is intentional and is now reported explicitly as
`rejected_by_sink`. An absent or incomplete device
consumer is not a DMA success.

The next matched run must expose the first source
tag's actual high 64 bits and early payload words
at `0x00771400` so that VIF1 opcode coverage
and the true VU1 producer can be derived from game
data, not guesswork. Implement a **stateful,
transactional VIF1 decoder and necessary VU1/GIF
path behaviour in a batch**, using the primary
reference semantics, and retire DMA only after
the full stream is genuinely consumed. Do not
increase the 100,000-dispatch budget to mask a
stuck device; do not set flag=0 or raise channel-1
IRQ as a shortcut.

## Exact acceptance criteria

1. Real VIF1 chain with TTE and TIE must deliver
   correctly ordered tag/payload stream to a
   channel-aware sink exactly once.
2. All source spans and limits must be validated
   before any sink callback.
3. Refusals must retain STR, MADR, TADR, QWC,
   guest RAM and D_STAT unchanged.
4. Only an accepted complete chain may clear STR
   and raise DMAC channel-1 status/interrupt.
5. CTest + Windows/Linux build, SIF and ROM0 CI
   green at exact commit; PR #142 remains draft.

## Matched USA Windows boot 2026-11-19 16:14 — NEXT address decode fix

Matched five artifacts by invocation: `boot(20261119-161406).log`, `expansion(20261119-161404).log`, `first-boot(20261119-161404).json`, `iop-disc-catalog(20261119-161407).json`, and `translation-cache(20261119-161408).json`; GS snapshots are from the same replay (`gs-display(20261119-161404).ppm`, `gs-upload(20261119-161404).ppm`, and WebGPU readback).

The first pending command slot at dispatch 10495 had VIF1 `CHCR=0x000001C5`, `TADR=0x00771400`, and `vif1.chain_reject=3` (`invalid_tag_address`). Its real first DMA tag `0x0077144020000003` encodes `NEXT`, QWC 3, ADDR `0x00771440`; the following TTE VIF code word is `0x5000000313000000` (`0x13000000` and `0x50000003`). Prior code incorrectly read the chain's external address as `tag.m_hi & 0xfffffff0` and tried `0x13000000` instead of reading bits 32..63 of `tag.m_lo`. That address is outside EE RAM, causing the genuine invalid-tag rejection and leaving the guest DMA command pending. This is a confirmed DMAC implementation defect, unrelated to newly added VU1 opcodes.

Fix: decode `external = (low >> 32) & 0xfffffff0`, retaining the scratchpad selector, independently of the upper TTE half. The previous `ee_vif1_chain` synthetic writer incorrectly encoded the external DMA address in the TTE half and therefore failed to detect the defect. The writer is corrected, and regressions cover the captured NEXT/TTE sequence, nonsequential NEXT routing, and fail-closed invalid-next rollback. The separate PATH3 tag parser already decoded bits 32..63 correctly.

**Verification boundary:** This fix is proven by source inspection and targeted regression once CI passes. The only matched Windows boot is from *before* the fix: at dispatch 36205, red-loop pending flag was still 1 with CHCR/TADR unchanged, and the run exhausted 100000 dispatches. No post-fix real-game advancement or GIF output may be asserted without new matched Windows evidence.
