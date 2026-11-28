#include "ee_runtime_profile.h"
#include "ee_performance_overlay.h"
#include "../../src/tools/ee_pc_profile.h"

#include <cstdio>
#include <string>

int main()
{
    using ee::e_runtime_profile_id;
    {
        ee::c_ee_performance_overlay overlay;
        ee::s_ee_performance_counters counters{};
        if (overlay.visible() || overlay.sample(0.0, counters))
        {
            return 11;
        }
        overlay.toggle();
        counters.m_clock_available = true;
        counters.m_crt_mode = 2u;
        if (!overlay.visible() || !overlay.sample(1.0, counters) ||
            overlay.sample(1.1, counters))
        {
            return 12;
        }
        counters.m_cycles = 147456000u;
        counters.m_presentations = 30u;
        counters.m_fields = 30u;
        counters.m_dispatches = 500000u;
        counters.m_instructions = 147000000u;
        counters.m_draws = 50000u;
        if (!overlay.sample(1.5, counters))
        {
            return 13;
        }
        ee::s_ee_overlay_text expected{};
        expected.set_line(0u, "PERFORMANCE  F5  CPU PROFILE F6");
        expected.set_line(1u, "SPEED       100.0% / UNLIMITED");
        expected.set_line(2u, "PRESENT      60.0 / 59.94 FPS");
        expected.set_line(3u, "GUEST        60.0 / 59.94 F/S");
        expected.set_line(4u, "EE          294.0 M INST/S");
        expected.set_line(5u, "DISPATCH   1000.0 K/S");
        expected.set_line(6u, "GS          100.0 K DRAWS/S");
        expected.set_line(7u, "CPU PROFILE OFF");
        if (overlay.text().m_words != expected.m_words)
        {
            return 14;
        }
        // Changing CRT mode reanchors sampling; PAL uses its own nominal rate.
        counters.m_crt_mode = 3u;
        if (!overlay.sample(1.5, counters))
        {
            return 19;
        }
        counters.m_cycles += 147456000u;
        counters.m_presentations += 20u;
        counters.m_fields += 25u;
        if (!overlay.sample(2.0, counters))
        {
            return 20;
        }
        expected.set_line(2u, "PRESENT      40.0 / 50.00 FPS");
        expected.set_line(3u, "GUEST        50.0 / 50.00 F/S");
        expected.set_line(4u, "EE            0.0 M INST/S");
        expected.set_line(5u, "DISPATCH      0.0 K/S");
        expected.set_line(6u, "GS            0.0 K DRAWS/S");
        if (overlay.text().m_words != expected.m_words)
        {
            return 21;
        }
        // Counter reset must not manufacture enormous rates.
        counters.m_fields = 0u;
        if (!overlay.sample(2.1, counters))
        {
            return 15;
        }
        expected = {};
        expected.set_line(0u, "PERFORMANCE  F5  CPU PROFILE F6");
        expected.set_line(1u, "SAMPLING...");
        if (overlay.text().m_words != expected.m_words)
        {
            return 16;
        }
        overlay.toggle();
        if (overlay.visible() || overlay.sample(3.0, counters) ||
            overlay.text().m_words != ee::s_ee_overlay_text{}.m_words)
        {
            return 17;
        }
        // Last row of A is 10001; first row is 01110.
        const auto a = ee::ee_overlay_glyph('A');
        if ((a[0] & 31u) != 14u || a[1] != 17u)
        {
            return 18;
        }
    }

    // A tiny cache forces collisions, including zero, unaligned and
    // full-width PCs. Rehashing the owning map must preserve references.
    ee::c_ee_pc_profile<4u> pcs(true);
    ee::c_ee_pc_profile<4u> disabled(false);
    disabled.record(0u, 1u, 2u, 0u, 3u);
    pcs.record(0u, 0u, 0u, 0u, 7u);
    pcs.record(0u, 1u, 10u, 2u, 11u, 3u);
    pcs.record(1u, 1u, 3u, 0u, 13u);
    pcs.record(16u, 1u, 4u, 0u, 17u);
    pcs.record(0xffffffffu, 1u, 5u, 1u, 19u);
    // Word PCs with low two bits equal to 1 and a zero folded low
    // component use a different slot from PC zero after its reinsert.
    pcs.record(0u, 1u, 20u, 8u, 23u);
    const auto* zero = &pcs.entries().at(0u);
    for (uint32_t i = 1u; i <= 4096u; ++i)
    {
        pcs.record(i * 16u + 4u, 1u, 1u, 0u, 1u);
    }
    pcs.record(0u, 1u, 30u, 4u, 29u, 7u);
    const auto& entry = pcs.entries().at(0u);
    if (!disabled.entries().empty() || &entry != zero ||
        pcs.entries().size() != 4100u || entry.m_calls != 4u ||
        entry.m_dispatches != 3u || entry.m_instructions != 60u ||
        entry.m_idle_instructions != 14u || entry.m_wall_ns != 70u ||
        entry.m_nested_ns != 10u || pcs.entries().at(1u).m_nested_ns != 0u ||
        pcs.entries().at(1u).m_wall_ns != 13u ||
        pcs.entries().at(16u).m_wall_ns != 17u ||
        pcs.entries().at(0xffffffffu).m_idle_instructions != 1u)
    {
        return 10;
    }

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
    uint64_t root_child_ns = 0u;
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
        root.stop();
        root_child_ns = root.child_ns();
    }
    const auto& state = ee::g_runtime_profile;
    const auto& root = state.m_counters[static_cast<std::size_t>(e_runtime_profile_id::ee_native_boundary)];
    const auto& child = state.m_counters[static_cast<std::size_t>(e_runtime_profile_id::vu1_run)];
    const auto& kick = state.m_counters[static_cast<std::size_t>(e_runtime_profile_id::vu1_xgkick)];
    if (root.m_calls != 1u || child.m_calls != 1u || kick.m_calls != 1u ||
        root_child_ns != child.m_inclusive_ns ||
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
