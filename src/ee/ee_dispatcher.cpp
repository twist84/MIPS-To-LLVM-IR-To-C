#include "ee_dispatcher.h"
#include "ee_dispatch_index.h"
#include "ee_exceptions.h"

#include <algorithm>
#include <limits>

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

    // Use one direct slot per aligned guest instruction address when
    // the covered code is compact. Keep the sorted index for sparse
    // overlays, high-address mappings and exact unaligned metadata.
    // Bound memory independently of arbitrary ELF address spans.
    constexpr uint64_t k_max_dense_slots = 4u * 1024u * 1024u;
    uint32_t first = std::numeric_limits<uint32_t>::max();
    uint32_t last = 0u;
    uint64_t aligned_count = 0u;
    for (const auto& item : index.pcs)
    {
        if (item.first != 0u && (item.first & 3u) == 0u)
        {
            first = std::min(first, item.first);
            last = item.first;
            ++aligned_count;
        }
    }
    if (aligned_count != 0u)
    {
        const uint64_t slots =
            (static_cast<uint64_t>(last) - first) / 4u + 1u;
        if (slots <= k_max_dense_slots && slots <= aligned_count * 16u)
        {
            index.m_dense_base = first;
            index.m_dense_owners.resize(static_cast<size_t>(slots), 0u);
            for (const auto& item : index.pcs)
            {
                if (item.first != 0u && (item.first & 3u) == 0u)
                {
                    index.m_dense_owners[(item.first - first) >> 2u] =
                        item.second + 1u;
                }
            }
        }
    }
    return index;
}

namespace
{

// The dense table already contains function indices. Keep the index on the
// hot dispatch path instead of converting it to a metadata pointer and
// immediately dividing a pointer difference by sizeof(s_recompiled_fn).
constexpr size_t k_no_dispatch_owner = std::numeric_limits<size_t>::max();

size_t find_dispatch_owner_index(
    const s_ee_dispatch_index& index,
    uint32_t guest_pc,
    size_t function_count)
{
    size_t owner = k_no_dispatch_owner;
    if (guest_pc == 0u)
    {
        if (index.has_zero_owner && index.zero_owner < function_count)
        {
            owner = index.zero_owner;
        }
    }
    else
    {
        const bool aligned = (guest_pc & 3u) == 0u;
        const size_t slot = (guest_pc - index.m_dense_base) >> 2u;
        if (aligned && guest_pc >= index.m_dense_base &&
            slot < index.m_dense_owners.size())
        {
            const uint32_t encoded = index.m_dense_owners[slot];
            if (encoded != 0u && encoded <= function_count)
            {
                owner = encoded - 1u;
            }
        }
        else
        {
            const auto found = std::lower_bound(
                index.pcs.begin(), index.pcs.end(), guest_pc,
                [](const std::pair<uint32_t, uint32_t>& entry, uint32_t pc)
                {
                    return entry.first < pc;
                });
            if (found != index.pcs.end() && found->first == guest_pc &&
                found->second < function_count)
            {
                owner = found->second;
            }
        }
    }
    return owner;
}

} // namespace

const s_recompiled_fn* ee_find_dispatch_owner(
    const s_recompiled_elf& compiled,
    const s_ee_dispatch_index& index,
    uint32_t guest_pc)
{
    const size_t owner = find_dispatch_owner_index(
        index, guest_pc, compiled.functions.size());
    return owner != k_no_dispatch_owner ? &compiled.functions[owner] : nullptr;
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

    while (true)
    {
        result.guest_pc = cpu->m_pc;

        if (result.dispatch_count >= max_dispatches)
        {
            result.status = e_ee_dispatch_status::DISPATCH_LIMIT;
            result.exit_reason = cpu->m_exit_reason;
            return result;
        }

        const size_t function_index = find_owner(cpu->m_pc);

        if (function_index == k_no_dispatch_owner)
        {
            // The current PC is outside the registered translated code set.
            // Preserve the architectural exit reason for the outer host.
            result.status = e_ee_dispatch_status::EXITED;
            result.exit_reason = cpu->m_exit_reason;
            return result;
        }

        if (function_index >= table.entry_count)
        {
            result.status = e_ee_dispatch_status::INVALID_ARGUMENT;
            result.exit_reason = cpu->m_exit_reason;
            return result;
        }

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

        // A one-call runner slice is the common HLE case. Finish its
        // budget directly, without starting another dispatch iteration.
        // The terminal reason and PC-zero checks above retain precedence.
        if (result.dispatch_count == max_dispatches)
        {
            result.status = e_ee_dispatch_status::DISPATCH_LIMIT;
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
            const s_recompiled_fn* owner = find_recompiled_function(*table.compiled, pc);
            return owner != nullptr
                ? static_cast<size_t>(owner - table.compiled->functions.data())
                : k_no_dispatch_owner;
        });
}

t_ee_translated_entry ee_resolve_dispatch_entry(
    const s_ee_dispatch_table& table,
    const s_ee_dispatch_index& index,
    uint32_t guest_pc)
{
    if (table.compiled == nullptr || table.entries == nullptr ||
        table.entry_count == 0u ||
        table.entry_count != table.compiled->functions.size())
    {
        return nullptr;
    }
    const size_t owner = find_dispatch_owner_index(
        index, guest_pc, table.entry_count);
    return owner != k_no_dispatch_owner ? table.entries[owner] : nullptr;
}

uint32_t ee_resolve_dispatch_owner_vaddr(
    const s_ee_dispatch_table& table,
    const s_ee_dispatch_index& index,
    uint32_t guest_pc,
    t_ee_translated_entry entry)
{
    if (guest_pc == 0u || entry == nullptr || table.compiled == nullptr ||
        table.entries == nullptr || table.entry_count == 0u ||
        table.entry_count != table.compiled->functions.size())
    {
        return 0u;
    }
    const size_t owner = find_dispatch_owner_index(
        index, guest_pc, table.entry_count);
    if (owner == k_no_dispatch_owner || table.entries[owner] != entry)
    {
        return 0u;
    }
    return table.compiled->functions[owner].vaddr;
}

s_ee_dispatch_result ee_dispatch_run_resolved(
    s_ee_cpu_state* cpu,
    s_ee_runtime* runtime,
    uint32_t guest_pc,
    t_ee_translated_entry entry)
{
    return ee_dispatch_run_resolved_inline(cpu, runtime, guest_pc, entry);
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
            return find_dispatch_owner_index(index, pc, table.entry_count);
        });
}

} // namespace ee
