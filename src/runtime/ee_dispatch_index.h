#pragma once

// Host-only dispatch acceleration. This header lives in src/runtime (not
// include/ee) on purpose: include/ee is part of the guest IR-generation
// fingerprint, and a dispatcher lookup change must not invalidate cached IR.

#include "ee_dispatcher.h"
#include "ee_interrupt_fast.h"

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

// Identify the first metadata owner of the actual callable native target.
// Zero means unavailable, including the synthetic PC-zero entry, a stale
// target pointer, an unowned PC or a malformed table. This identifies the
// owner, not a proof of its emitted instructions or execution bound.
uint32_t ee_resolve_dispatch_owner_vaddr(
    const s_ee_dispatch_table& table,
    const s_ee_dispatch_index& index,
    uint32_t guest_pc,
    t_ee_translated_entry entry);

// Execute exactly one already-resolved entry at its original PC, with
// the same interrupt, exit-reason and budget semantics as a one-entry
// indexed run. The caller must retain the owning library and invalidate
// cached targets before replacing it. No guest timing boundary is skipped.
s_ee_dispatch_result ee_dispatch_run_resolved(
    s_ee_cpu_state* cpu,
    s_ee_runtime* runtime,
    uint32_t guest_pc,
    t_ee_translated_entry entry);

// The caller has already resolved a non-null entry for cpu.m_pc.
// Construct the final result directly, without reading the old exit
// reason or initializing fields that a successful dispatch overwrites.
// PC zero remains the synthetic initial-entry sentinel.
inline s_ee_dispatch_result ee_dispatch_run_owned_inline(
    s_ee_cpu_state& cpu,
    s_ee_runtime* runtime,
    t_ee_translated_entry entry)
{
    const uint32_t guest_pc = cpu.m_pc;
    if (guest_pc != 0u &&
        ee_check_interrupts_inline(&cpu, runtime, guest_pc, 0u) != 0u)
    {
        return {e_ee_dispatch_status::EXITED,
            static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_EXCEPTION),
            0u, cpu.m_pc};
    }

    const uint32_t reason = entry(&cpu, runtime);
    cpu.m_exit_reason = reason;
    const uint32_t next_pc = cpu.m_pc;
    const bool limit = next_pc != 0u &&
        (reason == static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_RETURN) ||
         reason == static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH));
    return {limit ? e_ee_dispatch_status::DISPATCH_LIMIT :
        e_ee_dispatch_status::EXITED, reason, 1u, next_pc};
}

// Retain the checked API for callers without the runner's ownership
// guarantee, including null CPU/entry and mismatched PC diagnostics.
inline s_ee_dispatch_result ee_dispatch_run_resolved_inline(
    s_ee_cpu_state* cpu,
    s_ee_runtime* runtime,
    uint32_t guest_pc,
    t_ee_translated_entry entry)
{
    if (cpu == nullptr)
    {
        return {};
    }
    if (entry == nullptr || cpu->m_pc != guest_pc)
    {
        return {e_ee_dispatch_status::INVALID_ARGUMENT,
            cpu->m_exit_reason, 0u, cpu->m_pc};
    }
    return ee_dispatch_run_owned_inline(*cpu, runtime, entry);
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
