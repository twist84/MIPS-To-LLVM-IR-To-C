#pragma once

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

    e_result observe(uint64_t cycle)
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

    uint64_t due() const { return m_due; }

    bool consume(uint64_t cycles)
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
    uint64_t next_cycle() const
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

    bool active() const { return m_active; }

private:
    uint64_t m_last_cycle = 0u;
    uint64_t m_remainder = 0u;
    uint64_t m_due = 0u;
    bool m_active = false;
};
} // namespace ee
