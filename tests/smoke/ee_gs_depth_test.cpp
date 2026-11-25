#include "ee_gs.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
int fail(const char* reason)
{
    std::fprintf(stderr, "EE GS depth: %s\n", reason);
    return 1;
}

uint64_t bitblt(uint32_t bp, uint32_t bw, uint32_t psm)
{
    return (static_cast<uint64_t>(bp) << 32u) |
        (static_cast<uint64_t>(bw) << 48u) |
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

bool reg(ee::c_ee_gs_state& gs, uint32_t address, uint64_t value)
{
    ee::s_ee_gif_event e{};
    e.m_type = ee::e_ee_gif_event_type::packed;
    e.m_register = 0xeu;
    e.m_word0 = value;
    e.m_word1 = address;
    const std::array<uint8_t, 16> dummy{};
    return gs.consume(&e, 1u, dummy.data(), 1u);
}

bool image(ee::c_ee_gs_state& gs, const uint8_t* data, uint32_t qwc)
{
    ee::s_ee_gif_event e{};
    e.m_type = ee::e_ee_gif_event_type::image;
    e.m_qwc = qwc;
    return gs.consume(&e, 1u, data, qwc);
}

bool setup(
    ee::c_ee_gs_state& gs, uint32_t bp, uint32_t bw,
    uint32_t psm, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height)
{
    return reg(gs, 0x50u, bitblt(bp, bw, psm)) &&
        reg(gs, 0x51u, trxpos(x, y)) &&
        reg(gs, 0x52u, trxreg(width, height)) &&
        reg(gs, 0x53u, 0u);
}

bool upload(
    ee::c_ee_gs_state& gs, uint32_t bp, uint32_t bw,
    uint32_t psm, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height,
    const uint8_t* data, uint32_t qwc)
{
    return setup(gs, bp, bw, psm, x, y, width, height) &&
        image(gs, data, qwc);
}

bool read16(const ee::c_ee_gs_state& gs, uint32_t address, uint16_t expected)
{
    uint16_t actual = 0u;
    return gs.read_vram16(address, actual) && actual == expected;
}

bool read32(const ee::c_ee_gs_state& gs, uint32_t address, uint32_t expected)
{
    uint32_t actual = 0u;
    return gs.read_vram32(address, actual) && actual == expected;
}

void set16(uint8_t* data, uint32_t i, uint16_t value)
{
    data[i * 2u] = static_cast<uint8_t>(value);
    data[i * 2u + 1u] = static_cast<uint8_t>(value >> 8u);
}

void set32(uint8_t* data, uint32_t i, uint32_t value)
{
    for (uint32_t b = 0; b < 4u; ++b)
    {
        data[i * 4u + b] = static_cast<uint8_t>(value >> (b * 8u));
    }
}

void set24(uint8_t* data, uint32_t i, uint32_t value)
{
    for (uint32_t b = 0; b < 3u; ++b)
    {
        data[i * 3u + b] = static_cast<uint8_t>(value >> (b * 8u));
    }
}
} // namespace

int main()
{
    ee::c_ee_gs_state gs;
    constexpr uint32_t k_bp = 0x180u;
    constexpr uint32_t k_base = k_bp * 256u;

    // Unsupported/reserved PSM must not start a depth upload.
    if (!reg(gs, 0x50u, bitblt(k_bp, 2u, 0x03u)) ||
        !reg(gs, 0x51u, trxpos(0u, 31u)) ||
        !reg(gs, 0x52u, trxreg(65u, 2u)) ||
        reg(gs, 0x53u, 0u) ||
        gs.was_written(0x53u) ||
        gs.state().m_transfer_started)
    {
        return fail("reserved destination PSM was accepted");
    }

    // PSMZ32 uses 64x32 pages and the distinct kBlockZ32 table.
    // Fixed physical offsets calculated from reference g_offz32.
    // A 65x2 rectangle at y=31 crosses both GS page axes.
    std::vector<uint8_t> pixels32(33u * 16u, 0xeeu);
    for (uint32_t i = 0u; i < 130u; ++i)
    {
        set32(pixels32.data(), i, 0x70000000u + i);
    }
    if (!setup(gs, k_bp, 2u, 0x30u, 0u, 31u, 65u, 2u) ||
        !image(gs, pixels32.data(), 16u) ||
        gs.state().m_transfer_pixels_written != 64u ||
        !read32(gs, k_base + 4808u, 0x70000000u) ||
        !read32(gs, k_base + 4812u, 0x70000001u) ||
        !read32(gs, k_base + 5064u, 0x70000008u) ||
        !read32(gs, k_base + 5832u, 0x70000010u) ||
        !read32(gs, k_base + 712u, 0x70000020u) ||
        !read32(gs, k_base + 2044u, 0x7000003fu) ||
        !read32(gs, k_base + 2760u, 0u) ||
        !read32(gs, k_base + 13000u, 0u))
    {
        return fail("PSMZ32 block permutation or first DMA batch");
    }
    if (!image(gs, pixels32.data() + 16u * 16u, 17u) ||
        gs.state().m_transfer_pixels_written != 130u ||
        gs.state().m_image_qwc != 33u ||
        !read32(gs, k_base + 13000u, 0x70000040u) ||
        !read32(gs, k_base + 22528u, 0x70000041u) ||
        !read32(gs, k_base + 23552u, 0x70000051u) ||
        !read32(gs, k_base + 18432u, 0x70000061u) ||
        !read32(gs, k_base + 30720u, 0x70000081u) ||
        !read32(gs, k_base + 32768u, 0u))
    {
        return fail("PSMZ32 split DMA or X/Y page crossings");
    }

    // Seed a depth word with a distinctive upper byte. PSMZ24 must
    // preserve the byte while overwriting 24-bit Z, including when a
    // pixel is divided between two 16-byte GIF IMAGE quadwords.
    gs.reset();
    std::array<uint8_t, 16> seed{};
    set32(seed.data(), 0u, 0xabcd1234u);
    if (!upload(gs, k_bp, 2u, 0x30u, 0u, 31u,
                1u, 1u, seed.data(), 1u))
    {
        return fail("could not seed depth high byte");
    }
    std::vector<uint8_t> pixels24(25u * 16u, 0xe7u);
    for (uint32_t i = 0u; i < 130u; ++i)
    {
        set24(pixels24.data(), i, 0x00224400u + i);
    }
    if (!setup(gs, k_bp, 2u, 0x31u, 0u, 31u, 65u, 2u) ||
        !image(gs, pixels24.data(), 8u) ||
        gs.state().m_transfer_pixels_written != 42u ||
        gs.state().m_transfer_pending_size != 2u ||
        !read32(gs, k_base + 4808u, 0xab224400u) ||
        !read32(gs, k_base + 712u, 0x00224420u) ||
        !read32(gs, k_base + 1736u, 0u))
    {
        return fail("PSMZ24 high-byte preservation or 2-byte carry");
    }
    if (!image(gs, pixels24.data() + 8u * 16u, 17u) ||
        gs.state().m_transfer_pixels_written != 130u ||
        gs.state().m_transfer_pending_size != 0u ||
        !read32(gs, k_base + 1736u, 0x00224430u) ||
        !read32(gs, k_base + 13000u, 0x00224440u) ||
        !read32(gs, k_base + 22528u, 0x00224441u) ||
        !read32(gs, k_base + 30720u, 0x00224481u))
    {
        return fail("PSMZ24 cross-image pixel completion or final padding");
    }

    // Z16 and Z16S have separate 64x64 block permutations.
    // The 65x2 rectangle at y=63 crosses both page axes.
    std::vector<uint8_t> pixels16(17u * 16u, 0xccu);
    for (uint32_t i = 0u; i < 130u; ++i)
    {
        set16(pixels16.data(), i, static_cast<uint16_t>(0x5000u + i));
    }
    gs.reset();
    if (!setup(gs, k_bp, 2u, 0x32u, 0u, 63u, 65u, 2u) ||
        !image(gs, pixels16.data(), 8u) ||
        gs.state().m_transfer_pixels_written != 64u ||
        !read16(gs, k_base + 3528u, 0x5000u) ||
        !read16(gs, k_base + 3532u, 0x5001u) ||
        !read16(gs, k_base + 3530u, 0x5008u) ||
        !read16(gs, k_base + 4040u, 0x5010u) ||
        !read16(gs, k_base + 1480u, 0x5020u) ||
        !read16(gs, k_base + 2046u, 0x503fu) ||
        !read16(gs, k_base + 5576u, 0u))
    {
        return fail("PSMZ16 reference swizzle or halfwords");
    }
    if (!image(gs, pixels16.data() + 8u * 16u, 9u) ||
        gs.state().m_transfer_pixels_written != 130u ||
        !read16(gs, k_base + 11720u, 0x5040u) ||
        !read16(gs, k_base + 22528u, 0x5041u) ||
        !read16(gs, k_base + 20480u, 0x5061u) ||
        !read16(gs, k_base + 30720u, 0x5081u))
    {
        return fail("PSMZ16 page boundaries or DMA continuation");
    }

    gs.reset();
    if (!setup(gs, k_bp, 2u, 0x3au, 0u, 63u, 65u, 2u) ||
        !image(gs, pixels16.data(), 8u) ||
        gs.state().m_transfer_pixels_written != 64u ||
        !read16(gs, k_base + 5576u, 0x5000u) ||
        !read16(gs, k_base + 5580u, 0x5001u) ||
        !read16(gs, k_base + 5578u, 0x5008u) ||
        !read16(gs, k_base + 6088u, 0x5010u) ||
        !read16(gs, k_base + 1480u, 0x5020u) ||
        !read16(gs, k_base + 2046u, 0x503fu) ||
        !read16(gs, k_base + 3528u, 0u) ||
        !image(gs, pixels16.data() + 8u * 16u, 9u) ||
        gs.state().m_transfer_pixels_written != 130u ||
        !read16(gs, k_base + 13768u, 0x5040u) ||
        !read16(gs, k_base + 22528u, 0x5041u) ||
        !read16(gs, k_base + 18432u, 0x5061u) ||
        !read16(gs, k_base + 30720u, 0x5081u))
    {
        return fail("PSMZ16S block permutation, DMA or page boundaries");
    }

    // A rejected late FINISH must not commit staged depth pixels,
    // including the cursor; the same IMAGE can then be retried.
    if (!reg(gs, 0x53u, 0u))
    {
        return fail("could not restart depth transfer");
    }
    std::array<uint8_t, 32> retry{};
    retry.fill(0x99u);
    ee::s_ee_gif_event events[2]{};
    events[0].m_type = ee::e_ee_gif_event_type::image;
    events[0].m_qwc = 1u;
    events[1].m_type = ee::e_ee_gif_event_type::packed;
    events[1].m_register = 0xeu;
    events[1].m_byte_offset = 16u;
    events[1].m_word1 = 0x61u;
    if (gs.consume(events, 2u, retry.data(), 2u) ||
        gs.state().m_transfer_pixels_written != 0u ||
        !read16(gs, k_base + 5576u, 0x5000u) ||
        !image(gs, retry.data(), 1u) ||
        gs.state().m_transfer_pixels_written != 8u ||
        !read16(gs, k_base + 5576u, 0x9999u))
    {
        return fail("depth transfer rollback leaked writes or cursor");
    }

    // PSMZ16 updates one halfword without modifying its neighbor.
    gs.reset();
    constexpr uint32_t k_half_bp = 0x300u;
    constexpr uint32_t k_half_base = k_half_bp * 256u + 6144u;
    set32(seed.data(), 0u, 0x12345678u);
    std::array<uint8_t, 16> lower{};
    std::array<uint8_t, 16> upper{};
    set16(lower.data(), 0u, 0xabcdu);
    set16(upper.data(), 0u, 0x5678u);
    if (!upload(gs, k_half_bp + 24u, 1u, 0u, 0u, 0u,
                1u, 1u, seed.data(), 1u) ||
        !upload(gs, k_half_bp, 1u, 0x32u, 0u, 0u,
                1u, 1u, lower.data(), 1u) ||
        !read32(gs, k_half_base, 0x1234abcdu) ||
        !upload(gs, k_half_bp, 1u, 0x32u, 8u, 0u,
                1u, 1u, upper.data(), 1u) ||
        !read32(gs, k_half_base, 0x5678abcdu))
    {
        return fail("PSMZ16 did not preserve adjacent halfword");
    }

    // Depth addresses wrap around the 4 MiB GS VRAM buffer.
    gs.reset();
    if (!upload(gs, 0x3fffu, 1u, 0x30u, 32u, 31u,
                1u, 1u, seed.data(), 1u) ||
        !read32(gs, 456u, 0x12345678u) ||
        !read32(gs, 0x3fffu * 256u, 0u) ||
        !upload(gs, 0x3fffu, 1u, 0x3au, 32u, 63u,
                1u, 1u, lower.data(), 1u) ||
        !read16(gs, 1224u, 0xabcdu))
    {
        return fail("depth GS VRAM 4 MiB wrapping");
    }

    gs.reset();
    if (gs.state().m_transfer_started ||
        gs.state().m_transfer_pixels_written != 0u ||
        gs.state().m_image_qwc != 0u ||
        !read32(gs, 456u, 0u) ||
        !read16(gs, 1224u, 0u))
    {
        return fail("reset retained depth VRAM or transfer state");
    }

    std::puts("GS PSMZ32/24/16/16S depth swizzles, DMA, rollback: PASS");
    return 0;
}
