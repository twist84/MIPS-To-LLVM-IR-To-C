#pragma once

#include <cstdint>

namespace ee
{
class c_ee_hw_bus;
class c_ee_vsync_flags;
class ee_memory;

// Reference-style event budgets are a boot compatibility mode, not
// independently validated R5900 instruction issue/retirement timing.
enum class e_ee_cycle_source : uint8_t
{
    UNAVAILABLE = 0,
    VALIDATED_R5900 = 1,
    REFERENCE_COMPATIBILITY = 2
};

enum class e_ee_scheduled_event_result : uint8_t
{
    NO_VALIDATED_CLOCK = 0,
    NO_CONFIGURED_SCANOUT,
    NO_DUE_FIELD,
    FIELDS_DELIVERED,
    INVALID_TIMESTAMP_OR_DELIVERY
};

// Owns one monotonic EE time domain. It never computes instruction timing.
// A validated R5900 producer and an explicitly labelled approximate
// compatibility budget are distinct source types.
// Publishers must act in guest execution order, and service hardware only
// at safepoints after committing corresponding CPU/memory effects.
class c_ee_cycle_scheduler
{
public:
    void reset();
    bool install_validated_source();
    bool install_reference_compatibility_source();
    bool publish_cycle(uint64_t absolute_cycle);
    void record_scheduler_yield();

    e_ee_scheduled_event_result service_scanout(
        c_ee_hw_bus& hardware,
        c_ee_vsync_flags& vsync_flags,
        ee_memory& memory
    );

    e_ee_cycle_source source() const { return m_source; }
    uint64_t current_cycle() const { return m_current_cycle; }
    uint64_t scheduler_yields() const;

private:
    uint64_t m_current_cycle = 0u;
    uint64_t m_scheduler_yields = 0u;
    e_ee_cycle_source m_source = e_ee_cycle_source::UNAVAILABLE;
};

} // namespace ee
