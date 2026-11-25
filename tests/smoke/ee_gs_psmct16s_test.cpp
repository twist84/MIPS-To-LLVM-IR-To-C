#include "ee_gs.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
int fail(const char* reason)
{
    std::fprintf(stderr, "EE GS PSMCT16S: %s\n", reason);
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
    data[index * 2u + 0u] = static_cast<uint8_t>(value);
    data[index * 2u + 1u] = static_cast<uint8_t>(value >> 8u);
}

void set32(uint8_t* data, uint32_t value)
{
    for (uint32_t i = 0; i < 4u; ++i)
    {
        data[i] = static_cast<uint8_t>(value >> (i * 8u));
    }
}
} // namespace

int main()
{
    ee::c_ee_gs_state gs;
    constexpr uint32_t k_bp = 0x200u;
    constexpr uint32_t k_base = k_bp * 256u;

    // Both page boundaries are crossed by a 65x2 rectangle at y=63.
    // Fixed offsets are generated independently from the reference's
    // PSMCT16S tables; they distinguish PSMCT16S from PSMCT16.
    std::vector<uint8_t> payload(17u * 16u, 0xccu);
    for (uint32_t i = 0u; i < 130u; ++i)
    {
        set16(payload.data(), i, static_cast<uint16_t>(0x6001u + i));
    }
    if (!reg(gs, 0x50u, bitblt(k_bp, 2u, 0x0au)) ||
        !reg(gs, 0x51u, trxpos(0u, 63u)) ||
        !reg(gs, 0x52u, trxreg(65u, 2u)) ||
        !reg(gs, 0x53u, 0u) ||
        !gs.state().m_transfer_started ||
        !image(gs, payload.data(), 8u) ||
        gs.state().m_transfer_pixels_written != 64u ||
        gs.state().m_image_qwc != 8u ||
        !read16(gs, k_base + 3528u, 0x6001u) ||
        !read16(gs, k_base + 3532u, 0x6002u) ||
        !read16(gs, k_base + 3530u, 0x6009u) ||
        !read16(gs, k_base + 4040u, 0x6011u) ||
        !read16(gs, k_base + 7624u, 0x6021u) ||
        !read16(gs, k_base + 8190u, 0x6040u) ||
        !read16(gs, k_base + 5576u, 0u) ||
        !read16(gs, k_base + 8192u + 3528u, 0u))
    {
        return fail("PSMCT16S alternate swizzle or first IMAGE chunk");
    }

    if (!image(gs, payload.data() + 8u * 16u, 9u) ||
        gs.state().m_transfer_pixels_written != 130u ||
        gs.state().m_transfer_pending_size != 0u ||
        gs.state().m_image_qwc != 17u ||
        !read16(gs, k_base + 8192u + 3528u, 0x6041u) ||
        !read16(gs, k_base + 16384u, 0x6042u) ||
        !read16(gs, k_base + 16388u, 0x6043u) ||
        !read16(gs, k_base + 16386u, 0x604au) ||
        !read16(gs, k_base + 16384u + 512u, 0x6052u) ||
        !read16(gs, k_base + 24576u, 0x6082u) ||
        !read16(gs, k_base + 24576u + 4u, 0u))
    {
        return fail("PSMCT16S page crossing, DMA continuation or padding");
    }

    // A rejected late FINISH must preserve previous VRAM contents and
    // the transfer cursor, so the same IMAGE can be retried.
    std::array<uint8_t, 32> retry{};
    for (uint32_t i = 0u; i < 8u; ++i)
    {
        set16(retry.data(), i, static_cast<uint16_t>(0x9001u + i));
    }
    if (!reg(gs, 0x53u, 0u))
    {
        return fail("unable to restart transfer");
    }
    ee::s_ee_gif_event events[2]{};
    events[0].m_type = ee::e_ee_gif_event_type::image;
    events[0].m_qwc = 1u;
    events[1].m_type = ee::e_ee_gif_event_type::packed;
    events[1].m_register = 0xeu;
    events[1].m_byte_offset = 16u;
    events[1].m_word1 = 0x61u; // FINISH is unsupported
    if (gs.consume(events, 2u, retry.data(), 2u) ||
        !read16(gs, k_base + 3528u, 0x6001u) ||
        gs.state().m_transfer_pixels_written != 0u ||
        !image(gs, retry.data(), 1u) ||
        !read16(gs, k_base + 3528u, 0x9001u) ||
        gs.state().m_transfer_pixels_written != 8u)
    {
        return fail("unsupported late event committed partial VRAM state");
    }

    // PSMCT16S shares its column permutation with PSMCT16, including
    // the adjacent halfwords for x=0 and x=8 at y=0.
    gs.reset();
    constexpr uint32_t k_half_base = 0x300u * 256u;
    std::array<uint8_t, 16> seed{};
    std::array<uint8_t, 16> low{};
    std::array<uint8_t, 16> high{};
    set32(seed.data(), 0x12345678u);
    set16(low.data(), 0u, 0xabcdu);
    set16(high.data(), 0u, 0x5678u);
    if (!reg(gs, 0x50u, bitblt(0x300u, 1u, 0u)) ||
        !reg(gs, 0x51u, trxpos(0u, 0u)) ||
        !reg(gs, 0x52u, trxreg(1u, 1u)) ||
        !reg(gs, 0x53u, 0u) ||
        !image(gs, seed.data(), 1u) ||
        !reg(gs, 0x50u, bitblt(0x300u, 1u, 0x0au)) ||
        !reg(gs, 0x53u, 0u) ||
        !image(gs, low.data(), 1u) ||
        !read32(gs, k_half_base, 0x1234abcdu) ||
        !reg(gs, 0x51u, trxpos(8u, 0u)) ||
        !reg(gs, 0x53u, 0u) ||
        !image(gs, high.data(), 1u) ||
        !read32(gs, k_half_base, 0x5678abcdu))
    {
        return fail("PSMCT16S overwrote neighboring GS halfword");
    }

    // The destination base pointer is in 256-byte units. The second
    // PSMCT16S block wraps beyond the end of the 4 MiB allocation.
    gs.reset();
    std::array<uint8_t, 16> wrap{};
    set16(wrap.data(), 0u, 0xbeefu);
    if (!reg(gs, 0x50u, bitblt(0x3fffu, 1u, 0x0au)) ||
        !reg(gs, 0x51u, trxpos(16u, 0u)) ||
        !reg(gs, 0x52u, trxreg(1u, 1u)) ||
        !reg(gs, 0x53u, 0u) ||
        !image(gs, wrap.data(), 1u) ||
        !read16(gs, 0x100u, 0xbeefu) ||
        !read16(gs, 0x3fffu * 256u, 0u))
    {
        return fail("PSMCT16S VRAM address wrapping");
    }

    gs.reset();
    if (gs.state().m_transfer_started ||
        gs.state().m_transfer_pixels_written != 0u ||
        gs.state().m_image_qwc != 0u ||
        !read16(gs, 0x100u, 0u))
    {
        return fail("GS reset did not clear alternate-format state");
    }

    std::puts("GS PSMCT16S swizzle, DMA continuation, rollback: PASS");
    return 0;
}
