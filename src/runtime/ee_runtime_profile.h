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
    vu1_worker_wait,
    vif1_deferred_resume,
    vu0_sync_halted,
    vu0_sync_cmsar1,
    vu0_sync_micro,
    vu0_sync_invalid,
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
    {"vu1_compiled", "attempted_microinstruction_pairs"},
    {"vu1_worker_wait", "none"},
    {"vif1_deferred_resume", "none"},
    {"vu0_sync_halted", "none"},
    {"vu0_sync_cmsar1", "none"},
    {"vu0_sync_micro", "none"},
    {"vu0_sync_invalid", "none"}
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

// Completed-scope baseline for a wall-clock interval. Field endpoints
// are retained rather than assuming scanout counters never reset.
// GPU clock measurements are not host steady-clock scopes. A normal
// compute pass can be unmeasured when the single query slot is busy;
// never extrapolate those samples into an exact GPU execution total.
struct s_gpu_timestamp_profile
{
    bool m_supported = false;
    uint64_t m_compute_passes = 0u;
    uint64_t m_submitted_queries = 0u;
    uint64_t m_completed_queries = 0u;
    uint64_t m_failed_queries = 0u;
    uint64_t m_sampled_compute_ns = 0u;
    uint64_t m_detail_passes = 0u;
    uint32_t m_slot_capacity = 0u;
};

inline void write_gpu_timestamp_profile_json(FILE* output,
    const s_gpu_timestamp_profile& p)
{
    std::fprintf(output,
        "{\"supported\":%s,\"scope\":\"sampled_regular_compute_passes\","
        "\"compute_passes\":%llu,\"submitted_queries\":%llu,"
        "\"completed_queries\":%llu,\"failed_queries\":%llu,"
        "\"pending_queries\":%llu,\"unsampled_passes\":%llu,"
        "\"detail_passes_excluded\":%llu,\"slot_capacity\":%u,\"sampled_compute_ns\":",
        p.m_supported ? "true" : "false",
        static_cast<unsigned long long>(p.m_compute_passes),
        static_cast<unsigned long long>(p.m_submitted_queries),
        static_cast<unsigned long long>(p.m_completed_queries),
        static_cast<unsigned long long>(p.m_failed_queries),
        static_cast<unsigned long long>(p.m_submitted_queries -
            p.m_completed_queries - p.m_failed_queries),
        static_cast<unsigned long long>(p.m_compute_passes - p.m_submitted_queries),
        static_cast<unsigned long long>(p.m_detail_passes),
        p.m_slot_capacity);
    if (p.m_completed_queries != 0u)
    {
        std::fprintf(output, "%llu", static_cast<unsigned long long>(p.m_sampled_compute_ns));
    }
    else
    {
        std::fputs("null", output);
    }
    std::fputc('}', output);
}

struct s_runtime_profile_interval
{
    t_runtime_profile_clock::time_point m_start{};
    uint64_t m_dispatches = 0u;
    uint64_t m_cycles = 0u;
    uint64_t m_fields = 0u;
    uint64_t m_presentations = 0u;
    uint64_t m_covered_ns = 0u;
    s_gpu_timestamp_profile m_gpu{};
    std::array<s_runtime_profile_counter, k_runtime_profile_descriptors.size()> m_counters{};
};

struct s_runtime_profile_state
{
    bool m_enabled = false;
    t_runtime_profile_clock::time_point m_start{};
    c_runtime_profile_scope* m_active = nullptr;
    uint64_t m_covered_ns = 0u;
    std::array<s_runtime_profile_counter, k_runtime_profile_descriptors.size()> m_counters{};
    s_runtime_profile_interval m_interval{};
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
        g_runtime_profile.m_interval.m_start = g_runtime_profile.m_start;
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
        auto& state = g_runtime_profile;
        if (state.m_enabled)
        {
            // Scopes are thread-confined. Cache the owning state so the
            // stop path does not need to resolve thread-local storage again.
            m_state = &state;
            m_parent = state.m_active;
            state.m_active = this;
            m_start = t_runtime_profile_clock::now();
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

    // Completed direct child scopes already include all descendants.
    // Available after stop(), without another host clock read.
    uint64_t child_ns() const { return m_children_ns; }

    // Return the measured inclusive duration on the first stop only.
    // The caller can reuse it for legacy progress reporting without a
    // second pair of host clock reads. Measure-only scopes never
    // contribute to the profiler's nesting or counters.
    uint64_t stop()
    {
        uint64_t inclusive = 0u;
        if (m_state != nullptr || m_measure_only)
        {
            const auto end = t_runtime_profile_clock::now();
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - m_start).count();
            inclusive = static_cast<uint64_t>(elapsed);
            m_measure_only = false;
        }
        if (m_state != nullptr)
        {
            auto& state = *m_state;
            const uint64_t exclusive = inclusive - m_children_ns;
            auto& counter = state.m_counters[static_cast<std::size_t>(m_id)];
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
                state.m_covered_ns += inclusive;
            }
            state.m_active = m_parent;
            m_state = nullptr;
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
    s_runtime_profile_state* m_state = nullptr;
    bool m_measure_only = false;
};

// Sample only at a completed-scope report boundary. Periodic reports
// are overwritten on disk; a compact interval record in the replay log
// preserves the phase history without per-dispatch clocks or guest changes.
// Endpoints (not inferred frame deltas) expose scanout reconfiguration.
inline bool write_runtime_profile_interval(FILE* output, uint64_t dispatches,
    uint64_t cycles, uint64_t fields, uint64_t presentations, bool final,
    t_runtime_profile_clock::time_point now = t_runtime_profile_clock::now(),
    const s_gpu_timestamp_profile& gpu = {})
{
    auto& state = g_runtime_profile;
    if (!state.m_enabled || state.m_active != nullptr || output == nullptr)
    {
        return false;
    }
    auto& previous = state.m_interval;
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now - previous.m_start).count();
    if (elapsed <= 0 || (!final && elapsed < 1000000000ll))
    {
        return false;
    }
    const uint64_t wall_ns = static_cast<uint64_t>(elapsed);
    const uint64_t covered_ns = state.m_covered_ns - previous.m_covered_ns;
    std::fprintf(output,
        "EE profile interval: {\"schema\":\"ee-runtime-profile-interval-v1\","
        "\"start_dispatch\":%llu,\"end_dispatch\":%llu,"
        "\"start_cycle\":%llu,\"end_cycle\":%llu,"
        "\"start_fields\":%llu,\"end_fields\":%llu,"
        "\"start_presentations\":%llu,\"end_presentations\":%llu,"
        "\"wall_ns\":%llu,\"covered_ns\":%llu,\"uninstrumented_ns\":%llu,"
        "\"gpu_execution_ns\":null,\"gpu_compute_timestamps\":",
        static_cast<unsigned long long>(previous.m_dispatches),
        static_cast<unsigned long long>(dispatches),
        static_cast<unsigned long long>(previous.m_cycles),
        static_cast<unsigned long long>(cycles),
        static_cast<unsigned long long>(previous.m_fields),
        static_cast<unsigned long long>(fields),
        static_cast<unsigned long long>(previous.m_presentations),
        static_cast<unsigned long long>(presentations),
        static_cast<unsigned long long>(wall_ns),
        static_cast<unsigned long long>(covered_ns),
        static_cast<unsigned long long>(wall_ns >= covered_ns ? wall_ns - covered_ns : 0u));
    // Publish cumulative endpoints rather than pretending a query
    // collected in this interval necessarily executed in it.
    write_gpu_timestamp_profile_json(output, gpu);
    std::fputs(",\"gpu_compute_previous\":", output);
    write_gpu_timestamp_profile_json(output, previous.m_gpu);
    std::fputs(",\"subsystems\":{", output);
    for (std::size_t i = 0u; i < k_runtime_profile_descriptors.size(); ++i)
    {
        const auto& current = state.m_counters[i];
        const auto& old = previous.m_counters[i];
        std::fprintf(output,
            "%s\"%s\":{\"calls\":%llu,\"inclusive_ns\":%llu,"
            "\"exclusive_ns\":%llu,\"work\":%llu}",
            i != 0u ? "," : "", k_runtime_profile_descriptors[i].m_name,
            static_cast<unsigned long long>(current.m_calls - old.m_calls),
            static_cast<unsigned long long>(current.m_inclusive_ns - old.m_inclusive_ns),
            static_cast<unsigned long long>(current.m_exclusive_ns - old.m_exclusive_ns),
            static_cast<unsigned long long>(current.m_work - old.m_work));
    }
    std::fputs("}}\n", output);
    previous.m_start = now;
    previous.m_dispatches = dispatches;
    previous.m_cycles = cycles;
    previous.m_fields = fields;
    previous.m_presentations = presentations;
    previous.m_covered_ns = state.m_covered_ns;
    previous.m_gpu = gpu;
    previous.m_counters = state.m_counters;
    return true;
}

inline void write_runtime_profile_json(FILE* output, uint64_t dispatches,
    uint64_t fields, uint64_t presentations,
    const s_gpu_timestamp_profile& gpu = {})
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
        "\"gpu_compute_timestamps\":",
        static_cast<unsigned long long>(wall_ns),
        static_cast<unsigned long long>(covered_ns),
        static_cast<unsigned long long>(uninstrumented_ns),
        static_cast<unsigned long long>(dispatches),
        static_cast<unsigned long long>(fields),
        static_cast<unsigned long long>(presentations),
        seconds > 0.0 ? static_cast<double>(dispatches) / seconds : 0.0,
        seconds > 0.0 ? static_cast<double>(fields) / seconds : 0.0,
        seconds > 0.0 ? static_cast<double>(presentations) / seconds : 0.0);
    write_gpu_timestamp_profile_json(output, gpu);
    std::fputs(",\"subsystems\":{", output);

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
