#include "iop_hw.h"
#include "iop_kernel.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
int fail(const char* detail)
{
    std::fprintf(stderr, "IOP Timer 5 / R3000 exception: %s\n", detail);
    return 1;
}

void put32(std::vector<uint8_t>& rom, uint32_t offset, uint32_t opcode)
{
    for (uint32_t byte = 0u; byte < 4u; ++byte)
    {
        rom[offset + byte] = static_cast<uint8_t>(opcode >> (byte * 8u));
    }
}

bool install_irq_vector(iop::c_kernel& kernel)
{
    // Guest handler acknowledges Timer 5, reads EPC, then executes
    // RFE as the JR delay slot to restore the previous KU/IE state.
    const uint32_t code[] =
    {
        0x3c08bf80u, // lui t0,0xbf80
        0x35081070u, // ori t0,t0,0x1070 (I_STAT)
        0x3c09fffeu, // lui t1,0xfffe
        0x3529ffffu, // ori t1,t1,0xffff (ack only IRQ 16)
        0xad090000u, // sw t1,0(t0)
        0x401a7000u, // mfc0 k0,EPC
        0x03400008u, // jr k0
        0x42000010u  // rfe in delay slot
    };
    for (uint32_t index = 0u; index < 8u; ++index)
    {
        if (!kernel.cpu().write32(0xa0000080u + 4u * index, code[index]))
        {
            return false;
        }
    }
    return true;
}
} // namespace

int main()
{
    iop::c_hw hardware{};
    const uint32_t irq = 1u << iop::c_hw::k_timer5_irq;
    uint32_t readback = 0u;
    if (!hardware.write(iop::c_hw::k_i_mask, 4u, irq) ||
        !hardware.write(iop::c_hw::k_timer5_mode, 4u, 0x58u) ||
        !hardware.write(iop::c_hw::k_timer5_target, 4u, 4u) ||
        !hardware.write(iop::c_hw::k_timer5_count, 4u, 0u))
    {
        return fail("Timer 5 kernel register initialization");
    }
    hardware.advance_cycles(3u);
    if (!hardware.read(iop::c_hw::k_timer5_count, 4u, &readback) ||
        readback != 3u || hardware.pending_interrupts() != 0u)
    {
        return fail("Timer 5 counted or asserted interrupt early");
    }
    hardware.advance_cycles(1u);
    if (!hardware.read(iop::c_hw::k_timer5_count, 4u, &readback) ||
        readback != 0u || hardware.pending_interrupts() != irq ||
        !hardware.read(iop::c_hw::k_timer5_mode, 4u, &readback) ||
        (readback & 0x800u) == 0u ||
        !hardware.read(iop::c_hw::k_timer5_mode, 4u, &readback) ||
        (readback & 0x800u) != 0u)
    {
        return fail("Timer 5 target reset, IRQ or status flag read");
    }
    if (!hardware.write(iop::c_hw::k_i_stat, 4u, ~irq) ||
        hardware.pending_interrupts() != 0u)
    {
        return fail("Timer 5 interrupt acknowledgement");
    }
    hardware.advance_cycles(8u);
    if (hardware.pending_interrupts() != irq ||
        !hardware.read(iop::c_hw::k_timer5_count, 4u, &readback) ||
        readback != 0u)
    {
        return fail("Timer 5 must repeat at each target");
    }
    hardware.reset();
    // I_STAT can record a real target event without dispatching it;
    // I_MASK and I_CTRL independently gate interrupt visibility.
    if (!hardware.write(iop::c_hw::k_timer5_mode, 4u, 0x58u) ||
        !hardware.write(iop::c_hw::k_timer5_target, 4u, 3u))
    {
        return fail("Could not configure masked timer interrupt");
    }
    hardware.advance_cycles(3u);
    if (hardware.pending_interrupts() != 0u ||
        !hardware.read(iop::c_hw::k_i_stat, 4u, &readback) ||
        (readback & irq) == 0u ||
        !hardware.write(iop::c_hw::k_i_mask, 4u, irq) ||
        hardware.pending_interrupts() != irq ||
        !hardware.write(iop::c_hw::k_i_ctrl, 4u, 0u) ||
        hardware.pending_interrupts() != 0u ||
        !hardware.write(iop::c_hw::k_i_ctrl, 4u, 1u) ||
        hardware.pending_interrupts() != irq)
    {
        return fail("Masked I_STAT pending event / I_MASK / I_CTRL gating");
    }
    hardware.reset();
    if (!hardware.read(iop::c_hw::k_timer5_count, 4u, &readback) ||
        readback != 0u || hardware.pending_interrupts() != 0u)
    {
        return fail("Timer 5 reset state");
    }
    if (!hardware.write(iop::c_hw::k_timer5_mode, 4u, 0x2058u) ||
        !hardware.write(iop::c_hw::k_timer5_target, 4u, 2u) ||
        !hardware.write(iop::c_hw::k_i_mask, 4u, irq))
    {
        return fail("Timer 5 prescaler configuration");
    }
    hardware.advance_cycles(15u);
    if (!hardware.read(iop::c_hw::k_timer5_count, 4u, &readback) ||
        readback != 1u || hardware.pending_interrupts() != 0u)
    {
        return fail("Timer 5 clock /8 was not respected");
    }
    hardware.advance_cycles(1u);
    if (hardware.pending_interrupts() != irq ||
        !hardware.read(iop::c_hw::k_timer5_count, 4u, &readback) ||
        readback != 0u ||
        hardware.write(iop::c_hw::k_timer5_mode, 2u, 0x58u) ||
        hardware.write(iop::c_hw::k_timer5_mode, 4u, 0x1234u) ||
        hardware.read(iop::c_hw::k_timer5_mode + 1u, 1u, &readback) ||
        hardware.write(iop::c_hw::k_timer5_target + 4u, 4u, 0u))
    {
        return fail("Timer 5 prescaled compare or unsupported MMIO safety");
    }

    // Compare all four divider modes against quotient/remainder
    // boundaries, including a large advance that needs 64-bit addition.
    constexpr uint32_t dividers[] = {1u, 8u, 16u, 256u};
    for (uint32_t mode = 0u; mode < 4u; ++mode)
    {
        hardware.reset();
        const uint32_t div = dividers[mode];
        if (!hardware.write(iop::c_hw::k_timer5_mode, 4u,
                0x58u | (mode << 13u)) ||
            !hardware.write(iop::c_hw::k_timer5_target, 4u, 0u))
        {
            return fail("Timer 5 divider setup");
        }
        hardware.advance_cycles(div - 1u);
        hardware.advance_cycles(0xffffffffu);
        const uint64_t expected = (uint64_t{div - 1u} + 0xffffffffu) / div;
        if (!hardware.read(iop::c_hw::k_timer5_count, 4u, &readback) ||
            readback != static_cast<uint32_t>(expected))
        {
            return fail("Timer 5 shift/remainder parity");
        }
    }

    hardware.reset();
    if (hardware.next_pending_interrupt() != -1 ||
        !hardware.write(iop::c_hw::k_i_mask, 4u, (1u << 2u) | (1u << 31u)) ||
        !hardware.raise_interrupt(31u) || !hardware.raise_interrupt(2u) ||
        hardware.next_pending_interrupt() != 2 ||
        !hardware.acknowledge_interrupt(2u) ||
        hardware.next_pending_interrupt() != 31 ||
        !hardware.acknowledge_interrupt(31u) ||
        hardware.next_pending_interrupt() != -1)
    {
        return fail("ordinary interrupt selection or empty fast path");
    }

    std::vector<uint8_t> rom(iop::c_cpu::k_rom0_size, 0u);
    put32(rom, 0u, 0x24080401u); // addiu t0,zero,0x401 (IM2,IEc)
    put32(rom, 4u, 0x40886000u); // mtc0 t0,Status
    put32(rom, 8u, 0x24090055u); // addiu t1,zero,0x55
    put32(rom, 12u, 0x0000000du); // break

    iop::c_kernel kernel{};
    if (!kernel.start_rom0_boot(rom) || !install_irq_vector(kernel))
    {
        return fail("Could not install ROM0 guest interrupt-vector fixture");
    }
    iop::s_kernel_run_result result = kernel.run(2u, 1u);
    if (result.m_stop != iop::e_kernel_stop::budget_exhausted ||
        kernel.cpu().state().m_pc != 0xbfc00008u ||
        kernel.cpu().state().m_cop0_status != 0x401u ||
        !kernel.hardware().write(iop::c_hw::k_i_mask, 4u, irq) ||
        !kernel.hardware().raise_interrupt(16u))
    {
        return fail("Guest did not enable COP0 IM2 / IEc");
    }
    result = kernel.run(32u, 1u);
    if (result.m_stop != iop::e_kernel_stop::breakpoint ||
        result.m_guest_pc != 0xbfc0000cu ||
        kernel.cpu().state().m_gpr[9u] != 0x55u ||
        kernel.cpu().state().m_cop0_epc != 0xbfc00008u ||
        (kernel.cpu().state().m_cop0_cause & 0x8000007cu) != 0u ||
        kernel.cpu().state().m_cop0_status != 0x401u ||
        kernel.hardware().pending_interrupts() != 0u)
    {
        return fail("Guest IRQ vector / EPC / acknowledge / RFE / resume");
    }

    // IRQ arriving immediately after a branch must not preempt its
    // delay slot. Once that instruction retires, EPC is the next PC.
    kernel.reset();
    put32(rom, 8u, 0x10000001u);  // beq zero,zero, +1
    put32(rom, 12u, 0x240b0007u); // delay: addiu t3,zero,7
    put32(rom, 16u, 0x240c0009u); // target: addiu t4,zero,9
    put32(rom, 20u, 0x0000000du);
    if (!kernel.start_rom0_boot(rom) || !install_irq_vector(kernel) ||
        kernel.run(3u, 1u).m_stop != iop::e_kernel_stop::budget_exhausted ||
        !kernel.cpu().state().m_in_delay_slot ||
        !kernel.hardware().write(iop::c_hw::k_i_mask, 4u, irq) ||
        !kernel.hardware().raise_interrupt(16u))
    {
        return fail("Could not establish branch-pending guest IRQ");
    }
    result = kernel.run(1u, 1u);
    if (result.m_stop != iop::e_kernel_stop::budget_exhausted ||
        kernel.cpu().state().m_pc != 0xbfc00010u ||
        kernel.cpu().state().m_gpr[11u] != 7u)
    {
        return fail("Hardware IRQ interrupted an unretired branch delay");
    }
    result = kernel.run(32u, 1u);
    if (result.m_stop != iop::e_kernel_stop::breakpoint ||
        kernel.cpu().state().m_cop0_epc != 0xbfc00010u ||
        (kernel.cpu().state().m_cop0_cause & 0x8000007cu) != 0u ||
        kernel.cpu().state().m_gpr[12u] != 9u)
    {
        return fail("Deferred IRQ lost the post-delay-slot return PC");
    }

    // Unlike hardware IRQs, a synchronous SYSCALL in a branch delay
    // slot must set CAUSE.BD and EPC to the preceding branch.
    kernel.reset();
    put32(rom, 0u, 0x10000001u); // beq zero,zero, +1
    put32(rom, 4u, 0x0000000cu); // syscall in delay slot
    put32(rom, 8u, 0x0000000du); // original breakpoint
    if (!kernel.start_rom0_boot(rom) ||
        !kernel.cpu().write32(0x80000080u, 0x40086800u) ||
        !kernel.cpu().write32(0x80000084u, 0x40097000u) ||
        !kernel.cpu().write32(0x80000088u, 0x0000000du))
    {
        return fail("Could not install guest syscall-vector fixture");
    }
    result = kernel.run(16u, 1u);
    if (result.m_stop != iop::e_kernel_stop::breakpoint ||
        result.m_guest_pc != 0x80000088u ||
        kernel.cpu().state().m_cop0_epc != 0xbfc00000u ||
        (kernel.cpu().state().m_cop0_cause & 0x8000007cu) !=
            (0x80000000u | (8u << 2u)))
    {
        return fail("Guest SYSCALL exception BD/EPC semantics");
    }

    std::printf("IOP Timer 5 and R3000 guest exceptions: PASS\n");
    return 0;
}
