#include "ee_gs.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
int fail(const char* reason)
{
    std::fprintf(stderr, "EE GS indexed high lanes: %s\n", reason);
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

uint64_t tex0(
    uint32_t tbp, uint32_t tbw, uint32_t psm, uint32_t cbp,
    uint32_t cpsm, uint32_t csm, uint32_t csa, uint32_t cld)
{
    return static_cast<uint64_t>(tbp) |
        (static_cast<uint64_t>(tbw) << 14u) |
        (static_cast<uint64_t>(psm) << 20u) |
        (static_cast<uint64_t>(cbp) << 37u) |
        (static_cast<uint64_t>(cpsm) << 51u) |
        (static_cast<uint64_t>(csm) << 55u) |
        (static_cast<uint64_t>(csa) << 56u) |
        (static_cast<uint64_t>(cld) << 61u);
}

uint64_t texclut(uint32_t cbw, uint32_t cou, uint32_t cov)
{
    return static_cast<uint64_t>(cbw) |
        (static_cast<uint64_t>(cou) << 6u) |
        (static_cast<uint64_t>(cov) << 12u);
}

bool reg(ee::c_ee_gs_state& gs, uint32_t index, uint64_t value)
{
    ee::s_ee_gif_event e{};
    e.m_type = ee::e_ee_gif_event_type::packed;
    e.m_register = 0xeu;
    e.m_word0 = value;
    e.m_word1 = index;
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
    ee::c_ee_gs_state& gs,
    uint32_t bp, uint32_t bw, uint32_t psm,
    uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    return reg(gs, 0x50u, bitblt(bp, bw, psm)) &&
        reg(gs, 0x51u, trxpos(x, y)) &&
        reg(gs, 0x52u, trxreg(width, height)) &&
        reg(gs, 0x53u, 0u);
}

bool upload(
    ee::c_ee_gs_state& gs,
    uint32_t bp, uint32_t bw, uint32_t psm,
    uint32_t x, uint32_t y, uint32_t width, uint32_t height,
    const uint8_t* data, uint32_t qwc)
{
    return setup(gs, bp, bw, psm, x, y, width, height) &&
        image(gs, data, qwc);
}

bool read8(const ee::c_ee_gs_state& gs, uint32_t at, uint8_t expected)
{
    uint8_t actual = 0u;
    return gs.read_vram8(at, actual) && actual == expected;
}

bool read32(const ee::c_ee_gs_state& gs, uint32_t at, uint32_t expected)
{
    uint32_t actual = 0u;
    return gs.read_vram32(at, actual) && actual == expected;
}

bool clut(const ee::c_ee_gs_state& gs, uint32_t slot, uint32_t expected)
{
    uint32_t actual = 0u;
    return gs.read_clut_entry(slot, actual) && actual == expected;
}

bool indexed(
    const ee::c_ee_gs_state& gs, uint32_t context,
    uint32_t x, uint32_t y, uint32_t expected)
{
    uint32_t actual = 0u;
    return gs.read_indexed_clut_value(context, x, y, actual) &&
        actual == expected;
}

void set16(uint8_t* data, uint32_t index, uint16_t value)
{
    const uint32_t at = index * 2u;
    data[at] = static_cast<uint8_t>(value);
    data[at + 1u] = static_cast<uint8_t>(value >> 8u);
}

void set32(uint8_t* data, uint32_t index, uint32_t value)
{
    const uint32_t at = index * 4u;
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        data[at + i] = static_cast<uint8_t>(value >> (8u * i));
    }
}

void set4(uint8_t* data, uint32_t index, uint8_t value)
{
    const uint32_t at = index / 2u;
    const uint8_t shift = static_cast<uint8_t>((index & 1u) * 4u);
    const uint8_t mask = static_cast<uint8_t>(0x0fu << shift);
    data[at] = static_cast<uint8_t>(
        (data[at] & static_cast<uint8_t>(~mask)) |
        static_cast<uint8_t>((value & 0x0fu) << shift)
    );
}
} // namespace

int main()
{
    ee::c_ee_gs_state gs;
    constexpr uint32_t k_bp = 0x140u;
    constexpr uint32_t k_base = k_bp * 256u;

    // A reserved pixel-storage format must be rejected rather than
    // silently using an unrelated 32-bit color layout.
    if (!reg(gs, 0x50u, bitblt(k_bp, 2u, 0x03u)) ||
        !reg(gs, 0x51u, trxpos(0u, 31u)) ||
        !reg(gs, 0x52u, trxreg(65u, 2u)) ||
        reg(gs, 0x53u, 0u) ||
        gs.was_written(0x53u) ||
        gs.state().m_transfer_started)
    {
        return fail("unsupported reserved PSM 0x03 transfer was committed");
    }

    // High-byte formats share PSMCT32's 64x32 page and 256-byte
    // block permutation. 65x2 crosses X=64 and Y=32 page boundaries;
    // DBW=2 places the second page row 16 KiB further into GS VRAM.
    // These are fixed, reference-derived physical byte offsets.
    std::vector<uint8_t> payload8(9u * 16u, 0xe7u);
    for (uint32_t i = 0u; i < 130u; ++i)
    {
        payload8[i] = static_cast<uint8_t>(0x31u + i);
    }

    if (!setup(gs, k_bp, 2u, 0x1bu, 0u, 31u, 65u, 2u) ||
        !image(gs, payload8.data(), 4u) ||
        gs.state().m_transfer_pixels_written != 64u ||
        gs.state().m_image_qwc != 4u ||
        !read8(gs, k_base + 2763u, 0x31u) ||
        !read8(gs, k_base + 2767u, 0x32u) ||
        !read8(gs, k_base + 2779u, 0x33u) ||
        !read8(gs, k_base + 3019u, 0x39u) ||
        !read8(gs, k_base + 3787u, 0x41u) ||
        !read8(gs, k_base + 6859u, 0x51u) ||
        !read8(gs, k_base + 8191u, 0x70u) ||
        !read32(gs, k_base + 2760u, 0x31000000u) ||
        !read8(gs, k_base + 10955u, 0u))
    {
        return fail("PSMT8H first-image swizzle or upper-byte overwrite");
    }
    if (!image(gs, payload8.data() + 4u * 16u, 5u) ||
        gs.state().m_transfer_pixels_written != 130u ||
        gs.state().m_image_qwc != 9u ||
        gs.state().m_transfer_pending_size != 0u ||
        !read8(gs, k_base + 10955u, 0x71u) ||
        !read8(gs, k_base + 16387u, 0x72u) ||
        !read8(gs, k_base + 16391u, 0x73u) ||
        !read8(gs, k_base + 16643u, 0x7au) ||
        !read8(gs, k_base + 17411u, 0x82u) ||
        !read8(gs, k_base + 24579u, 0xb2u) ||
        !read8(gs, k_base + 16384u, 0u) ||
        !read8(gs, k_base + 16395u, 0u))
    {
        return fail("PSMT8H continued DMA, page crossing or padding");
    }

    // A failed combined IMAGE/FINISH event must not change VRAM or
    // advance the transfer cursor; retrying the IMAGE must succeed.
    if (!reg(gs, 0x53u, 0u))
    {
        return fail("could not restart PSMT8H transfer");
    }
    std::array<uint8_t, 32> retry{};
    retry.fill(0x5cu);
    ee::s_ee_gif_event events[2]{};
    events[0].m_type = ee::e_ee_gif_event_type::image;
    events[0].m_qwc = 1u;
    events[1].m_type = ee::e_ee_gif_event_type::packed;
    events[1].m_register = 0xeu;
    events[1].m_byte_offset = 16u;
    events[1].m_word1 = 0x61u;
    if (gs.consume(events, 2u, retry.data(), 2u) ||
        gs.state().m_transfer_pixels_written != 0u ||
        !read8(gs, k_base + 2763u, 0x31u) ||
        !image(gs, retry.data(), 1u) ||
        gs.state().m_transfer_pixels_written != 16u ||
        !read8(gs, k_base + 2763u, 0x5cu) ||
        !read32(gs, k_base + 2760u, 0x5c000000u))
    {
        return fail("PSMT8H failed-event rollback and retry");
    }

    // The same 65x2 fixture holds for HL/HH but input pixels occupy
    // 4 bits. The first IMAGE has 64 packed indices, and the second
    // continues onto the next GS page without per-row padding.
    std::vector<uint8_t> payload4(5u * 16u, 0u);
    for (uint32_t i = 0u; i < 130u; ++i)
    {
        set4(payload4.data(), i, static_cast<uint8_t>(1u + i % 15u));
    }
    gs.reset();
    if (!setup(gs, k_bp, 2u, 0x24u, 0u, 31u, 65u, 2u) ||
        !image(gs, payload4.data(), 2u) ||
        gs.state().m_transfer_pixels_written != 64u ||
        !read8(gs, k_base + 2763u, 0x01u) ||
        !read8(gs, k_base + 2767u, 0x02u) ||
        !read8(gs, k_base + 3019u, 0x09u) ||
        !read8(gs, k_base + 6859u, 0x03u) ||
        !read8(gs, k_base + 8191u, 0x04u) ||
        !read8(gs, k_base + 10955u, 0u) ||
        !image(gs, payload4.data() + 2u * 16u, 3u) ||
        gs.state().m_transfer_pixels_written != 130u ||
        gs.state().m_image_qwc != 5u ||
        !read8(gs, k_base + 10955u, 0x05u) ||
        !read8(gs, k_base + 16387u, 0x06u) ||
        !read8(gs, k_base + 16391u, 0x07u) ||
        !read8(gs, k_base + 16643u, 0x0eu) ||
        !read8(gs, k_base + 24579u, 0x0au) ||
        !read8(gs, k_base + 16387u - 3u, 0u))
    {
        return fail("PSMT4HL swizzle, DMA or high-byte low nibble");
    }

    gs.reset();
    if (!setup(gs, k_bp, 2u, 0x2cu, 0u, 31u, 65u, 2u) ||
        !image(gs, payload4.data(), 2u) ||
        gs.state().m_transfer_pixels_written != 64u ||
        !read8(gs, k_base + 2763u, 0x10u) ||
        !read8(gs, k_base + 3019u, 0x90u) ||
        !read8(gs, k_base + 8191u, 0x40u) ||
        !image(gs, payload4.data() + 32u, 3u) ||
        gs.state().m_transfer_pixels_written != 130u ||
        !read8(gs, k_base + 10955u, 0x50u) ||
        !read8(gs, k_base + 16387u, 0x60u) ||
        !read8(gs, k_base + 16643u, 0xe0u) ||
        !read8(gs, k_base + 24579u, 0xa0u) ||
        !read8(gs, k_base + 16384u, 0u))
    {
        return fail("PSMT4HH high-nibble swizzle or page crossing");
    }

    // The three upper-lane formats must retain all 24 lower bits.
    // HL and HH additionally retain the other upper-byte nibble.
    gs.reset();
    constexpr uint32_t k_shared_bp = 0x300u;
    constexpr uint32_t k_shared = k_shared_bp * 256u;
    std::array<uint8_t, 16> seed{};
    std::array<uint8_t, 16> one{};
    set32(seed.data(), 0u, 0xe2aabbccu);
    if (!upload(gs, k_shared_bp, 1u, 0u, 0u, 0u,
                1u, 1u, seed.data(), 1u))
    {
        return fail("could not seed shared PSMCT32 word");
    }
    one[0] = 0x5au;
    if (!upload(gs, k_shared_bp, 1u, 0x1bu, 0u, 0u,
                1u, 1u, one.data(), 1u) ||
        !read32(gs, k_shared, 0x5aaabbccu))
    {
        return fail("PSMT8H did not preserve lower 24 bits");
    }
    one[0] = 0x07u;
    if (!upload(gs, k_shared_bp, 1u, 0x24u, 0u, 0u,
                1u, 1u, one.data(), 1u) ||
        !read32(gs, k_shared, 0x57aabbccu))
    {
        return fail("PSMT4HL modified high nibble or lower bits");
    }
    one[0] = 0x0du;
    if (!upload(gs, k_shared_bp, 1u, 0x2cu, 0u, 0u,
                1u, 1u, one.data(), 1u) ||
        !read32(gs, k_shared, 0xd7aabbccu))
    {
        return fail("PSMT4HH modified low nibble or lower bits");
    }

    // High-byte addressing wraps from the last GS base block across
    // the 4 MiB boundary; 8-pixel PSMCT32 block 1 is +256 bytes.
    gs.reset();
    one[0] = 0xabu;
    if (!upload(gs, 0x3fffu, 1u, 0x1bu, 8u, 0u,
                1u, 1u, one.data(), 1u) ||
        !read8(gs, 3u, 0xabu) ||
        !read8(gs, 0x3fffu * 256u + 3u, 0u))
    {
        return fail("high-byte 4 MiB GS VRAM wrapping");
    }

    // CSM1 loads 256 entries for PSMT8H, using the same CLUT
    // index-bit permutation as PSMT8.
    gs.reset();
    constexpr uint32_t k_cbp32 = 0x100u;
    constexpr uint32_t k_tbp = 0x500u;
    std::vector<uint8_t> colors32(64u * 16u, 0u);
    for (uint32_t i = 0u; i < 256u; ++i)
    {
        set32(colors32.data(), i, 0xa5000000u | i);
    }
    std::array<uint8_t, 16> idx{};
    idx[0] = 8u;
    idx[1] = 16u;
    if (!upload(gs, k_cbp32, 1u, 0u, 0u, 0u,
                16u, 16u, colors32.data(), 64u) ||
        !upload(gs, k_tbp, 1u, 0x1bu, 0u, 0u,
                2u, 1u, idx.data(), 1u) ||
        !reg(gs, 0x06u, tex0(
            k_tbp, 1u, 0x1bu, k_cbp32, 0u, 0u, 0u, 1u)) ||
        !clut(gs, 8u, 0xa5000010u) ||
        !clut(gs, 16u, 0xa5000008u) ||
        !indexed(gs, 0u, 0u, 0u, 0xa5000010u) ||
        !indexed(gs, 0u, 1u, 0u, 0xa5000008u))
    {
        return fail("PSMT8H CSM1 256-entry CLUT mapping");
    }

    // Four-bit upper-nibble formats share the CSA-selected CLUT
    // bank, including 16-bit CPSM entries without RGB expansion.
    constexpr uint32_t k_cbp16 = 0x200u;
    std::array<uint8_t, 32> colors16{};
    for (uint32_t i = 0u; i < 16u; ++i)
    {
        set16(colors16.data(), i, static_cast<uint16_t>(0x4000u + i));
    }
    if (!upload(gs, k_cbp16, 1u, 0x0au, 0u, 0u,
                8u, 2u, colors16.data(), 2u) ||
        !reg(gs, 0x07u, tex0(
            k_tbp, 1u, 0x24u, k_cbp16, 0x0au, 0u, 3u, 1u)) ||
        !clut(gs, 61u, 0x400du) ||
        !indexed(gs, 1u, 0u, 0u, 0x4008u))
    {
        return fail("PSMT4HL raw CSM1 palette loading");
    }

    // Upload a single high byte with separate 4HL and 4HH indices.
    // CSM1 reads both from their respective nibble positions.
    one[0] = 0x0du;
    if (!upload(gs, k_tbp, 1u, 0x24u, 0u, 0u,
                1u, 1u, one.data(), 1u) ||
        !indexed(gs, 1u, 0u, 0u, 0x400du) ||
        !reg(gs, 0x06u, tex0(
            k_tbp, 1u, 0x2cu, k_cbp16, 0x0au, 0u, 3u, 1u)))
    {
        return fail("PSMT4HL data write or PSMT4HH TEX0 registration");
    }
    one[0] = 0x0bu;
    if (!upload(gs, k_tbp, 1u, 0x2cu, 0u, 0u,
                1u, 1u, one.data(), 1u) ||
        !read8(gs, k_tbp * 256u + 3u, 0xbdu) ||
        !indexed(gs, 0u, 0u, 0u, 0x400bu) ||
        !indexed(gs, 1u, 0u, 0u, 0x400du))
    {
        return fail("PSMT4HH independent nibble and CSM1 CLUT lookup");
    }

    // CSM2: PSMT8H uses TEXCLUT.CBW/COU/COV to load an unswapped
    // palette index. This also covers the PSMCT16S CLUT swizzle.
    constexpr uint32_t k_csm2_cbp = 0x380u;
    std::vector<uint8_t> colors_csm2(32u * 16u, 0u);
    for (uint32_t i = 0u; i < 256u; ++i)
    {
        set16(colors_csm2.data(), i, static_cast<uint16_t>(0x1100u + i));
    }
    idx[0] = 8u;
    if (!upload(gs, k_csm2_cbp, 6u, 0x0au, 16u, 7u,
                256u, 1u, colors_csm2.data(), 32u) ||
        !reg(gs, 0x1cu, texclut(6u, 1u, 7u)) ||
        !reg(gs, 0x06u, tex0(
            k_tbp, 1u, 0x1bu, k_csm2_cbp, 0x0au, 1u, 0u, 1u)) ||
        !clut(gs, 8u, 0x1108u) ||
        !clut(gs, 16u, 0x1110u))
    {
        return fail("PSMT8H CSM2 index mapping or TEXCLUT stride");
    }

    // Rewriting indices does not reload the palette (CLD controls it).
    if (!upload(gs, k_tbp, 1u, 0x1bu, 0u, 0u,
                1u, 1u, idx.data(), 1u) ||
        !indexed(gs, 0u, 0u, 0u, 0x1108u))
    {
        return fail("PSMT8H CSM2 indexed-color lookup");
    }

    uint8_t unused = 0u;
    if (gs.read_vram8(
            static_cast<uint32_t>(ee::c_ee_gs_state::k_vram_bytes),
            unused))
    {
        return fail("read_vram8 accepted an out-of-range offset");
    }

    gs.reset();
    uint32_t missing = 0u;
    if (gs.state().m_clut_load_count != 0u ||
        gs.state().m_transfer_started ||
        gs.read_indexed_clut_value(0u, 0u, 0u, missing) ||
        gs.read_clut_entry(8u, missing) ||
        !read8(gs, k_tbp * 256u + 3u, 0u))
    {
        return fail("reset did not clear indexed high-lane state");
    }

    std::puts("GS PSMT8H / PSMT4HL / PSMT4HH uploads and CLUT: PASS");
    return 0;
}
