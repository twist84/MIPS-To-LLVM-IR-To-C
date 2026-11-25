# M2.6 — Native VIF1 consumer, VU1 residency and GIF PATH2

> **Record status (2026-11-25):** Initial VIF1 consumer implementation record; later VU1/GS and original VIF tests supersede the initial boot status. See [current status](README.md), [retail validation](SDBZ_RUNTIME_VALIDATION_20261125.md) and [accuracy plan](ACCURACY_PLAN.md). Branch names, open PR instructions, and pending gates in the historical text refer to the documented revision.


**Status:** first implementation batch; full M2.6/VU1 game boot remains unverified.  
**Game:** USA `SLUS_214.42`, SHA-256 `1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`.  
**Branch:** `feature/sdbz-hle-first-bootstrap`; draft PR #142, do not merge.  
**Primary source:** `twist84/ps2-web-recomp:SDBZ`, `runtime/src/vif.cpp`, `runtime/src/dmac.cpp`, `runtime/src/gs/gif.cpp`, `runtime/src/memory.cpp` (Samir Nuri, MIT license).

## Architectural boundary

This project is the ps2-web-recomp-style HLE device runtime with **LLVM-IR guest translation**, not a separate PS2 emulator architecture. Port working original device algorithms instead of recreating them, while retaining local Allman braces, snake_case, `c_` classes, `s_` structs and `m_` members.

The 19 November 13:53 matched boot proves VIF1 CHCR=0x1c5 and TADR=0x00771400 at the first pending command-slot wait. However, `first-boot(20261119-135354).json` was generated **before** the M2.5 source-prefix probe. It does not contain the DMA tag or payload. No actual VIF opcode is yet proved reached. The following implementation provides reference-backed device semantics, **not an assertion about the game's particular VIF stream**.

## Batch A — Reference-derived VIF1 staging and device integration

New `src/runtime/ee_vif1.h/.cpp`:

- Persistent VIF1 code, mode, cycle, mask, row, col, double-buffer/base/offset/ITOP state and bounded pending 32-bit words. Commands can cross 128-bit DMA boundaries.
- STCYCL, BASE, OFFSET, ITOP, STMOD, MARK, STMASK, STROW and STCOL; NOP and no-active-VU FLUSHE/FLUSH/FLUSHA.
- Reference-derived UNPACK data-word counting, element sign extension, V2/V3 replication, V4_5 packing, row/column masks, cycle fill/skip and 1024-qword VU1 data-memory wrap.
- MPG transfers the original eight-byte microinstructions into 16 KiB VU1 micro memory. VU1 data also occupies 16 KiB; physical EE apertures `0x11008000..0x1100bfff` (code) and `0x1100c000..0x1100ffff` (data) share these arrays.
- DIRECT/DIRECTHL submit the supplied GIF words as **PATH2** packets to a separate stateful GIF parser instance and then to the existing GS / WebGPU event consumer.
- Existing GIF DMA channel 2 retains its independent PATH3 parser. The original M2.5 VIF1 chain walker now reaches a real channel-1 device sink.
- VIF1 MMIO at `0x10003c00..0x10003fff`, 128-bit FIFO at `0x10005000..0x10005fff`; FBRST resets VIF command state but retains VU1 resident code/data and ROW/COL, as in the reference.

The processor stages all command and VU memory updates before submitting any GIF output. Rejected decoder, VU or PATH2 transactions do not change VIF state or DMA registers, and the caller therefore does **not** raise D_STAT channel 1 or retire CHCR.STR. A successful source chain retires only through the existing M2.5 source validation and `c_ee_dmac::raise(1)` path; the game's original DMAC handler must release the pending command slot.

The native runner logs one bounded `VIF1 native reject` diagnostic containing reason, rejected VIFcode and prior pending word count. The return reason remains a host diagnostic, not a guest-visible DMA event.

## Intentional stop gates

- MSCAL, MSCALF and MSCNT are **rejected**, not pretended to execute. Completing these requires porting the original VU1 dual-issue micro interpreter and feeding **real VU1 XGKICK** to GIF PATH1.
- MSKPATH3 requires real GIF PATH3 masking/arbitration; it is rejected rather than silently applied.
- VIF IRQ commands whose interrupt is not masked require VIF INT emission, DMA stall and resume after FBRST; they currently reject without interrupt fabrication.
- REFS/STADR and MFIFO remain outside the synchronous M2.5 source-chain path. They cannot retire prematurely.
- The PATH2 parser uses the shared GIF packet/event semantics; path arbitration and synchronization with PATH1/PATH3 still require dedicated verification.
- A packet containing multiple nonadjacent DIRECT commands currently batches their GIF payload; an interleaving VU execution path must eventually preserve exact event ordering.
- A successful synthetic channel-1 DMA regression is not proof that the **original** SDBZ chain has advanced.

## Regression gate

`ee_vif1_device_test` checks NOP/STCYCL, V4-32 and signed V2-16 UNPACK, masked row/column/skip behavior, MPG byte residency, transaction rollback on MSCAL, command split across DMA boundaries, DIRECT/PATH2 GIF event delivery, unknown command/IRQ refusal, FIFO/FBRST register behavior, physical code/data windows and true DMAC1 TTE source-chain completion.

Retain existing `ee_vif1_chain`, GIF PATH3, GS/WebGPU, SIF, ROM0 and EE scheduler regressions. CI status must be recorded for the **exact final** branch head, not earlier commits.

## Next evidence-based batch

1. Run the already-integrated updated Windows launcher (after pulling the tested head). Match the final five boot artifacts and check `m2_sdbz_dma_wait.pending_wait.vif1.source_prefix_qwords`; decode tag low/high, actual tag type, addresses and first VIFcode.
2. If VIFcode is rejected, use the single native rejection code and real source bytes to determine the necessary coherent next device family.
3. For reached MSCAL/MSCALF/MSCNT, port VU1 micro execution from `ps2-web-recomp:SDBZ/runtime/src/vu_micro.cpp` and its `vu_ops` dependencies, preserving original code/data semantics and correct XGKICK/PATH1 producer traffic. Add finite execution and stall tests before treating it as complete.
4. Prove the original guest DMAC1 handler `0x00102a60` actually runs, channel 1 D_STAT/STR transitions, the guest pending flag decrements naturally and the old red fallback is not reached for missing device completion. Never clear it via host HLE.
