#pragma once

// Host-only dispatch acceleration. This header lives in src/runtime (not
// include/ee) on purpose: include/ee is part of the guest IR-generation
// fingerprint, and a dispatcher lookup change must not invalidate cached IR.

#include "ee_dispatcher.h"
#include "ee_exceptions.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace ee
{

// Guest-PC -> function-index map for one immutable s_recompiled_elf.
// First ownership and the PC-zero sentinel match find_recompiled_function().
// A bounded dense word-address window accelerates compact code images;
// sparse images and unaligned metadata retain the sorted lookup.
struct s_ee_dispatch_index
{
    std::vector<std::pair<uint32_t, uint32_t>> pcs; // (guest pc, function index)
    bool has_zero_owner = false;
    uint32_t zero_owner = 0;
    uint32_t m_dense_base = 0u;
    // Zero means unowned; otherwise store the first function index + 1.
    std::vector<uint32_t> m_dense_owners;
};

// Build once after the metadata is complete. Rebuild if functions change.
s_ee_dispatch_index ee_build_dispatch_index(const s_recompiled_elf& compiled);

// Same contract as find_recompiled_function(compiled, guest_pc).
const s_recompiled_fn* ee_find_dispatch_owner(
    const s_recompiled_elf& compiled,
    const s_ee_dispatch_index& index,
    uint32_t guest_pc);

// Resolve a callable native target for an exact guest PC. A null result
// is not a dispatch outcome: callers must use the normal dispatcher to
// distinguish unowned PCs from invalid native table entries.
t_ee_translated_entry ee_resolve_dispatch_entry(
    const s_ee_dispatch_table& table,
    const s_ee_dispatch_index& index,
    uint32_t guest_pc);

// Execute exactly one already-resolved entry at its original PC, with
// the same interrupt, exit-reason and budget semantics as a one-entry
// indexed run. The caller must retain the owning library and invalidate
// cached targets before replacing it. No guest timing boundary is skipped.
s_ee_dispatch_result ee_dispatch_run_resolved(
    s_ee_cpu_state* cpu,
    s_ee_runtime* runtime,
    uint32_t guest_pc,
    t_ee_translated_entry entry);

// Keep the single-entry path visible to the host runner optimizer.
// The out-of-line entry remains available to existing callers.
inline s_ee_dispatch_result ee_dispatch_run_resolved_inline(
    s_ee_cpu_state* cpu,
    s_ee_runtime* runtime,
    uint32_t guest_pc,
    t_ee_translated_entry entry)
{
    s_ee_dispatch_result result{};
    if (cpu != nullptr)
    {
        result.guest_pc = cpu->m_pc;
        result.exit_reason = cpu->m_exit_reason;
        if (entry != nullptr && cpu->m_pc == guest_pc)
        {
            result.status = e_ee_dispatch_status::EXITED;
            if (guest_pc != 0u &&
                ee_check_interrupts(cpu, runtime, guest_pc, 0u) != 0u)
            {
                result.exit_reason = static_cast<uint32_t>(
                    e_ee_exit_reason::EE_EXIT_EXCEPTION);
                result.guest_pc = cpu->m_pc;
            }
            else
            {
                const uint32_t reason = entry(cpu, runtime);
                cpu->m_exit_reason = reason;
                result.dispatch_count = 1u;
                result.exit_reason = reason;
                result.guest_pc = cpu->m_pc;
                if (cpu->m_pc != 0u &&
                    (reason == static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_RETURN) ||
                     reason == static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH)))
                {
                    result.status = e_ee_dispatch_status::DISPATCH_LIMIT;
                }
            }
        }
    }
    return result;
}

// Same contract as ee_dispatch_run(), using the index for owner lookup.
// The index must have been built from table.compiled.
s_ee_dispatch_result ee_dispatch_run_indexed(
    s_ee_cpu_state* cpu,
    s_ee_runtime* runtime,
    const s_ee_dispatch_table& table,
    const s_ee_dispatch_index& index,
    uint32_t max_dispatches);

} // namespace ee
