#include "ee_strict_native_word.h"
#include "ee_memory.h"
#include "ee_memory_bus_plan.h"
#include "ee_memory_fault_commit.h"

namespace ee
{
extern "C" uint32_t ee_strict_native_prepare_word(
    s_ee_cpu_state* cpu, s_ee_strict_native_word_context* context,
    uint32_t expected_pc, uint32_t expected_opcode, uint8_t** out)
{
    using result = e_ee_strict_ram_step_result;
    constexpr auto committed = static_cast<uint32_t>(result::COMMITTED);
    static_assert(committed == 0u);
    if (out == nullptr)
    {
        return static_cast<uint32_t>(result::UNSAFE_BOUNDARY);
    }
    *out = nullptr;
    if (cpu == nullptr || context == nullptr || context->m_memory == nullptr ||
        context->m_actual_delay_slot == nullptr ||
        !context->m_evidence.m_ordinary_boundary || !context->m_evidence.m_isolated_ram ||
        cpu->m_pc != expected_pc ||
        cpu->m_exit_reason != static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH) ||
        (cpu->m_cop0[COP0_Status] & STATUS_ERL) != 0u)
    {
        return static_cast<uint32_t>(result::UNSAFE_BOUNDARY);
    }
    if (!ee_strict_word_supported(expected_opcode))
    {
        return static_cast<uint32_t>(result::UNSUPPORTED_INSTRUCTION);
    }
    auto& memory = *context->m_memory;
    const auto& tlb = memory.tlb();
    const uint32_t status = cpu->m_cop0[COP0_Status];
    const uint32_t config = cpu->m_cop0[COP0_Config];
    const uint32_t saved_context = cpu->m_cop0[COP0_Context];
    const uint32_t entry_hi = cpu->m_cop0[COP0_EntryHi];
    s_ee_memory_fault_request request{};
    request.m_kind = e_ee_memory_request_kind::FETCH;
    request.m_address = expected_pc;
    request.m_faulting_pc = expected_pc;
    request.m_architectural_state_restored = true;
    const auto fault = [&]()
    {
        return static_cast<uint32_t>(ee_commit_memory_fault_at_boundary(
            *cpu, *context->m_actual_delay_slot, tlb, request) ==
            e_ee_memory_fault_commit_result::COMMITTED
                ? result::FAULT_COMMITTED : result::UNMODELED_ACCESS);
    };
    constexpr uint32_t retail_ram_end = 0x02000000u;
    const auto backing = [&](const s_ee_access_decision& access)
        -> uint8_t*
    {
        const auto bus = ee_plan_data_bus_span(access, 4u, config, retail_ram_end);
        if (bus.m_region != e_ee_bus_region::RAM ||
            bus.m_cache_policy != e_ee_data_cache_policy::UNCACHED)
        {
            return nullptr;
        }
        return memory.physical_ram_span(bus.m_address, bus.m_width);
    };
    const auto fetch = ee_plan_instruction_fetch(tlb, expected_pc, status,
                                                 config, saved_context, entry_hi);
    if (fetch.m_fault.m_valid)
    {
        return fault();
    }
    if (fetch.m_route != e_ee_fetch_route::CACHE_BYPASS_PHYSICAL)
    {
        return static_cast<uint32_t>(result::UNMODELED_ACCESS);
    }
    const uint8_t* code = backing(fetch.m_access);
    if (code == nullptr)
    {
        return static_cast<uint32_t>(result::UNMODELED_ACCESS);
    }
    const uint32_t live = static_cast<uint32_t>(code[0]) |
        (static_cast<uint32_t>(code[1]) << 8u) |
        (static_cast<uint32_t>(code[2]) << 16u) |
        (static_cast<uint32_t>(code[3]) << 24u);
    // A stale compiled instruction must never execute. This is not a
    // code cache invalidation/recompilation mechanism.
    if (live != expected_opcode)
    {
        return static_cast<uint32_t>(result::UNSUPPORTED_INSTRUCTION);
    }
    const uint32_t rs = (live >> 21u) & 31u;
    const uint32_t base = rs == 0u ? 0u : static_cast<uint32_t>(cpu->m_gpr[rs].m_lo);
    const uint32_t offset = static_cast<uint32_t>(
        static_cast<int32_t>(static_cast<int16_t>(live & 0xffffu)));
    request.m_kind = e_ee_memory_request_kind::DATA;
    request.m_data_kind = e_ee_data_access_kind::WORD;
    request.m_write_access = (live >> 26u) == 0x2bu;
    request.m_address = base + offset;
    const auto data = ee_plan_data_access(tlb, request.m_data_kind,
        request.m_write_access, request.m_address, status, config, saved_context, entry_hi);
    if (data.m_fault.m_valid)
    {
        return fault();
    }
    *out = backing(data.m_access);
    return *out != nullptr ? committed : static_cast<uint32_t>(result::UNMODELED_ACCESS);
}

} // namespace ee
