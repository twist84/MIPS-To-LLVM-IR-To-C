#include "ee_gs.h"

#include <array>
#include <cstdint>
#include <cstdio>

namespace
{
int fail(const char* message)
{
    std::fprintf(stderr, "GS PSMCT24: %s\n", message);
    return 1;
}

uint64_t bitblt(uint32_t bp, uint32_t width, uint32_t psm)
{
    return (static_cast<uint64_t>(bp) << 32u) |
        (static_cast<uint64_t>(width) << 48u) |
        (static_cast<uint64_t>(psm) << 56u);
}

uint64_t trxreg(uint32_t width, uint32_t height)
{
    return width | (static_cast<uint64_t>(height) << 32u);
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

bool image(
    ee::c_ee_gs_state& gs, const uint8_t* data, uint32_t qwc)
{
    ee::s_ee_gif_event event{};
    event.m_type = ee::e_ee_gif_event_type::image;
    event.m_qwc = qwc;
    return gs.consume(&event, 1u, data, qwc);
}

void set32(uint8_t* data, uint32_t index, uint32_t value)
{
    data[index * 4u + 0u] = static_cast<uint8_t>(value);
    data[index * 4u + 1u] = static_cast<uint8_t>(value >> 8u);
    data[index * 4u + 2u] = static_cast<uint8_t>(value >> 16u);
    data[index * 4u + 3u] = static_cast<uint8_t>(value >> 24u);
}

void set24(uint8_t* data, uint32_t index, uint32_t value)
{
    data[index * 3u + 0u] = static_cast<uint8_t>(value);
    data[index * 3u + 1u] = static_cast<uint8_t>(value >> 8u);
    data[index * 3u + 2u] = static_cast<uint8_t>(value >> 16u);
}

bool read(
    const ee::c_ee_gs_state& gs, uint32_t offset, uint32_t expected)
{
    uint32_t actual = 0u;
    return gs.read_vram32(offset, actual) && actual == expected;
}
} // namespace

int main()
{
    ee::c_ee_gs_state gs;
    constexpr uint32_t k_base = 0x60u * 256u;
    constexpr uint32_t k_offsets[6] = {0u, 4u, 16u, 20u, 32u, 36u};

    // Seed six PSMCT32 pixels with a nonzero high byte. A later
    // PSMCT24 upload must leave that byte untouched.
    std::array<uint8_t, 32> seed{};
    for (uint32_t i = 0u; i < 6u; ++i)
    {
        set32(seed.data(), i, 0x7e001000u | i);
    }
    if (!reg(gs, 0x50u, bitblt(0x60u, 1u, 0u)) ||
        !reg(gs, 0x51u, 0u) ||
        !reg(gs, 0x52u, trxreg(6u, 1u)) ||
        !reg(gs, 0x53u, 0u) ||
        !image(gs, seed.data(), 2u))
    {
        return fail("unable to seed 32-bit GS pixels");
    }

    std::array<uint8_t, 32> pixels{};
    pixels.fill(0xccu);
    for (uint32_t i = 0u; i < 6u; ++i)
    {
        set24(pixels.data(), i, 0x00112200u | (i + 1u));
    }
    if (!reg(gs, 0x50u, bitblt(0x60u, 1u, 1u)) ||
        !reg(gs, 0x53u, 0u) ||
        !image(gs, pixels.data(), 1u) ||
        gs.state().m_transfer_pixels_written != 5u ||
        gs.state().m_transfer_pending_size != 1u ||
        gs.state().m_transfer_pending[0] != 6u)
    {
        return fail("1-byte carry after 16-byte IMAGE");
    }

    for (uint32_t i = 0u; i < 5u; ++i)
    {
        if (!read(gs, k_base + k_offsets[i],
                  0x7e112200u | (i + 1u)))
        {
            return fail("PSMCT24 swizzle or high-byte preservation");
        }
    }
    if (!read(gs, k_base + k_offsets[5], 0x7e001005u))
    {
        return fail("incomplete pixel was committed prematurely");
    }

    if (!image(gs, pixels.data() + 16u, 1u) ||
        gs.state().m_transfer_pixels_written != 6u ||
        gs.state().m_transfer_pending_size != 0u ||
        gs.state().m_image_qwc != 2u ||
        !read(gs, k_base + k_offsets[5], 0x7e112206u) ||
        !read(gs, k_base + 48u, 0u))
    {
        return fail("continued IMAGE did not finish one split pixel");
    }

    // Starting a fresh transfer resets the previous cursor. Two
    // quadwords contain 10 complete PSMCT24 pixels plus two bytes.
    constexpr uint32_t k_other_base = 0x80u * 256u;
    std::array<uint8_t, 48> more{};
    more.fill(0xaau);
    for (uint32_t i = 0u; i < 11u; ++i)
    {
        set24(more.data(), i, 0x00550000u | (i + 1u));
    }
    if (!reg(gs, 0x50u, bitblt(0x80u, 1u, 1u)) ||
        !reg(gs, 0x52u, trxreg(11u, 1u)) ||
        !reg(gs, 0x53u, 0u) ||
        gs.state().m_transfer_pending_size != 0u ||
        !image(gs, more.data(), 2u) ||
        gs.state().m_transfer_pixels_written != 10u ||
        gs.state().m_transfer_pending_size != 2u ||
        !read(gs, k_other_base + 272u, 0u) ||
        !image(gs, more.data() + 32u, 1u) ||
        gs.state().m_transfer_pixels_written != 11u ||
        gs.state().m_transfer_pending_size != 0u ||
        !read(gs, k_other_base + 272u, 0x0055000bu))
    {
        return fail("2-byte carry and padded transfer completion");
    }

    // A failed late event must roll back BOTH staged pixels and the
    // incomplete pixel accumulator.
    if (!reg(gs, 0x53u, 0u))
    {
        return fail("unable to restart PSMCT24 transfer");
    }
    ee::s_ee_gif_event events[2]{};
    events[0].m_type = ee::e_ee_gif_event_type::image;
    events[0].m_qwc = 1u;
    events[1].m_type = ee::e_ee_gif_event_type::packed;
    events[1].m_register = 0xeu;
    events[1].m_byte_offset = 16u;
    events[1].m_word1 = 0x61u; // unsupported FINISH
    if (gs.consume(events, 2u, more.data(), 2u) ||
        gs.state().m_transfer_pixels_written != 0u ||
        gs.state().m_transfer_pending_size != 0u ||
        !image(gs, more.data(), 1u) ||
        gs.state().m_transfer_pixels_written != 5u ||
        gs.state().m_transfer_pending_size != 1u)
    {
        return fail("rejected batch leaked pixels or partial-pixel carry");
    }

    // Three odd-width rows, with the first IMAGE ending in the
    // middle of the second row and carrying one byte of pixel 5.
    // Compare split chunks against one contiguous IMAGE, including
    // final padding and each independently known swizzled address.
    ee::c_ee_gs_state split;
    ee::c_ee_gs_state whole;
    constexpr uint32_t k_rows_bp = 0x120u;
    constexpr uint32_t k_rows_base = k_rows_bp * 256u;
    constexpr uint32_t k_row_offsets[9] =
        {0u, 4u, 16u, 8u, 12u, 24u, 64u, 68u, 80u};
    std::array<uint8_t, 32> rows{};
    rows.fill(0xeeu);
    for (uint32_t i = 0u; i < 9u; ++i)
    {
        set24(rows.data(), i, 0x00abc000u | (i + 1u));
    }
    for (auto* target : {&split, &whole})
    {
        if (!reg(*target, 0x50u, bitblt(k_rows_bp, 1u, 1u)) ||
            !reg(*target, 0x51u, 0u) ||
            !reg(*target, 0x52u, trxreg(3u, 3u)) ||
            !reg(*target, 0x53u, 0u))
        {
            return fail("unable to configure odd-width RGB24 rows");
        }
    }
    if (!image(split, rows.data(), 1u) ||
        split.state().m_transfer_pixels_written != 5u ||
        split.state().m_transfer_pending_size != 1u ||
        !image(split, rows.data() + 16u, 1u) ||
        !image(whole, rows.data(), 2u) ||
        split.state().m_transfer_pixels_written != 9u ||
        split.state().m_transfer_pending_size != 0u ||
        split.vram() != whole.vram())
    {
        return fail("split odd-width RGB24 upload diverged from whole IMAGE");
    }
    for (uint32_t i = 0u; i < 9u; ++i)
    {
        if (!read(split, k_rows_base + k_row_offsets[i],
                0x00abc000u | (i + 1u)))
        {
            return fail("odd-width RGB24 row cursor or swizzle");
        }
    }

    gs.reset();
    if (gs.state().m_transfer_pending_size != 0u ||
        gs.state().m_transfer_pixels_written != 0u ||
        !read(gs, k_base, 0u) ||
        !read(gs, k_other_base, 0u))
    {
        return fail("reset did not clear PSMCT24 state or GS VRAM");
    }

    std::puts("GS PSMCT24 split pixels, alpha preservation and rollback: PASS");
    return 0;
}
