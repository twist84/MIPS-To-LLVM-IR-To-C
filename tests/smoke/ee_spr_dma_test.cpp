#include "ee_hw_bus.h"
#include "ee_memory.h"
#include "ee_exceptions.h"

#include <cstdint>
#include <cstdio>

namespace
{
constexpr uint32_t k_to_spr = ee::c_ee_dmac::k_to_spr_base;
constexpr uint32_t k_from_spr = ee::c_ee_dmac::k_from_spr_base;
constexpr uint32_t k_d_stat = ee::c_ee_dmac::k_stat_address;
constexpr uint32_t k_ip3 = 1u << (ee::CAUSE_IP_SHIFT + ee::INT_DMAC);
constexpr uint32_t k_channel8 = 1u << 8u;
constexpr uint32_t k_channel9 = 1u << 9u;

int fail(const char* text)
{
    std::fprintf(stderr, "EE SPR normal DMA: %s\n", text);
    return 1;
}

bool same(const ee::s_ee_u128& a, const ee::s_ee_u128& b)
{
    return a.m_lo == b.m_lo && a.m_hi == b.m_hi;
}

void write_quad(ee::s_ee_runtime* runtime, uint32_t address, const ee::s_ee_u128& value)
{
    ee::ee_mem_write128(runtime, address, &value);
}

ee::s_ee_u128 read_quad(ee::s_ee_runtime* runtime, uint32_t address)
{
    ee::s_ee_u128 value{};
    ee::ee_mem_read128(runtime, address, &value);
    return value;
}

void program_channel(ee::s_ee_runtime* runtime, uint32_t base,
                     uint32_t madr, uint32_t qwc, uint32_t sadr)
{
    ee::ee_mem_write32(runtime, base + 0x10u, madr);
    ee::ee_mem_write32(runtime, base + 0x20u, qwc);
    ee::ee_mem_write32(runtime, base + 0x80u, sadr);
}
} // namespace

int main()
{
    ee::ee_memory memory(0x2000u);
    ee::s_ee_cpu_state cpu{};
    ee::c_ee_hw_bus hardware{};
    hardware.bind_cpu(&cpu);
    hardware.bind_memory(&memory);

    ee::s_ee_runtime runtime{};
    runtime.m_user = &memory;
    runtime.m_mmio_user = &hardware;
    runtime.m_mmio_read = &ee::c_ee_hw_bus::mmio_read;
    runtime.m_mmio_write = &ee::c_ee_hw_bus::mmio_write;

    const ee::s_ee_u128 first{0x0102030405060708ull, 0x1112131415161718ull};
    const ee::s_ee_u128 second{0x2122232425262728ull, 0x3132333435363738ull};
    const ee::s_ee_u128 sentinel{0xf0f1f2f3f4f5f6f7ull, 0xf8f9fafbfcfdfeffull};

    cpu.m_cop0[ee::COP0_Status] =
        ee::STATUS_IE | ee::STATUS_EIE | k_ip3;

    // toSPR, normal mode. The 16KiB SPR wraps from 0x3FF0 back to 0x0000.
    write_quad(&runtime, 0x100u, first);
    write_quad(&runtime, 0x110u, second);
    ee::ee_mem_write32(&runtime, k_d_stat, k_channel9 << 16u);
    program_channel(&runtime, k_to_spr, 0x100u, 2u, 0x3ff0u);
    if (ee::ee_mem_read32(&runtime, k_to_spr + 0x20u) != 2u ||
        ee::ee_mem_read32(&runtime, k_to_spr + 0x80u) != 0x3ff0u)
    {
        return fail("toSPR register programming");
    }

    ee::ee_mem_write32(&runtime, k_to_spr, ee::c_ee_dmac::k_chcr_str);
    if (!same(read_quad(&runtime, ee::ee_memory::k_scratchpad_base + 0x3ff0u), first) ||
        !same(read_quad(&runtime, ee::ee_memory::k_scratchpad_base), second) ||
        ee::ee_mem_read32(&runtime, k_to_spr + 0x10u) != 0x120u ||
        ee::ee_mem_read32(&runtime, k_to_spr + 0x20u) != 0u ||
        ee::ee_mem_read32(&runtime, k_to_spr + 0x80u) != 0x10u ||
        (ee::ee_mem_read32(&runtime, k_to_spr) & ee::c_ee_dmac::k_chcr_str) != 0u ||
        (hardware.dmac().status() & k_channel9) == 0u ||
        (cpu.m_cop0[ee::COP0_Cause] & k_ip3) == 0u)
    {
        return fail("toSPR copy, SPR wrap, channel advancement or IRQ");
    }

    // D_STAT W1C completes the interrupt acknowledgement; data is retained.
    ee::ee_mem_write32(&runtime, k_d_stat, k_channel9);
    if ((hardware.dmac().status() & k_channel9) != 0u ||
        (cpu.m_cop0[ee::COP0_Cause] & k_ip3) != 0u ||
        !same(read_quad(&runtime, ee::ee_memory::k_scratchpad_base), second))
    {
        return fail("acknowledging toSPR completion changed data or failed to lower IP3");
    }

    // fromSPR must copy those exact qwords into a different RAM location.
    // Keep channel 8 masked initially: completion is latched, IP3 is not.
    program_channel(&runtime, k_from_spr, 0x200u, 2u, 0x3ff0u);
    ee::ee_mem_write32(&runtime, k_from_spr, ee::c_ee_dmac::k_chcr_str);
    if (!same(read_quad(&runtime, 0x200u), first) ||
        !same(read_quad(&runtime, 0x210u), second) ||
        ee::ee_mem_read32(&runtime, k_from_spr + 0x10u) != 0x220u ||
        ee::ee_mem_read32(&runtime, k_from_spr + 0x20u) != 0u ||
        ee::ee_mem_read32(&runtime, k_from_spr + 0x80u) != 0x10u ||
        (ee::ee_mem_read32(&runtime, k_from_spr) & ee::c_ee_dmac::k_chcr_str) != 0u ||
        (hardware.dmac().status() & k_channel8) == 0u ||
        (cpu.m_cop0[ee::COP0_Cause] & k_ip3) != 0u)
    {
        return fail("fromSPR transfer, register advancement or masked completion");
    }

    ee::ee_mem_write32(&runtime, k_d_stat, k_channel8 << 16u);
    if ((cpu.m_cop0[ee::COP0_Cause] & k_ip3) == 0u ||
        hardware.dmac().pending() != k_channel8)
    {
        return fail("enabling pending fromSPR completion did not raise IP3");
    }
    ee::ee_mem_write32(&runtime, k_d_stat, k_channel8);
    if ((cpu.m_cop0[ee::COP0_Cause] & k_ip3) != 0u)
    {
        return fail("fromSPR ACK did not lower IP3");
    }

    // The original hardware accepts neither channel's MADR.SPR bit.
    // It reads back clear and the transfer still targets physical RAM.
    program_channel(&runtime, k_to_spr, 0x80000100u, 2u, 0x3ff0u);
    if (ee::ee_mem_read32(&runtime, k_to_spr + 0x10u) != 0x100u)
    {
        return fail("toSPR MADR.SPR bit retained");
    }
    ee::ee_mem_write32(&runtime, k_to_spr, ee::c_ee_dmac::k_chcr_str);
    program_channel(&runtime, k_from_spr, 0x80000400u, 2u, 0x3ff0u);
    if (ee::ee_mem_read32(&runtime, k_from_spr + 0x10u) != 0x400u)
    {
        return fail("fromSPR MADR.SPR bit retained");
    }
    ee::ee_mem_write32(&runtime, k_from_spr, ee::c_ee_dmac::k_chcr_str);
    if (!same(read_quad(&runtime, 0x400u), first) ||
        !same(read_quad(&runtime, 0x410u), second) ||
        ee::ee_mem_read32(&runtime, k_to_spr + 0x10u) != 0x120u ||
        ee::ee_mem_read32(&runtime, k_from_spr + 0x10u) != 0x420u ||
        ee::ee_mem_read32(&runtime, k_to_spr + 0x20u) != 0u ||
        ee::ee_mem_read32(&runtime, k_from_spr + 0x20u) != 0u ||
        (ee::ee_mem_read32(&runtime, k_to_spr) & ee::c_ee_dmac::k_chcr_str) != 0u ||
        (ee::ee_mem_read32(&runtime, k_from_spr) & ee::c_ee_dmac::k_chcr_str) != 0u ||
        (hardware.dmac().status() & (k_channel8 | k_channel9)) != (k_channel8 | k_channel9))
    {
        return fail("MADR.SPR mask transfer did not copy and complete both directions");
    }
    ee::ee_mem_write32(&runtime, k_d_stat, k_channel8 | k_channel9);

    // Invalid length: entire RAM range must be backed before transferring.
    // Neither the last valid RAM quad nor any channel state may advance.
    write_quad(&runtime, 0x1ff0u, sentinel);
    write_quad(&runtime, ee::ee_memory::k_scratchpad_base + 0x20u, first);
    program_channel(&runtime, k_from_spr, 0x1ff0u, 2u, 0x20u);
    const uint32_t before_failure = hardware.dmac().status();
    ee::ee_mem_write32(&runtime, k_from_spr, ee::c_ee_dmac::k_chcr_str);
    if (!same(read_quad(&runtime, 0x1ff0u), sentinel) ||
        ee::ee_mem_read32(&runtime, k_from_spr + 0x10u) != 0x1ff0u ||
        ee::ee_mem_read32(&runtime, k_from_spr + 0x20u) != 2u ||
        ee::ee_mem_read32(&runtime, k_from_spr + 0x80u) != 0x20u ||
        (ee::ee_mem_read32(&runtime, k_from_spr) & ee::c_ee_dmac::k_chcr_str) == 0u ||
        hardware.dmac().status() != before_failure)
    {
        return fail("out-of-range DMA partially copied or falsely completed");
    }

    // Clear STR and try unsupported chain and interleave modes. Both must
    // remain visibly busy instead of silently claiming transfer success.
    ee::ee_mem_write32(&runtime, k_from_spr, 0u);
    ee::ee_mem_write32(&runtime, k_from_spr, ee::c_ee_dmac::k_chcr_str | (1u << 2u));
    if ((ee::ee_mem_read32(&runtime, k_from_spr) & ee::c_ee_dmac::k_chcr_str) == 0u ||
        hardware.dmac().status() != before_failure)
    {
        return fail("unsupported chain mode completed");
    }
    ee::ee_mem_write32(&runtime, k_from_spr, 0u);
    ee::ee_mem_write32(&runtime, k_from_spr, ee::c_ee_dmac::k_chcr_str | (2u << 2u));
    if ((ee::ee_mem_read32(&runtime, k_from_spr) & ee::c_ee_dmac::k_chcr_str) == 0u ||
        hardware.dmac().status() != before_failure)
    {
        return fail("unsupported interleave mode completed");
    }

    // Address alignment is enforced instead of silently rounding down.
    ee::ee_mem_write32(&runtime, k_from_spr, 0u);
    program_channel(&runtime, k_from_spr, 0x203u, 1u, 0x20u);
    ee::ee_mem_write32(&runtime, k_from_spr, ee::c_ee_dmac::k_chcr_str);
    if (ee::ee_mem_read32(&runtime, k_from_spr + 0x10u) != 0x203u ||
        hardware.dmac().status() != before_failure)
    {
        return fail("misaligned DMA pointer incorrectly accepted");
    }

    // No memory backing must fail closed. Restoring the backing and STR
    // must let the same channel be used again.
    ee::ee_mem_write32(&runtime, k_from_spr, 0u);
    hardware.bind_memory(nullptr);
    program_channel(&runtime, k_from_spr, 0x300u, 1u, 0x20u);
    ee::ee_mem_write32(&runtime, k_from_spr, ee::c_ee_dmac::k_chcr_str);
    if ((ee::ee_mem_read32(&runtime, k_from_spr) & ee::c_ee_dmac::k_chcr_str) == 0u ||
        hardware.dmac().status() != before_failure)
    {
        return fail("unbound DMA memory falsely completed");
    }
    hardware.bind_memory(&memory);
    ee::ee_mem_write32(&runtime, k_from_spr, 0u);
    ee::ee_mem_write32(&runtime, k_from_spr, ee::c_ee_dmac::k_chcr_str);
    if (!same(read_quad(&runtime, 0x300u), first) ||
        ee::ee_mem_read32(&runtime, k_from_spr + 0x10u) != 0x310u ||
        ee::ee_mem_read32(&runtime, k_from_spr + 0x20u) != 0u ||
        (hardware.dmac().status() & k_channel8) == 0u)
    {
        return fail("failed channel could not recover after rebinding memory");
    }

    // Both channel MMIO windows must support KSEG1 aliases when enabled.
    memory.set_kseg_ram_aliases(true);
    ee::ee_mem_write32(&runtime, 0xb000d480u, 0xffffffffu);
    if (ee::ee_mem_read32(&runtime, k_to_spr + 0x80u) != 0x3ff0u ||
        ee::ee_mem_read32(&runtime, 0xb000d480u) != 0x3ff0u)
    {
        return fail("toSPR SADDR mask or physical alias");
    }

    // Unsupported register offsets/widths do not mutate channel state.
    const uint32_t before_sadr = ee::ee_mem_read32(&runtime, k_to_spr + 0x80u);
    ee::ee_mem_write16(&runtime, k_to_spr + 0x80u, 0u);
    ee::ee_mem_write32(&runtime, k_to_spr + 0x60u, 0xffffffffu);
    if (ee::ee_mem_read32(&runtime, k_to_spr + 0x60u) != 0u ||
        ee::ee_mem_read16(&runtime, k_to_spr + 0x80u) != 0u ||
        ee::ee_mem_read32(&runtime, k_to_spr + 0x80u) != before_sadr)
    {
        return fail("unsupported SPR channel registers or widths accepted");
    }

    hardware.reset();
    if (ee::ee_mem_read32(&runtime, k_to_spr) != 0u ||
        ee::ee_mem_read32(&runtime, k_from_spr + 0x20u) != 0u ||
        hardware.dmac().status() != 0u ||
        (cpu.m_cop0[ee::COP0_Cause] & k_ip3) != 0u)
    {
        return fail("DMAC reset left channel registers or interrupt state");
    }

    std::puts("EE normal-mode toSPR/fromSPR DMA + wrap + D_STAT: PASS");
    return 0;
}
