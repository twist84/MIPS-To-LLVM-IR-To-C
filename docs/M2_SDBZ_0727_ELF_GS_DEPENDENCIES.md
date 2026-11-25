# M2.1: verified USA ELF DMA/VU predicate map (07:27 trace)

> **Record status (2026-11-25):** Pinned 07:27 ELF analysis; preserve the original instruction/provenance findings without treating the old replay as today's state. See [current status](README.md), [retail validation](SDBZ_RUNTIME_VALIDATION_20261125.md) and [accuracy plan](ACCURACY_PLAN.md). Branch names, open PR instructions, and pending gates in the historical text refer to the documented revision.


USA `SLUS_214.42` ELF was SHA-256 checked independently:
`1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`.
This exactly matches the 07:27 IOP catalog. The game ELF was not committed.

## Correction: the high-frequency VU/COP2 helper is not an idle loop

At `0x001734dc`, the game executes `jal 0x00173250`, returning at
`0x001734e4`. At `0x001734e8`, `sltiu $v1,$s1,0x20`,
and `0x001734f0` loops back to `0x001734d0` to save 32 VU
registers using the COP2 helper. The `0x001735a8` control-register
loop runs 16 further iterations. Therefore the most frequent native
dispatch transitions are register-save call/return boundaries, not
evidence of a stuck game update, VU microprogram or VBlank.

## Concrete guest predicates and producer

| ELF PC | Static predicate / guest action | Hardware producer |
|---|---|---|
| `0x001729b4..0x001729ec` | Poll channel control bit `0x100` in `*(uint32_t *)(a0+0)` until cleared, with finite counter/recovery | DMAC channel STR retirement |
| `0x00172e34..0x00172e40` | Normalize transfer address (`0x00172a90`) then poll channel control (`0x00172998`) | Physical or software DMA channel state |
| `0x00172e48..0x00172e80` | Conditional address update at offset `+0x30`; program `+0x20=0` and set CHCR flag `0x100` | Transfer setup, **not** waiting at this PC |
| `0x001730c0..0x001730f4` | In mode 1, return `(control >> 8) & 1`; otherwise wait in `0x00172998` | DMA STR state |
| `0x00105020..0x00105038` | Call `0x001730c0`; loop while it returns busy | DMA channel completion |
| `0x001027e0..0x0010280c` | Construct scratchpad GIF payload at `0x70000000`, call `0x001002b0`, FlushCache, and channel setup `0x00172e20` | Actual committed GIF DMA and packet consumption |
| `0x00102814..0x0010282c` | Read GIF CHCR at `0x1000a000`; test `0x100`; loop if busy | Physical GIF channel 2 |

The current `src/runtime/ee_dmac.cpp` commits a supported normal
or GIF source-chain transfer synchronously when its consumer accepts
all bytes, and then clears CHCR.STR. A rejected transfer can leave STR
set; this is a **candidate dependency**, not yet a proven run blocker.
Repeated returns into later guest PCs indicate that these checks are
not demonstrably stuck throughout 07:27. They do **not** prove the
transfer effects and output are correct.

## Newly instrumented evidence

`m2_runtime` now samples GIF channel-2 CHCR, MADR, QWC, TADR, GIF
MMIO write sequence and last transfer rejection, without modifying
guest state. A successful synchronous transfer may start and finish
between two dispatch boundaries; the write sequence is essential
to avoid falsely concluding that an idle CHCR means no activity.

M2.1 remains partial until an actually reached failing predicate is
linked to its missing producer and a tested fix. No synthetic DMA
completion or IRQ has been introduced.
