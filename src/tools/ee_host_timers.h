#pragma once

#include "ee_hw_bus.h"
#include "ee_runtime.h"

#include <cstdint>
#include <limits>

namespace ee
{
// Host-owned EE timer MMIO for HLE-first native boot. Kept outside the
// translated-code input directories so adding timer support does not
// invalidate preserved guest LLVM IR and native function objects.
//
// Counter enable/clock selection, COMP, overflow flags, reset-on-compare,
// and their real INTC causes follow ps2-web-recomp:SDBZ/runtime/src/hw.cpp.
// The compatibility clock uses guest instruction attempts plus modeled EE
// idle cycles. It is independent of wall time and host resumes.
class c_ee_host_timers
{
public:
    c_ee_host_timers(s_ee_runtime* runtime, c_ee_hw_bus* hardware)
        : m_runtime(runtime), m_hardware(hardware)
    {
        // Match the reference's reset configuration for EE Timer 3.
        m_timers[3].m_mode = 0x183u;
        schedule(3u, now());
        refresh_deadline();
    }

    static bool mmio_read(void* user, uint32_t address,
                          uint32_t width, s_ee_u128* value)
    {
        if (user == nullptr || value == nullptr)
        {
            return false;
        }
        c_ee_host_timers* timers = static_cast<c_ee_host_timers*>(user);
        uint32_t index = 0u;
        uint32_t offset = 0u;
        if (!decode(address, width, &index, &offset))
        {
            return c_ee_hw_bus::mmio_read(
                timers->m_hardware, address, width, value);
        }

        const uint64_t cycle = timers->now();
        timers->service(cycle);
        const s_timer& timer = timers->m_timers[index];
        uint32_t result = 0u;
        switch (offset)
        {
        case 0x00u: result = timers->count(index, cycle); break;
        case 0x10u: result = timer.m_mode; break;
        case 0x20u: result = timer.m_compare; break;
        case 0x30u: result = timer.m_hold; break;
        default: return false;
        }
        value->m_lo = result;
        value->m_hi = 0u;
        return true;
    }

    static bool mmio_write(void* user, uint32_t address,
                           uint32_t width, const s_ee_u128* value)
    {
        if (user == nullptr || value == nullptr)
        {
            return false;
        }
        c_ee_host_timers* timers = static_cast<c_ee_host_timers*>(user);
        uint32_t index = 0u;
        uint32_t offset = 0u;
        if (!decode(address, width, &index, &offset))
        {
            return c_ee_hw_bus::mmio_write(
                timers->m_hardware, address, width, value);
        }

        const uint64_t cycle = timers->now();
        timers->service(cycle);
        s_timer& timer = timers->m_timers[index];
        const uint32_t written = static_cast<uint32_t>(value->m_lo);
        switch (offset)
        {
        case 0x00u:
            timers->set_count(index, written, cycle);
            break;
        case 0x10u:
        {
            const uint32_t old_count = timers->count(index, cycle);
            // MODE.EQUF/OVFF are write-one-to-clear sticky status bits.
            timer.m_mode = (timer.m_mode & 0xc00u & ~(written & 0xc00u)) |
                (written & 0x3ffu);
            timers->set_count(index, old_count, cycle);
            break;
        }
        case 0x20u:
            timer.m_compare = written & 0xffffu;
            break;
        case 0x30u:
            timer.m_hold = written & 0xffffu;
            break;
        default:
            return false;
        }
        timers->schedule(index, cycle);
        timers->refresh_deadline();
        return true;
    }

    void service()
    {
        service(now());
    }

    void set_idle_cycles(uint64_t cycles)
    {
        if (m_runtime != nullptr)
        {
            m_runtime->m_cop0_clock_idle = cycles;
        }
    }

    uint64_t next_event() const
    {
        return m_next_event;
    }

    uint32_t current_count(uint32_t index) const
    {
        return index < 4u ? count(index, now()) : 0u;
    }

    uint32_t current_mode(uint32_t index) const
    {
        return index < 4u ? m_timers[index].m_mode : 0u;
    }

private:
    struct s_timer
    {
        uint32_t m_mode = 0u;
        uint32_t m_compare = 0u;
        uint32_t m_hold = 0u;
        uint32_t m_anchor_count = 0u;
        uint64_t m_anchor_cycle = 0u;
        uint64_t m_next_event = std::numeric_limits<uint64_t>::max();
    };

    static bool decode(uint32_t address, uint32_t width,
                       uint32_t* index, uint32_t* offset)
    {
        if (address < 0x10000000u || address >= 0x10002000u ||
            width != 4u || (address & 3u) != 0u)
        {
            return false;
        }
        const uint32_t local = address & 0x7ffu;
        if (local != 0x00u && local != 0x10u &&
            local != 0x20u && local != 0x30u)
        {
            return false;
        }
        *index = (address >> 11u) & 3u;
        *offset = local;
        return true;
    }

    uint64_t now() const
    {
        if (m_runtime == nullptr)
        {
            return 0u;
        }
        if (m_runtime->m_ee_cycle_scheduler.source() ==
            e_ee_cycle_source::REFERENCE_COMPATIBILITY)
        {
            // Unlike a post-dispatch timestamp, this advances *inside*
            // an instrumented native guest loop at its instruction sites.
            uint64_t cycle = 0u;
            if (!ee_reference_compatibility_cycle(*m_runtime, cycle))
            {
                m_runtime->m_perf_compat_fault = true;
                return m_runtime->m_ee_cycle_scheduler.current_cycle();
            }
            return cycle;
        }
        return m_runtime->m_ee_cycle_scheduler.current_cycle();
    }

    uint64_t divider(uint32_t index) const
    {
        const uint32_t source = m_timers[index].m_mode & 3u;
        switch (source)
        {
        case 0u: return 2u;    // EE bus clock
        case 1u: return 32u;   // bus / 16
        case 2u: return 512u;  // bus / 256
        default:
        {
            // HBlank clock: keep the reference's integer field cadence.
            const bool pal = m_hardware != nullptr &&
                m_hardware->gs_crt_mode() == 3u;
            const uint64_t frame_cycles = pal
                ? c_ee_scanout_clock::k_ee_clock_hz / 50u
                : c_ee_scanout_clock::k_ee_clock_hz * 1001u / 60000u;
            return (frame_cycles * 2u) / (pal ? 625u : 525u);
        }
        }
    }

    uint32_t count(uint32_t index, uint64_t cycle) const
    {
        const s_timer& timer = m_timers[index];
        if ((timer.m_mode & 0x80u) == 0u ||
            cycle < timer.m_anchor_cycle)
        {
            return timer.m_anchor_count & 0xffffu;
        }
        const uint64_t ticks =
            (cycle - timer.m_anchor_cycle) / divider(index);
        return static_cast<uint32_t>(
            (timer.m_anchor_count + ticks) & 0xffffu);
    }

    void set_count(uint32_t index, uint32_t value, uint64_t cycle)
    {
        m_timers[index].m_anchor_count = value & 0xffffu;
        m_timers[index].m_anchor_cycle = cycle;
    }

    void schedule(uint32_t index, uint64_t cycle)
    {
        s_timer& timer = m_timers[index];
        timer.m_next_event = std::numeric_limits<uint64_t>::max();
        if ((timer.m_mode & 0x80u) == 0u ||
            (timer.m_mode & 0x300u) == 0u)
        {
            return;
        }
        const uint32_t current = count(index, cycle);
        uint64_t ticks = 0x10000u - current;
        if ((timer.m_mode & 0x100u) != 0u)
        {
            uint64_t to_compare =
                (timer.m_compare - current) & 0xffffu;
            if (to_compare == 0u)
            {
                to_compare = 0x10000u;
            }
            if ((timer.m_mode & 0x200u) == 0u ||
                to_compare < ticks)
            {
                ticks = to_compare;
            }
        }
        const uint64_t div = divider(index);
        const uint64_t elapsed = cycle >= timer.m_anchor_cycle
            ? (cycle - timer.m_anchor_cycle) % div : 0u;
        const uint64_t future = ticks * div - elapsed;
        if (future < std::numeric_limits<uint64_t>::max() - cycle)
        {
            timer.m_next_event = cycle + future;
        }
    }

    void refresh_deadline()
    {
        m_next_event = std::numeric_limits<uint64_t>::max();
        for (const s_timer& timer : m_timers)
        {
            if (timer.m_next_event < m_next_event)
            {
                m_next_event = timer.m_next_event;
            }
        }
    }

    void service(uint64_t cycle)
    {
        // Guest register writes and actual timer events are the only
        // producers of new deadlines. The ordinary EE boundary needs
        // no four-timer scan while all events remain in the future.
        if (m_next_event == std::numeric_limits<uint64_t>::max() ||
            cycle < m_next_event)
        {
            return;
        }
        for (uint32_t index = 0u; index < 4u; ++index)
        {
            s_timer& timer = m_timers[index];
            while (timer.m_next_event != std::numeric_limits<uint64_t>::max() &&
                   timer.m_next_event <= cycle)
            {
                const uint64_t event_cycle = timer.m_next_event;
                const uint32_t value = count(index, event_cycle);
                const bool compare =
                    value == (timer.m_compare & 0xffffu);
                if (compare)
                {
                    timer.m_mode |= 0x400u;
                    if ((timer.m_mode & 0x100u) != 0u &&
                        m_hardware != nullptr)
                    {
                        (void)m_hardware->intc().raise(9u + index);
                    }
                    if ((timer.m_mode & 0x40u) != 0u)
                    {
                        set_count(index, 0u, event_cycle);
                    }
                }
                if (value == 0u && !(compare &&
                    (timer.m_mode & 0x40u) != 0u))
                {
                    timer.m_mode |= 0x800u;
                    if ((timer.m_mode & 0x200u) != 0u &&
                        m_hardware != nullptr)
                    {
                        (void)m_hardware->intc().raise(9u + index);
                    }
                }
                schedule(index, event_cycle);
                // Saturation is a stop condition; never spin on UINT64_MAX.
                if (timer.m_next_event <= event_cycle)
                {
                    timer.m_next_event =
                        std::numeric_limits<uint64_t>::max();
                    break;
                }
            }
        }
        refresh_deadline();
    }

    s_ee_runtime* m_runtime = nullptr;
    c_ee_hw_bus* m_hardware = nullptr;
    s_timer m_timers[4]{};
    uint64_t m_next_event = std::numeric_limits<uint64_t>::max();
};
} // namespace ee
