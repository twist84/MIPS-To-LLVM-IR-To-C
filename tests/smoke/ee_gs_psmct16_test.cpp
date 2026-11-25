#include "ee_gs.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
int fail(const char* reason)
{
    std::fprintf(stderr, "EE GS PSMCT16: %s\n", reason);
    return 1;
}

uint64_t bitblt(uint32_t bp, uint32_t width, uint32_t psm)
{
    return (static_cast<uint64_t>(bp) << 32u) |
        (static_cast<uint64_t>(width) << 48u) |
        (static_cast<uint64_t>(psm) << 56u);
}

uint64_t trxpos(uint32_t x, uint32_t y)
{
    return (static_cast<uint64_t>(x) << 32u) |
        (static_cast<uint64_t>(y) << 48u);
}

uint64_t trxreg(uint32_t w, uint32_t h)
{
    return static_cast<uint64_t>(w) |
        (static_cast<uint64_t>(h) << 32u);
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

bool image(ee::c_ee_gs_state& gs, const uint8_t* payload, uint32_t qwc)
{
    ee::s_ee_gif_event event{};
    event.m_type = ee::e_ee_gif_event_type::image;
    event.m_qwc = qwc;
    return gs.consume(&event, 1u, payload, qwc);
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

void set16(uint8_t* data, uint32_t index, uint16_t value)
{
    data[index * 2u] = static_cast<uint8_t>(value);
    data[index * 2u + 1u] = static_cast<uint8_t>(value >> 8u);
}

void set32(uint8_t* data, uint32_t value)
{
    data[0] = static_cast<uint8_t>(value);
    data[1] = static_cast<uint8_t>(value >> 8u);
    data[2] = static_cast<uint8_t>(value >> 16u);
    data[3] = static_cast<uint8_t>(value >> 24u);
}
} // namespace

int main()
{
    ee::c_ee_gs_state gs;
    constexpr uint32_t k_bp = 0x200u;
    constexpr uint32_t k_base = k_bp * 256u;

    // Reserved destination format 0x03 remains unsupported.
    // Rejection must not commit TRXDIR or start an upload.
    if (!reg(gs, 0x50u, bitblt(k_bp, 2u, 0x03u)) ||
        !reg(gs, 0x51u, trxpos(0u, 63u)) ||
        !reg(gs, 0x52u, trxreg(65u, 2u)) ||
        reg(gs, 0x53u, 0u) ||
        gs.was_written(0x53u) ||
        gs.state().m_transfer_started)
    {
        return fail("unsupported reserved PSM 0x03 was incorrectly treated as PSMCT16");
    }

    // Width 65 crosses the X page boundary; Y=63 crosses the Y page
    // boundary after one row. DBW=2 makes the second page row 16 KiB away.
    // These offsets are fixed independent GS address fixtures.
    std::vector<uint8_t> pixels(17u * 16u, 0xcdu);
    for (uint32_t i = 0u; i < 130u; ++i)
    {
        set16(pixels.data(), i, static_cast<uint16_t>(0x4001u + i));
    }

    if (!reg(gs, 0x50u, bitblt(k_bp, 2u, 0x02u)) ||
        !reg(gs, 0x53u, 0u) ||
        !gs.state().m_transfer_started ||
        !image(gs, pixels.data(), 8u) ||
        gs.state().m_transfer_pixels_written != 64u ||
        gs.state().m_image_qwc != 8u ||
        !read16(gs, k_base + 5576u, 0x4001u) ||
        !read16(gs, k_base + 5580u, 0x4002u) ||
        !read16(gs, k_base + 5578u, 0x4009u) ||
        !read16(gs, k_base + 6088u, 0x4011u) ||
        !read16(gs, k_base + 8192u + 5576u, 0u) ||
        !read16(gs, k_base + 16384u, 0u))
    {
        return fail("first IMAGE DMA or 16-bit swizzle offsets");
    }

    if (!image(gs, pixels.data() + 8u * 16u, 9u) ||
        gs.state().m_transfer_pixels_written != 130u ||
        gs.state().m_transfer_pending_size != 0u ||
        gs.state().m_image_qwc != 17u ||
        !read16(gs, k_base + 8192u + 5576u, 0x4041u) ||
        !read16(gs, k_base + 16384u, 0x4042u) ||
        !read16(gs, k_base + 16384u + 512u, 0x4052u) ||
        !read16(gs, k_base + 24576u, 0x4082u) ||
        !read16(gs, k_base + 16384u + 8u, 0u))
    {
        return fail("PSMCT16 page crossing, continuation or padding");
    }

    // An unsupported GS command after an IMAGE within the same batch
    // must roll back the pixel writes and the transfer cursor.
    std::array<uint8_t, 32> reject_packet{};
    for (uint32_t i = 0u; i < 8u; ++i)
    {
        set16(reject_packet.data(), i, static_cast<uint16_t>(0x9001u + i));
    }
    if (!reg(gs, 0x53u, 0u))
    {
        return fail("could not restart PSMCT16 upload");
    }
    ee::s_ee_gif_event events[2]{};
    events[0].m_type = ee::e_ee_gif_event_type::image;
    events[0].m_qwc = 1u;
    events[1].m_type = ee::e_ee_gif_event_type::packed;
    events[1].m_register = 0xeu;
    events[1].m_byte_offset = 16u;
    events[1].m_word1 = 0x61u; // unsupported FINISH
    if (gs.consume(events, 2u, reject_packet.data(), 2u) ||
        !read16(gs, k_base + 5576u, 0x4001u) ||
        gs.state().m_transfer_pixels_written != 0u ||
        !image(gs, reject_packet.data(), 1u) ||
        !read16(gs, k_base + 5576u, 0x9001u) ||
        gs.state().m_transfer_pixels_written != 8u)
    {
        return fail("failed batch changed VRAM or transfer cursor");
    }

    // A 16-bit pixel must overwrite only its chosen VRAM halfword,
    // including the x=8 high-halfword swizzle at the same word address.
    gs.reset();
    constexpr uint32_t k_half_base = 0x300u * 256u;
    std::array<uint8_t, 16> seed{};
    set32(seed.data(), 0x12345678u);
    std::array<uint8_t, 16> low{};
    set16(low.data(), 0u, 0xabcdu);
    std::array<uint8_t, 16> high{};
    set16(high.data(), 0u, 0x5678u);
    if (!reg(gs, 0x50u, bitblt(0x300u, 1u, 0u)) ||
        !reg(gs, 0x51u, trxpos(0u, 0u)) ||
        !reg(gs, 0x52u, trxreg(1u, 1u)) ||
        !reg(gs, 0x53u, 0u) ||
        !image(gs, seed.data(), 1u) ||
        !reg(gs, 0x50u, bitblt(0x300u, 1u, 2u)) ||
        !reg(gs, 0x53u, 0u) ||
        !image(gs, low.data(), 1u) ||
        !read32(gs, k_half_base, 0x1234abcdu) ||
        !reg(gs, 0x51u, trxpos(8u, 0u)) ||
        !reg(gs, 0x53u, 0u) ||
        !image(gs, high.data(), 1u) ||
        !read32(gs, k_half_base, 0x5678abcdu))
    {
        return fail("PSMCT16 did not preserve the neighboring halfword");
    }

    // BP is in 256-byte units; an x=16 pixel at the final BP wraps
    // at the 4-MiB backing store boundary.
    gs.reset();
    std::array<uint8_t, 16> wrap{};
    set16(wrap.data(), 0u, 0xbeefu);
    if (!reg(gs, 0x50u, bitblt(0x3fffu, 1u, 2u)) ||
        !reg(gs, 0x51u, trxpos(16u, 0u)) ||
        !reg(gs, 0x52u, trxreg(1u, 1u)) ||
        !reg(gs, 0x53u, 0u) ||
        !image(gs, wrap.data(), 1u) ||
        !read16(gs, 0x100u, 0xbeefu) ||
        !read16(gs, 0x3fffu * 256u, 0u))
    {
        return fail("PSMCT16 GS VRAM wrapping");
    }

    uint16_t unused = 0u;
    if (gs.read_vram16(
            static_cast<uint32_t>(ee::c_ee_gs_state::k_vram_bytes - 1u),
            unused))
    {
        return fail("out-of-range GS VRAM halfword read");
    }

    gs.reset();
    if (!read16(gs, 0x100u, 0u) ||
        gs.state().m_transfer_pixels_written != 0u ||
        gs.state().m_transfer_pending_size != 0u ||
        gs.state().m_transfer_started)
    {
        return fail("reset did not clear 16-bit VRAM or transfer state");
    }

    std::puts("GS PSMCT16 swizzle, split DMA, rollback and reset: PASS");
    return 0;
}
