#include "ee_strict_native_word.h"
#include "ee_memory.h"
#include "ee_exceptions.h"
#include <cstdio>
#include <initializer_list>

namespace ee
{
namespace
{
using native_entry = uint32_t (*)(s_ee_cpu_state*, s_ee_strict_native_word_context*);
using result = e_ee_strict_ram_step_result;

bool equal_cpu(const s_ee_cpu_state& a, const s_ee_cpu_state& b)
{
    if (a.m_pc != b.m_pc || a.m_exit_reason != b.m_exit_reason || a.m_pad != b.m_pad)
    {
        return false;
    }
    for (uint32_t i = 0u; i < 32u; ++i)
    {
        if (a.m_gpr[i].m_lo != b.m_gpr[i].m_lo || a.m_gpr[i].m_hi != b.m_gpr[i].m_hi ||
            a.m_cop0[i] != b.m_cop0[i])
        {
            return false;
        }
    }
    return true;
}
} // namespace

// Compare compiled LLVM execution with the strict interpreter on real
// independent RAM/TLB/CPU state; stale-code/PC guards are native-only.
extern "C" uint32_t ee_test_strict_native_word(native_entry entry, uint32_t opcode)
{
    if (entry == nullptr || !ee_strict_word_supported(opcode))
    {
        return 0u;
    }
    constexpr uint32_t pc = 0x00400000u;
    constexpr uint32_t data = pc + 0x1000u;
    constexpr uint32_t dispatch = static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH);
    constexpr uint32_t code_lo = (1u << 6u) | 0x12u;
    constexpr uint32_t data_lo = (2u << 6u) | 0x16u;
    constexpr uint32_t original = 0x80ff8180u;
    constexpr uint64_t marker = 0x1122334455667788ull;
    const bool store = (opcode >> 26u) == 0x2bu;
    const uint32_t rt = (opcode >> 16u) & 31u;
    for (uint32_t scenario = 0u; scenario < 12u; ++scenario)
    {
        ee_memory native_ram(0x3000u);
        ee_memory interpreted_ram(0x3000u);
        uint32_t lo = data_lo;
        if (scenario == 2u) lo &= ~2u;
        if (scenario == 3u) lo &= ~4u;
        if (scenario == 4u) lo = (2u << 6u) | 0x1eu;
        if (scenario == 5u) lo = (3u << 6u) | 0x16u;
        const uint32_t live_opcode = scenario == 10u ? 0u : opcode;
        for (ee_memory* memory : {&native_ram, &interpreted_ram})
        {
            if (!memory->tlb().write(0u, 0u, pc | 0xabu, code_lo, lo))
            {
                return 0u;
            }
            memory->write32(0x1000u, live_opcode);
            memory->write32(0x2000u, original);
        }
        s_ee_cpu_state cpu{};
        cpu.m_pc = pc;
        cpu.m_exit_reason = dispatch;
        cpu.m_cop0[COP0_Status] = 0x10u;
        cpu.m_cop0[COP0_EntryHi] = scenario == 6u ? 0xacu : 0xabu;
        cpu.m_cop0[COP0_Config] = 1u << 16u;
        cpu.m_gpr[2] = {marker, marker};
        cpu.m_gpr[4].m_lo = (scenario == 1u ? data + 1u :
            scenario == 7u ? 0x00600000u : data) + 4u;
        if (scenario == 11u) cpu.m_pc += 4u;
        s_ee_cpu_state reference = cpu;
        bool native_delay = true;
        bool reference_delay = true;
        const s_ee_strict_ram_evidence evidence{scenario != 8u, scenario != 9u};
        s_ee_strict_native_word_context context{&native_ram, &native_delay, evidence};
        const auto actual = static_cast<result>(entry(&cpu, &context));
        result wanted{};
        if (scenario == 10u || scenario == 11u)
        {
            wanted = scenario == 10u ? result::UNSUPPORTED_INSTRUCTION : result::UNSAFE_BOUNDARY;
        }
        else
        {
            wanted = ee_execute_strict_ram_step(reference, interpreted_ram, reference_delay, evidence);
        }
        if (actual != wanted || !equal_cpu(cpu, reference) || native_delay != reference_delay ||
            native_ram.read32(0x2000u) != interpreted_ram.read32(0x2000u) ||
            native_ram.read32(0x1000u) != live_opcode)
        {
            std::fprintf(stderr, "strict native mismatch: opcode=%08x scenario=%u actual=%u wanted=%u\n",
                opcode, scenario, static_cast<unsigned>(actual), static_cast<unsigned>(wanted));
            return 0u;
        }
        // Directed independent checks so agreement alone cannot mask a
        // shared failure to execute or a shared no-fault implementation.
        if (scenario == 0u &&
            (actual != result::COMMITTED || cpu.m_pc != pc + 4u ||
             cpu.m_gpr[2].m_hi != marker ||
             cpu.m_gpr[2].m_lo != (!store && rt == 2u ? 0xffffffff80ff8180ull : marker) ||
             native_ram.read32(0x2000u) != (store ? (rt == 0u ? 0u :
                static_cast<uint32_t>(marker)) : original)))
        {
            return 0u;
        }
        if ((scenario == 1u || scenario == 2u || scenario == 6u || scenario == 7u ||
             (scenario == 3u && store)) &&
            (actual != result::FAULT_COMMITTED || native_ram.read32(0x2000u) != original ||
             cpu.m_gpr[2].m_lo != marker))
        {
            return 0u;
        }
    }
    return 12u;
}

} // namespace ee
