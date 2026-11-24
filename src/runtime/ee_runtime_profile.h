#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace ee
{
enum class e_runtime_profile_id : std::size_t
{
    ee_native_boundary,
    iop_service,
    dmac_vif1_chain,
    dmac_gif_chain,
    dmac_normal,
    vif1_consume,
    vif1_command,
    vif1_unpack,
    vif1_mpg,
    vu1_run,
    vu1_xgkick,
    gif_ordered,
    gif_consume,
    gs_transaction,
    gs_clut,
    gs_local_copy,
    gs_host_upload,
    gs_read_image,
    webgpu_bridge,
    webgpu_draw_cpu,
    webgpu_batch_cpu,
    webgpu_submit_cpu,
    webgpu_wait_cpu,
    webgpu_present_cpu,
    webgpu_readback_cpu,
    device_service,
    frame_pacing_sleep,
    window_pump,
    intc_service,
    thread_schedule,
    ee_hle_syscall,
    dispatch_diagnostics,
    progress_log,
    dispatch_trace_log,
    compile_wait,
    report_snapshot_body,
    vu1_compiled,
    count
};

struct s_runtime_profile_descriptor
{
    const char* m_name;
    const char* m_work_unit;
};

inline constexpr std::array<s_runtime_profile_descriptor,
    static_cast<std::size_t>(e_runtime_profile_id::count)> k_runtime_profile_descriptors{{
    {"ee_native_boundary", "none"},
    {"iop_service", "none"},
    {"dmac_vif1_chain", "none"},
    {"dmac_gif_chain", "none"},
    {"dmac_normal", "none"},
    {"vif1_consume", "input_qwc"},
    {"vif1_command", "none"},
    {"vif1_unpack", "none"},
    {"vif1_mpg", "microinstructions_uploaded"},
    {"vu1_run", "attempted_microinstruction_pairs"},
    {"vu1_xgkick", "none"},
    {"gif_ordered", "segments"},
    {"gif_consume", "input_qwc"},
    {"gs_transaction", "events"},
    {"gs_clut", "none"},
    {"gs_local_copy", "none"},
    {"gs_host_upload", "input_bytes"},
    {"gs_read_image", "requested_qwc"},
    {"webgpu_bridge", "events"},
    {"webgpu_draw_cpu", "none"},
    {"webgpu_batch_cpu", "none"},
    {"webgpu_submit_cpu", "none"},
    {"webgpu_wait_cpu", "none"},
    {"webgpu_present_cpu", "none"},
    {"webgpu_readback_cpu", "requested_bytes"},
    {"device_service", "none"},
    {"frame_pacing_sleep", "none"},
    {"window_pump", "none"},
    {"intc_service", "none"},
    {"thread_schedule", "none"},
    {"ee_hle_syscall", "none"},
    {"dispatch_diagnostics", "none"},
    {"progress_log", "none"},
    {"dispatch_trace_log", "none"},
    {"compile_wait", "none"},
    {"report_snapshot_body", "none"},
    {"vu1_compiled", "attempted_microinstruction_pairs"}
}};

using t_runtime_profile_clock = std::chrono::steady_clock;

struct s_runtime_profile_counter
{
    uint64_t m_calls = 0u;
    uint64_t m_inclusive_ns = 0u;
    uint64_t m_exclusive_ns = 0u;
    uint64_t m_max_inclusive_ns = 0u;
    uint64_t m_max_exclusive_ns = 0u;
    uint64_t m_work = 0u;
};

class c_runtime_profile_scope;

struct s_runtime_profile_state
{
    bool m_enabled = false;
    t_runtime_profile_clock::time_point m_start{};
    c_runtime_profile_scope* m_active = nullptr;
    uint64_t m_covered_ns = 0u;
    std::array<s_runtime_profile_counter, k_runtime_profile_descriptors.size()> m_counters{};
};

// The current synchronous runner owns all measured scopes on its thread.
// Worker-thread collection and aggregation are deliberately not implied.
inline thread_local s_runtime_profile_state g_runtime_profile{};

inline void runtime_profile_begin(bool enabled)
{
    g_runtime_profile = {};
    g_runtime_profile.m_enabled = enabled;
    if (enabled)
    {
        g_runtime_profile.m_start = t_runtime_profile_clock::now();
    }
}

class c_runtime_profile_scope
{
public:
    explicit c_runtime_profile_scope(e_runtime_profile_id id,
        uint64_t work = 0u, const uint32_t* work_at_end = nullptr,
        bool measure_without_profile = false)
        : m_id(id), m_work(work), m_work_at_end(work_at_end)
    {
        if (g_runtime_profile.m_enabled)
        {
            m_parent = g_runtime_profile.m_active;
            g_runtime_profile.m_active = this;
            m_start = t_runtime_profile_clock::now();
            m_active = true;
        }
        else if (measure_without_profile)
        {
            // Legacy runner progress timing remains available even
            // when the detailed profiler is disabled.
            m_start = t_runtime_profile_clock::now();
            m_measure_only = true;
        }
    }

    c_runtime_profile_scope(const c_runtime_profile_scope&) = delete;
    c_runtime_profile_scope& operator=(const c_runtime_profile_scope&) = delete;

    ~c_runtime_profile_scope()
    {
        stop();
    }

    // Return the measured inclusive duration on the first stop only.
    // The caller can reuse it for legacy progress reporting without a
    // second pair of host clock reads. Measure-only scopes never
    // contribute to the profiler's nesting or counters.
    uint64_t stop()
    {
        uint64_t inclusive = 0u;
        if (m_active || m_measure_only)
        {
            const auto end = t_runtime_profile_clock::now();
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - m_start).count();
            inclusive = static_cast<uint64_t>(elapsed);
            m_measure_only = false;
        }
        if (m_active)
        {
            const uint64_t exclusive = inclusive - m_children_ns;
            auto& counter = g_runtime_profile.m_counters[static_cast<std::size_t>(m_id)];
            ++counter.m_calls;
            counter.m_inclusive_ns += inclusive;
            counter.m_exclusive_ns += exclusive;
            counter.m_work += m_work_at_end != nullptr ? *m_work_at_end : m_work;
            if (inclusive > counter.m_max_inclusive_ns)
            {
                counter.m_max_inclusive_ns = inclusive;
            }
            if (exclusive > counter.m_max_exclusive_ns)
            {
                counter.m_max_exclusive_ns = exclusive;
            }
            if (m_parent != nullptr)
            {
                m_parent->m_children_ns += inclusive;
            }
            else
            {
                g_runtime_profile.m_covered_ns += inclusive;
            }
            g_runtime_profile.m_active = m_parent;
            m_active = false;
        }
        return inclusive;
    }

private:
    e_runtime_profile_id m_id;
    uint64_t m_work;
    const uint32_t* m_work_at_end;
    c_runtime_profile_scope* m_parent = nullptr;
    t_runtime_profile_clock::time_point m_start{};
    uint64_t m_children_ns = 0u;
    bool m_active = false;
    bool m_measure_only = false;
};

inline void write_runtime_profile_json(FILE* output, uint64_t dispatches,
    uint64_t fields, uint64_t presentations)
{
    if (!g_runtime_profile.m_enabled)
    {
        std::fputs("{\"schema\":\"ee-runtime-profile-v1\",\"enabled\":false}", output);
        return;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        t_runtime_profile_clock::now() - g_runtime_profile.m_start).count();
    const uint64_t wall_ns = static_cast<uint64_t>(elapsed);
    const uint64_t covered_ns = g_runtime_profile.m_covered_ns;
    const uint64_t uninstrumented_ns = wall_ns >= covered_ns ? wall_ns - covered_ns : 0u;
    const double seconds = static_cast<double>(wall_ns) / 1e9;
    std::fprintf(output,
        "{\"schema\":\"ee-runtime-profile-v1\",\"enabled\":true,"
        "\"clock\":\"steady_clock\",\"scope\":\"runner_thread_completed_scopes\","
        "\"wall_ns\":%llu,\"covered_ns\":%llu,\"uninstrumented_ns\":%llu,"
        "\"dispatches\":%llu,\"guest_fields\":%llu,\"presentations\":%llu,"
        "\"dispatches_per_second\":%.6f,\"guest_fields_per_second\":%.6f,"
        "\"presentations_per_second\":%.6f,\"gpu_execution_ns\":null,"
        "\"subsystems\":{",
        static_cast<unsigned long long>(wall_ns),
        static_cast<unsigned long long>(covered_ns),
        static_cast<unsigned long long>(uninstrumented_ns),
        static_cast<unsigned long long>(dispatches),
        static_cast<unsigned long long>(fields),
        static_cast<unsigned long long>(presentations),
        seconds > 0.0 ? static_cast<double>(dispatches) / seconds : 0.0,
        seconds > 0.0 ? static_cast<double>(fields) / seconds : 0.0,
        seconds > 0.0 ? static_cast<double>(presentations) / seconds : 0.0);

    for (std::size_t i = 0u; i < k_runtime_profile_descriptors.size(); ++i)
    {
        const auto& descriptor = k_runtime_profile_descriptors[i];
        const auto& counter = g_runtime_profile.m_counters[i];
        std::fprintf(output,
            "%s\"%s\":{\"calls\":%llu,\"inclusive_ns\":%llu,\"exclusive_ns\":%llu,"
            "\"average_inclusive_ns\":%llu,\"average_exclusive_ns\":%llu,"
            "\"max_inclusive_ns\":%llu,\"max_exclusive_ns\":%llu,"
            "\"work_unit\":\"%s\",\"work\":%llu}",
            i != 0u ? "," : "", descriptor.m_name,
            static_cast<unsigned long long>(counter.m_calls),
            static_cast<unsigned long long>(counter.m_inclusive_ns),
            static_cast<unsigned long long>(counter.m_exclusive_ns),
            static_cast<unsigned long long>(counter.m_calls != 0u ?
                counter.m_inclusive_ns / counter.m_calls : 0u),
            static_cast<unsigned long long>(counter.m_calls != 0u ?
                counter.m_exclusive_ns / counter.m_calls : 0u),
            static_cast<unsigned long long>(counter.m_max_inclusive_ns),
            static_cast<unsigned long long>(counter.m_max_exclusive_ns),
            descriptor.m_work_unit,
            static_cast<unsigned long long>(counter.m_work));
    }
    std::fputs("}}", output);
}
} // namespace ee
