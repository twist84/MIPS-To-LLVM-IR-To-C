#include "ee_strict_firmware_pilot.h"
#include "ee_decoder.h"

#include <array>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <string_view>
#include <vector>

namespace
{
struct s_trace
{
    uint32_t m_pc = 0u;
    uint32_t m_opcode = 0u;
    uint32_t m_next_pc = 0u;
    uint64_t m_ee_cycle = 0u;
    uint64_t m_iop_cycle = 0u;
    bool m_slot_pending = false;
};

const char* boundary_name(ee::e_ee_firmware_boundary boundary)
{
    using type = ee::e_ee_firmware_boundary;
    switch (boundary)
    {
    case type::RETIRED: return "RETIRED";
    case type::HOST_REFUSAL: return "HOST_REFUSAL";
    case type::BUDGET: return "BUDGET";
    case type::TIMER_DEADLINE: return "TIMER_DEADLINE";
    case type::INTERRUPT_ENTRY: return "INTERRUPT_ENTRY";
    case type::DEVICE_FAULT: return "DEVICE_FAULT";
    }
    return "UNKNOWN";
}
} // namespace

int main(int argc, char** argv)
{
    constexpr uint32_t default_budget = 2000000u;
    uint32_t budget = default_budget;
    if (argc < 2 || argc > 3)
    {
        std::fprintf(stderr, "usage: ee_iop_rom0_diagnostic ROM0 [EE-slot-budget]\n");
        return 1;
    }
    if (argc == 3)
    {
        const std::string_view arg(argv[2]);
        const auto parsed = std::from_chars(arg.data(), arg.data() + arg.size(), budget);
        if (parsed.ec != std::errc{} || parsed.ptr != arg.data() + arg.size() ||
            budget == 0u || budget > ee::c_ee_strict_firmware_pilot::k_slot_limit)
        {
            std::fprintf(stderr, "invalid EE slot budget\n");
            return 1;
        }
    }
    std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
    if (!file || file.tellg() != static_cast<std::streamoff>(ee::s_ee_strict_rom0_view::k_size))
    {
        std::fprintf(stderr, "ROM0 must be exactly 4 MiB\n");
        return 1;
    }
    std::vector<uint8_t> rom(ee::s_ee_strict_rom0_view::k_size);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(rom.data()), static_cast<std::streamsize>(rom.size())))
    {
        return 1;
    }

    ee::c_ee_strict_firmware_pilot pilot(rom);
    // Explicit PAL reference-machine policy, no guest CRT syscall.
    if (!pilot.attach_iop(rom, true) || !pilot.configure_scanout(3u, 0u))
    {
        std::fprintf(stderr, "dual reset setup refused\n");
        return 1;
    }
    std::printf("Dual ROM0 reset: bytes=%zu ee=%08x iop=%08x ratio=8:1 video=PAL budget=%u\n",
        rom.size(), pilot.cpu().m_pc, pilot.iop_state()->m_pc, budget);

    using boundary = ee::e_ee_firmware_boundary;
    ee::s_ee_firmware_step stop{};
    stop.m_boundary = boundary::BUDGET;
    std::array<s_trace, 64u> tail{};
    uint64_t count = 0u;
    uint64_t interrupts = 0u;
    for (; pilot.retired() < budget;)
    {
        const uint32_t pc = pilot.cpu().m_pc;
        const uint64_t before = pilot.retired();
        const uint64_t iop_before = pilot.iop_cycles();
        const auto step = pilot.step(static_cast<uint32_t>(budget - before));
        if (step.m_boundary == boundary::INTERRUPT_ENTRY)
        {
            if (pilot.retired() != before || pilot.iop_cycles() != iop_before ||
                step.m_interrupt_vector == 0u || ++interrupts > budget)
            {
                std::fprintf(stderr, "invalid zero-slot interrupt progress\n");
                return 2;
            }
            continue;
        }
        if (step.m_boundary != boundary::RETIRED)
        {
            stop = step;
            break;
        }
        if (step.m_instruction.m_retired == 0u ||
            pilot.retired() != before + step.m_instruction.m_retired ||
            pilot.retired() > budget || pilot.cycle() != pilot.retired() ||
            pilot.iop_cycles() != iop_before + step.m_iop_executed ||
            pilot.iop_cycles() + pilot.iop_debt() != pilot.cycle() / 8u)
        {
            std::fprintf(stderr, "invalid synchronized retirement accounting\n");
            return 2;
        }
        tail[count % tail.size()] = {pc, step.m_instruction.m_opcode,
            pilot.cpu().m_pc, pilot.cycle(), pilot.iop_cycles(), pilot.delay_slot_pending()};
        ++count;
        if (step.m_interrupt_vector != 0u)
        {
            ++interrupts;
        }
        if (pilot.sif_faulted())
        {
            stop.m_boundary = boundary::DEVICE_FAULT;
            break;
        }
    }
    const auto& cpu = pilot.cpu();
    const auto* iop = pilot.iop_state();
    const auto& instruction = stop.m_instruction;
    std::printf("Dual ROM0 stop: boundary=%s result=%u ee_retired=%llu ee_pc=%08x detail_pc=%08x opcode=%08x data_address=%08x slot_pending=%u\n",
        boundary_name(stop.m_boundary), static_cast<unsigned>(instruction.m_result),
        static_cast<unsigned long long>(pilot.retired()), cpu.m_pc, instruction.m_detail_pc,
        instruction.m_opcode, instruction.m_data_address, pilot.delay_slot_pending() ? 1u : 0u);
    std::printf("Dual ROM0 clocks: ee=%llu iop=%llu debt=%llu next_iop=%llu interrupts=%llu\n",
        static_cast<unsigned long long>(pilot.cycle()), static_cast<unsigned long long>(pilot.iop_cycles()),
        static_cast<unsigned long long>(pilot.iop_debt()), static_cast<unsigned long long>(pilot.next_iop_event()),
        static_cast<unsigned long long>(interrupts));
    std::printf("Dual ROM0 EE COP0: status=%08x cause=%08x epc=%08x count=%08x compare=%08x\n",
        cpu.m_cop0[ee::COP0_Status], cpu.m_cop0[ee::COP0_Cause], cpu.m_cop0[ee::COP0_EPC],
        cpu.m_cop0[ee::COP0_Count], cpu.m_cop0[ee::COP0_Compare]);
    std::printf("Dual ROM0 hardware: intc_stat=%08x intc_mask=%08x dctrl=%08x dstat=%08x str=%08x iop_istat=%08x gs_csr=%016llx\n",
        pilot.intc_status(), pilot.intc_mask(), pilot.dmac_control(), pilot.dmac_status(),
        pilot.dmac_started(), pilot.iop_irq_status(), static_cast<unsigned long long>(pilot.gs_csr()));
    std::printf("Dual ROM0 SIF: mscom=%08x smcom=%08x msflag=%08x smflag=%08x sif0_bytes=%llu sif1_bytes=%llu fifo1_words=%u\n",
        pilot.sif_register_value(1u), pilot.sif_register_value(2u), pilot.sif_register_value(3u), pilot.sif_register_value(4u),
        static_cast<unsigned long long>(pilot.sif0_delivered()), static_cast<unsigned long long>(pilot.sif1_delivered()),
        pilot.sif1_fifo_words());
    std::printf("Dual ROM0 GS reset: count=%u upper=%u siglblid=%016llx\n",
        pilot.gs_reset_count(), pilot.gs_upper_count(),
        static_cast<unsigned long long>(pilot.gs_siglblid()));
    const auto serial = pilot.serial_output();
    std::printf("Dual ROM0 serial: bytes=%zu\n", serial.size());
    // Hex encoding preserves control bytes and avoids guest text being
    // interpreted as workflow commands.
    for (size_t offset = 0u; offset < serial.size(); offset += 32u)
    {
        std::printf("Dual ROM0 serial data: offset=%zu hex=", offset);
        for (size_t i = offset; i < serial.size() && i - offset < 32u; ++i)
        {
            std::printf("%02x", static_cast<unsigned>(serial[i]));
        }
        std::printf("\n");
    }
    std::printf("Dual ROM0 IOP: pc=%08x\n", iop->m_pc);
    if (const auto& run = pilot.iop_last_run(); run.has_value())
    {
        std::printf("Dual ROM0 last IOP step: stop=%u executed=%llu pc=%08x opcode=%08x\n",
            static_cast<unsigned>(run->m_stop), static_cast<unsigned long long>(run->m_executed),
            run->m_guest_pc, run->m_opcode);
    }
    for (uint32_t reg = 0u; reg < 32u; ++reg)
    {
        std::printf("Dual ROM0 GPR: r%02u ee=%016llx iop=%08x\n", reg,
            static_cast<unsigned long long>(cpu.m_gpr[reg].m_lo), iop->m_gpr[reg]);
    }
    const uint64_t first = count > tail.size() ? count - tail.size() : 0u;
    for (uint64_t i = first; i < count; ++i)
    {
        const auto& entry = tail[i % tail.size()];
        std::printf("Dual ROM0 trace: pc=%08x opcode=%08x decoded=%s next=%08x ee=%llu iop=%llu slot=%u\n",
            entry.m_pc, entry.m_opcode, ee::decode(entry.m_pc, entry.m_opcode).mnemonic, entry.m_next_pc,
            static_cast<unsigned long long>(entry.m_ee_cycle), static_cast<unsigned long long>(entry.m_iop_cycle),
            entry.m_slot_pending ? 1u : 0u);
    }
    // A diagnostic stop is not a successful firmware boot. Only the
    // execution/accounting contract is asserted here, not progress.
    if (pilot.retired() == 0u || pilot.iop_cycles() == 0u)
    {
        std::fprintf(stderr, "no dual-domain guest progress\n");
        return 2;
    }
    return 0;
}
