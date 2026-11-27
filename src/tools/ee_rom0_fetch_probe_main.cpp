#include "ee_strict_reset.h"
#include "ee_decoder.h"
#include "ee_exceptions.h"
#include <cstdio>
#include <fstream>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: ee_rom0_fetch_probe ROM0.bin\n");
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
    if (!file || file.tellg() != static_cast<std::streamoff>(ee::s_ee_strict_rom0_view::k_size))
    {
        std::fprintf(stderr, "expected exactly 4 MiB ROM0\n");
        return 3;
    }
    std::vector<uint8_t> bytes(ee::s_ee_strict_rom0_view::k_size);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
    {
        return 3;
    }
    const ee::s_ee_strict_rom0_view rom{bytes.data(), bytes.size()};
    const ee::c_ee_tlb tlb{};
    // Classification only: no full reset initialization or retirement.
    constexpr uint32_t status = ee::STATUS_BEV | ee::STATUS_ERL;
    constexpr uint32_t pcs[] = {0xbfc00000u, ee::VEC_TLB_REFILL_BEV1, ee::VEC_COMMON_BEV1};
    for (const uint32_t pc : pcs)
    {
        const auto fetch = ee::ee_plan_instruction_fetch(tlb, pc, status, 0u, 0u, 0u);
        const uint8_t* code = rom.fetch_span(fetch);
        if (code != bytes.data() + (pc - 0xbfc00000u))
        {
            return 4;
        }
        const uint32_t word = ee::ee_read_instruction_word(code);
        const auto ins = ee::decode(pc, word);
        std::printf("EE ROM0 fetch: pc=%08x physical=%08x word=%08x decoded=%s valid=%u\n",
            pc, fetch.m_access.m_physical_address, word, ins.mnemonic,
            static_cast<unsigned>(ins.valid));
    }
    std::puts("EE ROM0 fetch probe: PASS (no instructions retired during fetch probe)");
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    const auto step = ee::ee_execute_strict_reset_prefix(cpu, rom);
    if (step.m_result != ee::e_ee_reset_prefix_result::COMMITTED)
    {
        std::fprintf(stderr, "EE reset prefix refused: result=%u opcode=%08x\n",
            static_cast<unsigned>(step.m_result), step.m_opcode);
        return 5;
    }
    if (cpu.m_pc != 0xbfc00004u || cpu.m_cop0[ee::COP0_Status] != 0u)
    {
        return 6;
    }
    std::printf("EE reset prefix: retired=1 pc=%08x status=%08x opcode=%08x\n",
        cpu.m_pc, cpu.m_cop0[ee::COP0_Status], step.m_opcode);
    // Twelve actual retirements include the initial Status write and
    // the selected EE path, skipping the unexecuted IOP setup.
    constexpr uint32_t budget = 12u;
    uint32_t retired = step.m_retired;
    while (retired < budget)
    {
        const uint32_t pc = cpu.m_pc;
        const auto action = ee::ee_execute_strict_rom_register_step(cpu, rom, budget - retired);
        if (action.m_result != ee::e_ee_reset_prefix_result::COMMITTED ||
            action.m_retired == 0u || action.m_retired > budget - retired)
        {
            std::fprintf(stderr, "EE reset register stop: pc=%08x opcode=%08x result=%u retired=%u\n",
                pc, action.m_opcode, static_cast<unsigned>(action.m_result), retired);
            return 8;
        }
        retired += action.m_retired;
        std::printf("EE reset register step: pc=%08x opcode=%08x count=%u next=%08x total=%u\n",
            pc, action.m_opcode, action.m_retired, cpu.m_pc, retired);
    }
    if (retired != budget || cpu.m_pc != 0xbfc00044u ||
        cpu.m_gpr[8].m_lo != 0x2e00u || cpu.m_gpr[9].m_lo != 0x2e00u ||
        cpu.m_gpr[24].m_lo != 0xffffffffa0010000ull ||
        cpu.m_gpr[15].m_lo != 0xffffffffbfc0012cull)
    {
        std::fprintf(stderr, "EE processor-selection state mismatch\n");
        return 9;
    }
    std::printf("EE reset selection: retired=%u pc=%08x prid=%08x t8=%016llx t7=%016llx\n",
        retired, cpu.m_pc, cpu.m_cop0[ee::COP0_PRId],
        static_cast<unsigned long long>(cpu.m_gpr[24].m_lo),
        static_cast<unsigned long long>(cpu.m_gpr[15].m_lo));
    // Check the first actual ROM data read and the resulting guest
    // branch independently of the interpreter's own decode.
    constexpr uint32_t read_checkpoint = 20u;
    while (retired < read_checkpoint)
    {
        const auto action = ee::ee_execute_strict_rom_register_step(cpu, rom,
            read_checkpoint - retired);
        if (action.m_result != ee::e_ee_reset_prefix_result::COMMITTED ||
            action.m_retired == 0u || action.m_retired > read_checkpoint - retired)
        {
            std::fprintf(stderr, "EE reset first ROM read refused: pc=%08x opcode=%08x result=%u retired=%u\n",
                cpu.m_pc, action.m_opcode, static_cast<unsigned>(action.m_result), retired);
            return 10;
        }
        retired += action.m_retired;
    }
    const uint32_t first_byte = bytes[0x1000u];
    if (cpu.m_gpr[8].m_lo != 0xffffffffbfc01000ull ||
        cpu.m_gpr[9].m_lo != 0xffffffffbfc00000ull ||
        cpu.m_gpr[10].m_lo != first_byte || cpu.m_gpr[14].m_lo != 0x20u ||
        cpu.m_pc != (first_byte == 0u ? 0xbfc000bcu : 0xbfc00064u))
    {
        std::fprintf(stderr, "EE first ROM data read state mismatch\n");
        return 11;
    }
    std::printf("EE reset ROM read: retired=%u address=bfc01000 byte=%02x next=%08x\n",
        retired, first_byte, cpu.m_pc);

    // Explore the actual supported prefix, not an invented device
    // response. Preserve a small committed tail for diagnosis.
    constexpr uint32_t exploration_budget = 65536u;
    struct s_trace
    {
        uint32_t m_pc;
        uint32_t m_opcode;
        uint32_t m_count;
        uint32_t m_next;
        uint32_t m_total;
    };
    constexpr uint32_t tail_capacity = 32u;
    s_trace tail[tail_capacity]{};
    uint32_t trace_count = 0u;
    ee::s_ee_reset_prefix_step stop{ee::e_ee_reset_prefix_result::BUDGET_EXHAUSTED};
    for (; retired < exploration_budget;)
    {
        const uint32_t pc = cpu.m_pc;
        const auto action = ee::ee_execute_strict_rom_register_step(cpu, rom,
            exploration_budget - retired);
        if (action.m_result != ee::e_ee_reset_prefix_result::COMMITTED)
        {
            stop = action;
            break;
        }
        if (action.m_retired == 0u || action.m_retired > exploration_budget - retired)
        {
            return 12;
        }
        retired += action.m_retired;
        tail[trace_count % tail_capacity] = {pc, action.m_opcode, action.m_retired, cpu.m_pc, retired};
        ++trace_count;
    }
    const uint32_t first_trace = trace_count > tail_capacity ? trace_count - tail_capacity : 0u;
    for (uint32_t i = first_trace; i < trace_count; ++i)
    {
        const auto& item = tail[i % tail_capacity];
        std::printf("EE reset trace: pc=%08x opcode=%08x count=%u next=%08x total=%u\n",
            item.m_pc, item.m_opcode, item.m_count, item.m_next, item.m_total);
    }
    std::printf("EE reset exploration stop: retired=%u pc=%08x result=%u detail_pc=%08x opcode=%08x data_address=%08x\n",
        retired, cpu.m_pc, static_cast<unsigned>(stop.m_result), stop.m_detail_pc,
        stop.m_opcode, stop.m_data_address);
    for (uint32_t i = 0u; i < 32u; ++i)
    {
        std::printf("EE reset GPR: r%02u=%016llx\n", i,
            static_cast<unsigned long long>(i == 0u ? 0u : cpu.m_gpr[i].m_lo));
    }
    const auto next = ee::ee_plan_instruction_fetch(tlb, cpu.m_pc,
        cpu.m_cop0[ee::COP0_Status], cpu.m_cop0[ee::COP0_Config],
        cpu.m_cop0[ee::COP0_Context], cpu.m_cop0[ee::COP0_EntryHi]);
    const uint8_t* code = rom.fetch_span(next);
    if (code == nullptr)
    {
        std::puts("EE reset lookahead unavailable at committed stop");
        return 0;
    }
    const uint32_t word = ee::ee_read_instruction_word(code);
    const auto ins = ee::decode(cpu.m_pc, word);
    std::printf("EE reset prefix stop: pc=%08x opcode=%08x decoded=%s valid=%u (diagnostic host stop)\n",
        cpu.m_pc, word, ins.mnemonic, static_cast<unsigned>(ins.valid));
    // Capture a bounded lookahead for the next implementation batch;
    // these words are fetched and decoded only, never executed.
    for (uint32_t i = 1u; i < 16u; ++i)
    {
        const uint32_t pc = cpu.m_pc + i * 4u;
        const auto plan = ee::ee_plan_instruction_fetch(tlb, pc, 0u, 0u, 0u, 0u);
        const uint8_t* look = rom.fetch_span(plan);
        if (look == nullptr)
        {
            break;
        }
        const uint32_t opcode = ee::ee_read_instruction_word(look);
        const auto decoded = ee::decode(pc, opcode);
        std::printf("EE reset lookahead: pc=%08x opcode=%08x decoded=%s\n",
            pc, opcode, decoded.mnemonic);
    }
    return 0;
}
