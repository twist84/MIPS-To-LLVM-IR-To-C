#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace ee
{
// Reference compatibility clock: eight EE budget cycles per IOP cycle.
// This is not a calibrated R5900 timing model. Keep the fractional
// remainder and outstanding debt across host dispatch boundaries.
class c_ee_iop_clock
{
public:
    enum class e_result : uint8_t
    {
        accepted,
        nonmonotonic,
        overflow
    };

    static constexpr uint64_t k_no_deadline = std::numeric_limits<uint64_t>::max();

    constexpr e_result observe(uint64_t cycle)
    {
        if (!m_active)
        {
            m_active = true;
            m_last_cycle = cycle;
            return e_result::accepted;
        }
        if (cycle < m_last_cycle)
        {
            return e_result::nonmonotonic;
        }

        const uint64_t delta = cycle - m_last_cycle;
        const uint64_t fraction = (delta % 8u) + m_remainder;
        const uint64_t accrued = delta / 8u + fraction / 8u;
        if (m_due > k_no_deadline - accrued)
        {
            return e_result::overflow;
        }
        m_last_cycle = cycle;
        m_due += accrued;
        m_remainder = fraction % 8u;
        return e_result::accepted;
    }

    constexpr uint64_t due() const { return m_due; }

    constexpr bool consume(uint64_t cycles)
    {
        if (cycles > m_due)
        {
            return false;
        }
        m_due -= cycles;
        return true;
    }

    // Earliest EE timestamp at which the existing service cadence has
    // IOP work due. An outstanding backlog is due immediately. A
    // not-yet-anchored or unrepresentable deadline cannot authorize
    // native chaining; the caller must handle those cases separately.
    constexpr uint64_t next_cycle() const
    {
        if (!m_active)
        {
            return k_no_deadline;
        }
        if (m_due != 0u)
        {
            return m_last_cycle;
        }
        const uint64_t distance = 8u - m_remainder;
        return distance < k_no_deadline - m_last_cycle ?
            m_last_cycle + distance : k_no_deadline;
    }

    // Project a relative kernel/device deadline onto the compatibility
    // clock. Existing debt must first be serviced; do not extrapolate
    // from an unanchored clock or overflow the timestamp.
    constexpr uint64_t cycle_after_iop(uint64_t cycles) const
    {
        if (!m_active || cycles == k_no_deadline)
        {
            return k_no_deadline;
        }
        if (m_due != 0u || cycles == 0u)
        {
            return m_last_cycle;
        }
        if (cycles > k_no_deadline / 8u)
        {
            return k_no_deadline;
        }
        const uint64_t distance = cycles * 8u - m_remainder;
        return distance < k_no_deadline - m_last_cycle ?
            m_last_cycle + distance : k_no_deadline;
    }

    // A projection is only meaningful from the actual published EE
    // boundary, not a stale observation before a host continuation.
    constexpr bool anchored_at(uint64_t cycle) const
    {
        return m_active && cycle != k_no_deadline && m_last_cycle == cycle;
    }

    constexpr bool active() const { return m_active; }

private:
    uint64_t m_last_cycle = 0u;
    uint64_t m_remainder = 0u;
    uint64_t m_due = 0u;
    bool m_active = false;
};
// Preserve the legacy 256-cycle service turns and 64-turn cap. Only
// combine complete original turns strictly before a known IOP event;
// never split a turn at a predicted event or consume more debt than due.
struct s_ee_iop_service_batch
{
    uint32_t m_cycles = 0u;
    uint32_t m_turns = 0u;
};

inline constexpr s_ee_iop_service_batch ee_iop_service_batch(
    uint64_t due, uint32_t turns_left, uint64_t idle_interval)
{
    constexpr uint32_t turn_cycles = 256u;
    if (due == 0u || turns_left == 0u)
    {
        return {};
    }
    const uint32_t ordinary = static_cast<uint32_t>(
        std::min<uint64_t>(due, turn_cycles));
    if (idle_interval <= turn_cycles ||
        idle_interval == c_ee_iop_clock::k_no_deadline || turns_left == 1u)
    {
        return {ordinary, 1u};
    }
    const uint64_t cap = static_cast<uint64_t>(std::min(turns_left, 64u)) * turn_cycles;
    uint64_t cycles = std::min({due, cap, idle_interval - 1u});
    if (cycles < due)
    {
        cycles -= cycles % turn_cycles;
    }
    if (cycles <= ordinary)
    {
        return {ordinary, 1u};
    }
    return {static_cast<uint32_t>(cycles),
        static_cast<uint32_t>((cycles + turn_cycles - 1u) / turn_cycles)};
}

} // namespace ee
