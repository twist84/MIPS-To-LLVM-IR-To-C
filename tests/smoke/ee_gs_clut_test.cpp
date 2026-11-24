#include "ee_gs.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
int fail(const char* reason)
{
    std::fprintf(stderr, "EE GS CLUT: %s\n", reason);
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

bool image(ee::c_ee_gs_state& gs, const uint8_t* payload, uint32_t qwc)
{
    ee::s_ee_gif_event e{};
    e.m_type = ee::e_ee_gif_event_type::image;
    e.m_qwc = qwc;
    return gs.consume(&e, 1u, payload, qwc);
}

bool upload(
    ee::c_ee_gs_state& gs, uint32_t bp, uint32_t bw,
    uint32_t psm, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height,
    const uint8_t* data, uint32_t qwc)
{
    return reg(gs, 0x50u, bitblt(bp, bw, psm)) &&
        reg(gs, 0x51u, trxpos(x, y)) &&
        reg(gs, 0x52u, trxreg(width, height)) &&
        reg(gs, 0x53u, 0u) &&
        image(gs, data, qwc);
}

void set32(uint8_t* data, uint32_t index, uint32_t value)
{
    const uint32_t at = index * 4u;
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        data[at + i] = static_cast<uint8_t>(value >> (8u * i));
    }
}

void set16(uint8_t* data, uint32_t index, uint16_t value)
{
    data[index * 2u] = static_cast<uint8_t>(value);
    data[index * 2u + 1u] = static_cast<uint8_t>(value >> 8u);
}

bool entry(const ee::c_ee_gs_state& gs, uint32_t slot, uint32_t expected)
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
} // namespace

int main()
{
    ee::c_ee_gs_state gs;
    uint32_t missing = 0u;
    if (gs.read_clut_entry(0u, missing) ||
        gs.read_clut_entry(512u, missing) ||
        gs.read_indexed_clut_value(0u, 0u, 0u, missing) ||
        gs.read_indexed_clut_value(2u, 0u, 0u, missing))
    {
        return fail("initial CLUT unexpectedly contains loaded entries");
    }

    // CSM1 PSMT8: upload 16x16 PSMCT32 palette words in raster order.
    // CLUT indices 8 and 16 swap their bits 3 and 4 before VRAM lookup.
    constexpr uint32_t k_cbp32 = 0x100u;
    constexpr uint32_t k_tbp8 = 0x500u;
    std::vector<uint8_t> colors(64u * 16u, 0u);
    for (uint32_t i = 0u; i < 256u; ++i)
    {
        set32(colors.data(), i, 0xa5000000u | i);
    }
    std::array<uint8_t, 16> indices8{};
    indices8[0] = 8u;
    indices8[1] = 16u;
    indices8[2] = 31u;
    if (!upload(gs, k_cbp32, 1u, 0u, 0u, 0u,
                16u, 16u, colors.data(), 64u) ||
        !upload(gs, k_tbp8, 1u, 0x13u, 0u, 0u,
                3u, 1u, indices8.data(), 1u))
    {
        return fail("could not upload CSM1 palette and indexed texels");
    }

    const uint64_t csm1_8 = tex0(
        k_tbp8, 1u, 0x13u, k_cbp32, 0u, 0u, 0u, 1u
    );
    if (!reg(gs, 0x06u, tex0(
            k_tbp8, 1u, 0x13u, k_cbp32, 0u, 0u, 0u, 0u)) ||
        gs.state().m_clut_load_count != 0u ||
        gs.read_clut_entry(8u, missing) ||
        !reg(gs, 0x06u, csm1_8) ||
        gs.state().m_clut_load_count != 1u ||
        !entry(gs, 0u, 0xa5000000u) ||
        !entry(gs, 8u, 0xa5000010u) ||
        !entry(gs, 16u, 0xa5000008u) ||
        !entry(gs, 31u, 0xa500001fu) ||
        !indexed(gs, 0u, 0u, 0u, 0xa5000010u) ||
        !indexed(gs, 0u, 1u, 0u, 0xa5000008u) ||
        !indexed(gs, 0u, 2u, 0u, 0xa500001fu))
    {
        return fail("CSM1 PSMT8 256-entry bit permutation or TEX0 CLD");
    }

    // The CLUT snapshot remains unchanged after VRAM writes until
    // another qualifying TEX0/TEX2 load. CLD2 remembers CBP0 and
    // CLD4 skips same-CBP loads; CLD3/5 do likewise for CBP1.
    std::array<uint8_t, 16> revised{};
    set32(revised.data(), 0u, 0xdeadbeefu);
    if (!upload(gs, k_cbp32, 1u, 0u, 0u, 1u,
                1u, 1u, revised.data(), 1u) ||
        !entry(gs, 8u, 0xa5000010u) ||
        !reg(gs, 0x06u, tex0(
            k_tbp8, 1u, 0x13u, k_cbp32, 0u, 0u, 0u, 0u)) ||
        !entry(gs, 8u, 0xa5000010u) ||
        !reg(gs, 0x06u, tex0(
            k_tbp8, 1u, 0x13u, k_cbp32, 0u, 0u, 0u, 2u)) ||
        gs.state().m_clut_cbp0 != k_cbp32 ||
        !entry(gs, 8u, 0xdeadbeefu) ||
        gs.state().m_clut_load_count != 2u)
    {
        return fail("TEX0 snapshot, CLD0 or CLD2 handling");
    }

    set32(revised.data(), 0u, 0x12345678u);
    if (!upload(gs, k_cbp32, 1u, 0u, 0u, 1u,
                1u, 1u, revised.data(), 1u) ||
        !reg(gs, 0x06u, tex0(
            k_tbp8, 1u, 0x13u, k_cbp32, 0u, 0u, 0u, 4u)) ||
        !entry(gs, 8u, 0xdeadbeefu) ||
        gs.state().m_clut_load_count != 2u ||
        !reg(gs, 0x06u, csm1_8) ||
        !entry(gs, 8u, 0x12345678u) ||
        gs.state().m_clut_load_count != 3u ||
        !reg(gs, 0x06u, tex0(
            k_tbp8, 1u, 0x13u, k_cbp32, 0u, 0u, 0u, 3u)) ||
        gs.state().m_clut_cbp1 != k_cbp32 ||
        gs.state().m_clut_load_count != 4u ||
        !reg(gs, 0x06u, tex0(
            k_tbp8, 1u, 0x13u, k_cbp32, 0u, 0u, 0u, 5u)) ||
        gs.state().m_clut_load_count != 4u ||
        !reg(gs, 0x06u, tex0(
            k_tbp8, 1u, 0x13u, k_cbp32, 0u, 0u, 0u, 7u)) ||
        gs.state().m_clut_load_count != 4u)
    {
        return fail("CLD4/5 cache slots, forced CLD1 or CLD7");
    }

    // CSM1 PSMT4 with PSMCT16S palette storage: 8x2 palette words.
    // CSA=3 maps the sixteen loaded entries to CLUT slots 48..63.
    constexpr uint32_t k_cbp16s = 0x200u;
    constexpr uint32_t k_tbp4 = 0x600u;
    std::array<uint8_t, 32> colors16{};
    for (uint32_t i = 0u; i < 16u; ++i)
    {
        set16(colors16.data(), i, static_cast<uint16_t>(0x4000u + i));
    }
    std::array<uint8_t, 16> index4{};
    index4[0] = 0x0du;
    if (!upload(gs, k_cbp16s, 1u, 0x0au, 0u, 0u,
                8u, 2u, colors16.data(), 2u) ||
        !upload(gs, k_tbp4, 1u, 0x14u, 0u, 0u,
                1u, 1u, index4.data(), 1u) ||
        !reg(gs, 0x06u, tex0(
            k_tbp4, 1u, 0x14u, k_cbp16s, 0x0au, 0u, 3u, 1u)) ||
        !entry(gs, 48u, 0x4000u) ||
        !entry(gs, 61u, 0x400du) ||
        !entry(gs, 63u, 0x400fu) ||
        !indexed(gs, 0u, 0u, 0u, 0x400du))
    {
        return fail("CSM1 PSMT4 CSA offset or raw 16S palette words");
    }

    // CSM2 uses TEXCLUT's CBW/COU/COV rather than a CSM1 bit swap.
    // CBW=0 is allowed by the reference's min-one-page stride.
    constexpr uint32_t k_cbp_csm2 = 0x300u;
    std::array<uint8_t, 64> linear{};
    for (uint32_t i = 0u; i < 16u; ++i)
    {
        set32(linear.data(), i, 0xc0000000u + i);
    }
    if (!upload(gs, k_cbp_csm2, 1u, 0u, 32u, 4u,
                16u, 1u, linear.data(), 4u) ||
        !reg(gs, 0x1cu, texclut(0u, 2u, 4u)) ||
        !reg(gs, 0x06u, tex0(
            k_tbp4, 1u, 0x14u, k_cbp_csm2, 0u, 1u, 4u, 1u)) ||
        !entry(gs, 64u, 0xc0000000u) ||
        !entry(gs, 77u, 0xc000000du) ||
        !entry(gs, 79u, 0xc000000fu) ||
        !indexed(gs, 0u, 0u, 0u, 0xc000000du))
    {
        return fail("CSM2 PSMCT32 TEXCLUT/CSA addressing");
    }

    // PSMCT16 CSM2 uses the ordinary (not 16S) halfword swizzle.
    constexpr uint32_t k_cbp_csm2_16 = 0x340u;
    std::array<uint8_t, 16> single{};
    set16(single.data(), 0u, 0x72a5u);
    if (!upload(gs, k_cbp_csm2_16, 1u, 2u, 16u, 2u,
                1u, 1u, single.data(), 1u) ||
        !reg(gs, 0x1cu, texclut(1u, 1u, 2u)) ||
        !reg(gs, 0x06u, tex0(
            k_tbp4, 1u, 0x14u, k_cbp_csm2_16, 2u, 1u, 6u, 1u)) ||
        !entry(gs, 96u, 0x72a5u))
    {
        return fail("CSM2 PSMCT16 palette storage");
    }

    // CSM2 PSMT8 + PSMCT16S: COU=1, COV=7, CBW=6, 256 halfword
    // palette entries across 64-pixel page boundaries. This path
    // must not apply the CSM1 bit-3/4 index permutation.
    constexpr uint32_t k_cbp_csm2_16s = 0x380u;
    std::vector<uint8_t> linear16s(32u * 16u, 0u);
    for (uint32_t i = 0u; i < 256u; ++i)
    {
        set16(linear16s.data(), i, static_cast<uint16_t>(0x1100u + i));
    }
    if (!upload(gs, k_cbp_csm2_16s, 6u, 0x0au, 16u, 7u,
                256u, 1u, linear16s.data(), 32u) ||
        !reg(gs, 0x1cu, texclut(6u, 1u, 7u)) ||
        !reg(gs, 0x06u, tex0(
            k_tbp8, 1u, 0x13u, k_cbp_csm2_16s, 0x0au, 1u, 0u, 1u)) ||
        !entry(gs, 0u, 0x1100u) ||
        !entry(gs, 8u, 0x1108u) ||
        !entry(gs, 16u, 0x1110u) ||
        !entry(gs, 255u, 0x11ffu) ||
        !indexed(gs, 0u, 0u, 0u, 0x1108u))
    {
        return fail("CSM2 PSMT8 256-entry page stride or 16S swizzle");
    }

    // TEX2_2 can load CLUT for the other texture context and keeps
    // its existing TBP/TBW bits after the masked TEX0 merge.
    if (!reg(gs, 0x07u, tex0(
            k_tbp4, 1u, 0x14u, k_cbp16s, 0x0au, 0u, 3u, 0u)) ||
        !reg(gs, 0x17u, tex0(
            0u, 0u, 0x14u, k_cbp16s, 0x0au, 0u, 3u, 1u)) ||
        !indexed(gs, 1u, 0u, 0u, 0x400du) ||
        gs.state().m_clut_load_count != 9u)
    {
        return fail("TEX2_2 CLD load or context-2 palette lookup");
    }

    // Palette loading within an event batch must read IMAGE bytes
    // already staged at the TEX0 write. A later invalid event must
    // roll back *both* the staged VRAM and the CLUT snapshot.
    if (!reg(gs, 0x50u, bitblt(k_cbp32, 1u, 0u)) ||
        !reg(gs, 0x51u, trxpos(0u, 1u)) ||
        !reg(gs, 0x52u, trxreg(1u, 1u)) ||
        !reg(gs, 0x53u, 0u))
    {
        return fail("could not begin same-batch palette IMAGE");
    }
    std::array<uint8_t, 48> packet{};
    set32(packet.data(), 0u, 0xf00dbabeu);
    ee::s_ee_gif_event events[3]{};
    events[0].m_type = ee::e_ee_gif_event_type::image;
    events[0].m_qwc = 1u;
    events[1].m_type = ee::e_ee_gif_event_type::packed;
    events[1].m_register = 0xeu;
    events[1].m_byte_offset = 16u;
    events[1].m_word0 = csm1_8;
    events[1].m_word1 = 0x06u;
    events[2].m_type = ee::e_ee_gif_event_type::packed;
    events[2].m_register = 0xeu;
    events[2].m_byte_offset = 32u;
    events[2].m_word1 = 0x60u; // SIGNAL not implemented
    uint32_t previous_vram = 0u;
    if (!gs.read_vram32(k_cbp32 * 256u + 8u, previous_vram))
    {
        return fail("unable to inspect CSM1 palette VRAM");
    }
    const uint64_t previous_load_count = gs.state().m_clut_load_count;
    if (gs.consume(events, 3u, packet.data(), 3u) ||
        gs.state().m_clut_load_count != previous_load_count ||
        gs.state().m_transfer_pixels_written != 0u ||
        !gs.read_vram32(k_cbp32 * 256u + 8u, missing) ||
        missing != previous_vram ||
        !entry(gs, 8u, 0x1108u) ||
        !gs.consume(events, 2u, packet.data(), 2u) ||
        !entry(gs, 8u, 0xf00dbabeu) ||
        gs.state().m_transfer_pixels_written != 1u ||
        gs.state().m_clut_load_count != previous_load_count + 1u)
    {
        return fail("atomic IMAGE-to-CLUT overlay and late-event rollback");
    }

    // Unsupported CPSM cannot be silently treated as PSMCT32;
    // CLD=1 rejection leaves both TEX0 state and CLUT unchanged.
    const uint64_t retained_tex0 = gs.register_value(0x06u);
    if (reg(gs, 0x06u, tex0(
            k_tbp8, 1u, 0x13u, k_cbp32, 1u, 0u, 0u, 1u)) ||
        gs.register_value(0x06u) != retained_tex0 ||
        !entry(gs, 8u, 0xf00dbabeu))
    {
        return fail("unsupported palette storage modified GS state");
    }

    // One transaction repeatedly reads the same palette, interleaved
    // with full-word and nibble writes, then uses it as a local-copy
    // source. Later writes must not retroactively change earlier CLUT
    // slots. A failed transaction must discard the shared shadow.
    constexpr uint32_t k_copy_cbp = 0x180u;
    std::vector<ee::s_ee_gif_event> interleaved;
    std::vector<uint8_t> payload;
    const auto push_register = [&](uint32_t index, uint64_t value)
    {
        ee::s_ee_gif_event event{};
        event.m_type = ee::e_ee_gif_event_type::packed;
        event.m_register = 0xeu;
        event.m_byte_offset = static_cast<uint32_t>(payload.size());
        event.m_word0 = value;
        event.m_word1 = index;
        interleaved.push_back(event);
        payload.resize(payload.size() + 16u);
    };
    const auto push_upload = [&](uint32_t psm, uint32_t value)
    {
        push_register(0x50u, bitblt(k_cbp32, 1u, psm));
        push_register(0x51u, trxpos(0u, 1u));
        push_register(0x52u, trxreg(1u, 1u));
        push_register(0x53u, 0u);
        ee::s_ee_gif_event event{};
        event.m_type = ee::e_ee_gif_event_type::image;
        event.m_byte_offset = static_cast<uint32_t>(payload.size());
        event.m_qwc = 1u;
        interleaved.push_back(event);
        payload.resize(payload.size() + 16u);
        set32(payload.data() + event.m_byte_offset, 0u, value);
    };
    const auto push_clut = [&](uint32_t cbp, uint32_t csa)
    {
        push_register(0x06u, tex0(
            k_tbp4, 1u, 0x14u, cbp, 0u, 0u, csa, 1u));
    };
    push_clut(k_cbp32, 0u);
    push_upload(0u, 0x11223344u);
    push_clut(k_cbp32, 1u);
    push_upload(0x24u, 0x0au); // PSMT4HL updates the low alpha nibble
    push_clut(k_cbp32, 2u);
    push_register(0x50u, bitblt(k_copy_cbp, 1u, 0u) |
        k_cbp32 | (1ull << 16u));
    push_register(0x51u, trxpos(0u, 1u) | (1ull << 16u));
    push_register(0x52u, trxreg(1u, 1u));
    push_register(0x53u, 2u);
    push_clut(k_copy_cbp, 3u);
    push_upload(0u, 0x55667788u);
    const uint64_t before_interleaved = gs.state().m_clut_load_count;
    const size_t accepted_count = interleaved.size();
    push_register(0x60u, 0u);
    if (gs.consume(interleaved.data(), interleaved.size(), payload.data(),
            static_cast<uint32_t>(payload.size() / 16u)) ||
        gs.state().m_clut_load_count != before_interleaved ||
        !entry(gs, 8u, 0xf00dbabeu) ||
        !gs.read_vram32(k_cbp32 * 256u + 8u, missing) ||
        missing != 0xf00dbabeu ||
        !gs.read_vram32(k_copy_cbp * 256u + 8u, missing) ||
        missing != 0u ||
        !gs.consume(interleaved.data(), accepted_count, payload.data(),
            static_cast<uint32_t>(accepted_count)) ||
        gs.state().m_clut_load_count != before_interleaved + 4u ||
        !entry(gs, 8u, 0xf00dbabeu) ||
        !entry(gs, 24u, 0x11223344u) ||
        !entry(gs, 40u, 0x1a223344u) ||
        !entry(gs, 56u, 0x1a223344u) ||
        !gs.read_vram32(k_cbp32 * 256u + 8u, missing) ||
        missing != 0x55667788u ||
        !gs.read_vram32(k_copy_cbp * 256u + 8u, missing) ||
        missing != 0x1a223344u)
    {
        return fail("incremental CLUT/local-copy view, nibble ordering or rollback");
    }

    // Exercise every CSM1 table entry for both palette sizes and all
    // three storage formats, including wrap at the final VRAM block.
    // Upload independent raster values and verify the expected index
    // permutation and CSA slot selection for every entry.
    for (uint32_t cpsm : {0u, 2u, 0x0au})
    {
        for (uint32_t count : {16u, 256u})
        {
            ee::c_ee_gs_state palette;
            constexpr uint32_t cbp = 0x3fffu;
            const uint32_t bytes = cpsm == 0u ? 4u : 2u;
            std::vector<uint8_t> data(count * bytes);
            for (uint32_t i = 0u; i < count; ++i)
            {
                if (bytes == 4u)
                {
                    set32(data.data(), i, 0x98760000u | i);
                }
                else
                {
                    set16(data.data(), i, static_cast<uint16_t>(0x7600u | i));
                }
            }
            const bool eight = count == 256u;
            if (!upload(palette, cbp, 1u, cpsm, 0u, 0u,
                        eight ? 16u : 8u, eight ? 16u : 2u,
                        data.data(), static_cast<uint32_t>(data.size() / 16u)) ||
                !reg(palette, 0x06u, tex0(0u, 1u, eight ? 0x13u : 0x14u,
                    cbp, cpsm, 0u, 31u, 1u)))
            {
                return fail("wrapped CSM1 palette fixture failed");
            }
            for (uint32_t i = 0u; i < count; ++i)
            {
                const uint32_t j = eight ? (i & ~0x18u) |
                    ((i & 8u) << 1u) | ((i & 16u) >> 1u) : i;
                const uint32_t slot = eight ? i : 240u + i;
                if (!entry(palette, slot,
                        (bytes == 4u ? 0x98760000u : 0x7600u) | j))
                {
                    return fail("wrapped CSM1 palette entry mismatch");
                }
            }
        }
    }

    gs.reset();
    if (gs.read_clut_entry(8u, missing) ||
        gs.state().m_clut_load_count != 0u ||
        gs.state().m_clut_cbp0 != 0xffffffffu ||
        gs.state().m_clut_cbp1 != 0xffffffffu ||
        gs.read_indexed_clut_value(0u, 0u, 0u, missing))
    {
        return fail("reset did not clear CLUT state or cached CBPs");
    }

    std::puts("GS CLUT CSM1/CSM2, CLD, TEX2 and atomic snapshot: PASS");
    return 0;
}
