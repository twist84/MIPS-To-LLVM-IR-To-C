// Generic dispatcher glue for "data dispatch" libraries (live cache-miss
// overlays). link_native_objects.py --data-dispatch emits the per-library
// function table as a plain C translation unit (no headers, compiled in
// milliseconds) and links it with this object, which is compiled once and
// cached. The C++ chunked dispatcher remains the format for full libraries.

#include "ee_dispatch_index.h"

#include <cstdint>
#include <vector>

extern "C"
{
extern const std::uint32_t ee_dispatch_data_function_count;
extern const std::uint32_t ee_dispatch_data_entry_point;
extern const std::uint32_t ee_dispatch_data_functions_discovered;
extern const std::uint32_t ee_dispatch_data_vaddrs[];
// function_count + 1 offsets into ee_dispatch_data_block_pcs.
extern const std::uint32_t ee_dispatch_data_block_offsets[];
extern const std::uint32_t ee_dispatch_data_block_pcs[];
extern const ee::t_ee_translated_entry ee_dispatch_data_entries[];
}

namespace
{

struct s_data_dispatch
{
    ee::s_recompiled_elf compiled{};
    std::vector<ee::t_ee_translated_entry> entries;
    ee::s_ee_dispatch_index index;
    ee::s_ee_dispatch_table table{};

    s_data_dispatch()
    {
        const std::uint32_t count = ee_dispatch_data_function_count;
        compiled.entry_point = ee_dispatch_data_entry_point;
        compiled.function_count = count;
        compiled.functions_discovered = ee_dispatch_data_functions_discovered;
        compiled.functions.resize(count);
        entries.assign(ee_dispatch_data_entries, ee_dispatch_data_entries + count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            ee::s_recompiled_fn& function = compiled.functions[i];
            function.vaddr = ee_dispatch_data_vaddrs[i];
            function.block_pcs.assign(
                ee_dispatch_data_block_pcs + ee_dispatch_data_block_offsets[i],
                ee_dispatch_data_block_pcs + ee_dispatch_data_block_offsets[i + 1]);
            function.block_count =
                static_cast<std::uint32_t>(function.block_pcs.size());
        }
        index = ee::ee_build_dispatch_index(compiled);
        table = ee::s_ee_dispatch_table{&compiled, entries.data(), entries.size()};
    }
};

const s_data_dispatch& data_dispatch()
{
    static const s_data_dispatch dispatch{};
    return dispatch;
}

} // namespace

#if defined(_WIN32)
#define EE_GAME_EXPORT __declspec(dllexport)
#else
#define EE_GAME_EXPORT __attribute__((visibility("default")))
#endif

// Optional host-only ABI. Existing runners can continue using ee_game_run.
extern "C" EE_GAME_EXPORT ee::t_ee_translated_entry ee_game_resolve_entry(
    std::uint32_t guest_pc)
{
    const s_data_dispatch& dispatch = data_dispatch();
    return ee::ee_resolve_dispatch_entry(dispatch.table, dispatch.index, guest_pc);
}

extern "C" EE_GAME_EXPORT std::uint32_t ee_game_run(
    ee::s_ee_cpu_state* cpu,
    ee::s_ee_runtime* runtime,
    std::uint32_t max_dispatches,
    std::uint32_t* out_dispatch_count,
    std::uint32_t* out_guest_pc,
    std::uint32_t* out_status)
{
    const s_data_dispatch& dispatch = data_dispatch();
    const ee::s_ee_dispatch_result result = ee::ee_dispatch_run_indexed(
        cpu, runtime, dispatch.table, dispatch.index, max_dispatches);
    if (out_dispatch_count) *out_dispatch_count = result.dispatch_count;
    if (out_guest_pc) *out_guest_pc = result.guest_pc;
    if (out_status) *out_status = static_cast<std::uint32_t>(result.status);
    return result.exit_reason;
}
