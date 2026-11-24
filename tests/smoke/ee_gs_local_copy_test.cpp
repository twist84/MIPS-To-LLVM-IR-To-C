#include "ee_gs.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
int fail(const char* reason)
{
    std::fprintf(stderr, "EE GS local copy: %s\n", reason);
    return 1;
}

uint64_t bitblt(
    uint32_t sbp, uint32_t sbw, uint32_t spsm,
    uint32_t dbp, uint32_t dbw, uint32_t dpsm)
{
    return static_cast<uint64_t>(sbp) |
        (static_cast<uint64_t>(sbw) << 16u) |
        (static_cast<uint64_t>(spsm) << 24u) |
        (static_cast<uint64_t>(dbp) << 32u) |
        (static_cast<uint64_t>(dbw) << 48u) |
        (static_cast<uint64_t>(dpsm) << 56u);
}

uint64_t trxpos(
    uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy,
    uint32_t dir = 0u)
{
    return static_cast<uint64_t>(sx) |
        (static_cast<uint64_t>(sy) << 16u) |
        (static_cast<uint64_t>(dx) << 32u) |
        (static_cast<uint64_t>(dy) << 48u) |
        (static_cast<uint64_t>(dir) << 59u);
}

uint64_t trxreg(uint32_t width, uint32_t height)
{
    return width | (static_cast<uint64_t>(height) << 32u);
}

bool reg(ee::c_ee_gs_state& gs, uint32_t address, uint64_t value)
{
    ee::s_ee_gif_event e{};
    e.m_type = ee::e_ee_gif_event_type::packed;
    e.m_register = 0xeu;
    e.m_word0 = value;
    e.m_word1 = address;
    const std::array<uint8_t, 16> unused{};
    return gs.consume(&e, 1u, unused.data(), 1u);
}

bool image(ee::c_ee_gs_state& gs, const uint8_t* data, uint32_t qwc)
{
    ee::s_ee_gif_event e{};
    e.m_type = ee::e_ee_gif_event_type::image;
    e.m_qwc = qwc;
    return gs.consume(&e, 1u, data, qwc);
}

bool upload(
    ee::c_ee_gs_state& gs, uint32_t bp, uint32_t bw,
    uint32_t psm, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height,
    const uint8_t* data, uint32_t qwc)
{
    return reg(gs, 0x50u, bitblt(0u, 0u, 0u, bp, bw, psm)) &&
        reg(gs, 0x51u, trxpos(0u, 0u, x, y)) &&
        reg(gs, 0x52u, trxreg(width, height)) &&
        reg(gs, 0x53u, 0u) &&
        image(gs, data, qwc);
}

bool copy(
    ee::c_ee_gs_state& gs,
    uint32_t sbp, uint32_t sbw, uint32_t spsm,
    uint32_t dbp, uint32_t dbw, uint32_t dpsm,
    uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy,
    uint32_t width, uint32_t height, uint32_t dir = 0u)
{
    return reg(gs, 0x50u, bitblt(sbp, sbw, spsm, dbp, dbw, dpsm)) &&
        reg(gs, 0x51u, trxpos(sx, sy, dx, dy, dir)) &&
        reg(gs, 0x52u, trxreg(width, height)) &&
        reg(gs, 0x53u, 2u);
}

bool read8(const ee::c_ee_gs_state& gs, uint32_t at, uint8_t expected)
{
    uint8_t actual = 0u;
    return gs.read_vram8(at, actual) && actual == expected;
}

bool read16(const ee::c_ee_gs_state& gs, uint32_t at, uint16_t expected)
{
    uint16_t actual = 0u;
    return gs.read_vram16(at, actual) && actual == expected;
}

bool read32(const ee::c_ee_gs_state& gs, uint32_t at, uint32_t expected)
{
    uint32_t actual = 0u;
    return gs.read_vram32(at, actual) && actual == expected;
}

void set32(uint8_t* data, uint32_t index, uint32_t value)
{
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        data[index * 4u + i] = static_cast<uint8_t>(
            value >> (i * 8u)
        );
    }
}

uint64_t tex0(
    uint32_t tbp, uint32_t tbw, uint32_t psm,
    uint32_t cbp, uint32_t csa, uint32_t cld)
{
    return static_cast<uint64_t>(tbp) |
        (static_cast<uint64_t>(tbw) << 14u) |
        (static_cast<uint64_t>(psm) << 20u) |
        (static_cast<uint64_t>(cbp) << 37u) |
        (static_cast<uint64_t>(csa) << 56u) |
        (static_cast<uint64_t>(cld) << 61u);
}

bool indexed(
    const ee::c_ee_gs_state& gs, uint32_t x,
    uint32_t y, uint32_t expected)
{
    uint32_t value = 0u;
    return gs.read_indexed_clut_value(0u, x, y, value) &&
        value == expected;
}

bool test_formats()
{
    ee::c_ee_gs_state gs;
    constexpr uint32_t k_src = 0x180u;
    constexpr uint32_t k_dst = 0x600u;

    // Each format reads and writes its own reference-derived physical
    // layout, including the depth-specific first block at byte 6144.
    struct s_format
    {
        uint32_t psm;
        uint32_t offset;
        uint32_t expected;
        uint8_t bytes;
    };
    const s_format formats[] =
    {
        {0x00u, 0u, 0x89abcdefu, 4u},
        {0x01u, 0u, 0x00abcdefu, 3u},
        {0x02u, 0u, 0x0000cdefu, 2u},
        {0x0au, 0u, 0x0000cdefu, 2u},
        {0x13u, 0u, 0x000000efu, 1u},
        {0x14u, 0u, 0x0000000fu, 0u},
        {0x1bu, 3u, 0x000000efu, 1u},
        {0x24u, 3u, 0x0000000fu, 0u},
        {0x2cu, 3u, 0x000000f0u, 0u},
        {0x30u, 6144u, 0x89abcdefu, 4u},
        {0x31u, 6144u, 0x00abcdefu, 3u},
        {0x32u, 6144u, 0x0000cdefu, 2u},
        {0x3au, 6144u, 0x0000cdefu, 2u}
    };

    for (const s_format& f : formats)
    {
        gs.reset();
        std::array<uint8_t, 16> seed{};
        set32(seed.data(), 0u, 0x89abcdefu);
        if (!upload(gs, k_src, 1u, f.psm, 0u, 0u,
                    1u, 1u, seed.data(), 1u) ||
            !copy(gs, k_src, 1u, f.psm,
                  k_dst, 1u, f.psm, 0u, 0u, 0u, 0u, 1u, 1u) ||
            gs.state().m_local_copy_count != 1u ||
            gs.state().m_local_copy_pixels != 1u ||
            gs.state().m_transfer_started)
        {
            return false;
        }
        const uint32_t at = k_dst * 256u + f.offset;
        if (f.bytes == 4u || f.bytes == 3u)
        {
            if (!read32(gs, at, f.expected))
            {
                return false;
            }
        }
        else if (f.bytes == 2u)
        {
            if (!read16(gs, at, static_cast<uint16_t>(f.expected)))
            {
                return false;
            }
        }
        else if (!read8(gs, at, static_cast<uint8_t>(f.expected)))
        {
            return false;
        }
    }
    return true;
}

bool test_host_upload_page_tables()
{
    // The local-copy destination still uses the original scalar
    // pixel_write path. Compare entire VRAM images after equivalent
    // host uploads for all formats, including split payloads,
    // odd row widths, page edges, coordinate and physical VRAM wrap.
    constexpr uint32_t formats[] = {
        0x00u, 0x01u, 0x02u, 0x0au, 0x13u, 0x14u, 0x1bu,
        0x24u, 0x2cu, 0x30u, 0x31u, 0x32u, 0x3au
    };
    constexpr uint32_t k_width = 137u;
    constexpr uint32_t k_height = 135u;
    constexpr uint32_t k_pixels = k_width * k_height;
    constexpr uint32_t k_src = 0x1000u;
    constexpr uint32_t k_dst = 0x3fffu;
    std::vector<uint8_t> source((k_pixels * 4u + 15u) & ~15u);
    for (uint32_t i = 0u; i < k_pixels; ++i)
    {
        set32(source.data(), i, i * 0x9e3779b9u + 0x12345678u);
    }
    ee::c_ee_gs_state actual;
    ee::c_ee_gs_state reference;
    for (uint32_t psm : formats)
    {
        const uint32_t bits = (psm == 0x14u || psm == 0x24u || psm == 0x2cu) ? 4u :
            (psm == 0x13u || psm == 0x1bu) ? 8u :
            (psm == 0x02u || psm == 0x0au || psm == 0x32u || psm == 0x3au) ? 16u :
            (psm == 0x01u || psm == 0x31u) ? 24u : 32u;
        std::vector<uint8_t> payload(((k_pixels * bits + 7u) / 8u + 15u) & ~15u);
        for (uint32_t i = 0u; i < k_pixels; ++i)
        {
            if (bits == 4u)
            {
                payload[i / 2u] |= static_cast<uint8_t>(
                    (source[i * 4u] & 15u) << ((i & 1u) * 4u));
            }
            else
            {
                for (uint32_t b = 0u; b < bits / 8u; ++b)
                {
                    payload[i * (bits / 8u) + b] = source[i * 4u + b];
                }
            }
        }
        actual.reset();
        reference.reset();
        for (auto* gs : {&actual, &reference})
        {
            if (!upload(*gs, k_src, 3u, 0u, 0u, 0u, k_width, k_height,
                    source.data(), static_cast<uint32_t>(source.size() / 16u)))
            {
                return false;
            }
        }
        if (!copy(reference, k_src, 3u, 0u, k_dst, 5u, psm,
                0u, 0u, 2041u, 2043u, k_width, k_height) ||
            !reg(actual, 0x50u, bitblt(0u, 0u, 0u, k_dst, 5u, psm)) ||
            !reg(actual, 0x51u, trxpos(0u, 0u, 2041u, 2043u)) ||
            !reg(actual, 0x52u, trxreg(k_width, k_height)) ||
            !reg(actual, 0x53u, 0u))
        {
            return false;
        }
        const uint32_t qwc = static_cast<uint32_t>(payload.size() / 16u);
        for (uint32_t at = 0u; at < qwc;)
        {
            const uint32_t chunk = (qwc - at < 17u) ? qwc - at : 17u;
            if (!image(actual, payload.data() + at * 16u, chunk))
            {
                return false;
            }
            at += chunk;
        }
        if (actual.state().m_transfer_pixels_written != k_pixels ||
            actual.state().m_transfer_pending_size != 0u ||
            actual.vram() != reference.vram())
        {
            std::fprintf(stderr, "upload page-table mismatch PSM 0x%x\n", psm);
            return false;
        }
    }
    return true;
}

bool test_directions_and_overlap()
{
    ee::c_ee_gs_state gs;
    constexpr uint32_t k_src = 0x100u;
    constexpr uint32_t k_dst = 0x300u;
    std::array<uint8_t, 16> payload{};
    const uint32_t src[4] = {0x11u, 0x22u, 0x33u, 0x44u};
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        set32(payload.data(), i, src[i]);
    }

    const uint32_t expected[4][4] =
    {
        {0x11u, 0x22u, 0x33u, 0x44u},
        {0x22u, 0x11u, 0x44u, 0x33u},
        {0x33u, 0x44u, 0x11u, 0x22u},
        {0x44u, 0x33u, 0x22u, 0x11u}
    };
    // PSMCT32 x/y=(0,0),(1,0),(0,1),(1,1) lie at
    // offsets 0,4,8,12 respectively in the reference swizzle.
    for (uint32_t dir = 0u; dir < 4u; ++dir)
    {
        gs.reset();
        if (!upload(gs, k_src, 1u, 0u, 0u, 0u, 2u, 2u,
                    payload.data(), 1u) ||
            !copy(gs, k_src, 1u, 0u, k_dst, 1u, 0u,
                  0u, 0u, 0u, 0u, 2u, 2u, dir) ||
            !read32(gs, k_dst * 256u + 0u, expected[dir][0]) ||
            !read32(gs, k_dst * 256u + 4u, expected[dir][1]) ||
            !read32(gs, k_dst * 256u + 8u, expected[dir][2]) ||
            !read32(gs, k_dst * 256u + 12u, expected[dir][3]))
        {
            return false;
        }
    }

    gs.reset();
    std::array<uint8_t, 32> overlap{};
    for (uint32_t i = 0u; i < 5u; ++i)
    {
        set32(overlap.data(), i, 0x1000u + i);
    }
    // src [0,1,2,3] -> dst [1,2,3,4]. A forward live read
    // would incorrectly propagate the first pixel across all five.
    if (!upload(gs, k_src, 1u, 0u, 0u, 0u, 5u, 1u,
                overlap.data(), 2u) ||
        !copy(gs, k_src, 1u, 0u, k_src, 1u, 0u,
              0u, 0u, 1u, 0u, 4u, 1u) ||
        !read32(gs, k_src * 256u + 0u, 0x1000u) ||
        !read32(gs, k_src * 256u + 4u, 0x1000u) ||
        !read32(gs, k_src * 256u + 16u, 0x1001u) ||
        !read32(gs, k_src * 256u + 20u, 0x1002u) ||
        !read32(gs, k_src * 256u + 32u, 0x1003u) ||
        gs.state().m_image_qwc != 2u ||
        gs.state().m_local_copy_count != 1u)
    {
        return false;
    }

    // Reverse the overlapping result using DIR bit 0. The source
    // itself is frozen for the duration of this copy as well.
    if (!copy(gs, k_src, 1u, 0u, k_src, 1u, 0u,
              0u, 0u, 0u, 0u, 4u, 1u, 1u) ||
        !read32(gs, k_src * 256u + 0u, 0x1002u) ||
        !read32(gs, k_src * 256u + 4u, 0x1001u) ||
        !read32(gs, k_src * 256u + 16u, 0x1000u) ||
        !read32(gs, k_src * 256u + 20u, 0x1000u) ||
        gs.state().m_local_copy_count != 2u ||
        gs.state().m_local_copy_pixels != 8u ||
        gs.state().m_image_qwc != 2u)
    {
        return false;
    }
    return true;
}

bool test_cross_formats_and_wrapping()
{
    ee::c_ee_gs_state gs;
    std::array<uint8_t, 16> one{};
    set32(one.data(), 0u, 0x12aabbccu);

    // PSMCT32 -> PSMZ24 copies only the low 24 bits of a
    // source pixel and preserves the destination depth high byte.
    if (!upload(gs, 0x100u, 1u, 0u, 0u, 0u,
                1u, 1u, one.data(), 1u))
    {
        return false;
    }
    set32(one.data(), 0u, 0x99000011u);
    if (!upload(gs, 0x300u, 1u, 0x30u, 0u, 0u,
                1u, 1u, one.data(), 1u) ||
        !copy(gs, 0x100u, 1u, 0u, 0x300u, 1u, 0x31u,
              0u, 0u, 0u, 0u, 1u, 1u) ||
        !read32(gs, 0x300u * 256u + 6144u, 0x99aabbccu))
    {
        return false;
    }

    gs.reset();
    one[0] = 0x7bu;
    if (!upload(gs, 0x100u, 1u, 0x13u, 0u, 0u,
                1u, 1u, one.data(), 1u) ||
        !copy(gs, 0x100u, 1u, 0x13u, 0x300u, 1u, 0x30u,
              0u, 0u, 0u, 0u, 1u, 1u) ||
        !read32(gs, 0x300u * 256u + 6144u, 0x0000007bu))
    {
        return false;
    }

    gs.reset();
    one[0] = 0x0eu;
    if (!upload(gs, 0x100u, 1u, 0x14u, 0u, 0u,
                1u, 1u, one.data(), 1u) ||
        !copy(gs, 0x100u, 1u, 0x14u, 0x300u, 1u, 0x02u,
              0u, 0u, 0u, 0u, 1u, 1u) ||
        !read16(gs, 0x300u * 256u, 0x000eu))
    {
        return false;
    }

    gs.reset();
    set32(one.data(), 0u, 0xabcdef21u);
    if (!upload(gs, 0x100u, 1u, 0x30u, 0u, 0u,
                1u, 1u, one.data(), 1u) ||
        !copy(gs, 0x100u, 1u, 0x30u, 0x300u, 1u, 0x2cu,
              0u, 0u, 0u, 0u, 1u, 1u) ||
        !read8(gs, 0x300u * 256u + 3u, 0x10u))
    {
        return false;
    }

    // Both 11-bit coordinate wrap and X=64 page crossing use the
    // reference's 64x32 PSMCT32 pages with DBW=2.
    gs.reset();
    set32(one.data(), 0u, 0x12345678u);
    set32(one.data(), 1u, 0x87654321u);
    if (!upload(gs, 0x180u, 2u, 0u, 2047u, 0u,
                2u, 1u, one.data(), 1u) ||
        !copy(gs, 0x180u, 2u, 0u, 0x280u, 2u, 0u,
              2047u, 0u, 63u, 0u, 2u, 1u) ||
        !read32(gs, 0x280u * 256u + 5428u, 0x12345678u) ||
        !read32(gs, 0x280u * 256u + 8192u, 0x87654321u))
    {
        return false;
    }

    // Copy into x=8 of the last 256-byte GS base block. The
    // 256-byte x-block offset wraps to address zero in 4 MiB VRAM.
    if (!copy(gs, 0x180u, 2u, 0u, 0x3fffu, 1u, 0u,
              0u, 0u, 8u, 0u, 1u, 1u) ||
        !read32(gs, 0u, 0x87654321u) ||
        !read32(gs, 0x3fffu * 256u, 0u))
    {
        return false;
    }
    return true;
}

bool test_ordering_and_rollback()
{
    ee::c_ee_gs_state gs;
    constexpr uint32_t k_bp = 0x400u;

    // Configure host-to-local using the same base as a later copy,
    // with the source fields already present in BITBLTBUF.
    if (!reg(gs, 0x50u, bitblt(
            k_bp, 1u, 0u, k_bp, 1u, 0u)) ||
        !reg(gs, 0x51u, trxpos(0u, 0u, 0u, 0u)) ||
        !reg(gs, 0x52u, trxreg(1u, 1u)) ||
        !reg(gs, 0x53u, 0u))
    {
        return false;
    }

    // One batch: IMAGE writes x0, copy x0->x1, then copy x1->x2.
    // Its writes must be visible to the following local copy even
    // though no VRAM writes are committed until the batch succeeds.
    std::array<uint8_t, 96> data{};
    set32(data.data(), 0u, 0xdeadbeefu);
    ee::s_ee_gif_event events[6]{};
    events[0].m_type = ee::e_ee_gif_event_type::image;
    events[0].m_qwc = 1u;
    for (uint32_t i = 1u; i < 6u; ++i)
    {
        events[i].m_type = ee::e_ee_gif_event_type::packed;
        events[i].m_register = 0xeu;
        events[i].m_byte_offset = i * 16u;
    }
    events[1].m_word1 = 0x51u;
    events[1].m_word0 = trxpos(0u, 0u, 1u, 0u);
    events[2].m_word1 = 0x53u;
    events[2].m_word0 = 2u;
    events[3].m_word1 = 0x51u;
    events[3].m_word0 = trxpos(1u, 0u, 2u, 0u);
    events[4].m_word1 = 0x53u;
    events[4].m_word0 = 2u;
    events[5].m_word1 = 0x60u; // unsupported SIGNAL

    // A failed trailing event must roll back the IMAGE, both copies,
    // register changes and their diagnostic counters in one unit.
    if (gs.consume(events, 6u, data.data(), 6u) ||
        !read32(gs, k_bp * 256u + 0u, 0u) ||
        !read32(gs, k_bp * 256u + 4u, 0u) ||
        !read32(gs, k_bp * 256u + 16u, 0u) ||
        gs.state().m_image_qwc != 0u ||
        gs.state().m_local_copy_count != 0u ||
        !gs.state().m_transfer_started)
    {
        return false;
    }

    // A second transaction with different data must not see the
    // rejected transaction's journal or its populated shadow.
    set32(data.data(), 0u, 0x12345678u);
    if (!gs.consume(events, 5u, data.data(), 5u) ||
        !read32(gs, k_bp * 256u + 0u, 0x12345678u) ||
        !read32(gs, k_bp * 256u + 4u, 0x12345678u) ||
        !read32(gs, k_bp * 256u + 16u, 0x12345678u) ||
        gs.state().m_image_qwc != 1u ||
        gs.state().m_local_copy_count != 2u ||
        gs.state().m_local_copy_pixels != 2u ||
        gs.state().m_transfer_started ||
        image(gs, data.data(), 1u))
    {
        return false;
    }

    // Reuse the same allocations after reset. Both the old source
    // and its shadow must be replaced by the new committed VRAM.
    gs.reset();
    if (!reg(gs, 0x50u, bitblt(k_bp, 1u, 0u, k_bp, 1u, 0u)) ||
        !reg(gs, 0x51u, trxpos(0u, 0u, 0u, 0u)) ||
        !reg(gs, 0x52u, trxreg(1u, 1u)) ||
        !reg(gs, 0x53u, 0u))
    {
        return false;
    }
    set32(data.data(), 0u, 0x89abcdefu);
    if (!gs.consume(events, 5u, data.data(), 5u) ||
        !read32(gs, k_bp * 256u + 0u, 0x89abcdefu) ||
        !read32(gs, k_bp * 256u + 4u, 0x89abcdefu) ||
        !read32(gs, k_bp * 256u + 16u, 0x89abcdefu) ||
        gs.state().m_local_copy_count != 2u)
    {
        return false;
    }
    return true;
}

bool test_clut_and_rejections()
{
    ee::c_ee_gs_state gs;
    uint32_t value = 0u;

    // Missing transfer registers, invalid source/destination widths,
    // unsupported PSM and zero-size rectangles reject TRXDIR=2
    // without committing a new GS register or any VRAM writes.
    if (reg(gs, 0x53u, 2u) || gs.was_written(0x53u) ||
        !reg(gs, 0x50u, bitblt(0x100u, 0u, 0u,
                              0x200u, 1u, 0u)) ||
        !reg(gs, 0x51u, trxpos(0u, 0u, 0u, 0u)) ||
        !reg(gs, 0x52u, trxreg(1u, 1u)) ||
        reg(gs, 0x53u, 2u) ||
        !reg(gs, 0x50u, bitblt(0x100u, 1u, 3u,
                              0x200u, 1u, 0u)) ||
        reg(gs, 0x53u, 2u) ||
        !reg(gs, 0x50u, bitblt(0x100u, 1u, 0u,
                              0x200u, 1u, 3u)) ||
        reg(gs, 0x53u, 2u) ||
        !reg(gs, 0x50u, bitblt(0x100u, 1u, 0u,
                              0x200u, 0u, 0u)) ||
        reg(gs, 0x53u, 2u) ||
        !reg(gs, 0x50u, bitblt(0x100u, 1u, 0u,
                              0x200u, 1u, 0u)) ||
        !reg(gs, 0x52u, trxreg(0u, 1u)) ||
        reg(gs, 0x53u, 2u) ||
        !reg(gs, 0x52u, trxreg(2049u, 2048u)) ||
        reg(gs, 0x53u, 2u) ||
        gs.was_written(0x53u))
    {
        return false;
    }

    // Snapshot CLUT values remain loaded across an index copy; CLUT
    // is a separate internal palette, not a live view of GS VRAM.
    gs.reset();
    constexpr uint32_t k_palette = 0x100u;
    constexpr uint32_t k_src = 0x500u;
    constexpr uint32_t k_dst = 0x600u;
    std::array<uint8_t, 64> colors{};
    for (uint32_t i = 0u; i < 16u; ++i)
    {
        set32(colors.data(), i, 0xa0000000u + i);
    }
    std::array<uint8_t, 16> index_data{};
    index_data[0] = 0x0du;
    if (!upload(gs, k_palette, 1u, 0u, 0u, 0u,
                8u, 2u, colors.data(), 4u) ||
        !upload(gs, k_src, 1u, 0x14u, 0u, 0u,
                1u, 1u, index_data.data(), 1u) ||
        !reg(gs, 0x06u, tex0(k_dst, 1u, 0x14u,
                             k_palette, 0u, 1u)) ||
        !indexed(gs, 0u, 0u, 0xa0000000u) ||
        !copy(gs, k_src, 1u, 0x14u, k_dst, 1u, 0x14u,
              0u, 0u, 0u, 0u, 1u, 1u) ||
        !indexed(gs, 0u, 0u, 0xa000000du) ||
        gs.state().m_clut_load_count != 1u)
    {
        return false;
    }

    // A local copy that overwrites a palette entry must not reload
    // CLUT; explicit CLD=1 causes the refreshed raw value to appear.
    std::array<uint8_t, 16> changed{};
    set32(changed.data(), 0u, 0xb00b000du);
    if (!upload(gs, 0x800u, 1u, 0u, 0u, 0u,
                1u, 1u, changed.data(), 1u) ||
        !copy(gs, 0x800u, 1u, 0u, k_palette, 1u, 0u,
              0u, 0u, 5u, 1u, 1u, 1u) ||
        !indexed(gs, 0u, 0u, 0xa000000du) ||
        !reg(gs, 0x06u, tex0(k_dst, 1u, 0x14u,
                             k_palette, 0u, 1u)) ||
        !indexed(gs, 0u, 0u, 0xb00b000du))
    {
        return false;
    }

    gs.reset();
    if (gs.state().m_local_copy_count != 0u ||
        gs.state().m_local_copy_pixels != 0u ||
        gs.read_clut_entry(0u, value) ||
        gs.state().m_transfer_started)
    {
        return false;
    }
    return true;
}
} // namespace

int main()
{
    if (!test_formats())
    {
        return fail("13 pixel formats: raw source reads and destination writes");
    }
    if (!test_host_upload_page_tables())
    {
        return fail("host upload page-table differential across all 13 PSMs");
    }
    if (!test_directions_and_overlap())
    {
        return fail("DIR flips or source/destination overlap");
    }
    if (!test_cross_formats_and_wrapping())
    {
        return fail("cross-PSM copies, GS pages or 4 MiB coordinate wrap");
    }
    if (!test_ordering_and_rollback())
    {
        return fail("intra-batch IMAGE/copy ordering or atomic rollback");
    }
    if (!test_clut_and_rejections())
    {
        return fail("invalid transfer rejection, CLUT snapshot or reset");
    }

    std::puts("GS TRXDIR=2 local copy, 13 PSMs, DIR, rollback: PASS");
    return 0;
}
