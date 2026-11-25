# M2.4 — USA SDBZ command-slot timeout and native red-loop producer

> **Record status (2026-11-25):** Matched historical command-slot/red-loop investigation, not a current retail blocker. See [current status](README.md), [retail validation](SDBZ_RUNTIME_VALIDATION_20261125.md) and [accuracy plan](ACCURACY_PLAN.md). Branch names, open PR instructions, and pending gates in the historical text refer to the documented revision.


**Observed local batch:** `boot(20261119-112918).log`,
`first-boot(20261119-112917).json`, 11:29 matching
expansion, catalog, translation-cache and three PPMs.

**Exact executable:** USA `SLUS_214.42`, ELF SHA-256
`1b213a85a2c305dc0f893c007a502c93e0851d1bb7db6164f9e0b72adb074a16`.
The ELF was inspected directly; these PCs are not inferred from
host dispatch frequency alone.

**Primary architecture reference:** `twist84/ps2-web-recomp:SDBZ`;
preserve real DMA/INTC/GS producer semantics and the project's
HLE-first, reference-compatible (not calibrated R5900) clock.

## Result: the 2,275 red sprites are an explicit guest failure loop

All first/last eight accepted sprite samples agree:

| Register | Value / decoded meaning |
|---|---|
| PRIM / effective PRIM | `0x6`, untextured sprite |
| RGBAQ | `0x3f800000000000ff` (red, Q=1) |
| First XYZ | `0x0000000072007000` |
| Second XYZ | `0x000000008e009000` |
| XYOFFSET_1 | `0x0000720800007000` |
| SCISSOR_1 | `0x0` (both x/y min/max zero) |
| FRAME_1 | alternates `0x0000000100080070` and `0x0000000100080000` |
| ZBUF_1 | `0x00000000300000e0` |
| TEST_1 | `0x0000000000030003` |

The effective sprite spans **x=0..512** and approximately
**y=-0.5..447.5** after subtracting XYOFFSET and dividing by 16.
Since no `SCISSOR_1=0x40` write was committed, the scissor
admits only **(0,0)**. The WebGPU image is black except for
exactly one **RGB(254,0,0)** pixel at (0,0): the source red
byte `0xff` is reduced to `0xfe` because **FRAME_1.FBMSK=1**
preserves the zero low bit. Thus the observed raster result is
**consistent with the submitted GS state**; arbitrary scissor
expansion, fake draw output, or a GPU shader rewrite is unwarranted.
The independent CPU display/upload PPMs are all-black because
that consumer only captures register/transfer VRAM, not raster draws.

These red sprites appear when the game has already taken its
**command-slot timeout recovery path**, not when drawing a menu.

## ELF-derived exact PC→predicate→producer chain

- `0x001024d0` initially checks `0x00502368` and
  `0x0050236c`: if both zero it returns without waiting.
- `0x00102560` reads `0x005030ac`, flips the low buffer
  selection with XOR 1 and reads the **opposite slot** at
  `0x00502368 + 4*(active_slot ^ 1)`.
- `0x001025a4..0x001025d4` loops while the selected
  pending word is nonzero. An iteration threshold of
  `0xBB9` (3001) activates a timer test.
- `0x001025b4..0x001025c4` reads hardware EE Timer 0
  COUNT at `0x10000000`, compares against `0xEA61`
  (60,001) and branches to `0x00102608` when the
  threshold is reached.
- `0x00102608..0x00102644` runs a bounded repeat of
  original guest recovery helpers `0x00173888` and
  `0x00173460`, with retry threshold `0x65` (101).
- `0x00102648..0x00102654` then **loops forever calling**
  `0x00102670`. That routine constructs the red A+D
  `PRIM/RGBAQ/XYZ2` and two alternating FRAME writes.

The first observed native dispatch into `0x00102670`
is **dispatch 36,205** (transition from
`0x00102634` at dispatch 36,204). Later calls account
for the 2,275 red sprite pairs. The apparent hot loop at
`0x00173250` is a finite VU-register save helper reached
during recovery, as already documented in M2.1.

**Crucial limitation:** this proves the **failure branch and its
wait predicate**, not which underlying producer or EE-cycle
inaccuracy caused the opposite-buffer pending flag to remain
set. Candidate producers include the game's own queue
consumer, a DMA/interrupt completion path, or a timing
difference. None may be assumed without correlated
guest memory and transport evidence. SIO2MAN remains the
only proven loaded IRX (13 committed RPCs).

## Follow-up: read-only evidence at the exact first bad transition

New `ee_sdbz_dma_wait_probe.h` snapshots raw backed EE RAM
at **three bounded, semantically distinct points**:

1. **First wait entry** `0x00102560`: baseline state before
   the opposite-slot wait logic, which may return successfully.
2. **First pending wait entry** `0x00102560` where the actual
   polled opposite-slot flag is nonzero; unlike the baseline,
   this identifies the first known outstanding command.
3. **Red-loop entry** `0x00102670`: first state after the
   timeout/recovery sequence.

The distinction is necessary because the 11:29 trace reaches
`0x00102560` more than once. Capturing only the earliest call
could retain a successful wait and miss the failure trigger.

Each sample records dispatch number, thread, all 32 words
of `0x00502350..0x005023cf` (including both flags), and
relevant queue-selection/command addresses:
`0x00502630`, `0x00502634`, `0x0050263c`,
`0x00502640`, `0x00503084`, `0x005030ac`,
`0x005023f0`. The `selected_slot` field is the
active selector and `polled_slot` = selector XOR 1,
matching the ELF. Invalid buffer selection cannot be
presented as a valid probed flag.

The output lives in `first-boot.json.m2_sdbz_dma_wait`,
schema `ee-sdbz-dma-wait-v1`, as `wait`,
`pending_wait`, and `red_loop`. If a PC is not reached or
guest RAM is not backed, the corresponding sample is null.
The observer uses only `ee_memory::read32` on already-backed
addresses: **no MMIO reads, writes, IRQs, DMA starts, retries,
fake semaphore releases, clock changes or guest instruction
execution**. A CTest regression checks one-shot capture,
active/opposite-slot reporting, immutable first values,
and unchanged guest memory.

### Next decision

On a matching future run, inspect whether the **polled**
pending flag is still nonzero at red-loop entry and which
command/queue values differ between `pending_wait` and `red_loop`. Then find its
actual guest or device writer and compare that producer
with `ps2-web-recomp:SDBZ`. Only change HLE/DMAC/INTC
behavior after a specific missing or incorrect event has
been demonstrated and regression-tested. **Do not increase
the dispatch budget, unconditionally clear the pending
flag, or force a wider GS scissor** as a workaround.

### Regression gate

- All exact M2.4 probe tests pass.
- Full build and WebGPU/SIF/ROM0 CI green at exact commit.
- PR #142 stays draft/unmerged.
- The recorded live-boot output is still limited to the
  100,000-dispatch budget; this commit is instrumentation,
  **not a guest-visible boot fix**.


## M2.4 VIF1/DMAC1 expected completion — evidence and diagnostic gate

The **11:29 final boot** registers `AddDmacHandler cause=1
handler=0x00102a60` and enables DMAC1. Its ELF handler
decrements the selected guest command pending count at
`0x00102b08..0x00102b10` after channel status/STR
checks. That final trace contains **15 DMAC channel 5
callbacks and zero channel 1 callbacks**. Consequently
DMAC1/VIF1 completion is the strongest known candidate
for the unretired buffer slot, not an unrendered menu frame.

The current native `ee_dmac.cpp` routes VIF1 starts to
`transfer_device_normal(1)`, which refuses source-chain
mode; for normal VIF1 outbound data it uses a sink currently
bound to a GIF PATH3 parser accepting only channel 2. This
**does not establish that the guest's stalled transfer was a
chain**: only the actual CHCR/MADR/QWC/TADR at the failure
boundary can settle that. Compared with
`twist84/ps2-web-recomp:SDBZ`,
`runtime/src/dmac.cpp::run_chain` and
`runtime/src/vif.cpp::transfer`, an actual implementation
will need genuine VIF1 payload parsing/state effects and
proper producer-driven DMA completion, not an IRQ shortcut.

The bounded `m2_sdbz_dma_wait` probe now additionally
reads **physical VIF1 channel 1 state** through a new
host-only `c_ee_dmac::vif1_channel_state()` accessor:
`CHCR`, `MADR`, `QWC`, `TADR` and `D_STAT`
status/pending. These are attached to `wait`,
`pending_wait` and `red_loop` snapshots.
No guest MMIO read, DMA scheduling, timer update, CPU
write, IRQ or other guest-visible action is performed.
The test explicitly starts an **unsupported synthetic
VIF1 chain (CHCR=0x105)** and verifies it remains
unretired while the probe captures its immutable channel
state and does not fabricate a completion interrupt.

**Next decision:** Confirm which VIF1 mode, address and
QWC the guest actually reached, and whether STR remains set,
then implement the necessary real VIF1 delivery and
channel-1 completion semantics in a batch of independently
verified tests. Never clear STR or pending flags merely to
avoid the red fallback loop.
