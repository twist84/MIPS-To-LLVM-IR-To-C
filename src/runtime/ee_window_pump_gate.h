#pragma once

#include <cstdint>

namespace ee
{
// Host-only polling gate. Never suppress a newly observed guest field.
// The stride counts device-service calls, not cycles or guest events.
class c_ee_window_pump_gate final
{
public:
    explicit c_ee_window_pump_gate(uint32_t stride = 1u)
        : m_stride(stride != 0u ? stride : 1u)
    {
    }

    bool should_pump(uint64_t field)
    {
        bool pump = false;
        if (field != m_last_field || m_remaining == 0u)
        {
            m_last_field = field;
            m_remaining = m_stride - 1u;
            pump = true;
        }
        else
        {
            --m_remaining;
        }
        return pump;
    }

private:
    uint32_t m_stride = 1u;
    uint32_t m_remaining = 0u;
    uint64_t m_last_field = 0u;
};
} // namespace ee
