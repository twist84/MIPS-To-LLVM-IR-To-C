#include "ee_cycle_scheduler.h"
#include "ee_hw_bus.h"
#include "ee_vsync_flags.h"
#include "ee_memory.h"

#include <limits>

namespace ee
{
void c_ee_cycle_scheduler::reset()
{
    m_current_cycle = 0u;
    m_scheduler_yields = 0u;
    m_source = e_ee_cycle_source::UNAVAILABLE;
}

bool c_ee_cycle_scheduler::install_validated_source()
{
    // Installation is deliberately explicit: neither a syscall, dispatch,
    // loop yield nor CRT configuration may silently authorize EE timing.
    if (m_source != e_ee_cycle_source::UNAVAILABLE ||
        m_current_cycle != 0u)
    {
        return false;
    }

    m_source = e_ee_cycle_source::VALIDATED_R5900;
    return true;
}

bool c_ee_cycle_scheduler::install_reference_compatibility_source()
{
    if (m_source != e_ee_cycle_source::UNAVAILABLE ||
        m_current_cycle != 0u)
    {
        return false;
    }

    m_source = e_ee_cycle_source::REFERENCE_COMPATIBILITY;
    return true;
}

bool c_ee_cycle_scheduler::publish_cycle(uint64_t absolute_cycle)
{
    if (m_source == e_ee_cycle_source::UNAVAILABLE ||
        absolute_cycle < m_current_cycle)
    {
        return false;
    }

    m_current_cycle = absolute_cycle;
    return true;
}

void c_ee_cycle_scheduler::record_scheduler_yield()
{
    if (m_scheduler_yields != std::numeric_limits<uint64_t>::max())
    {
        ++m_scheduler_yields;
    }
}

e_ee_scheduled_event_result c_ee_cycle_scheduler::service_scanout(
    c_ee_hw_bus& hardware,
    c_ee_vsync_flags& vsync_flags,
    ee_memory& memory
)
{
    if (m_source == e_ee_cycle_source::UNAVAILABLE)
    {
        return e_ee_scheduled_event_result::NO_VALIDATED_CLOCK;
    }

    if (!hardware.scanout().enabled())
    {
        return e_ee_scheduled_event_result::NO_CONFIGURED_SCANOUT;
    }

    const uint64_t previous_fields = hardware.scanout().fields_started();
    if (!hardware.scanout().advance_to(
            m_current_cycle, &hardware.intc(), &vsync_flags, &memory))
    {
        return e_ee_scheduled_event_result::INVALID_TIMESTAMP_OR_DELIVERY;
    }

    return hardware.scanout().fields_started() == previous_fields
        ? e_ee_scheduled_event_result::NO_DUE_FIELD
        : e_ee_scheduled_event_result::FIELDS_DELIVERED;
}

uint64_t c_ee_cycle_scheduler::current_cycle() const
{
    return m_current_cycle;
}

uint64_t c_ee_cycle_scheduler::scheduler_yields() const
{
    return m_scheduler_yields;
}
} // namespace ee
