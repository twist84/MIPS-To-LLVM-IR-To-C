#pragma once

#include <cstdint>
#include <vector>

namespace iop
{
// Guest-visible 16-bit SPU2 register bank from ps2-web-recomp/SDBZ.
// Register/transfer state and a guest-cycle sample clock. No voice mixer,
// sampled audio, automatic DMA completion, or periodic SPU IRQ is inferred.
class c_spu2
{
public:
    static constexpr uint32_t k_base = 0x1f900000u;
    static constexpr uint32_t k_size = 0x800u;
    static constexpr uint32_t k_irq_line = 9u;
    static constexpr uint32_t k_ram_words = 1u << 20u;
    // Nominal PS2 IOP clock 36.864 MHz / 48 kHz (reference SPU2).
    static constexpr uint32_t k_cycles_per_sample = 768u;

    c_spu2();
    void reset();

    // Start on the first valid-width SPU2 access, like the reference.
    // Count elapsed guest cycles, not generated audio frames or host time.
    void advance_cycles(uint32_t cycles)
    {
        if (m_clock_active)
        {
            m_clock_cycles += cycles;
        }
    }
    bool clock_active() const { return m_clock_active; }
    uint64_t clock_cycles() const { return m_clock_cycles; }
    uint64_t elapsed_samples() const { return m_clock_cycles / k_cycles_per_sample; }
    uint32_t sample_phase() const
    {
        return static_cast<uint32_t>(m_clock_cycles % k_cycles_per_sample);
    }

    // Physical IOP bus addresses. 16- and 32-bit transfers only.
    bool read(uint32_t address, uint32_t width, uint32_t* value);
    bool write(uint32_t address, uint32_t width, uint32_t value);
    bool irq_pending() const;
    // One bit for each newly latched SPU2 core interrupt.
    uint32_t irq_latches() const;
    // SPU2 uses shared 2 MiB sound RAM for both core DMA channels.
    // An ordinary DMA copies real little-endian halfwords starting at
    // the core's guest-programmed TSA, wrapping in the sound RAM.
    // AutoDMA streaming is a different mode and is not implemented.
    bool auto_dma_enabled(uint32_t core) const;
    bool copy_dma(uint32_t core, std::vector<uint8_t>& data, bool to_spu);
    bool ram_word(uint32_t address, uint16_t* value) const;

private:
    struct s_core
    {
        // 0x000..0x3FF for each core, including voice parameter latches.
        uint16_t m_registers[0x200]{};
        // Reference master-volume window at 0x760 and 0x788.
        uint16_t m_master[8]{};
        uint32_t m_irqa = 0x800u;
        uint32_t m_tsa = 0u;
        uint32_t m_endx = 0u;
        uint32_t m_key_mask = 0u;
        uint32_t m_statx_settling = 0u;
        uint16_t m_attr = 0u;
        bool m_irq = false;
    };

    static uint32_t set_hi(uint32_t address, uint32_t value);
    static uint32_t set_lo(uint32_t address, uint32_t value);

    bool read16(uint32_t offset, uint16_t* value);
    bool write16(uint32_t offset, uint16_t value);
    bool read_core(uint32_t index, uint32_t offset, uint16_t* value);
    bool write_core(uint32_t index, uint32_t offset, uint16_t value);

    bool m_clock_active = false;
    uint64_t m_clock_cycles = 0u;
    s_core m_cores[2]{};
    // 2 MiB of SPU2 sound RAM, allocated on the heap (not the IOP stack).
    std::vector<uint16_t> m_ram;
    uint16_t m_spdif_out = 0u;
    uint16_t m_spdif_mode = 0u;
    uint16_t m_spdif_media = 0u;
};
} // namespace iop
