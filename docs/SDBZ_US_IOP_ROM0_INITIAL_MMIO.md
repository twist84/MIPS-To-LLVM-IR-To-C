# IOP ROM0: first hardware register milestone

> **Record status (2026-11-25):** Historical ROM0 initial MMIO investigation, not the normal HLE launch path. See [current status](README.md), [retail validation](SDBZ_RUNTIME_VALIDATION_20261125.md) and [accuracy plan](ACCURACY_PLAN.md). Any branch, PR, pending replay or feature-status claims below refer to the documented stage.


The IOP ROM0 reset-vector path introduced in PR #81 now has a small
guest-visible hardware model. It handles only the devices below,
rather than returning a zero value for every unimplemented register.

| Physical address | Device | Implemented behavior |
| --- | --- | --- |
| `0x1F800000..0x1F8003FF` | IOP scratchpad | 1 KiB RAM, little-endian 8/16/32-bit accesses, bounded and alignment checked |
| `0x1F801070` | `I_STAT` | Read latched pending IRQ bits; write AND-mask to acknowledge zero bits |
| `0x1F801074` | `I_MASK` | Read/write interrupt enable mask |
| `0x1F801078` | `I_CTRL` | Bit 0 global IRQ enable; read returns prior value and disables; write selects bit 0 |
| `0xFFFE0130` | Cache control | Read/write 32-bit control value; cache operation semantics not yet implemented |

The scratchpad and IRQ registers accept physical and direct KSEG0/KSEG1
aliases. The cache-control register is explicitly addressed in high
kernel space; no other upper memory address is implicitly accepted.
Unknown IOP devices, including timers, DMA and SIF, still cause a
guest memory fault rather than an invented successful read/write.

`iop::c_hw` is connected to the R3000 memory access path only when
the kernel starts from a caller-supplied ROM0 image. Reset clears the
scratchpad and device registers. The prior standalone CPU and
IRX-only bootstrap do not acquire a permissive hardware bus.

IRQ status and mask tracking is bookkeeping only: there is **no**
R3000 COP0 interrupt exception delivery in this milestone. Register
read/write behavior is modeled, but it does not imply the BIOS has
successfully initialized the IOP, and no SMFLAG readiness is created.

The new `iop_hw_test` covers register semantics and executes original
synthetic MIPS I ROM0 instructions that read/write scratchpad, cache
control and I_MASK. It also checks reset behavior, unknown register
rejection, and no hardware attachment after kernel reset.

Source references:

- [PS2Tek, IOP memory map and interrupts](https://israpps.github.io/ps2tek/)
- [`ps2-web-recomp` IOP hardware reference](https://github.com/twist84/ps2-web-recomp/blob/SDBZ/runtime/src/iop/hw.cpp)

Next: run the latest native runner with a legitimate local BIOS ROM0
supplied using `SDBZ_IOP_ROM0` and inspect the first stopped IOP
instruction. Then implement only the observed missing IOP device or
exception handling; don't infer successful initialization from a
nonzero SIF bit unless the guest IOP actually produced it.
