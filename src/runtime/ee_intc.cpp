#include "ee_intc.h"
#include "ee_exceptions.h"

namespace ee
{
void c_ee_intc::sync_interrupt_line()
{
    if (m_cpu == nullptr)
    {
        return;
    }

    // The pending level is visible in COP0 Cause.IP2. Raising the level
    // does not itself perform guest exception entry: that is the EE
    // scheduler's responsibility.
    if (pending() != 0)
    {
        ee_interrupt_request(m_cpu, INT_INTC);
    }
    else
    {
        ee_interrupt_clear(m_cpu, INT_INTC);
    }
}

void c_ee_intc::bind_cpu(s_ee_cpu_state* cpu)
{
    if (m_cpu != nullptr && m_cpu != cpu)
    {
        ee_interrupt_clear(m_cpu, INT_INTC);
    }

    m_cpu = cpu;
    sync_interrupt_line();
}

void c_ee_intc::reset()
{
    m_status = 0;
    m_mask = 0;
    sync_interrupt_line();
}

bool c_ee_intc::raise(uint32_t cause)
{
    if (cause >= 32)
    {
        return false;
    }

    m_status |= uint32_t{1} << cause;
    sync_interrupt_line();
    return true;
}

bool c_ee_intc::mmio_read(void* user, uint32_t physical_address,
                          uint32_t width, s_ee_u128* result)
{
    if (user == nullptr || result == nullptr || width != 4)
    {
        return false;
    }

    const c_ee_intc* controller = static_cast<const c_ee_intc*>(user);
    switch (physical_address)
    {
    case k_stat_address:
        *result = {controller->m_status, 0};
        return true;
    case k_mask_address:
        *result = {controller->m_mask, 0};
        return true;
    default:
        return false;
    }
}

bool c_ee_intc::mmio_write(void* user, uint32_t physical_address,
                           uint32_t width, const s_ee_u128* value)
{
    if (user == nullptr || value == nullptr || width != 4)
    {
        return false;
    }

    c_ee_intc* controller = static_cast<c_ee_intc*>(user);
    const uint32_t word = static_cast<uint32_t>(value->m_lo);
    switch (physical_address)
    {
    case k_stat_address:
        // INTC_STAT is write-one-to-clear, not a regular storage register.
        controller->m_status &= ~word;
        controller->sync_interrupt_line();
        return true;
    case k_mask_address:
        // The real EE INTC_MASK is write-one-to-toggle.
        controller->m_mask ^= word;
        controller->sync_interrupt_line();
        return true;
    default:
        return false;
    }
}
} // namespace ee
