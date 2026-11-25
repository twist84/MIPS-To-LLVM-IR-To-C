#include "ee_hw_bus.h"
#include "ee_memory.h"
#include "ee_exceptions.h"

#include <cstdint>
#include <cstdio>

namespace
{
int fail(const char* text)
{
    std::fprintf(stderr, "EE DMAC status: %s\n", text);
    return 1;
}
} // namespace

int main()
{
    ee::s_ee_cpu_state cpu{};
    ee::ee_memory ram(4096);
    ee::c_ee_hw_bus hardware;
    hardware.bind_cpu(&cpu);

    ee::s_ee_runtime runtime{};
    runtime.m_user = &ram;
    runtime.m_mmio_user = &hardware;
    runtime.m_mmio_read = &ee::c_ee_hw_bus::mmio_read;
    runtime.m_mmio_write = &ee::c_ee_hw_bus::mmio_write;

    constexpr uint32_t d_stat = ee::c_ee_dmac::k_stat_address;
    constexpr uint32_t i_stat = ee::c_ee_intc::k_stat_address;
    constexpr uint32_t i_mask = ee::c_ee_intc::k_mask_address;
    constexpr uint32_t dmac_ip = 1u << (ee::CAUSE_IP_SHIFT + ee::INT_DMAC);
    constexpr uint32_t intc_ip = 1u << (ee::CAUSE_IP_SHIFT + ee::INT_INTC);
    constexpr uint32_t ch1 = 1u << 1;
    constexpr uint32_t ch8 = 1u << 8;
    constexpr uint32_t d_pcr = ee::c_ee_dmac::k_pcr_address;

    if (ee::ee_cop0_get_branch_condition(nullptr) != 2u ||
        ee::ee_cop0_get_branch_condition(&runtime) != 1u)
    {
        return fail("BC0 missing bus or empty selection");
    }
    ee::ee_mem_write32(&runtime, d_pcr, ch1 | ch8);
    if (ee::ee_mem_read32(&runtime, d_pcr) != (ch1 | ch8) ||
        ee::ee_cop0_get_branch_condition(&runtime) != 0u ||
        !hardware.dmac().raise(1u) ||
        ee::ee_cop0_get_branch_condition(&runtime) != 0u ||
        !hardware.dmac().raise(8u) ||
        ee::ee_cop0_get_branch_condition(&runtime) != 1u ||
        hardware.dmac().pending() != 0u)
    {
        return fail("BC0 requires all selected completions independent of CIM");
    }
    ee::ee_mem_write32(&runtime, d_stat, ch1);
    if (ee::ee_cop0_get_branch_condition(&runtime) != 0u)
    {
        return fail("BC0 must observe W1C completion");
    }
    ee::ee_mem_write32(&runtime, d_pcr, ch8);
    if (ee::ee_cop0_get_branch_condition(&runtime) != 1u)
    {
        return fail("BC0 must observe CPC changes");
    }
    hardware.dmac().reset();

    cpu.m_cop0[ee::COP0_Status] = ee::STATUS_IE | ee::STATUS_EIE |
                                   dmac_ip | intc_ip;
    if (ee::ee_mem_read32(&runtime, d_stat) != 0 ||
        hardware.dmac().pending() != 0 ||
        hardware.dmac().started_channels() != 0u ||
        (cpu.m_cop0[ee::COP0_Cause] & (dmac_ip | intc_ip)) != 0)
    {
        return fail("reset DMAC status or CPU cause");
    }

    if (!hardware.dmac().raise(1u) || hardware.dmac().raise(10u) ||
        hardware.dmac().status() != ch1 ||
        hardware.dmac().pending() != 0 ||
        ee::ee_interrupts_enabled(&cpu))
    {
        return fail("unmasked completion raised an interrupt");
    }

    // D_STAT upper half toggles masks, not overwrites them.
    ee::ee_mem_write32(&runtime, d_stat, ch1 << 16u);
    if (ee::ee_mem_read32(&runtime, d_stat) != (ch1 | (ch1 << 16u)) ||
        hardware.dmac().pending() != ch1 ||
        (cpu.m_cop0[ee::COP0_Cause] & dmac_ip) == 0 ||
        !ee::ee_interrupts_enabled(&cpu))
    {
        return fail("enabling masked pending DMA completion");
    }

    // Direct physical aliases are opt-in; the fixture must enable them
    // before touching the KSEG1 address.
    ram.set_kseg_ram_aliases(true);
    ee::ee_mem_write32(&runtime, 0xb000e010u, ch1 << 16u);
    if (hardware.dmac().status() != ch1 ||
        hardware.dmac().pending() != 0 ||
        (cpu.m_cop0[ee::COP0_Cause] & dmac_ip) != 0)
    {
        return fail("DMA mask did not toggle off through KSEG1 alias");
    }

    // Acknowledge source while keeping a different source pending.
    hardware.dmac().raise(8u);
    ee::ee_mem_write32(&runtime, d_stat, (ch1 | ch8) << 16u);
    ee::ee_mem_write32(&runtime, d_stat, ch1);
    if (ee::ee_mem_read32(&runtime, d_stat) !=
            (ch8 | ((ch1 | ch8) << 16u)) ||
        hardware.dmac().pending() != ch8 ||
        (cpu.m_cop0[ee::COP0_Cause] & dmac_ip) == 0)
    {
        return fail("DMA write-one-to-clear status or independent sources");
    }

    // INTC and DMAC must share the bus but use different COP0 IP bits.
    hardware.intc().raise(2);
    ee::ee_mem_write32(&runtime, i_mask, 1u << 2);
    if (ee::ee_mem_read32(&runtime, i_stat) != 1u << 2 ||
        (cpu.m_cop0[ee::COP0_Cause] & (dmac_ip | intc_ip)) !=
            (dmac_ip | intc_ip))
    {
        return fail("DMAC/INTC bus dispatch or simultaneous pending bits");
    }

    ee::ee_mem_write32(&runtime, d_stat, ch8);
    if ((cpu.m_cop0[ee::COP0_Cause] & dmac_ip) != 0 ||
        (cpu.m_cop0[ee::COP0_Cause] & intc_ip) == 0)
    {
        return fail("DMA acknowledgement cleared unrelated INTC interrupt");
    }
    ee::ee_mem_write32(&runtime, i_stat, 1u << 2);
    if ((cpu.m_cop0[ee::COP0_Cause] & (dmac_ip | intc_ip)) != 0)
    {
        return fail("INTC acknowledgement did not clear its separate line");
    }

    // VIF1 register state is now backed, but a missing host VIF
    // consumer must not generate fake DMA completion or consume QWC.
    ee::ee_mem_write32(&runtime, 0x10009020u, 0x10u);
    ee::ee_mem_write32(&runtime, 0x10009000u, 0x101u);
    if (ee::ee_mem_read32(&runtime, 0x10009000u) != 0x101u ||
        ee::ee_mem_read32(&runtime, 0x10009020u) != 0x10u ||
        hardware.dmac().status() != ((ch1 | ch8) << 16u) ||
        hardware.dmac().started_channels() != ch1 || hardware.dmac().pending() != 0u)
    {
        return fail("missing VIF1 consumer falsely completed DMA");
    }

    // SIF0 DMA channel 5 has register state but no host IOP producer.
    // CHCR.STR must remain set; enabling chain mode alone cannot
    // invent bytes, a DMA completion, or a pending interrupt.
    constexpr uint32_t sif0 = ee::c_ee_dmac::k_sif0_base;
    const uint32_t status_before_sif0 = hardware.dmac().status();
    ee::ee_mem_write32(&runtime, sif0 + 0x10u, 0x00123000u);
    ee::ee_mem_write32(&runtime, sif0 + 0x20u, 0x12345u);
    ee::ee_mem_write32(&runtime, sif0, 0x184u);
    if (ee::ee_mem_read32(&runtime, sif0) != 0x184u ||
        ee::ee_mem_read32(&runtime, sif0 + 0x10u) != 0x00123000u ||
        ee::ee_mem_read32(&runtime, sif0 + 0x20u) != 0x2345u ||
        hardware.dmac().status() != status_before_sif0 ||
        hardware.dmac().started_channels() != (ch1 | (1u << 5u)) ||
        hardware.dmac().sif0_receive_armed_only())
    {
        return fail("SIF0 register bank or unimplemented IOP completion");
    }

    // The SIF0 channel has no scratchpad address register.
    ee::s_ee_u128 unsupported_sif0{};
    if (ee::c_ee_hw_bus::mmio_read(
            &hardware, sif0 + 0x80u, 4u, &unsupported_sif0) ||
        ee::c_ee_hw_bus::mmio_write(
            &hardware, sif0 + 0x80u, 4u, &unsupported_sif0))
    {
        return fail("SIF0 incorrectly exposes SPR-only SADR");
    }

    ee::ee_mem_write32(&runtime, sif0 + 0x20u, 0u);
    ee::ee_mem_write32(&runtime, sif0, 0x184u);
    if (ee::ee_mem_read32(&runtime, sif0 + 0x20u) != 0u ||
        ee::ee_mem_read32(&runtime, sif0) != 0x184u ||
        hardware.dmac().status() != status_before_sif0)
    {
        return fail("SifSetDChain SIF0 QWC/CHCR reinitialization");
    }

    // A cleared STR removes only that channel from the active mask.
    ee::ee_mem_write32(&runtime, 0x10009000u, 0u);
    if (hardware.dmac().started_channels() != (1u << 5u))
    {
        return fail("clearing VIF1 STR did not remove its active bit");
    }

    // Passive receive readiness is narrower than STR: a real FIFO
    // callback, unacknowledged raw completion or residual data blocks it.
    if (!hardware.dmac().sif0_receive_armed_only())
    {
        return fail("empty HLE SIF0 receiver not recognized");
    }
    uint32_t starts = 0u;
    hardware.dmac().bind_sif0_start(&starts, [](void* user)
    {
        ++*static_cast<uint32_t*>(user);
    });
    if (hardware.dmac().sif0_receive_armed_only() || starts != 0u)
    {
        return fail("genuine SIF0 transport or query side effect");
    }
    hardware.dmac().bind_sif0_start(nullptr, nullptr);
    if (!hardware.dmac().raise(5u) ||
        hardware.dmac().sif0_receive_armed_only())
    {
        return fail("raw SIF0 completion misclassified as passive");
    }
    hardware.dmac().acknowledge(5u);
    if (!hardware.dmac().sif0_receive_armed_only())
    {
        return fail("acknowledged SIF0 receiver not recognized");
    }
    ee::ee_mem_write32(&runtime, sif0, 0u);
    if (hardware.dmac().sif0_receive_armed_only())
    {
        return fail("stopped SIF0 misclassified as armed");
    }
    ee::ee_mem_write32(&runtime, sif0, 0x100u);
    if (hardware.dmac().sif0_receive_armed_only())
    {
        return fail("normal SIF0 mode misclassified as HLE chain readiness");
    }
    ee::ee_mem_write32(&runtime, sif0, 0u);
    ee::ee_mem_write32(&runtime, sif0, 0x184u);

    // The SIF mailbox MMIO and SifSetReg/GetReg share one state owner.
    constexpr uint32_t mscom = ee::c_ee_sif::k_mscom_address;
    constexpr uint32_t smcom = ee::c_ee_sif::k_smcom_address;
    constexpr uint32_t msflag = ee::c_ee_sif::k_msflag_address;
    constexpr uint32_t smflag = ee::c_ee_sif::k_smflag_address;
    constexpr uint32_t sysreg = ee::c_ee_sif::k_sysreg_base;
    uint32_t sif_result = 0xffffffffu;

    if (!hardware.sif().get_reg(sysreg, &sif_result) ||
        sif_result != 0u ||
        !hardware.sif().get_reg(sysreg + 2u, &sif_result) ||
        sif_result != 0u ||
        ee::ee_mem_read32(&runtime, smcom) != 0u ||
        ee::ee_mem_read32(&runtime, smflag) != 0u)
    {
        return fail("SIF initially fabricated an IOP or RPC bootstrap");
    }

    ee::ee_mem_write32(&runtime, mscom, 0x001abc00u);
    ee::ee_mem_write32(&runtime, msflag, 0x00010000u);
    ee::ee_mem_write32(&runtime, 0xb000f220u, 0x00020000u);
    if (!hardware.sif().get_reg(1u, &sif_result) ||
        sif_result != 0x001abc00u ||
        ee::ee_mem_read32(&runtime, mscom) != 0x001abc00u ||
        ee::ee_mem_read32(&runtime, msflag) != 0x00030000u ||
        !hardware.sif().get_reg(3u, &sif_result) ||
        sif_result != 0x00030000u)
    {
        return fail("SIF MSCOM/MSFLAG MMIO and syscall state disagree");
    }

    hardware.sif().publish_iop_subaddr(0x0001e000u);
    hardware.sif().publish_iop_smflag(0x00060000u);
    ee::ee_mem_write32(&runtime, smcom, 0x00bad000u); // EE cannot write IOP mailbox.
    ee::ee_mem_write32(&runtime, smflag, 0x00020000u); // EE clears IOP flags.
    if (ee::ee_mem_read32(&runtime, smcom) != 0x0001e000u ||
        ee::ee_mem_read32(&runtime, smflag) != 0x00040000u ||
        !hardware.sif().get_reg(2u, &sif_result) ||
        sif_result != 0x0001e000u ||
        !hardware.sif().get_reg(4u, &sif_result) ||
        sif_result != 0x00040000u)
    {
        return fail("SIF IOP-only mailbox or SMFLAG clear semantics");
    }

    if (!hardware.sif().set_reg(sysreg, 0x001fe000u) ||
        !hardware.sif().set_reg(sysreg + 1u, 0x001df000u) ||
        !hardware.sif().set_reg(sysreg + 2u, 1u) ||
        !hardware.sif().get_reg(sysreg, &sif_result) ||
        sif_result != 0x001fe000u ||
        !hardware.sif().get_reg(sysreg + 2u, &sif_result) ||
        sif_result != 1u)
    {
        return fail("SIF kernel system register read/write");
    }

    if (hardware.sif().get_reg(0x80000003u, &sif_result) ||
        hardware.sif().set_reg(0x80000003u, 1u) ||
        hardware.sif().get_reg(2u, nullptr) ||
        hardware.sif().set_reg(2u, 0x00bad000u) ||
        ee::c_ee_hw_bus::mmio_write(
            &hardware, smcom, 4u, &unsupported_sif0))
    {
        return fail("unimplemented SIF register or IOP-only EE write");
    }

    ee::ee_mem_write16(&runtime, mscom, 0xffffu);
    ee::ee_mem_write64(&runtime, mscom, ~uint64_t{0});
    if (ee::ee_mem_read32(&runtime, mscom) != 0x001abc00u ||
        ee::ee_mem_read16(&runtime, mscom) != 0u ||
        ee::ee_mem_read64(&runtime, mscom) != 0u)
    {
        return fail("unsupported SIF MMIO width modified mailbox");
    }

    // Only 32-bit D_STAT accesses are supported at this milestone.
    ee::ee_mem_write8(&runtime, d_stat, 0xffu);
    ee::ee_mem_write16(&runtime, d_stat, 0xffffu);
    ee::ee_mem_write64(&runtime, d_stat, ~uint64_t{0});
    ee::s_ee_u128 all_ones{~uint64_t{0}, ~uint64_t{0}};
    ee::ee_mem_write128(&runtime, d_stat, &all_ones);
    ee::ee_mem_read128(&runtime, d_stat, &all_ones);
    if (hardware.dmac().status() != ((ch1 | ch8) << 16u) ||
        ee::ee_mem_read8(&runtime, d_stat) != 0 ||
        ee::ee_mem_read16(&runtime, d_stat) != 0 ||
        ee::ee_mem_read64(&runtime, d_stat) != 0 ||
        all_ones.m_lo != 0 || all_ones.m_hi != 0)
    {
        return fail("unsupported access widths mutated D_STAT");
    }

    if (!hardware.dmac().acknowledge(1u) ||
        hardware.dmac().acknowledge(10u))
    {
        return fail("DMA channel ack validation");
    }

    ee::s_ee_cpu_state next_cpu{};
    hardware.dmac().raise(8);
    hardware.bind_cpu(&next_cpu);
    if ((cpu.m_cop0[ee::COP0_Cause] & dmac_ip) != 0 ||
        (next_cpu.m_cop0[ee::COP0_Cause] & dmac_ip) == 0)
    {
        return fail("CPU rebind did not migrate the pending DMA IP3 bit");
    }

    hardware.reset();
    if (hardware.dmac().status() != 0 ||
        hardware.dmac().pending() != 0 ||
        hardware.dmac().started_channels() != 0u ||
        hardware.intc().status() != 0 ||
        ee::ee_mem_read32(&runtime, sif0) != 0u ||
        ee::ee_mem_read32(&runtime, sif0 + 0x20u) != 0u ||
        ee::ee_mem_read32(&runtime, mscom) != 0u ||
        ee::ee_mem_read32(&runtime, smcom) != 0u ||
        ee::ee_mem_read32(&runtime, msflag) != 0u ||
        ee::ee_mem_read32(&runtime, smflag) != 0u ||
        !hardware.sif().get_reg(sysreg, &sif_result) ||
        sif_result != 0u ||
        !hardware.sif().get_reg(sysreg + 2u, &sif_result) ||
        sif_result != 0u ||
        (next_cpu.m_cop0[ee::COP0_Cause] & (dmac_ip | intc_ip)) != 0)
    {
        return fail("hardware reset did not release pending interrupts");
    }

    std::puts("EE D_STAT / status mask / COP0 IP3: PASS");
    return 0;
}
