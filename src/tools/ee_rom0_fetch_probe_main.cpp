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
    const auto next = ee::ee_plan_instruction_fetch(tlb, cpu.m_pc,
        cpu.m_cop0[ee::COP0_Status], cpu.m_cop0[ee::COP0_Config],
        cpu.m_cop0[ee::COP0_Context], cpu.m_cop0[ee::COP0_EntryHi]);
    const uint8_t* code = rom.fetch_span(next);
    if (code == nullptr)
    {
        return 7;
    }
    const uint32_t word = ee::ee_read_instruction_word(code);
    const auto ins = ee::decode(cpu.m_pc, word);
    std::printf("EE reset prefix stop: pc=%08x opcode=%08x decoded=%s valid=%u (one-instruction budget)\n",
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
            return 7;
        }
        const uint32_t opcode = ee::ee_read_instruction_word(look);
        const auto decoded = ee::decode(pc, opcode);
        std::printf("EE reset lookahead: pc=%08x opcode=%08x decoded=%s\n",
            pc, opcode, decoded.mnemonic);
    }
    return 0;
}
