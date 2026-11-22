#include "ee_dispatcher.h"
#include "ee_dispatch_index.h"
#include "ee_exceptions.h"

#include <algorithm>

namespace ee
{

s_ee_dispatch_index ee_build_dispatch_index(const s_recompiled_elf& compiled)
{
    s_ee_dispatch_index index{};
    size_t total = 0;
    for (const auto& fn : compiled.functions)
    {
        total += fn.block_pcs.size();
    }
    index.pcs.reserve(total);

    const uint32_t count = static_cast<uint32_t>(compiled.functions.size());
    for (uint32_t i = 0; i < count; ++i)
    {
        const s_recompiled_fn& fn = compiled.functions[i];
        // find_recompiled_function() answers PC zero with the first function
        // that is either the ELF entry or lists zero as a block PC.
        if (!index.has_zero_owner &&
            (fn.vaddr == compiled.entry_point ||
             std::find(fn.block_pcs.begin(), fn.block_pcs.end(), 0u) !=
                 fn.block_pcs.end()))
        {
            index.has_zero_owner = true;
            index.zero_owner = i;
        }
        for (const uint32_t pc : fn.block_pcs)
        {
            index.pcs.emplace_back(pc, i);
        }
    }

    // Sort by PC, then by function index, and keep the first owner of each
    // PC: identical to the linear scan's first-match rule for overlaps.
    std::sort(index.pcs.begin(), index.pcs.end());
    index.pcs.erase(
        std::unique(index.pcs.begin(), index.pcs.end(),
                    [](const auto& a, const auto& b) { return a.first == b.first; }),
        index.pcs.end());
    return index;
}

const s_recompiled_fn* ee_find_dispatch_owner(
    const s_recompiled_elf& compiled,
    const s_ee_dispatch_index& index,
    uint32_t guest_pc)
{
    if (guest_pc == 0u)
    {
        return index.has_zero_owner &&
               index.zero_owner < compiled.functions.size()
            ? &compiled.functions[index.zero_owner]
            : nullptr;
    }
    const auto found = std::lower_bound(
        index.pcs.begin(), index.pcs.end(), guest_pc,
        [](const std::pair<uint32_t, uint32_t>& entry, uint32_t pc)
        {
            return entry.first < pc;
        });
    if (found == index.pcs.end() || found->first != guest_pc ||
        found->second >= compiled.functions.size())
    {
        return nullptr;
    }
    return &compiled.functions[found->second];
}

namespace
{

template <typename t_find_owner>
s_ee_dispatch_result dispatch_run_impl(
    s_ee_cpu_state* cpu,
    s_ee_runtime* runtime,
    const s_ee_dispatch_table& table,
    uint32_t max_dispatches,
    const t_find_owner& find_owner)
{
    s_ee_dispatch_result result{};

    if (cpu == nullptr ||
        table.compiled == nullptr ||
        table.entries == nullptr ||
        table.entry_count == 0 ||
        table.entry_count != table.compiled->functions.size() ||
        max_dispatches == 0)
    {
        result.status = e_ee_dispatch_status::INVALID_ARGUMENT;
        if (cpu != nullptr)
        {
            result.exit_reason = cpu->m_exit_reason;
            result.guest_pc = cpu->m_pc;
        }
        return result;
    }

    const s_recompiled_fn* const first_function =
        table.compiled->functions.data();
    const s_recompiled_fn* const end_function =
        first_function + table.compiled->functions.size();

    while (true)
    {
        result.guest_pc = cpu->m_pc;

        if (result.dispatch_count >= max_dispatches)
        {
            result.status = e_ee_dispatch_status::DISPATCH_LIMIT;
            result.exit_reason = cpu->m_exit_reason;
            return result;
        }

        const s_recompiled_fn* const owner = find_owner(cpu->m_pc);

        if (owner == nullptr)
        {
            // The current PC is outside the registered translated code set.
            // Preserve the architectural exit reason for the outer host.
            result.status = e_ee_dispatch_status::EXITED;
            result.exit_reason = cpu->m_exit_reason;
            return result;
        }

        if (owner < first_function || owner >= end_function)
        {
            result.status = e_ee_dispatch_status::INVALID_ARGUMENT;
            result.exit_reason = cpu->m_exit_reason;
            return result;
        }

        const size_t function_index =
            static_cast<size_t>(owner - first_function);
        const t_ee_translated_entry entry = table.entries[function_index];

        if (entry == nullptr)
        {
            result.status = e_ee_dispatch_status::INVALID_ARGUMENT;
            result.exit_reason = cpu->m_exit_reason;
            return result;
        }

        // Only fully committed, mapped guest function-entry boundaries
        // can be checked here. A translated branch has completed its delay
        // slot before it returns to this dispatcher. We deliberately skip
        // the synthetic PC-zero start-of-run sentinel.
        //
        // A guest function that loops without returning still cannot be
        // preempted at instruction granularity; that requires instrumented
        // dispatch safepoints and a timed device scheduler.
        if (cpu->m_pc != 0u &&
            ee_check_interrupts(cpu, runtime, cpu->m_pc, 0u) != 0u)
        {
            result.status = e_ee_dispatch_status::EXITED;
            result.exit_reason = static_cast<uint32_t>(
                e_ee_exit_reason::EE_EXIT_EXCEPTION
            );
            result.guest_pc = cpu->m_pc;
            return result;
        }

        const uint32_t exit_reason = entry(cpu, runtime);
        cpu->m_exit_reason = exit_reason;

        ++result.dispatch_count;
        result.exit_reason = exit_reason;
        result.guest_pc = cpu->m_pc;

        if (exit_reason != static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_RETURN) &&
            exit_reason != static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH))
        {
            result.status = e_ee_dispatch_status::EXITED;
            return result;
        }

        // PC zero is accepted as a start-of-run sentinel only. A guest JR
        // to a null return address must not map back to the ELF entry after
        // translated code has already executed.
        if (cpu->m_pc == 0u)
        {
            result.status = e_ee_dispatch_status::EXITED;
            return result;
        }

        // RETURN can lead back into a translated caller continuation; DISPATCH
        // can lead to another translated function. Resolve both through the
        // same block-PC ownership table on the next iteration.
    }
}

} // namespace

s_ee_dispatch_result ee_dispatch_run(
    s_ee_cpu_state* cpu,
    s_ee_runtime* runtime,
    const s_ee_dispatch_table& table,
    uint32_t max_dispatches)
{
    return dispatch_run_impl(
        cpu, runtime, table, max_dispatches,
        [&table](uint32_t pc)
        {
            return find_recompiled_function(*table.compiled, pc);
        });
}

s_ee_dispatch_result ee_dispatch_run_indexed(
    s_ee_cpu_state* cpu,
    s_ee_runtime* runtime,
    const s_ee_dispatch_table& table,
    const s_ee_dispatch_index& index,
    uint32_t max_dispatches)
{
    return dispatch_run_impl(
        cpu, runtime, table, max_dispatches,
        [&table, &index](uint32_t pc)
        {
            return ee_find_dispatch_owner(*table.compiled, index, pc);
        });
}

} // namespace ee
