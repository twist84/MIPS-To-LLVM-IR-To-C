#include "iop_spu2.h"

#include <algorithm>

namespace iop
{
c_spu2::c_spu2()
    : m_ram(k_ram_words, 0u)
{
    reset();
}

void c_spu2::reset()
{
    m_clock_active = false;
    m_clock_cycles = 0u;
    for (s_core& core : m_cores)
    {
        core = s_core{};
        core.m_master[0] = 0x3fffu;
        core.m_master[1] = 0x3fffu;
        core.m_master[4] = 0x7fffu;
        core.m_master[5] = 0x7fffu;
        core.m_master[6] = 0x7fffu;
        core.m_master[7] = 0x7fffu;
    }
    std::fill(m_ram.begin(), m_ram.end(), 0u);
    m_spdif_out = 0u;
    m_spdif_mode = 0u;
    m_spdif_media = 0u;
}

uint32_t c_spu2::set_hi(uint32_t address, uint32_t value)
{
    return ((value & 0x0fu) << 16u) | (address & 0xffffu);
}

uint32_t c_spu2::set_lo(uint32_t address, uint32_t value)
{
    return (address & 0xf0000u) | (value & 0xffffu);
}

bool c_spu2::irq_pending() const
{
    return irq_latches() != 0u;
}

uint32_t c_spu2::irq_latches() const
{
    return (m_cores[0].m_irq ? 1u : 0u) |
           (m_cores[1].m_irq ? 2u : 0u);
}

bool c_spu2::auto_dma_enabled(uint32_t core) const
{
    return core < 2u &&
        (m_cores[core].m_registers[0x1b0u / 2u] &
         (uint16_t{1} << core)) != 0u;
}

bool c_spu2::ram_word(uint32_t address, uint16_t* value) const
{
    if (value == nullptr || address >= k_ram_words)
    {
        return false;
    }
    *value = m_ram[address];
    return true;
}

bool c_spu2::copy_dma(
    uint32_t core, std::vector<uint8_t>& data, bool to_spu
)
{
    if (core >= 2u || data.empty() || (data.size() & 1u) != 0u ||
        data.size() > k_ram_words * 2u ||
        (to_spu && auto_dma_enabled(core)))
    {
        return false;
    }

    m_clock_active = true;
    const uint32_t halfwords = static_cast<uint32_t>(data.size() / 2u);
    s_core& selected = m_cores[core];
    const uint32_t start = selected.m_tsa & (k_ram_words - 1u);
    if (to_spu)
    {
        // The primary reference checks both cores' IRQA against
        // the sound-RAM range written by DMA, including wraparound.
        for (s_core& candidate : m_cores)
        {
            const uint32_t distance =
                (candidate.m_irqa - start) & (k_ram_words - 1u);
            if ((candidate.m_attr & 0x40u) != 0u &&
                !candidate.m_irq && distance < halfwords)
            {
                candidate.m_irq = true;
            }
        }
    }
    for (uint32_t i = 0u; i < halfwords; ++i)
    {
        const uint32_t address = (start + i) & (k_ram_words - 1u);
        if (to_spu)
        {
            m_ram[address] = static_cast<uint16_t>(
                static_cast<uint16_t>(data[2u * i]) |
                (static_cast<uint16_t>(data[2u * i + 1u]) << 8u));
        }
        else
        {
            const uint16_t sample = m_ram[address];
            data[2u * i] = static_cast<uint8_t>(sample);
            data[2u * i + 1u] = static_cast<uint8_t>(sample >> 8u);
        }
    }
    selected.m_tsa = (start + halfwords) & (k_ram_words - 1u);
    return true;
}

bool c_spu2::read(uint32_t address, uint32_t width, uint32_t* value)
{
    if (value == nullptr || (width != 2u && width != 4u) ||
        address < k_base || address - k_base >= k_size ||
        (address & (width - 1u)) != 0u ||
        width > k_size - (address - k_base))
    {
        return false;
    }

    m_clock_active = true;
    const uint32_t offset = address - k_base;
    uint16_t low = 0u;
    if (!read16(offset, &low))
    {
        return false;
    }

    uint16_t high = 0u;
    if (width == 4u && !read16(offset + 2u, &high))
    {
        return false;
    }
    *value = static_cast<uint32_t>(low) |
             (static_cast<uint32_t>(high) << 16u);
    return true;
}

bool c_spu2::write(uint32_t address, uint32_t width, uint32_t value)
{
    if ((width != 2u && width != 4u) ||
        address < k_base || address - k_base >= k_size ||
        (address & (width - 1u)) != 0u ||
        width > k_size - (address - k_base))
    {
        return false;
    }

    m_clock_active = true;
    const uint32_t offset = address - k_base;
    if (!write16(offset, static_cast<uint16_t>(value)))
    {
        return false;
    }
    if (width == 4u &&
        !write16(offset + 2u, static_cast<uint16_t>(value >> 16u)))
    {
        return false;
    }
    return true;
}

bool c_spu2::read_core(
    uint32_t index, uint32_t offset, uint16_t* value
)
{
    s_core& core = m_cores[index];
    if (offset < 0x180u)
    {
        // ENVX and current sweep volumes require an actual voice clock.
        // Idle voices return zero; once keyed, refuse invented playback.
        const uint32_t voice = offset >> 4u;
        const uint32_t field = offset & 0x0fu;
        if ((core.m_key_mask & (1u << voice)) != 0u &&
            (field == 0x0au || field == 0x0cu || field == 0x0eu))
        {
            return false;
        }
    }
    else if (offset >= 0x1c0u && offset < 0x2e0u)
    {
        const uint32_t voice = (offset - 0x1c0u) / 12u;
        const uint32_t field = (offset - 0x1c0u) % 12u;
        if (field == 8u || field == 10u)
        {
            // NAX advances only when ADPCM playback actually consumes
            // samples. When the voice is inactive the reference starts
            // at SSA + 1; never invent progression for a keyed voice.
            if ((core.m_key_mask & (1u << voice)) != 0u)
            {
                return false;
            }
            const uint32_t base = 0x1c0u + voice * 12u;
            const uint32_t ssa =
                ((core.m_registers[base / 2u] & 0x0fu) << 16u) |
                core.m_registers[base / 2u + 1u];
            const uint32_t nax = (ssa + 1u) & (k_ram_words - 1u);
            *value = static_cast<uint16_t>(
                field == 8u ? nax >> 16u : nax
            );
            return true;
        }
    }

    switch (offset)
    {
    case 0x19au:
        *value = core.m_attr;
        return true;
    case 0x19cu:
        *value = static_cast<uint16_t>(core.m_irqa >> 16u);
        return true;
    case 0x19eu:
        *value = static_cast<uint16_t>(core.m_irqa);
        return true;
    case 0x1a8u:
        *value = static_cast<uint16_t>(core.m_tsa >> 16u);
        return true;
    case 0x1aau:
        *value = static_cast<uint16_t>(core.m_tsa);
        return true;
    case 0x1b0u:
        *value = core.m_registers[offset / 2u];
        return true;
    case 0x340u:
        *value = static_cast<uint16_t>(core.m_endx);
        return true;
    case 0x342u:
        *value = static_cast<uint16_t>(core.m_endx >> 16u);
        return true;
    case 0x344u:
        // Reference SPU2 STATX waits 16 reads after core enable, then
        // exposes the ordinary powered-up status bit.
        if (core.m_statx_settling != 0u)
        {
            --core.m_statx_settling;
            *value = 0u;
        }
        else
        {
            *value = 0x80u;
        }
        return true;
    default:
        *value = core.m_registers[offset / 2u];
        return true;
    }
}

bool c_spu2::write_core(
    uint32_t index, uint32_t offset, uint16_t value
)
{
    s_core& core = m_cores[index];

    // Preserve guest programming of voice, ADSR, reverb, and core
    // registers. Dynamic voice readback is explicitly gated above.
    core.m_registers[offset / 2u] = value;

    switch (offset)
    {
    case 0x19au:
        if ((value & 0x8000u) != 0u &&
            (core.m_attr & 0x8000u) == 0u)
        {
            core.m_statx_settling = 16u;
        }
        core.m_attr = value;
        if ((value & 0x40u) == 0u)
        {
            core.m_irq = false;
        }
        break;
    case 0x19cu:
        core.m_irqa = set_hi(core.m_irqa, value);
        break;
    case 0x19eu:
        core.m_irqa = set_lo(core.m_irqa, value);
        break;
    case 0x1a0u:
    case 0x1a2u:
    {
        const uint32_t bits =
            static_cast<uint32_t>(value) <<
            (offset == 0x1a2u ? 16u : 0u);
        core.m_key_mask |= bits;
        core.m_endx &= ~bits;
        break;
    }
    case 0x1a4u:
    case 0x1a6u:
        core.m_key_mask &=
            ~(static_cast<uint32_t>(value) <<
              (offset == 0x1a6u ? 16u : 0u));
        break;
    case 0x1a8u:
        core.m_tsa = set_hi(core.m_tsa, value);
        break;
    case 0x1aau:
        core.m_tsa = set_lo(core.m_tsa, value);
        break;
    case 0x1acu:
        // This is a real 16-bit sound-RAM data-port write. The SPU2
        // can assert IRQ9 only when its guest-programmed IRQA is hit;
        // it cannot manufacture DMA completion, key-on output, or
        // any periodically ticking mixer event.
        if ((core.m_attr & 0x40u) != 0u &&
            !core.m_irq && core.m_tsa == core.m_irqa)
        {
            core.m_irq = true;
        }
        m_ram[core.m_tsa & (k_ram_words - 1u)] = value;
        core.m_tsa = (core.m_tsa + 1u) & (k_ram_words - 1u);
        break;
    case 0x340u:
        core.m_endx &= 0xffff0000u;
        break;
    case 0x342u:
        core.m_endx &= 0x0000ffffu;
        break;
    default:
        break;
    }
    return true;
}

bool c_spu2::read16(uint32_t offset, uint16_t* value)
{
    if (offset < 0x760u)
    {
        return read_core(
            (offset & 0x400u) != 0u ? 1u : 0u,
            offset & 0x3feu, value
        );
    }
    if (offset < 0x7b0u)
    {
        const uint32_t bank = (offset - 0x760u) / 0x28u;
        const uint32_t field = (offset - 0x760u) % 0x28u;
        *value = field < 0x10u ?
            m_cores[bank].m_master[field / 2u] : 0u;
        return true;
    }

    switch (offset)
    {
    case 0x7c0u:
        *value = m_spdif_out;
        return true;
    case 0x7c2u:
        *value = static_cast<uint16_t>(
            (m_cores[0].m_irq ? 4u : 0u) |
            (m_cores[1].m_irq ? 8u : 0u)
        );
        return true;
    case 0x7c6u:
        *value = m_spdif_mode;
        return true;
    case 0x7c8u:
        *value = m_spdif_media;
        return true;
    case 0x7cau:
        // PS2SDK LIBSD writes 8 to this SPDIF control halfword during
        // InitSpdif(). The primary ps2-web-recomp/SDBZ model leaves it
        // unlatched and reads back zero. This is not a ready/status bit.
        *value = 0u;
        return true;
    default:
        // Unknown global status is not automatically a zero/ready bit.
        return false;
    }
}

bool c_spu2::write16(uint32_t offset, uint16_t value)
{
    if (offset < 0x760u)
    {
        return write_core(
            (offset & 0x400u) != 0u ? 1u : 0u,
            offset & 0x3feu, value
        );
    }
    if (offset < 0x7b0u)
    {
        const uint32_t bank = (offset - 0x760u) / 0x28u;
        const uint32_t field = (offset - 0x760u) % 0x28u;
        if (field < 0x10u)
        {
            m_cores[bank].m_master[field / 2u] = value;
        }
        return true;
    }

    switch (offset)
    {
    case 0x7c0u:
        m_spdif_out = value;
        return true;
    case 0x7c2u:
        // Reference write_global ignores the IRQ status halfword.
        // In particular SW at 0x7c0 must not fault after writing OUT.
        return true;
    case 0x7c6u:
        m_spdif_mode = value;
        return true;
    case 0x7c8u:
        m_spdif_media = value;
        return true;
    case 0x7cau:
        // PS2SDK LIBSD InitSpdif writes 8 here. The reference
        // ps2-web-recomp/SDBZ write_global() deliberately ignores it.
        // Accept the actual control write without inventing a latch,
        // sound readiness, DMA completion, an interrupt, or an RPC reply.
        return true;
    default:
        return false;
    }
}
} // namespace iop
