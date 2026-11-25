#include "ee_gs.h"

#include <array>
#include <cstdint>
#include <cstdio>

namespace
{
int fail(const char* reason)
{
    std::fprintf(stderr, "GS HWREG: %s\n", reason);
    return 1;
}

uint64_t bitblt(uint32_t bp, uint32_t bw, uint32_t psm,
                bool source = false)
{
    return source ?
        static_cast<uint64_t>(bp) |
        (static_cast<uint64_t>(bw) << 16u) |
        (static_cast<uint64_t>(psm) << 24u) :
        (static_cast<uint64_t>(bp) << 32u) |
        (static_cast<uint64_t>(bw) << 48u) |
        (static_cast<uint64_t>(psm) << 56u);
}

uint64_t trxreg(uint32_t width, uint32_t height)
{
    return width | (static_cast<uint64_t>(height) << 32u);
}

uint64_t word64(const uint8_t* bytes)
{
    uint64_t result = 0u;
    for (uint32_t i = 0u; i < 8u; ++i)
    {
        result |= static_cast<uint64_t>(bytes[i]) << (i * 8u);
    }
    return result;
}

bool reg(ee::c_ee_gs_state& gs, uint32_t index, uint64_t value)
{
    ee::s_ee_gif_event event{};
    event.m_type = ee::e_ee_gif_event_type::packed;
    event.m_register = 0xeu;
    event.m_word0 = value;
    event.m_word1 = index;
    const std::array<uint8_t, 16> dummy{};
    return gs.consume(&event, 1u, dummy.data(), 1u);
}

bool image(ee::c_ee_gs_state& gs, const uint8_t* bytes,
           uint32_t qwc)
{
    ee::s_ee_gif_event event{};
    event.m_type = ee::e_ee_gif_event_type::image;
    event.m_qwc = qwc;
    return gs.consume(&event, 1u, bytes, qwc);
}

bool setup(ee::c_ee_gs_state& gs, uint32_t bp,
           uint32_t psm, uint32_t width,
           uint32_t height, uint32_t dir = 0u)
{
    return reg(gs, 0x50u, bitblt(bp, 1u, psm, dir == 1u)) &&
        reg(gs, 0x51u, 0u) &&
        reg(gs, 0x52u, trxreg(width, height)) &&
        reg(gs, 0x53u, dir);
}

bool read32(const ee::c_ee_gs_state& gs,
            uint32_t offset, uint32_t expected)
{
    uint32_t value = 0u;
    return gs.read_vram32(offset, value) && value == expected;
}

bool test_all_formats()
{
    const uint8_t data[8] =
    {
        0x11u, 0x22u, 0x33u, 0x44u,
        0x55u, 0x66u, 0x77u, 0x88u
    };
    struct s_format
    {
        uint32_t psm;
        uint32_t bits;
    };
    const s_format formats[] =
    {
        {0x00u, 32u}, {0x01u, 24u}, {0x02u, 16u},
        {0x0au, 16u}, {0x13u, 8u}, {0x14u, 4u},
        {0x1bu, 8u}, {0x24u, 4u}, {0x2cu, 4u},
        {0x30u, 32u}, {0x31u, 24u}, {0x32u, 16u},
        {0x3au, 16u}
    };

    ee::c_ee_gs_state gs;
    for (const s_format& f : formats)
    {
        gs.reset();
        const uint32_t pixels =
            f.bits == 24u ? 2u : 64u / f.bits;
        if (!setup(gs, 0x180u, f.psm, pixels, 1u) ||
            !reg(gs, 0x54u, word64(data)) ||
            gs.state().m_hwreg_write_count != 1u ||
            gs.state().m_hwreg_bytes != 8u ||
            gs.state().m_transfer_pixels_written != pixels ||
            gs.state().m_image_qwc != 0u ||
            !gs.state().m_image_data.empty() ||
            // 24-bit writes stop at the completed two-pixel
            // rectangle; its unused final HWREG bytes are padding.
            gs.state().m_transfer_pending_size != 0u ||
            !gs.was_written(0x54u) ||
            gs.register_value(0x54u) != word64(data))
        {
            return false;
        }

        if (!setup(gs, 0x180u, f.psm, pixels, 1u, 1u))
        {
            return false;
        }
        std::array<uint8_t, 16> readback{};
        readback.fill(0xeeu);
        if (!gs.read_image(readback.data(), 1u))
        {
            return false;
        }
        for (size_t i = 0u; i < readback.size(); ++i)
        {
            const uint8_t expected =
                i < (f.bits == 24u ? 6u : 8u) ? data[i] : 0u;
            if (readback[i] != expected)
            {
                return false;
            }
        }
    }
    gs.reset();
    return gs.state().m_hwreg_write_count == 0u &&
        gs.state().m_hwreg_bytes == 0u;
}

bool test_mixed_24_bit()
{
    ee::c_ee_gs_state gs;
    constexpr uint32_t bp = 0x100u;
    constexpr uint32_t base = bp * 256u;
    constexpr uint32_t offsets[9] =
        {0u, 4u, 16u, 20u, 32u, 36u, 48u, 52u, 256u};

    // Seed upper bytes that PSMCT24 must preserve.
    std::array<uint8_t, 48> seed{};
    for (uint32_t i = 0u; i < 9u; ++i)
    {
        seed[i * 4u + 3u] = 0x7eu;
    }
    if (!setup(gs, bp, 0u, 9u, 1u) ||
        !image(gs, seed.data(), 3u) ||
        !setup(gs, bp, 1u, 9u, 1u))
    {
        return false;
    }

    // Three events in one atomic batch:
    // HWREG 8 bytes -> IMAGE 16 bytes -> HWREG 8 bytes.
    // The 24-bit pixel carry must cross both boundaries.
    std::array<uint8_t, 32> source{};
    source.fill(0xf3u);
    for (uint32_t i = 0u; i < 27u; ++i)
    {
        source[i] = static_cast<uint8_t>(i + 1u);
    }
    std::array<uint8_t, 48> packet{};
    for (uint32_t i = 0u; i < 16u; ++i)
    {
        packet[16u + i] = source[8u + i];
    }
    ee::s_ee_gif_event events[3]{};
    events[0].m_type = ee::e_ee_gif_event_type::packed;
    events[0].m_register = 0xeu;
    events[0].m_word0 = word64(source.data());
    events[0].m_word1 = 0x54u;
    events[1].m_type = ee::e_ee_gif_event_type::image;
    events[1].m_qwc = 1u;
    events[1].m_byte_offset = 16u;
    events[2].m_type = ee::e_ee_gif_event_type::packed;
    events[2].m_register = 0xeu;
    events[2].m_word0 = word64(source.data() + 24u);
    events[2].m_word1 = 0x54u;
    events[2].m_byte_offset = 32u;

    if (!gs.consume(events, 3u, packet.data(), 3u) ||
        gs.state().m_transfer_pixels_written != 9u ||
        gs.state().m_transfer_pending_size != 0u ||
        gs.state().m_hwreg_write_count != 2u ||
        gs.state().m_hwreg_bytes != 16u ||
        gs.state().m_image_qwc != 1u ||
        gs.state().m_image_data.size() != 16u)
    {
        return false;
    }
    for (uint32_t i = 0u; i < 9u; ++i)
    {
        const uint32_t value =
            0x7e000000u |
            static_cast<uint32_t>(source[i * 3u]) |
            (static_cast<uint32_t>(source[i * 3u + 1u]) << 8u) |
            (static_cast<uint32_t>(source[i * 3u + 2u]) << 16u);
        if (!read32(gs, base + offsets[i], value))
        {
            return false;
        }
    }
    // Re-read the same raw 27-byte pixel sequence as two DMA qwords.
    if (!setup(gs, bp, 1u, 9u, 1u, 1u))
    {
        return false;
    }
    std::array<uint8_t, 32> back{};
    if (!gs.read_image(back.data(), 2u))
    {
        return false;
    }
    for (uint32_t i = 0u; i < 32u; ++i)
    {
        if (back[i] != (i < 27u ? source[i] : 0u))
        {
            return false;
        }
    }
    return true;
}

bool test_odd_width_four_bit()
{
    // A 7x5 rectangle has 35 indices; the first 8-byte HWREG
    // supplies 16, then an IMAGE qword supplies the other 19.
    // No padding nibble may be inserted between odd-width rows.
    for (const uint32_t psm : {0x14u, 0x24u, 0x2cu})
    {
        ee::c_ee_gs_state gs;
        std::array<uint8_t, 24> source{};
        source.fill(0xeeu);
        for (uint32_t i = 0u; i < 35u; ++i)
        {
            const uint32_t at = i / 2u;
            const uint8_t shift =
                static_cast<uint8_t>((i & 1u) * 4u);
            const uint8_t mask = static_cast<uint8_t>(0x0fu << shift);
            source[at] = static_cast<uint8_t>(
                (source[at] & static_cast<uint8_t>(~mask)) |
                (((i * 3u + 1u) & 0x0fu) << shift)
            );
        }
        if (!setup(gs, 0x100u, psm, 7u, 5u) ||
            !reg(gs, 0x54u, word64(source.data())) ||
            !image(gs, source.data() + 8u, 1u) ||
            gs.state().m_transfer_pixels_written != 35u ||
            gs.state().m_hwreg_write_count != 1u ||
            gs.state().m_image_qwc != 1u ||
            !setup(gs, 0x100u, psm, 7u, 5u, 1u))
        {
            return false;
        }
        std::array<uint8_t, 32> result{};
        if (!gs.read_image(result.data(), 2u))
        {
            return false;
        }
        for (uint32_t i = 0u; i < 32u; ++i)
        {
            const uint8_t expected =
                i < 17u ? source[i] :
                i == 17u ? static_cast<uint8_t>(
                    source[i] & 0x0fu) : 0u;
            if (result[i] != expected)
            {
                return false;
            }
        }
    }
    return true;
}

bool test_lanes_and_rollback()
{
    ee::c_ee_gs_state gs;
    constexpr uint32_t bp = 0x300u;
    constexpr uint32_t base = bp * 256u;
    std::array<uint8_t, 16> seed{};
    seed[0] = 0x78u;
    seed[1] = 0x56u;
    seed[2] = 0x34u;
    seed[3] = 0x12u;
    if (!setup(gs, bp, 0u, 1u, 1u) ||
        !image(gs, seed.data(), 1u) ||
        !setup(gs, bp, 0x24u, 1u, 1u) ||
        !reg(gs, 0x54u, 0x0bu) ||
        !read32(gs, base, 0x1b345678u) ||
        !setup(gs, bp, 0x2cu, 1u, 1u) ||
        !reg(gs, 0x54u, 0x0au) ||
        !read32(gs, base, 0xab345678u))
    {
        return false;
    }

    gs.reset();
    if (!setup(gs, bp, 0u, 2u, 1u))
    {
        return false;
    }
    std::array<uint8_t, 32> packet{};
    ee::s_ee_gif_event events[2]{};
    events[0].m_type = ee::e_ee_gif_event_type::packed;
    events[0].m_register = 0xeu;
    events[0].m_word0 = 0x0123456789abcdefull;
    events[0].m_word1 = 0x54u;
    events[1].m_type = ee::e_ee_gif_event_type::packed;
    events[1].m_register = 0xeu;
    events[1].m_word1 = 0x61u; // unsupported FINISH
    events[1].m_byte_offset = 16u;
    if (gs.consume(events, 2u, packet.data(), 2u) ||
        !read32(gs, base, 0u) ||
        !read32(gs, base + 4u, 0u) ||
        gs.was_written(0x54u) ||
        gs.state().m_hwreg_write_count != 0u ||
        gs.state().m_transfer_pixels_written != 0u ||
        !gs.state().m_transfer_started ||
        !gs.consume(events, 1u, packet.data(), 1u) ||
        !read32(gs, base, 0x89abcdefu) ||
        !read32(gs, base + 4u, 0x01234567u) ||
        gs.state().m_hwreg_write_count != 1u)
    {
        return false;
    }

    // HWREG cannot feed IMAGE data when TRXDIR=1, or without
    // a valid destination format or active host-to-local transfer.
    if (!setup(gs, bp, 0u, 2u, 1u, 1u) ||
        reg(gs, 0x54u, 1u) ||
        !setup(gs, bp, 0u, 2u, 1u) ||
        !reg(gs, 0x53u, 0u) ||
        !reg(gs, 0x54u, 0u) ||
        gs.state().m_hwreg_write_count != 2u)
    {
        return false;
    }
    gs.reset();
    return gs.state().m_hwreg_write_count == 0u &&
        gs.state().m_hwreg_bytes == 0u &&
        !gs.was_written(0x54u);
}
} // namespace

int main()
{
    if (!test_all_formats())
    {
        return fail("HWREG 13-format pixel packing and image readback");
    }
    if (!test_mixed_24_bit())
    {
        return fail("HWREG-IMAGE-HWREG 24-bit carry and high-byte preservation");
    }
    if (!test_odd_width_four_bit())
    {
        return fail("HWREG and IMAGE 4-bit indices across odd rows");
    }
    if (!test_lanes_and_rollback())
    {
        return fail("high-byte nibble preservation, rollback and invalid mode");
    }
    std::puts("GS HWREG 8-byte uploads, 13 PSMs, mixed IMAGE, rollback: PASS");
    return 0;
}
