#include "ee_runtime_profile.h"

#include <cstdio>
#include <string>

int main()
{
    using ee::e_runtime_profile_id;
    ee::runtime_profile_begin(false);
    {
        ee::c_runtime_profile_scope scope(e_runtime_profile_id::ee_native_boundary);
    }
    if (ee::g_runtime_profile.m_counters[0].m_calls != 0u)
    {
        return 1;
    }

    {
        ee::c_runtime_profile_scope measure(e_runtime_profile_id::iop_service,
            0u, nullptr, true);
        // A second stop must not record another measurement.
        (void)measure.stop();
        if (measure.stop() != 0u || ee::g_runtime_profile.m_active != nullptr ||
            ee::g_runtime_profile.m_covered_ns != 0u ||
            ee::g_runtime_profile.m_counters[static_cast<std::size_t>(
                e_runtime_profile_id::iop_service)].m_calls != 0u)
        {
            return 6;
        }
    }

    ee::runtime_profile_begin(true);
    {
        ee::c_runtime_profile_scope measure(e_runtime_profile_id::iop_service,
            0u, nullptr, true);
        const uint64_t elapsed = measure.stop();
        const auto& iop = ee::g_runtime_profile.m_counters[static_cast<std::size_t>(
            e_runtime_profile_id::iop_service)];
        if (iop.m_calls != 1u || iop.m_inclusive_ns != elapsed ||
            measure.stop() != 0u || ee::g_runtime_profile.m_active != nullptr)
        {
            return 7;
        }
    }
    ee::runtime_profile_begin(true);
    uint32_t steps = 0u;
    {
        ee::c_runtime_profile_scope root(e_runtime_profile_id::ee_native_boundary);
        {
            ee::c_runtime_profile_scope child(e_runtime_profile_id::vu1_run, 0u, &steps);
            {
                ee::c_runtime_profile_scope kick(e_runtime_profile_id::vu1_xgkick);
            }
            steps = 37u;
            child.stop();
            child.stop();
        }
    }
    const auto& state = ee::g_runtime_profile;
    const auto& root = state.m_counters[static_cast<std::size_t>(e_runtime_profile_id::ee_native_boundary)];
    const auto& child = state.m_counters[static_cast<std::size_t>(e_runtime_profile_id::vu1_run)];
    const auto& kick = state.m_counters[static_cast<std::size_t>(e_runtime_profile_id::vu1_xgkick)];
    if (root.m_calls != 1u || child.m_calls != 1u || kick.m_calls != 1u ||
        root.m_inclusive_ns != root.m_exclusive_ns + child.m_inclusive_ns ||
        child.m_inclusive_ns != child.m_exclusive_ns + kick.m_inclusive_ns ||
        state.m_covered_ns != root.m_exclusive_ns + child.m_exclusive_ns + kick.m_exclusive_ns ||
        child.m_work != 37u || state.m_active != nullptr)
    {
        return 2;
    }
    // Exercise same-ID nesting: exclusive time must still add up exactly.
    {
        ee::c_runtime_profile_scope first(e_runtime_profile_id::gif_consume, 3u);
        ee::c_runtime_profile_scope second(e_runtime_profile_id::gif_consume, 5u);
    }
    const auto& gif = state.m_counters[static_cast<std::size_t>(e_runtime_profile_id::gif_consume)];
    if (gif.m_calls != 2u || gif.m_work != 8u ||
        state.m_covered_ns != root.m_inclusive_ns + gif.m_exclusive_ns)
    {
        return 3;
    }
    // A device-service scope can contain pacing and presentation without
    // counting either child twice. Snapshot timing is completed before
    // report serialization, just as in the runner.
    {
        ee::c_runtime_profile_scope device(e_runtime_profile_id::device_service);
        {
            ee::c_runtime_profile_scope sleep(e_runtime_profile_id::frame_pacing_sleep);
        }
        {
            ee::c_runtime_profile_scope window(e_runtime_profile_id::window_pump);
            ee::c_runtime_profile_scope present(e_runtime_profile_id::webgpu_present_cpu);
        }
    }
    {
        ee::c_runtime_profile_scope report(e_runtime_profile_id::report_snapshot_body);
        report.stop();
    }
    uint64_t exclusive = 0u;
    for (const auto& counter : state.m_counters)
    {
        exclusive += counter.m_exclusive_ns;
    }
    if (exclusive != state.m_covered_ns || state.m_active != nullptr ||
        state.m_counters[static_cast<std::size_t>(e_runtime_profile_id::report_snapshot_body)].m_calls != 1u)
    {
        return 5;
    }
    // Intervals contain only completed-scope deltas. A throttled
    // sample must not consume its baseline, and the final short sample
    // must retain the last interval (including scanout counter reset).
    FILE* intervals = std::tmpfile();
    if (intervals == nullptr)
    {
        return 8;
    }
    const auto start = state.m_interval.m_start;
    const auto first = start + std::chrono::seconds(2);
    const auto second = first + std::chrono::milliseconds(250);
    const bool throttled = !ee::write_runtime_profile_interval(intervals,
        10u, 20u, 2u, 1u, false, start + std::chrono::milliseconds(500));
    bool active_refused = false;
    {
        ee::c_runtime_profile_scope scope(e_runtime_profile_id::gif_consume, 7u);
        active_refused = !ee::write_runtime_profile_interval(intervals,
            10u, 20u, 2u, 1u, true, first);
    }
    const bool emitted = ee::write_runtime_profile_interval(intervals,
        100u, 200u, 3u, 2u, false, first,
        {true, 10u, 8u, 6u, 1u, 1200u, 0u, 8u});
    {
        ee::c_runtime_profile_scope scope(e_runtime_profile_id::gif_consume, 11u);
    }
    const bool final_emitted = ee::write_runtime_profile_interval(intervals,
        150u, 300u, 1u, 3u, true, second,
        {true, 20u, 16u, 14u, 1u, 2700u, 2u, 8u});
    std::rewind(intervals);
    std::string records;
    char buffer[1024];
    while (std::fgets(buffer, sizeof(buffer), intervals) != nullptr)
    {
        records += buffer;
    }
    std::fclose(intervals);
    const auto first_record = records.find("EE profile interval: ");
    const auto second_record = records.find("EE profile interval: ", first_record + 1u);
    if (!throttled || !active_refused || !emitted || !final_emitted ||
        first_record == std::string::npos || second_record == std::string::npos ||
        records.find("\"start_dispatch\":0,\"end_dispatch\":100") == std::string::npos ||
        records.find("\"start_dispatch\":100,\"end_dispatch\":150", second_record) == std::string::npos ||
        records.find("\"start_fields\":3,\"end_fields\":1", second_record) == std::string::npos ||
        records.find("\"wall_ns\":250000000", second_record) == std::string::npos ||
        records.find("\"gif_consume\":{\"calls\":1,", second_record) == std::string::npos ||
        records.find("\"work\":11}", second_record) == std::string::npos ||
        records.find("\"pending_queries\":1,\"unsampled_passes\":2") == std::string::npos ||
        records.find("\"slot_capacity\":8,\"sampled_compute_ns\":2700", second_record) == std::string::npos ||
        records.find("\"sampled_compute_ns\":1200", second_record) == std::string::npos ||
        records.find("\"gpu_execution_ns\":null", second_record) == std::string::npos)
    {
        return 9;
    }

    ee::runtime_profile_begin(false);
    if (ee::g_runtime_profile.m_covered_ns != 0u || ee::g_runtime_profile.m_enabled ||
        ee::g_runtime_profile.m_interval.m_dispatches != 0u)
    {
        return 4;
    }
    return 0;
}
