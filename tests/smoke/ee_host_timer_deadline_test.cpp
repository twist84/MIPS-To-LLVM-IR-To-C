#include "ee_host_timers.h"
#include "ee_iop_clock.h"

#include <cstdio>
#include <limits>

namespace
{
bool check(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "EE timer deadline: %s\n", message);
    }
    return condition;
}

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
        t_iop_clock clock;
        if (!check(!clock.active() && clock.next_cycle() == t_iop_clock::k_no_deadline,
                "unanchored IOP clock has no usable deadline") ||
            !check(clock.observe(100u) == e_result::accepted &&
                clock.active() && clock.due() == 0u && clock.next_cycle() == 108u,
                "IOP clock initial anchor") ||
            !check(clock.observe(107u) == e_result::accepted &&
                clock.due() == 0u && clock.next_cycle() == 108u,
                "fractional IOP deadline") ||
            !check(clock.observe(106u) == e_result::nonmonotonic &&
                clock.next_cycle() == 108u,
                "nonmonotonic IOP timestamp changed state") ||
            !check(clock.observe(109u) == e_result::accepted &&
                clock.due() == 1u && clock.next_cycle() == 109u,
                "IOP debt is immediately due") ||
            !check(!clock.consume(2u) && clock.due() == 1u &&
                clock.consume(1u) && clock.next_cycle() == 116u,
                "IOP debt consumption lost fractional remainder"))
        {
            return 1;
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
            return 1;
        }

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
        t_iop_clock near_limit;
        if (!check(near_limit.observe(t_iop_clock::k_no_deadline - 3u) ==
                e_result::accepted && near_limit.next_cycle() == t_iop_clock::k_no_deadline &&
                near_limit.observe(t_iop_clock::k_no_deadline) == e_result::accepted &&
                near_limit.due() == 0u,
                "IOP deadline overflow must not wrap"))
        {
            return 1;
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
    std::puts("EE host timer deadline: PASS");
    return 0;
}
