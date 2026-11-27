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
    const auto transfer = ee_decode_strict_transfer(opcode);
    if (entry == nullptr || transfer.m_width == 0u)
    {
        return 0u;
    }
    constexpr uint32_t pc = 0x00400000u;
    constexpr uint32_t data = pc + 0x1000u;
    constexpr uint32_t dispatch = static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH);
    constexpr uint32_t code_lo = (1u << 6u) | 0x12u;
    constexpr uint32_t data_lo = (2u << 6u) | 0x16u;
    constexpr s_ee_u128 original{0x8877665580ff8180ull, 0xffeeddccbbaa9988ull};
    constexpr uint64_t marker = 0x1122334455667788ull;
    const bool store = transfer.m_store;
    const uint32_t rt = (opcode >> 16u) & 31u;
    const uint32_t scenarios = 12u + transfer.m_width;
    for (uint32_t scenario = 0u; scenario < scenarios; ++scenario)
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
            memory->write128(0x2000u, &original);
        }
        s_ee_cpu_state cpu{};
        cpu.m_pc = pc;
        cpu.m_exit_reason = dispatch;
        cpu.m_cop0[COP0_Status] = 0x10u;
        cpu.m_cop0[COP0_EntryHi] = scenario == 6u ? 0xacu : 0xabu;
        cpu.m_cop0[COP0_Config] = 1u << 16u;
        cpu.m_gpr[2] = {marker, marker};
        cpu.m_gpr[4].m_lo = (scenario == 1u ? data + 1u :
            scenario == 7u ? 0x00600000u :
            scenario >= 12u ? data + scenario - 12u : data) + 4u;
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
            native_ram.read64(0x2000u) != interpreted_ram.read64(0x2000u) ||
            native_ram.read64(0x2008u) != interpreted_ram.read64(0x2008u) ||
            native_ram.read32(0x1000u) != live_opcode)
        {
            std::fprintf(stderr, "strict native mismatch: opcode=%08x scenario=%u actual=%u wanted=%u\n",
                opcode, scenario, static_cast<unsigned>(actual), static_cast<unsigned>(wanted));
            return 0u;
        }
        // Independently require the expected architectural outcome, not
        // just equality with the reference executor.
        const bool aligned = transfer.m_partial || transfer.m_width == 16u ||
            transfer.m_width == 1u || scenario != 1u;
        const bool offset_aligned = scenario < 12u || transfer.m_partial ||
            transfer.m_width == 16u || ((scenario - 12u) & (transfer.m_width - 1u)) == 0u;
        const bool fault = !aligned || !offset_aligned || scenario == 2u ||
            (scenario == 3u && store) || scenario == 6u || scenario == 7u;
        if (fault && (actual != result::FAULT_COMMITTED ||
            native_ram.read64(0x2000u) != original.m_lo ||
            native_ram.read64(0x2008u) != original.m_hi ||
            cpu.m_gpr[2].m_lo != marker || cpu.m_gpr[2].m_hi != marker))
        {
            return 0u;
        }
        if (!fault && (scenario <= 3u || scenario >= 12u) &&
            (actual != result::COMMITTED || cpu.m_pc != pc + 4u ||
             ((store || rt == 0u || transfer.m_width != 16u) && cpu.m_gpr[2].m_hi != marker)))
        {
            return 0u;
        }
        if (scenario == 0u && transfer.m_width == 16u &&
            (store ? (native_ram.read64(0x2000u) != (rt == 0u ? 0u : marker) ||
                      native_ram.read64(0x2008u) != (rt == 0u ? 0u : marker))
                   : (rt != 0u && (cpu.m_gpr[2].m_lo != original.m_lo ||
                                   cpu.m_gpr[2].m_hi != original.m_hi))))
        {
            return 0u;
        }
    }
    return scenarios;
}


extern "C" uint32_t ee_test_strict_native_sequence(native_entry entry)
{
    if (entry == nullptr)
    {
        return 0u;
    }
    constexpr uint32_t pc = 0x00400ff8u;
    constexpr uint32_t data = 0x00600000u;
    constexpr uint32_t dispatch = static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH);
    constexpr uint32_t exception = static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_EXCEPTION);
    constexpr uint32_t code_lo0 = (1u << 6u) | 0x16u;
    constexpr uint32_t code_lo1 = (2u << 6u) | 0x16u;
    constexpr uint32_t data_lo = (3u << 6u) | 0x16u;
    constexpr uint32_t words[] =
    {
        0xac820000u, // sw r2,0(r4)
        0x8cc50000u, // lw r5,0(r6)
        0xac850004u  // sw r5,4(r4), next virtual page
    };
    constexpr uint32_t code_address[] = {0x1ff8u, 0x1ffcu, 0x2000u};
    constexpr uint32_t original = 0x80ff8180u;
    constexpr uint32_t marker = 0x55667788u;
    constexpr uint64_t old_dest = 0x0123456789abcdefull;
    constexpr uint64_t upper = 0xfedcba9876543210ull;
    const s_ee_strict_ram_evidence evidence{true, true};
    for (uint32_t scenario = 0u; scenario < 8u; ++scenario)
    {
        ee_memory native_ram(0x4000u);
        ee_memory interpreted_ram(0x4000u);
        for (ee_memory* memory : {&native_ram, &interpreted_ram})
        {
            if (!memory->tlb().write(0u, 0u, (pc & ~0x1fffu) | 0xabu,
                    code_lo0, scenario == 3u ? code_lo1 & ~2u : code_lo1) ||
                !memory->tlb().write(1u, 0u, data | 0xabu,
                    scenario == 7u ? data_lo & ~4u : data_lo, 0x12u))
            {
                return 0u;
            }
            for (uint32_t i = 0u; i < 3u; ++i)
            {
                const bool stale = (scenario == 4u && i == 1u) ||
                                   (scenario == 6u && i == 2u);
                memory->write32(code_address[i], stale ? 0u : words[i]);
            }
            memory->write32(0x3000u, original);
            memory->write32(0x3004u, original);
        }
        s_ee_cpu_state cpu{};
        cpu.m_pc = pc;
        cpu.m_exit_reason = dispatch;
        cpu.m_cop0[COP0_Status] = 0x10u;
        cpu.m_cop0[COP0_EntryHi] = 0xabu;
        cpu.m_gpr[2] = {marker, upper};
        cpu.m_gpr[4].m_lo = scenario == 5u ? pc + 4u : data;
        cpu.m_gpr[5] = {old_dest, upper};
        cpu.m_gpr[6].m_lo = scenario == 1u ? 0x00800000u :
            scenario == 2u ? data + 1u : data;
        s_ee_cpu_state reference = cpu;
        bool native_delay = true;
        bool reference_delay = true;
        s_ee_strict_native_word_context context{&native_ram, &native_delay, evidence};
        const auto actual = static_cast<result>(entry(&cpu, &context));
        result wanted = result::COMMITTED;
        for (uint32_t i = 0u; i < 3u; ++i)
        {
            // A stale native word is a host refusal rather than guest
            // interpretation of the changed opcode. Stop the reference
            // at precisely that boundary to compare committed prefixes.
            if (((scenario == 4u || scenario == 5u) && i == 1u) ||
                (scenario == 6u && i == 2u))
            {
                wanted = result::UNSUPPORTED_INSTRUCTION;
                break;
            }
            wanted = ee_execute_strict_ram_step(reference, interpreted_ram,
                                                 reference_delay, evidence);
            if (wanted != result::COMMITTED)
            {
                break;
            }
        }
        if (actual != wanted || !equal_cpu(cpu, reference) ||
            native_delay != reference_delay)
        {
            std::fprintf(stderr, "strict sequence state mismatch: scenario=%u\n", scenario);
            return 0u;
        }
        for (uint32_t address = 0u; address < 0x4000u; address += 4u)
        {
            if (native_ram.read32(address) != interpreted_ram.read32(address))
            {
                return 0u;
            }
        }
        const bool first_committed = scenario != 7u;
        const bool second_committed = scenario == 0u || scenario == 3u || scenario == 6u;
        const bool fault = scenario == 1u || scenario == 2u || scenario == 3u || scenario == 7u;
        const uint32_t fault_index = scenario == 7u ? 0u : scenario == 3u ? 2u : 1u;
        const uint32_t code = scenario == 7u ? EX_Mod :
            scenario == 2u ? EX_AdEL : EX_TLBL;
        if (native_ram.read32(0x3000u) !=
                (first_committed && scenario != 5u ? marker : original) ||
            native_ram.read32(0x3004u) != (scenario == 0u ? marker : original) ||
            cpu.m_gpr[5].m_lo != (second_committed ? marker : old_dest) ||
            cpu.m_gpr[5].m_hi != upper)
        {
            return 0u;
        }
        if (fault)
        {
            const uint32_t address = scenario == 1u ? 0x00800000u :
                scenario == 2u ? data + 1u : scenario == 3u ? pc + 8u : data;
            if (actual != result::FAULT_COMMITTED || cpu.m_exit_reason != exception ||
                cpu.m_pc != (scenario == 1u ? VEC_TLB_REFILL_BEV0 : VEC_COMMON_BEV0) ||
                cpu.m_cop0[COP0_EPC] != pc + fault_index * 4u ||
                cpu.m_cop0[COP0_BadVAddr] != address ||
                cpu.m_cop0[COP0_Cause] != (code << 2u) || native_delay)
            {
                return 0u;
            }
        }
        else if (cpu.m_exit_reason != dispatch ||
            cpu.m_pc != pc + (scenario == 0u ? 12u : scenario == 6u ? 8u : 4u) ||
            actual != (scenario == 0u ? result::COMMITTED : result::UNSUPPORTED_INSTRUCTION) ||
            !native_delay)
        {
            return 0u;
        }
        if (scenario == 5u && native_ram.read32(code_address[1]) != marker)
        {
            return 0u;
        }
    }
    return 8u;
}


extern "C" uint32_t ee_test_strict_native_branch(native_entry entry,
    uint32_t branch_opcode, uint32_t slot_opcode)
{
    if (entry == nullptr || !ee_strict_branch_supported(branch_opcode) ||
        (slot_opcode != 0x8c850000u && slot_opcode != 0xac850000u))
    {
        return 0u;
    }
    constexpr uint32_t pc = 0x00400ffcu;
    constexpr uint32_t data = 0x00600000u;
    constexpr uint32_t target = 0x0040100cu;
    constexpr uint32_t fallthrough = 0x00401004u;
    constexpr uint32_t dispatch = static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH);
    constexpr uint32_t exception = static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_EXCEPTION);
    constexpr uint32_t code_lo0 = (1u << 6u) | 0x16u;
    constexpr uint32_t code_lo1 = (2u << 6u) | 0x16u;
    constexpr uint32_t data_lo = (3u << 6u) | 0x16u;
    constexpr uint32_t original = 0x80ff8180u;
    constexpr uint64_t upper = 0xfedcba9876543210ull;
    constexpr uint32_t pending = 0x40028400u;
    constexpr uint32_t old_epc = 0x01234000u;
    const bool likely = ((branch_opcode >> 26u) & 0x10u) != 0u;
    const bool unequal_branch = ((branch_opcode >> 26u) & 1u) != 0u;
    const bool zero_rhs = ((branch_opcode >> 16u) & 31u) == 0u;
    const bool store = (slot_opcode >> 26u) == 0x2bu;
    for (uint32_t scenario = 0u; scenario < 17u; ++scenario)
    {
        const bool taken = scenario != 1u && scenario != 3u &&
            scenario != 5u && scenario != 8u;
        const bool equal = taken != unequal_branch;
        const bool annulled = likely && !taken;
        const bool nested = scenario == 12u || scenario == 13u;
        const bool bev = nested;
        const bool slot_invalid = scenario == 2u || scenario == 3u || scenario == 12u;
        const bool misaligned = scenario == 4u || scenario == 5u || scenario == 13u;
        const bool stale_slot = scenario == 7u || scenario == 8u;
        const bool branch_fault = scenario == 10u || scenario == 11u;
        const uint32_t old_status = 0x10u |
            (nested ? STATUS_EXL : 0u) | (bev ? STATUS_BEV : 0u);
        const uint32_t old_cause = pending | (scenario == 13u ? CAUSE_BD : 0u);
        ee_memory memory(0x4000u);
        if (!memory.tlb().write(0u, 0u, (pc & ~0x1fffu) | 0xabu,
                scenario == 10u ? code_lo0 & ~2u : code_lo0,
                slot_invalid ? code_lo1 & ~2u : code_lo1) ||
            !memory.tlb().write(1u, 0u, data | 0xabu,
                scenario == 14u ? data_lo & ~4u :
                scenario == 15u ? (4u << 6u) | 0x16u : data_lo, 0x12u))
        {
            return 0u;
        }
        memory.write32(0x1ffcu, scenario == 9u ? 0u : branch_opcode);
        memory.write32(0x2000u, stale_slot ? 0u : slot_opcode);
        memory.write32(0x3000u, original);
        memory.write32(0x3004u, original);
        s_ee_cpu_state cpu{};
        cpu.m_pc = pc;
        cpu.m_exit_reason = dispatch;
        cpu.m_cop0[COP0_Status] = old_status;
        cpu.m_cop0[COP0_Cause] = old_cause;
        cpu.m_cop0[COP0_EPC] = old_epc;
        cpu.m_cop0[COP0_EntryHi] = scenario == 11u ? 0xacu : 0xabu;
        cpu.m_cop0[COP0_BadVAddr] = 0x12345678u;
        cpu.m_gpr[0] = {0xdeadbeefu, upper};
        cpu.m_gpr[3] = {0x100000000ull, upper};
        cpu.m_gpr[4].m_lo = scenario == 6u ? 0x00800000u : data + (misaligned ? 1u : 0u);
        // Compare 64-bit low halves, not low words or upper GPR halves.
        const uint64_t lhs = zero_rhs ? (equal ? 0u : 0x100000000ull)
                                      : (equal ? 0x100000000ull : 0u);
        cpu.m_gpr[5] = {lhs, upper ^ 1u};
        const auto before = cpu;
        bool actual_delay = true;
        s_ee_strict_native_word_context context{&memory, &actual_delay,
            {scenario != 16u, true}};
        const auto actual = static_cast<result>(entry(&cpu, &context));
        result expected = result::COMMITTED;
        uint32_t code = 0u;
        uint32_t bad = 0u;
        bool bd = false;
        bool refill = false;
        if (scenario == 16u)
        {
            expected = result::UNSAFE_BOUNDARY;
        }
        else if (branch_fault)
        {
            expected = result::FAULT_COMMITTED;
            code = EX_TLBL;
            bad = pc;
            refill = scenario == 11u;
        }
        else if (scenario == 9u)
        {
            expected = result::UNSUPPORTED_INSTRUCTION;
        }
        else if (!annulled)
        {
            bd = true;
            if (slot_invalid)
            {
                expected = result::FAULT_COMMITTED;
                code = EX_TLBL;
                bad = pc + 4u;
            }
            else if (stale_slot)
            {
                expected = result::UNSUPPORTED_INSTRUCTION;
            }
            else if (misaligned)
            {
                expected = result::FAULT_COMMITTED;
                code = store ? EX_AdES : EX_AdEL;
                bad = data + 1u;
            }
            else if (scenario == 6u)
            {
                expected = result::FAULT_COMMITTED;
                code = store ? EX_TLBS : EX_TLBL;
                bad = 0x00800000u;
                refill = true;
            }
            else if (scenario == 14u && store)
            {
                expected = result::FAULT_COMMITTED;
                code = EX_Mod;
                bad = data;
            }
            else if (scenario == 15u)
            {
                expected = result::UNMODELED_ACCESS;
            }
        }
        if (actual != expected)
        {
            std::fprintf(stderr, "strict branch result mismatch: branch=%08x slot=%08x scenario=%u actual=%u expected=%u\n",
                branch_opcode, slot_opcode, scenario, static_cast<unsigned>(actual),
                static_cast<unsigned>(expected));
            return 0u;
        }
        const bool executed = expected == result::COMMITTED && !annulled;
        if (memory.read32(0x3000u) != (executed && store ? static_cast<uint32_t>(lhs) : original) ||
            memory.read32(0x3004u) != original ||
            cpu.m_gpr[5].m_lo != (executed && !store ? 0xffffffff80ff8180ull : lhs) ||
            cpu.m_gpr[5].m_hi != (upper ^ 1u) ||
            cpu.m_gpr[0].m_lo != before.m_gpr[0].m_lo ||
            cpu.m_gpr[3].m_lo != before.m_gpr[3].m_lo)
        {
            return 0u;
        }
        if (expected == result::COMMITTED)
        {
            if (cpu.m_pc != (taken ? target : fallthrough) || cpu.m_exit_reason != dispatch ||
                cpu.m_cop0[COP0_Status] != old_status || cpu.m_cop0[COP0_Cause] != old_cause ||
                cpu.m_cop0[COP0_EPC] != old_epc || !actual_delay)
            {
                return 0u;
            }
        }
        else if (expected == result::FAULT_COMMITTED)
        {
            const uint32_t vector = refill && !nested
                ? (bev ? VEC_TLB_REFILL_BEV1 : VEC_TLB_REFILL_BEV0)
                : (bev ? VEC_COMMON_BEV1 : VEC_COMMON_BEV0);
            const uint32_t cause = pending | (code << 2u) |
                ((nested ? scenario == 13u : bd) ? CAUSE_BD : 0u);
            if (cpu.m_pc != vector || cpu.m_exit_reason != exception ||
                cpu.m_pad != pc + (bd ? 4u : 0u) ||
                cpu.m_cop0[COP0_Status] != (old_status | STATUS_EXL) ||
                cpu.m_cop0[COP0_Cause] != cause ||
                cpu.m_cop0[COP0_EPC] != (nested ? old_epc : pc) ||
                cpu.m_cop0[COP0_BadVAddr] != bad || actual_delay != bd)
            {
                return 0u;
            }
        }
        else if (!equal_cpu(cpu, before) || !actual_delay)
        {
            return 0u;
        }
    }
    return 17u;
}

} // namespace ee
