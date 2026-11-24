#pragma once

// Host-only dispatch acceleration. This header lives in src/runtime (not
// include/ee) on purpose: include/ee is part of the guest IR-generation
// fingerprint, and a dispatcher lookup change must not invalidate cached IR.

#include "ee_dispatcher.h"

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

// Same contract as ee_dispatch_run(), using the index for owner lookup.
// The index must have been built from table.compiled.
s_ee_dispatch_result ee_dispatch_run_indexed(
    s_ee_cpu_state* cpu,
    s_ee_runtime* runtime,
    const s_ee_dispatch_table& table,
    const s_ee_dispatch_index& index,
    uint32_t max_dispatches);

} // namespace ee
