#pragma once

#include "ee_runtime.h"

#include <cstdint>

namespace ee
{
// EE INTC register model. The status/mask operations follow
// ps2-web-recomp/runtime/src/hw.cpp. This is not a hardware scheduler:
// devices must explicitly call raise(), and the host still decides when
// to run the guest's interrupt exception handler.
class c_ee_intc
{
public:
    static constexpr uint32_t k_stat_address = 0x1000f000u;
    static constexpr uint32_t k_mask_address = 0x1000f010u;

    void bind_cpu(s_ee_cpu_state* cpu);
    void reset();
    bool raise(uint32_t cause);

    uint32_t status() const
    {
        return m_status;
    }
    uint32_t mask() const
    {
        return m_mask;
    }
    uint32_t pending() const
    {
        return m_status & m_mask;
    }

    static bool mmio_read(void* user, uint32_t physical_address,
                          uint32_t width, s_ee_u128* result);
    static bool mmio_write(void* user, uint32_t physical_address,
                           uint32_t width, const s_ee_u128* value);

private:
    void sync_interrupt_line();

    s_ee_cpu_state* m_cpu = nullptr;
    uint32_t m_status = 0;
    uint32_t m_mask = 0;
};
} // namespace ee
