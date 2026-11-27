#include "ee_host_timers.h"
#include "ee_iop_clock.h"
#include "ee_execution_horizon.h"

#include <cstdio>
#include <limits>
#include <vector>

namespace
{
constexpr bool check(bool condition, const char* message)
{
    if !consteval
    {
        if (!condition)
        {
            std::fprintf(stderr, "EE timer deadline: %s\n", message);
        }
    }
    return condition;
}

consteval bool test_iop_clock_boundaries()
{
    using t_iop_clock = ee::c_ee_iop_clock;
    using e_result = t_iop_clock::e_result;
    t_iop_clock clock;
    if (!check(!clock.active() && clock.next_cycle() == t_iop_clock::k_no_deadline,
            "unanchored IOP clock has no usable deadline") ||
        !check(clock.observe(100u) == e_result::accepted &&
            clock.active() && clock.due() == 0u && clock.next_cycle() == 108u,
            "IOP clock initial anchor") ||
        !check(clock.observe(107u) == e_result::accepted &&
            clock.due() == 0u && clock.next_cycle() == 108u,
            "fractional IOP deadline") ||
        !check(clock.cycle_after_iop(1u) == 108u &&
            clock.cycle_after_iop(10u) == 180u &&
            clock.cycle_after_iop(t_iop_clock::k_no_deadline) ==
                t_iop_clock::k_no_deadline &&
            clock.cycle_after_iop(t_iop_clock::k_no_deadline / 8u + 1u) ==
                t_iop_clock::k_no_deadline,
            "IOP lookahead projection preserves fractional cycles and overflow") ||
        !check(clock.observe(106u) == e_result::nonmonotonic &&
            clock.next_cycle() == 108u,
            "nonmonotonic IOP timestamp changed state") ||
        !check(clock.observe(109u) == e_result::accepted &&
            clock.due() == 1u && clock.next_cycle() == 109u,
            "IOP debt is immediately due") ||
        !check(clock.cycle_after_iop(10u) == 109u,
            "IOP backlog prevents future projection") ||
        !check(!clock.consume(2u) && clock.due() == 1u &&
            clock.consume(1u) && clock.next_cycle() == 116u,
            "IOP debt consumption lost fractional remainder"))
    {
        return false;
    }

    // A large EE slice may leave a backlog after 64 turns of 256.
    // Later boundaries must preserve that debt, even at the same cycle.
    const uint64_t cycle = 109u + 8u * 20000u;
    if (!check(clock.observe(cycle) == e_result::accepted &&
            clock.due() == 20000u &&
            clock.consume(64u * 256u) && clock.due() == 3616u &&
            clock.next_cycle() == cycle &&
            clock.observe(cycle) == e_result::accepted &&
            clock.due() == 3616u && clock.consume(3616u) &&
            clock.next_cycle() == cycle + 7u,
            "IOP backlog across bounded service turns"))
    {
        return false;
    }

    t_iop_clock near_limit;
    if (!check(near_limit.observe(t_iop_clock::k_no_deadline - 3u) ==
            e_result::accepted && near_limit.next_cycle() == t_iop_clock::k_no_deadline &&
            near_limit.observe(t_iop_clock::k_no_deadline) == e_result::accepted &&
            near_limit.due() == 0u,
            "IOP deadline overflow must not wrap"))
    {
        return false;
    }
    return true;
}

static_assert(test_iop_clock_boundaries(), "IOP clock boundary regression");

consteval bool test_iop_service_batch_boundaries()
{
    const auto empty = ee::ee_iop_service_batch(0u, 64u, 1024u);
    const auto exhausted = ee::ee_iop_service_batch(500u, 0u, 1024u);
    const auto unknown = ee::ee_iop_service_batch(1024u, 64u, UINT64_MAX);
    const auto exact = ee::ee_iop_service_batch(1024u, 64u, 512u);
    const auto before = ee::ee_iop_service_batch(1024u, 64u, 513u);
    const auto tail = ee::ee_iop_service_batch(257u, 64u, 258u);
    const auto capped = ee::ee_iop_service_batch(UINT64_MAX, 64u, 100000u);
    const auto last = ee::ee_iop_service_batch(1024u, 1u, 10000u);
    if (!check(empty.m_cycles == 0u && empty.m_turns == 0u &&
            exhausted.m_cycles == 0u && exhausted.m_turns == 0u &&
            unknown.m_cycles == 256u && unknown.m_turns == 1u &&
            exact.m_cycles == 256u && exact.m_turns == 1u &&
            before.m_cycles == 512u && before.m_turns == 2u &&
            tail.m_cycles == 257u && tail.m_turns == 2u &&
            capped.m_cycles == 16384u && capped.m_turns == 64u &&
            last.m_cycles == 256u && last.m_turns == 1u,
            "IOP idle batch planning at exact event, tail and service cap"))
    {
        return false;
    }
    return true;
}

static_assert(test_iop_service_batch_boundaries(), "IOP service batch boundary regression");

// C++23 requires this fixture to execute during compilation, including
// the temporary vector storage and the instruction reader callback.
consteval bool test_bounded_alu_horizon()
{
    const auto slots = [](const std::vector<uint32_t>& words,
        uint32_t base = 0x1000u)
    {
        return ee::ee_horizon_alu_exit_slots(base,
            [&](uint32_t pc, uint32_t* word)
            {
                if (pc < base || (pc - base) / 4u >= words.size())
                {
                    return false;
                }
                *word = words[(pc - base) / 4u];
                return true;
            });
    };
    if (!check(slots({0x25080001u, 0x03e00008u, 0x00000000u}) == 3u &&
            slots({0x0c04000cu, 0x24020007u}) == 2u &&
            slots({0x0320f809u, 0x00000000u}) == 2u &&
            slots({0x03e10008u, 0u}) == 0u &&
            slots({0x03e00008u, 0x8c020000u}) == 0u &&
            slots({0x8c020000u, 0x03e00008u, 0u}) == 0u &&
            slots({0x40826000u, 0x03e00008u, 0u}) == 0u &&
            slots({0x1000ffffu, 0u}) == 0u &&
            slots({0x20020001u, 0x03e00008u, 0u}) == 0u &&
            slots({0x03e00008u}) == 0u &&
            slots({0x03e00008u, 0u}, 0u) == 0u &&
            slots({0x03e00008u, 0u}, 0xfffffffcu) == 0u &&
            slots(std::vector<uint32_t>(32u, 0u)) == 0u &&
            ee::ee_horizon_bound_fits(10u, 14u, 3u) &&
            !ee::ee_horizon_bound_fits(10u, 13u, 3u) &&
            !ee::ee_horizon_bound_fits(10u, 9u, 3u) &&
            !ee::ee_horizon_bound_fits(10u, UINT64_MAX, 3u) &&
            !ee::ee_horizon_bound_fits(10u, 14u, 0u),
            "bounded ALU paths and strict deadline"))
    {
        return false;
    }
    std::vector<uint32_t> limit(32u, 0u);
    limit[30] = 0x03e00008u;
    if (!check(slots(limit) == 32u,
            "maximum bounded ALU path includes delay slot"))
    {
        return false;
    }
    limit[30] = 0u;
    limit[31] = 0x03e00008u;
    limit.push_back(0u);
    if (!check(slots(limit) == 0u, "delay slot exceeds bounded path"))
    {
        return false;
    }
    return true;
}

static_assert(test_bounded_alu_horizon(), "bounded ALU horizon regression");

bool write_timer(ee::c_ee_host_timers& timers, uint32_t index,
    uint32_t offset, uint32_t value)
{
    const ee::s_ee_u128 quad{value, 0u};
    return ee::c_ee_host_timers::mmio_write(&timers,
        0x10000000u + index * 0x800u + offset, 4u, &quad);
}
}

int main()
{
    using t_iop_clock = ee::c_ee_iop_clock;
    using e_result = t_iop_clock::e_result;
    {
        // Compare partitioned elapsed budgets against one large advance.
        t_iop_clock partitioned;
        t_iop_clock combined;
        (void)partitioned.observe(0u);
        (void)combined.observe(0u);
        uint64_t elapsed = 0u;
        uint64_t consumed = 0u;
        for (uint32_t i = 0u; i < 1024u; ++i)
        {
            elapsed += i % 37u;
            if (partitioned.observe(elapsed) != e_result::accepted)
            {
                return 1;
            }
            const uint64_t work = partitioned.due() / 2u;
            if (!partitioned.consume(work))
            {
                return 1;
            }
            consumed += work;
        }
        if (!check(combined.observe(elapsed) == e_result::accepted &&
                combined.due() == consumed + partitioned.due() &&
                combined.consume(consumed) &&
                combined.next_cycle() == partitioned.next_cycle(),
                "IOP partitioned and combined debt disagree"))
        {
            return 1;
        }

    }
    // Each combined batch must be exactly the concatenation of legacy
    // turns and end strictly before the predicted autonomous event.
    {
        for (uint32_t due = 1u; due < 1400u; due += 17u)
        {
            for (uint32_t interval = 0u; interval < 1400u; interval += 19u)
            {
                for (uint32_t limit = 1u; limit <= 6u; ++limit)
                {
                    const auto batch = ee::ee_iop_service_batch(due, limit, interval);
                    uint32_t expected = 0u;
                    for (uint32_t turn = 0u; turn < batch.m_turns; ++turn)
                    {
                        expected += std::min(due - expected, 256u);
                    }
                    if (!check(batch.m_cycles == expected &&
                            batch.m_turns >= 1u && batch.m_turns <= limit &&
                            (batch.m_turns == 1u || batch.m_cycles < interval),
                            "IOP batch split or crossed an event"))
                    {
                        return 1;
                    }
                }
            }
        }
    }
    ee::s_ee_runtime runtime{};
    ee::c_ee_hw_bus bus{};
    ee::s_ee_cpu_state cpu{};
    bus.bind_cpu(&cpu);
    if (!runtime.m_ee_cycle_scheduler.install_reference_compatibility_source())
    {
        return 1;
    }
    {
        constexpr uint64_t none = ee::k_ee_no_deadline;
        using e_source = ee::e_ee_horizon_source;
        if (!check(ee::ee_cop0_compare_deadline(runtime, 100u) == 100u,
                "uninitialized Count requires service"))
        {
            return 1;
        }
        runtime.m_cop0_clock_initialized = true;
        runtime.m_cop0_clock_last = 100u;
        runtime.m_cop0_clock_count = 0xfffffffcu;
        runtime.m_cop0_clock_compare = 2u;
        if (!check(ee::ee_cop0_compare_deadline(runtime, 99u) == 99u &&
                ee::ee_cop0_compare_deadline(runtime, 100u) == 106u &&
                ee::ee_cop0_compare_deadline(runtime, 105u) == 106u &&
                ee::ee_cop0_compare_deadline(runtime, 107u) == 107u,
                "COP0 deadline across Count wrap and overdue event"))
        {
            return 1;
        }
        runtime.m_cop0_clock_compare = runtime.m_cop0_clock_count;
        if (!check(ee::ee_cop0_compare_deadline(runtime, 100u) ==
                100u + (uint64_t{1u} << 32u),
                "Count equality waits a full wrap"))
        {
            return 1;
        }
        runtime.m_cop0_clock_ip7 = true;
        if (!check(ee::ee_cop0_compare_deadline(runtime, 100u) == none,
                "latched IP7 has no new compare edge"))
        {
            return 1;
        }
        runtime.m_cop0_clock_ip7 = false;
        runtime.m_cop0_clock_last = none - 3u;
        if (!check(ee::ee_cop0_compare_deadline(runtime, none - 3u) == none - 3u,
                "unrepresentable COP0 deadline requires service"))
        {
            return 1;
        }

        const auto timer = ee::ee_execution_horizon(100u, 110u, 120u, 130u, 140u, false);
        const auto scanout = ee::ee_execution_horizon(100u, none, 120u, 130u, 140u, false);
        const auto cop0 = ee::ee_execution_horizon(100u, none, none, 130u, 140u, false);
        const auto iop = ee::ee_execution_horizon(100u, 110u, 120u, 130u, 101u, false);
        const auto due = ee::ee_execution_horizon(100u, 99u, none, none, none, false);
        const auto blocked = ee::ee_execution_horizon(100u, 110u, 120u, 130u, 140u, true);
        const auto empty = ee::ee_execution_horizon(100u, none, none, none, none, false);
        const auto tied = ee::ee_execution_horizon(100u, 110u, 110u, 110u, 110u, false);
        if (!check(timer.m_cycle == 110u && timer.m_source == e_source::timer &&
                scanout.m_cycle == 120u && scanout.m_source == e_source::scanout &&
                cop0.m_cycle == 130u && cop0.m_source == e_source::cop0 &&
                iop.m_cycle == 101u && iop.m_source == e_source::iop &&
                due.m_cycle == 100u && due.m_source == e_source::timer &&
                blocked.m_cycle == 100u && blocked.m_source == e_source::barrier &&
                empty.m_cycle == none && empty.m_source == e_source::none &&
                tied.m_cycle == 110u && tied.m_source == e_source::timer,
                "unified horizon chooses earliest known event or barrier"))
        {
            return 1;
        }
        runtime.m_cop0_clock_initialized = false;
        runtime.m_cop0_clock_last = 0u;
        runtime.m_cop0_clock_count = 0u;
        runtime.m_cop0_clock_compare = 0u;
    }
    // A host continuation can advance slots or HLE budget before the
    // next publication. A caught-up EE clock alone does not establish
    // that the IOP observed or consumed the corresponding time.
    {
        ee::s_ee_runtime checkpoint{};
        t_iop_clock clock;
        if (!check(!ee::ee_horizon_checkpoint_ready(checkpoint, clock, false) &&
                checkpoint.m_ee_cycle_scheduler.install_reference_compatibility_source() &&
                ee::ee_horizon_checkpoint_ready(checkpoint, clock, false) &&
                !ee::ee_horizon_checkpoint_ready(checkpoint, clock, true) &&
                clock.observe(0u) == e_result::accepted &&
                ee::ee_horizon_checkpoint_ready(checkpoint, clock, true),
                "checkpoint source and initial IOP anchor"))
        {
            return 1;
        }
        checkpoint.m_reference_instruction_slots = 7u;
        if (!check(!ee::ee_horizon_checkpoint_ready(checkpoint, clock, false) &&
                checkpoint.m_ee_cycle_scheduler.publish_cycle(7u) &&
                !ee::ee_horizon_checkpoint_ready(checkpoint, clock, true) &&
                clock.observe(7u) == e_result::accepted &&
                ee::ee_horizon_checkpoint_ready(checkpoint, clock, true) &&
                clock.next_cycle() == 8u,
                "stale publication and fractional anchor"))
        {
            return 1;
        }
        checkpoint.m_hle_timing_boundaries = 1u;
        if (!check(!ee::ee_horizon_checkpoint_ready(checkpoint, clock, true) &&
                checkpoint.m_ee_cycle_scheduler.publish_cycle(307u) &&
                !ee::ee_horizon_checkpoint_ready(checkpoint, clock, true) &&
                clock.observe(307u) == e_result::accepted && clock.due() == 38u &&
                !ee::ee_horizon_checkpoint_ready(checkpoint, clock, true) &&
                clock.consume(37u) &&
                !ee::ee_horizon_checkpoint_ready(checkpoint, clock, true) &&
                clock.consume(1u) &&
                ee::ee_horizon_checkpoint_ready(checkpoint, clock, true) &&
                clock.next_cycle() == 312u,
                "HLE continuation and partially consumed debt"))
        {
            return 1;
        }
        checkpoint.m_reference_instruction_slots = UINT64_MAX;
        if (!check(!ee::ee_horizon_checkpoint_ready(checkpoint, clock, false),
                "overflowed live budget authorized a checkpoint"))
        {
            return 1;
        }
    }
    // Snapshot comparisons detect scheduler-visible endpoint differences,
    // including an unchanged Compare value with a different clock anchor.
    {
        ee::s_ee_runtime probe_runtime{};
        ee::s_ee_cpu_state probe_cpu{};
        const auto initial = ee::ee_horizon_cop0_snapshot(probe_cpu, probe_runtime);
        if (!check(ee::ee_horizon_cop0_snapshot(probe_cpu, probe_runtime) == initial,
                "passive COP0 snapshot changed state"))
        {
            return 1;
        }
        for (uint32_t reg : {ee::COP0_Status, ee::COP0_Cause, ee::COP0_Count,
                ee::COP0_Compare, ee::COP0_EPC, ee::COP0_ErrorEPC})
        {
            probe_cpu.m_cop0[reg] = 1u;
            if (!check(ee::ee_horizon_cop0_snapshot(probe_cpu, probe_runtime) != initial,
                    "COP0 register change escaped snapshot"))
            {
                return 1;
            }
            probe_cpu.m_cop0[reg] = 0u;
        }
        probe_runtime.m_cop0_clock_last = 1u;
        if (!check(ee::ee_horizon_cop0_snapshot(probe_cpu, probe_runtime) != initial,
                "COP0 synchronization anchor change escaped snapshot"))
        {
            return 1;
        }
        probe_runtime.m_cop0_clock_last = 0u;
        probe_runtime.m_cop0_clock_ip7 = true;
        if (!check(ee::ee_horizon_cop0_snapshot(probe_cpu, probe_runtime) != initial,
                "COP0 timer latch escaped snapshot"))
        {
            return 1;
        }
        probe_runtime.m_cop0_clock_ip7 = false;
        probe_runtime.m_perf_control = 1u;
        if (!check(ee::ee_horizon_cop0_snapshot(probe_cpu, probe_runtime) != initial,
                "performance control change escaped snapshot"))
        {
            return 1;
        }
    }
    ee::c_ee_host_timers timers(&runtime, &bus);
    constexpr uint64_t no_event = std::numeric_limits<uint64_t>::max();

    // Disable the reset-configured timer 3, then insert two compare
    // events in reverse deadline order (bus clock = EE / 2).
    if (!check(write_timer(timers, 3u, 0x10u, 0u) &&
            timers.next_event() == no_event, "disabled timer retained deadline") ||
        !check(write_timer(timers, 0u, 0x20u, 20u) &&
            write_timer(timers, 0u, 0x10u, 0x180u) &&
            timers.next_event() == 40u, "timer 0 deadline") ||
        !check(write_timer(timers, 1u, 0x20u, 5u) &&
            write_timer(timers, 1u, 0x10u, 0x180u) &&
            timers.next_event() == 10u, "earlier timer insertion"))
    {
        return 1;
    }

    runtime.m_reference_instruction_slots = 9u;
    timers.service();
    if (!check(timers.next_event() == 10u && bus.intc().status() == 0u &&
            timers.current_count(1u) == 4u, "early service fired timer"))
    {
        return 1;
    }
    runtime.m_reference_instruction_slots = 10u;
    timers.service();
    if (!check(timers.next_event() == 40u &&
            bus.intc().status() == (1u << 10u) &&
            (timers.current_mode(1u) & 0x400u) != 0u,
            "exact compare did not refresh minimum"))
    {
        return 1;
    }

    // Reprogram the current earliest timer to a later event, then
    // disable it. The old cached deadline must never fire.
    if (!check(write_timer(timers, 0u, 0x20u, 50u) &&
            timers.next_event() == 100u, "compare rewrite retained old deadline") ||
        !check(write_timer(timers, 0u, 0x10u, 0u) &&
            timers.next_event() == 10u + 0x20000u,
            "disable did not expose next timer"))
    {
        return 1;
    }

    // Count rewrite and reset-on-compare schedule recurring deadlines.
    if (!check(write_timer(timers, 2u, 0x00u, 0u) &&
            write_timer(timers, 2u, 0x20u, 3u) &&
            write_timer(timers, 2u, 0x10u, 0x1c0u) &&
            timers.next_event() == 16u, "recurring deadline setup"))
    {
        return 1;
    }
    runtime.m_reference_instruction_slots = 21u;
    timers.service();
    if (!check(timers.next_event() == 22u &&
            timers.current_count(2u) == 2u &&
            (bus.intc().status() & (1u << 11u)) != 0u,
            "reset-on-compare rescheduling"))
    {
        return 1;
    }
    runtime.m_reference_instruction_slots = 22u;
    timers.service();
    if (!check(timers.next_event() == 28u && timers.current_count(2u) == 0u,
            "recurring exact deadline"))
    {
        return 1;
    }

    // MMIO observes elapsed instructions even before the runner's next
    // service checkpoint, and the idle offset shares the same timebase.
    timers.set_idle_cycles(6u);
    ee::s_ee_u128 read{};
    if (!check(ee::c_ee_host_timers::mmio_read(&timers,
            0x10001000u, 4u, &read) &&
            read.m_lo == 0u && timers.next_event() == 34u,
            "MMIO did not deliver due timer using live idle offset"))
    {
        return 1;
    }
    if (!check(write_timer(timers, 1u, 0x10u, 0u) &&
            write_timer(timers, 2u, 0x10u, 0u) &&
            timers.next_event() == no_event, "last deadline not removed"))
    {
        return 1;
    }
    // The host-only MMIO generation includes timer and ordinary bus
    // callbacks, not elapsed device time or passive deadline queries.
    const uint64_t generation = timers.mmio_generation();
    timers.service();
    (void)timers.next_event();
    if (!check(timers.mmio_generation() == generation,
            "passive timer service advanced MMIO generation") ||
        !check(ee::c_ee_host_timers::mmio_read(&timers,
            0x10000000u, 4u, &read) &&
            timers.mmio_generation() == generation + 1u,
            "timer read did not advance MMIO generation") ||
        !check(write_timer(timers, 0u, 0x20u, 42u) &&
            timers.mmio_generation() == generation + 2u,
            "timer write did not advance MMIO generation"))
    {
        return 1;
    }
    (void)ee::c_ee_host_timers::mmio_read(&timers, 0xffffffffu, 4u, &read);
    if (!check(timers.mmio_generation() == generation + 3u,
            "delegated MMIO attempt was not observed"))
    {
        return 1;
    }
    std::puts("EE host timer deadline: PASS");
    return 0;
}
