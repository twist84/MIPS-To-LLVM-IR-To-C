#pragma once

#include "ee_memory.h"
#include <cstdint>

namespace ee
{
// Host-only callback-boundary correlation for the retail command-slot
// wait. Kept outside the EE lowering/cache provenance input directories.
class c_ee_sdbz_slot_callback_probe
{
public:
    static constexpr uint32_t k_poll_pc = 0x001025a4u;
    static constexpr uint32_t k_vif1_handler_pc = 0x00102a60u;
    static constexpr uint32_t k_slot_flag_base = 0x00502368u;
    static constexpr uint32_t k_slot_selector = 0x005030acu;

    struct s_slot_observation
    {
        uint32_t m_selector = 0u;
        uint32_t m_flag0 = 0u;
        uint32_t m_flag1 = 0u;
    };

    // Sample backed RAM only. No MMIO or guest timer reads, and no
    // writes to the slot, DMA or interrupt state.
    static bool sample_slots(ee_memory* memory, s_slot_observation& sample)
    {
        if (memory == nullptr ||
            !memory->contains(k_slot_flag_base, 8u) ||
            !memory->contains(k_slot_selector, 4u))
        {
            return false;
        }
        sample.m_selector = memory->read32(k_slot_selector);
        sample.m_flag0 = memory->read32(k_slot_flag_base);
        sample.m_flag1 = memory->read32(k_slot_flag_base + 4u);
        return true;
    }

    // Only correlate the previously decoded retail handler with a
    // suspended command-slot loop. A callback boundary is not an
    // instruction-level writer trace.
    static bool is_slot_callback(bool is_intc, uint32_t cause,
                                 uint32_t handler_pc, uint32_t resume_pc)
    {
        return !is_intc && cause == 1u &&
            handler_pc == k_vif1_handler_pc && resume_pc == k_poll_pc;
    }
};
} // namespace ee
