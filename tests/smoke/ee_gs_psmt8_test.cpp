#include "ee_gs.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
int fail(const char* reason)
{
    std::fprintf(stderr, "EE GS PSMT8: %s\n", reason);
    return 1;
}

uint64_t bitblt(uint32_t base, uint32_t width, uint32_t psm)
{
    return (static_cast<uint64_t>(base) << 32u) |
        (static_cast<uint64_t>(width) << 48u) |
        (static_cast<uint64_t>(psm) << 56u);
}

uint64_t trxpos(uint32_t x, uint32_t y)
{
    return (static_cast<uint64_t>(x) << 32u) |
        (static_cast<uint64_t>(y) << 48u);
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

bool image(ee::c_ee_gs_state& gs, const uint8_t* data, uint32_t qwc)
{
    ee::s_ee_gif_event event{};
    event.m_type = ee::e_ee_gif_event_type::image;
    event.m_qwc = qwc;
    return gs.consume(&event, 1u, data, qwc);
}

bool read8(const ee::c_ee_gs_state& gs, uint32_t offset, uint8_t expected)
{
    uint8_t actual = 0u;
    return gs.read_vram8(offset, actual) && actual == expected;
}

bool read32(const ee::c_ee_gs_state& gs, uint32_t offset, uint32_t expected)
{
    uint32_t actual = 0u;
    return gs.read_vram32(offset, actual) && actual == expected;
}

void set32(uint8_t* data, uint32_t value)
{
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        data[i] = static_cast<uint8_t>(value >> (8u * i));
    }
}
} // namespace

int main()
{
    ee::c_ee_gs_state gs;
    constexpr uint32_t k_bp = 0x140u;
    constexpr uint32_t k_base = k_bp * 256u;

    // Reserved destination format 0x03 has no supported swizzle.
    // Unsupported transfer setup must not commit TRXDIR.
    if (!reg(gs, 0x50u, bitblt(k_bp, 4u, 0x03u)) ||
        !reg(gs, 0x51u, trxpos(0u, 63u)) ||
        !reg(gs, 0x52u, trxreg(129u, 2u)) ||
        reg(gs, 0x53u, 0u) ||
        gs.was_written(0x53u) ||
        gs.state().m_transfer_started)
    {
        return fail("reserved PSM 0x03 configuration accepted or partially committed");
    }

    // PSMT8: 128x64-pixel GS pages, 8 KiB per page. The 129x2
    // rectangle crosses both page axes with DBW=4 (two pages/row).
    // Expected VRAM byte offsets come from independent reference
    // GS swizzle fixtures, not a duplicate of the implementation.
    std::vector<uint8_t> pixels(17u * 16u, 0xeeu);
    for (uint32_t i = 0u; i < 258u; ++i)
    {
        pixels[i] = static_cast<uint8_t>(i + 0x11u);
    }

    if (!reg(gs, 0x50u, bitblt(k_bp, 4u, 0x13u)) ||
        !reg(gs, 0x53u, 0u) ||
        !gs.state().m_transfer_started ||
        !image(gs, pixels.data(), 8u) ||
        gs.state().m_transfer_pixels_written != 128u ||
        gs.state().m_image_qwc != 8u ||
        !read8(gs, k_base + 2761u, 0x11u) ||
        !read8(gs, k_base + 2765u, 0x12u) ||
        !read8(gs, k_base + 2777u, 0x13u) ||
        !read8(gs, k_base + 2763u, 0x19u) ||
        !read8(gs, k_base + 3017u, 0x21u) ||
        !read8(gs, k_base + 6857u, 0x51u) ||
        !read8(gs, k_base + 8191u, 0x90u) ||
        !read8(gs, k_base + 10953u, 0u) ||
        !read8(gs, k_base + 16384u, 0u))
    {
        return fail("first IMAGE batch, byte lanes or block swizzling");
    }

    // Complete the rectangle in a separate DMA image batch. The final
    // 14 bytes of the last quadword are padding and must not be written.
    if (!image(gs, pixels.data() + 128u, 9u) ||
        gs.state().m_transfer_pixels_written != 258u ||
        gs.state().m_transfer_pending_size != 0u ||
        gs.state().m_image_qwc != 17u ||
        !read8(gs, k_base + 10953u, 0x91u) ||
        !read8(gs, k_base + 16384u, 0x92u) ||
        !read8(gs, k_base + 16388u, 0x93u) ||
        !read8(gs, k_base + 16386u, 0x9au) ||
        !read8(gs, k_base + 16640u, 0xa2u) ||
        !read8(gs, k_base + 20480u, 0xd2u) ||
        !read8(gs, k_base + 24576u, 0x12u) ||
        !read8(gs, k_base + 16392u, 0u))
    {
        return fail("page crossings, continued IMAGE or trailing padding");
    }

    // Roll back all staged byte writes if a late event is unsupported.
    // The prior transfer remains intact and the cursor can be retried.
    if (!reg(gs, 0x53u, 0u))
    {
        return fail("unable to restart PSMT8 transfer");
    }
    std::array<uint8_t, 32> retry{};
    retry.fill(0xe1u);
    ee::s_ee_gif_event events[2]{};
    events[0].m_type = ee::e_ee_gif_event_type::image;
    events[0].m_qwc = 1u;
    events[1].m_type = ee::e_ee_gif_event_type::packed;
    events[1].m_register = 0xeu;
    events[1].m_byte_offset = 16u;
    events[1].m_word1 = 0x61u; // unsupported FINISH
    if (gs.consume(events, 2u, retry.data(), 2u) ||
        gs.state().m_transfer_pixels_written != 0u ||
        !read8(gs, k_base + 2761u, 0x11u) ||
        !image(gs, retry.data(), 1u) ||
        gs.state().m_transfer_pixels_written != 16u ||
        !read8(gs, k_base + 2761u, 0xe1u))
    {
        return fail("invalid GS event committed pixels or cursor");
    }

    // PSMT8 writes only a single byte within an existing GS word.
    // This covers lane 0 (x=0) and lane 2 (x=8).
    gs.reset();
    constexpr uint32_t k_half_bp = 0x300u;
    constexpr uint32_t k_half_base = k_half_bp * 256u;
    std::array<uint8_t, 16> seed{};
    std::array<uint8_t, 16> lo{};
    std::array<uint8_t, 16> hi{};
    set32(seed.data(), 0x11223344u);
    lo[0] = 0xabu;
    hi[0] = 0xcdu;
    if (!reg(gs, 0x50u, bitblt(k_half_bp, 1u, 0u)) ||
        !reg(gs, 0x51u, trxpos(0u, 0u)) ||
        !reg(gs, 0x52u, trxreg(1u, 1u)) ||
        !reg(gs, 0x53u, 0u) ||
        !image(gs, seed.data(), 1u) ||
        !reg(gs, 0x50u, bitblt(k_half_bp, 1u, 0x13u)) ||
        !reg(gs, 0x53u, 0u) ||
        !image(gs, lo.data(), 1u) ||
        !read32(gs, k_half_base, 0x112233abu) ||
        !reg(gs, 0x51u, trxpos(8u, 0u)) ||
        !reg(gs, 0x53u, 0u) ||
        !image(gs, hi.data(), 1u) ||
        !read32(gs, k_half_base, 0x11cd33abu))
    {
        return fail("PSMT8 overwrote adjacent byte lanes");
    }

    // DBW=1 is a half-page (64px) logical width but the reference
    // uses a minimum page-row stride of one full 128px page.
    gs.reset();
    std::array<uint8_t, 16> one{};
    one[0] = 0x7bu;
    if (!reg(gs, 0x50u, bitblt(0x90u, 1u, 0x13u)) ||
        !reg(gs, 0x51u, trxpos(0u, 64u)) ||
        !reg(gs, 0x52u, trxreg(1u, 1u)) ||
        !reg(gs, 0x53u, 0u) ||
        !image(gs, one.data(), 1u) ||
        !read8(gs, 0x90u * 256u + 8192u, 0x7bu) ||
        !read8(gs, 0x90u * 256u, 0u))
    {
        return fail("PSMT8 minimum page stride for DBW=1");
    }

    // The final 256-byte base block plus an x=16 256-byte block
    // wraps to the first byte of the 4 MiB VRAM allocation.
    gs.reset();
    one[0] = 0xb7u;
    if (!reg(gs, 0x50u, bitblt(0x3fffu, 2u, 0x13u)) ||
        !reg(gs, 0x51u, trxpos(16u, 0u)) ||
        !reg(gs, 0x52u, trxreg(1u, 1u)) ||
        !reg(gs, 0x53u, 0u) ||
        !image(gs, one.data(), 1u) ||
        !read8(gs, 0u, 0xb7u) ||
        !read8(gs, 0x3fffu * 256u, 0u))
    {
        return fail("PSMT8 4 MiB GS VRAM wrap");
    }

    uint8_t unused = 0u;
    if (gs.read_vram8(
            static_cast<uint32_t>(ee::c_ee_gs_state::k_vram_bytes),
            unused))
    {
        return fail("out-of-range VRAM byte access accepted");
    }

    gs.reset();
    if (!read8(gs, 0u, 0u) ||
        gs.state().m_transfer_started ||
        gs.state().m_transfer_pixels_written != 0u ||
        gs.state().m_image_qwc != 0u)
    {
        return fail("GS reset did not clear indexed VRAM or transfer state");
    }

    std::puts("GS PSMT8 swizzle, DMA continuation, rollback: PASS");
    return 0;
}
