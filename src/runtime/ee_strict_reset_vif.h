#pragma once

#include "ee_strict_reset.h"
#include "ee_vif1.h"

namespace ee
{
// Reuse the production VIF command-state reset, with isolated cold
// devices and no DMA, interrupt or VU execution callbacks attached.
class c_ee_reset_vif_devices
{
public:
    c_ee_reset_vif_devices()
    {
        m_vif0.bind_vu0(&m_vu0);
    }

    s_ee_reset_vif_write transport()
    {
        return {this, &write};
    }

    uint32_t reset_count(uint32_t channel) const
    {
        return channel < 2u ? m_reset_count[channel] : 0u;
    }

    c_ee_vif1& device(uint32_t channel)
    {
        return channel == 0u ? m_vif0 : m_vif1;
    }

private:
    static bool write(void* user, uint32_t address, uint32_t value)
    {
        if (user == nullptr || value != 1u ||
            (address != 0x10003810u && address != 0x10003c10u))
        {
            return false;
        }
        auto& self = *static_cast<c_ee_reset_vif_devices*>(user);
        const uint32_t channel = address == 0x10003810u ? 0u : 1u;
        const s_ee_u128 word{value, 0u};
        if (!c_ee_vif1::mmio_write(&self.device(channel), address, 4u, &word))
        {
            return false;
        }
        ++self.m_reset_count[channel];
        return true;
    }

    s_ee_vu0_state m_vu0{};
    c_ee_vif1 m_vif0{};
    c_ee_vif1 m_vif1{};
    uint32_t m_reset_count[2]{};
};
} // namespace ee
