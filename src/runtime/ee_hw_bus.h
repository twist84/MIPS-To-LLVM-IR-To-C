#pragma once

#include "ee_dmac.h"
#include "ee_gs_display.h"
#include "ee_intc.h"
#include "ee_scanout_clock.h"
#include "ee_sif.h"
#include "ee_vif1.h"
#include "ipu/ee_ipu.h"

#include <cstdint>
#include <vector>

namespace ee
{
// Minimal host MMIO dispatcher. It only routes registers implemented by
// these device models; unsupported addresses do not acquire fake state.
class c_ee_hw_bus
{
public:
    c_ee_hw_bus()
    {
        // The IPU pulls/pushes through DMA channels 4/3 and raises INTC 8.
        m_ipu.bind(&m_dmac, &m_intc);
        m_dmac.bind_ipu(&m_ipu, &c_ee_ipu::dma_started_callback);
    }
    c_ee_hw_bus(const c_ee_hw_bus&) = delete;
    c_ee_hw_bus& operator=(const c_ee_hw_bus&) = delete;

    void bind_cpu(s_ee_cpu_state* cpu)
    {
        m_intc.bind_cpu(cpu);
        m_dmac.bind_cpu(cpu);
    }

    void bind_memory(ee_memory* memory)
    {
        m_dmac.bind_memory(memory);
    }

    void bind_device_transfer(
        void* user, c_ee_dmac::t_device_transfer_callback callback)
    {
        m_dmac.bind_device_transfer(user, callback);
    }

    void bind_vif1(c_ee_vif1* vif1)
    {
        m_vif1 = vif1;
    }

    void bind_device_download(
        void* user, c_ee_dmac::t_device_download_callback callback)
    {
        m_dmac.bind_device_download(user, callback);
    }

    void reset()
    {
        m_intc.reset();
        m_dmac.reset();
        if (m_vif1 != nullptr)
        {
            m_vif1->reset();
        }
        m_sif.reset();
        m_ipu.reset();
        m_gs_imr = 0u;
        m_gs_display = {};
        m_scanout = {};
        m_gs_crt_interlace = 0u;
        m_gs_crt_mode = 0u;
        m_gs_crt_ffmd = 0u;
    }

    c_ee_intc& intc()
    {
        return m_intc;
    }

    c_ee_dmac& dmac()
    {
        return m_dmac;
    }

    c_ee_sif& sif()
    {
        return m_sif;
    }

    c_ee_ipu& ipu()
    {
        return m_ipu;
    }

    c_ee_scanout_clock& scanout()
    {
        return m_scanout;
    }

    const c_ee_scanout_clock& scanout() const
    {
        return m_scanout;
    }

    const s_ee_gs_display_registers& gs_display_registers() const
    {
        return m_gs_display;
    }

    uint32_t gs_crt_mode() const
    {
        return m_gs_crt_mode;
    }

    // These are privileged GS registers, not the GIF A+D register bank.
    // No write below can advance scanout, signal VBlank or draw primitives.
    static int32_t display_register_index(uint32_t address)
    {
        switch (address & ~4u)
        {
        case 0x12000000u: return 0; // PMODE
        case 0x12000020u: return 1; // SMODE2
        case 0x12000070u: return 2; // DISPFB1
        case 0x12000080u: return 3; // DISPLAY1
        case 0x12000090u: return 4; // DISPFB2
        case 0x120000a0u: return 5; // DISPLAY2
        default: return -1;
        }
    }

    static constexpr uint32_t k_gs_csr = 0x12001000u;
    static constexpr uint32_t k_gs_imr = 0x12001010u;

    // GS_IMR is a privileged GS register, rather than a GIF packet register.
    // It is reached by the EE kernel's GsPutIMR syscall.  The current GS
    // model has no IRQ delivery, but retains the documented mask field so
    // kernel calls and later reads observe coherent hardware state.
    void set_gs_imr(uint64_t value)
    {
        m_gs_imr = value & 0x0000ff00ULL;
    }

    uint64_t gs_imr() const
    {
        return m_gs_imr;
    }

    // SetGsCrt configures the GS display circuit.  Presentation and vblank
    // are not modelled here, but retaining this hardware configuration lets
    // GS setup code observe a coherent boot-time display state.
    bool set_gs_crt(uint32_t interlace, uint32_t mode, uint32_t ffmd,
                    uint64_t current_ee_cycle = 0u)
    {
        if (interlace > 1u || ffmd > 1u || mode > 0xffu)
        {
            return false;
        }
        // Establish the field epoch at the supplied EE cycle. An
        // unvalidated runner supplies zero and cannot advance the clock.
        // Do not partially commit CRT mode fields on timing overflow.
        if (mode == 2u || mode == 3u)
        {
            if (!m_scanout.configure(
                    interlace, mode, ffmd, current_ee_cycle))
            {
                return false;
            }
        }
        else
        {
            // No calibrated field rate exists for other CRT modes.
            // A previous NTSC/PAL configuration must not keep firing.
            m_scanout = {};
        }
        m_gs_crt_interlace = interlace;
        m_gs_crt_mode = mode;
        m_gs_crt_ffmd = ffmd;
        return true;
    }

    // Host-only observation of IPU registers, IPU FIFOs and DMA channels
    // 3/4 (fromIPU/toIPU). Observation never claims the access.
    struct s_ipu_observation
    {
        struct s_access
        {
            uint32_t address = 0u;
            uint32_t width = 0u;
            uint64_t value = 0u;
            bool write = false;
        };
        uint64_t reads = 0u;
        uint64_t writes = 0u;
        uint64_t commands[16] = {};
        std::vector<s_access> first;
    };

    static bool is_ipu_address(uint32_t address)
    {
        return (address >= 0x10002000u && address < 0x10002040u) ||
               (address >= 0x10007000u && address < 0x10007020u) ||
               (address >= 0x1000b000u && address < 0x1000b040u) ||
               (address >= 0x1000b400u && address < 0x1000b440u);
    }

    void observe_ipu(uint32_t address, uint32_t width, uint64_t value, bool write)
    {
        (write ? m_ipu_observation.writes : m_ipu_observation.reads) += 1u;
        if (write && address == 0x10002000u)
        {
            m_ipu_observation.commands[(value >> 28u) & 15u] += 1u;
        }
        if (m_ipu_observation.first.size() < 4096u)
        {
            m_ipu_observation.first.push_back({address, width, value, write});
        }
    }

    const s_ipu_observation& ipu_observation() const
    {
        return m_ipu_observation;
    }

    static bool mmio_read(void* user, uint32_t address,
                          uint32_t width, s_ee_u128* value)
    {
        if (user == nullptr)
        {
            return false;
        }
        if (is_ipu_address(address))
        {
            // Observe the value actually returned by the device.
            const bool handled = mmio_read_device(user, address, width, value);
            static_cast<c_ee_hw_bus*>(user)->observe_ipu(
                address, width,
                handled && value != nullptr ? value->m_lo : 0u, false);
            return handled;
        }
        return mmio_read_device(user, address, width, value);
    }

    static bool mmio_read_device(void* user, uint32_t address,
                                 uint32_t width, s_ee_u128* value)
    {
        c_ee_hw_bus* bus = static_cast<c_ee_hw_bus*>(user);
        if (bus->m_dmac.deferred_vif1_pending())
        {
            if (address == c_ee_vif1::k_register_base ||
                (address >= c_ee_dmac::k_vif1_base &&
                 address < c_ee_dmac::k_vif1_base + 0x100u) ||
                address == c_ee_dmac::k_stat_address)
            {
                // Polls may observe real outstanding work; they do
                // not force a join or invent a completed DMA.
                (void)bus->m_dmac.service_deferred_vif1();
            }
            else if (is_vif1_ordered_address(address))
            {
                (void)bus->m_dmac.service_deferred_vif1(true);
            }
        }
        const int32_t display_index = display_register_index(address);
        if (display_index >= 0)
        {
            if (value == nullptr || (width != 4u && width != 8u) ||
                (width == 8u && (address & 4u) != 0u))
            {
                return false;
            }
            const uint64_t data = bus->m_gs_display.register_value(
                static_cast<uint32_t>(display_index));
            value->m_lo = (address & 4u) != 0u ? data >> 32u : data;
            value->m_hi = 0u;
            return true;
        }
        if (value != nullptr && (width == 4u || width == 8u) &&
            (address == k_gs_csr || address == k_gs_imr ||
             (width == 4u &&
              (address == k_gs_csr + 4u || address == k_gs_imr + 4u))))
        {
            const uint64_t register_value =
                (address == k_gs_csr || address == k_gs_csr + 4u)
                    ? bus->m_scanout.csr() : bus->m_gs_imr;
            value->m_lo = (address & 4u) != 0u
                ? register_value >> 32u : register_value;
            value->m_hi = 0u;
            return true;
        }
        return c_ee_ipu::mmio_read(&bus->m_ipu, address, width, value) ||
               (bus->m_vif1 != nullptr &&
                c_ee_vif1::mmio_read(bus->m_vif1, address, width, value)) ||
               c_ee_intc::mmio_read(&bus->m_intc, address, width, value) ||
               c_ee_dmac::mmio_read(&bus->m_dmac, address, width, value) ||
               c_ee_sif::mmio_read(&bus->m_sif, address, width, value);
    }

    static bool mmio_write(void* user, uint32_t address,
                           uint32_t width, const s_ee_u128* value)
    {
        if (user == nullptr)
        {
            return false;
        }
        if (is_ipu_address(address))
        {
            static_cast<c_ee_hw_bus*>(user)->observe_ipu(
                address, width, value != nullptr ? value->m_lo : 0u, true);
        }

        c_ee_hw_bus* bus = static_cast<c_ee_hw_bus*>(user);
        if (bus->m_dmac.deferred_vif1_pending() &&
            is_vif1_ordered_address(address))
        {
            if (address == c_ee_vif1::k_register_base + 0x10u &&
                width == 4u && value != nullptr && (value->m_lo & 1u) != 0u)
            {
                // FBRST discards uncommitted VIF output, leaving DMA
                // registers for the guest to stop/reset explicitly.
                bus->m_dmac.cancel_deferred_vif1();
            }
            else
            {
                (void)bus->m_dmac.service_deferred_vif1(true);
            }
        }
        const int32_t display_index = display_register_index(address);
        if (display_index >= 0)
        {
            if (value == nullptr || (width != 4u && width != 8u) ||
                (width == 8u && (address & 4u) != 0u))
            {
                return false;
            }
            const size_t index = static_cast<size_t>(display_index);
            uint64_t data = bus->m_gs_display.register_value(
                static_cast<uint32_t>(index));
            if (width == 8u)
            {
                data = value->m_lo;
            }
            else if ((address & 4u) != 0u)
            {
                data = (data & 0xffffffffull) |
                    ((value->m_lo & 0xffffffffull) << 32u);
            }
            else
            {
                data = (data & 0xffffffff00000000ull) |
                    (value->m_lo & 0xffffffffull);
            }
            bus->m_gs_display.set_register(
                static_cast<uint32_t>(index), data);
            return true;
        }
        if (value != nullptr && (width == 4u || width == 8u) &&
            (address == k_gs_csr || address == k_gs_imr ||
             (width == 4u &&
              (address == k_gs_csr + 4u || address == k_gs_imr + 4u))))
        {
            const uint64_t written = (address & 4u) != 0u
                ? value->m_lo << 32u : value->m_lo;
            if (address == k_gs_csr || address == k_gs_csr + 4u)
            {
                bus->m_scanout.acknowledge_csr(written);
            }
            else
            {
                // Privileged mask register, no event generated by writes.
                bus->set_gs_imr(written);
            }
            return true;
        }
        return c_ee_ipu::mmio_write(&bus->m_ipu, address, width, value) ||
               (bus->m_vif1 != nullptr &&
                c_ee_vif1::mmio_write(bus->m_vif1, address, width, value)) ||
               c_ee_intc::mmio_write(&bus->m_intc, address, width, value) ||
               c_ee_dmac::mmio_write(&bus->m_dmac, address, width, value) ||
               c_ee_sif::mmio_write(&bus->m_sif, address, width, value);
    }

private:
    static bool is_vif1_ordered_address(uint32_t address)
    {
        return (address >= c_ee_vif1::k_register_base &&
                address < c_ee_vif1::k_register_base + 0x400u) ||
               (address >= c_ee_vif1::k_fifo_base &&
                address < c_ee_vif1::k_fifo_base + 0x1000u) ||
               (address >= c_ee_vif1::k_vu1_code_base &&
                address < c_ee_vif1::k_vu1_data_base +
                    c_ee_vif1::k_vu1_memory_size) ||
               (address >= 0x12000000u && address < 0x12002000u);
    }

    c_ee_intc m_intc;
    c_ee_dmac m_dmac;
    c_ee_vif1* m_vif1 = nullptr;
    c_ee_sif m_sif;
    c_ee_ipu m_ipu;
    c_ee_scanout_clock m_scanout;
    uint64_t m_gs_imr = 0u;
    s_ee_gs_display_registers m_gs_display{};
    uint32_t m_gs_crt_interlace = 0u;
    uint32_t m_gs_crt_mode = 0u;
    uint32_t m_gs_crt_ffmd = 0u;
    s_ipu_observation m_ipu_observation;
};
} // namespace ee
