#pragma once

#include "ee_strict_reset.h"
#include "ee_vif1.h"
#include "ee_intc.h"
#include "../tools/ee_host_timers.h"

namespace ee
{
// Reuse the production VIF and INTC register models with isolated cold
// devices. No DMA, interrupt sources, or VU execution callbacks attached.
class c_ee_reset_vif_devices
{
public:
    c_ee_reset_vif_devices()
    {
        m_vif0.bind_vu0(&m_vu0);
    }

    s_ee_reset_mmio transport()
    {
        return {this, &write, &read};
    }

    struct s_timer_write
    {
        uint32_t m_address = 0u;
        uint32_t m_value = 0u;
        uint32_t m_mode = 0u;
        uint32_t m_count = 0u;
    };

    uint32_t timer_write_count() const { return m_timer_write_count; }
    s_timer_write timer_write(uint32_t index) const
    {
        return index < 4u ? m_timer_writes[index] : s_timer_write{};
    }

    uint32_t intc_mask() const { return m_intc.mask(); }
    uint32_t intc_status() const { return m_intc.status(); }
    uint32_t intc_read_count() const { return m_intc_read_count; }
    uint32_t intc_write_count() const { return m_intc_write_count; }
    uint32_t first_intc_read() const { return m_first_intc_read; }
    uint32_t first_intc_write() const { return m_first_intc_write; }
    uint32_t first_intc_mask() const { return m_first_intc_mask; }

    uint32_t reset_count(uint32_t channel) const
    {
        return channel < 2u ? m_reset_count[channel] : 0u;
    }

    uint32_t fifo_count(uint32_t channel) const
    {
        return channel < 2u ? m_fifo_count[channel] : 0u;
    }

    s_ee_u128 last_fifo(uint32_t channel) const
    {
        return channel < 2u ? m_last_fifo[channel] : s_ee_u128{};
    }

    c_ee_vif1& device(uint32_t channel)
    {
        return channel == 0u ? m_vif0 : m_vif1;
    }

private:
    static bool read(void* user, uint32_t address, uint32_t width, s_ee_u128* value)
    {
        if (user == nullptr || value == nullptr || address != c_ee_intc::k_mask_address ||
            width != 4u)
        {
            return false;
        }
        auto& self = *static_cast<c_ee_reset_vif_devices*>(user);
        if (!c_ee_intc::mmio_read(&self.m_intc, address, width, value))
        {
            return false;
        }
        if (self.m_intc_read_count++ == 0u)
        {
            self.m_first_intc_read = static_cast<uint32_t>(value->m_lo);
        }
        return true;
    }

    static bool write(void* user, uint32_t address, uint32_t width, const s_ee_u128* value)
    {
        if (user == nullptr || value == nullptr)
        {
            return false;
        }
        auto& self = *static_cast<c_ee_reset_vif_devices*>(user);
        if (width == 4u &&
            (address == 0x10001800u || address == 0x10001810u || address == 0x10001820u))
        {
            // Register programming only. No clock is supplied to this
            // isolated host timer; guest reads are still refused.
            if (!c_ee_host_timers::mmio_write(&self.m_timers, address, width, value))
            {
                return false;
            }
            if (self.m_timer_write_count < 4u)
            {
                self.m_timer_writes[self.m_timer_write_count] = {address,
                    static_cast<uint32_t>(value->m_lo), self.m_timers.current_mode(3u),
                    self.m_timers.current_count(3u)};
            }
            ++self.m_timer_write_count;
            return true;
        }
        if (address == c_ee_intc::k_mask_address && width == 4u)
        {
            if (!c_ee_intc::mmio_write(&self.m_intc, address, width, value))
            {
                return false;
            }
            if (self.m_intc_write_count++ == 0u)
            {
                self.m_first_intc_write = static_cast<uint32_t>(value->m_lo);
                self.m_first_intc_mask = self.m_intc.mask();
            }
            return true;
        }
        const bool reset = width == 4u && static_cast<uint32_t>(value->m_lo) == 1u &&
            (address == 0x10003810u || address == 0x10003c10u);
        const bool fifo = width == 16u && (address == 0x10004000u || address == 0x10005000u);
        if (!reset && !fifo)
        {
            return false;
        }
        const uint32_t channel = address == 0x10003810u || address == 0x10004000u ? 0u : 1u;
        if (!c_ee_vif1::mmio_write(&self.device(channel), address, width, value))
        {
            return false;
        }
        if (reset)
        {
            ++self.m_reset_count[channel];
        }
        else
        {
            ++self.m_fifo_count[channel];
            self.m_last_fifo[channel] = *value;
        }
        return true;
    }

    s_ee_vu0_state m_vu0{};
    c_ee_vif1 m_vif0{};
    c_ee_vif1 m_vif1{};
    uint32_t m_reset_count[2]{};
    uint32_t m_fifo_count[2]{};
    s_ee_u128 m_last_fifo[2]{};
    c_ee_intc m_intc{};
    c_ee_host_timers m_timers{nullptr, nullptr};
    uint32_t m_timer_write_count = 0u;
    s_timer_write m_timer_writes[4]{};
    uint32_t m_intc_read_count = 0u;
    uint32_t m_intc_write_count = 0u;
    uint32_t m_first_intc_read = 0u;
    uint32_t m_first_intc_write = 0u;
    uint32_t m_first_intc_mask = 0u;
};
} // namespace ee
