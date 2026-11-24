#include "ee_gs.h"
#include "ee_runtime_profile.h"
#include "ee_graphics_batch.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace ee
{
namespace
{
void trace_rejection(const char* function, int line)
{
    const char* dump_path = std::getenv("EE_REJECTED_PATH2_DUMP");
    if (dump_path != nullptr && *dump_path != '\0')
    {
        std::fprintf(stderr, "GS rejection in %s:%d\n", function, line);
    }
}

constexpr uint8_t k_block32[4][8] =
{
    {0, 1, 4, 5, 16, 17, 20, 21},
    {2, 3, 6, 7, 18, 19, 22, 23},
    {8, 9, 12, 13, 24, 25, 28, 29},
    {10, 11, 14, 15, 26, 27, 30, 31}
};

constexpr uint8_t k_column32[2][8] =
{
    {0, 1, 4, 5, 8, 9, 12, 13},
    {2, 3, 6, 7, 10, 11, 14, 15}
};

// Depth buffers share the color column tables, but use independent
// GS block permutations from ps2-web-recomp GS build_tables().
constexpr uint8_t k_blockz32[4][8] =
{
    {24, 25, 28, 29, 8, 9, 12, 13},
    {26, 27, 30, 31, 10, 11, 14, 15},
    {16, 17, 20, 21, 0, 1, 4, 5},
    {18, 19, 22, 23, 2, 3, 6, 7}
};

// Exact PSMCT16 block/column permutations from ps2-web-recomp.
constexpr uint8_t k_block16[8][4] =
{
    {0, 2, 8, 10},
    {1, 3, 9, 11},
    {4, 6, 12, 14},
    {5, 7, 13, 15},
    {16, 18, 24, 26},
    {17, 19, 25, 27},
    {20, 22, 28, 30},
    {21, 23, 29, 31}
};

constexpr uint8_t k_block16s[8][4] =
{
    {0, 2, 16, 18},
    {1, 3, 17, 19},
    {8, 10, 24, 26},
    {9, 11, 25, 27},
    {4, 6, 20, 22},
    {5, 7, 21, 23},
    {12, 14, 28, 30},
    {13, 15, 29, 31}
};

constexpr uint8_t k_column16[2][16] =
{
    {0, 2, 8, 10, 16, 18, 24, 26, 1, 3, 9, 11, 17, 19, 25, 27},
    {4, 6, 12, 14, 20, 22, 28, 30, 5, 7, 13, 15, 21, 23, 29, 31}
};

constexpr uint8_t k_blockz16[8][4] =
{
    {24, 26, 16, 18},
    {25, 27, 17, 19},
    {28, 30, 20, 22},
    {29, 31, 21, 23},
    {8, 10, 0, 2},
    {9, 11, 1, 3},
    {12, 14, 4, 6},
    {13, 15, 5, 7}
};

constexpr uint8_t k_blockz16s[8][4] =
{
    {24, 26, 8, 10},
    {25, 27, 9, 11},
    {16, 18, 0, 2},
    {17, 19, 1, 3},
    {28, 30, 12, 14},
    {29, 31, 13, 15},
    {20, 22, 4, 6},
    {21, 23, 5, 7}
};

// PSMT8 uses the reference's word-column permutation; each index
// selects a single byte lane within the corresponding 32-bit word.
constexpr uint8_t k_column_word[2][2][8] =
{
    {
        {0, 1, 4, 5, 8, 9, 12, 13},
        {2, 3, 6, 7, 10, 11, 14, 15}
    },
    {
        {8, 9, 12, 13, 0, 1, 4, 5},
        {10, 11, 14, 15, 2, 3, 6, 7}
    }
};

uint32_t read_le32(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8u) |
        (static_cast<uint32_t>(data[2]) << 16u) |
        (static_cast<uint32_t>(data[3]) << 24u);
}
} // namespace

uint32_t c_ee_gs_state::psmct32_address(
    uint32_t base, uint32_t width, uint32_t x, uint32_t y)
{
    // The reference maps PSMCT32 at 64x32 pixels per 8192-byte page.
    const uint64_t page =
        (static_cast<uint64_t>(x / 64u) +
         static_cast<uint64_t>(y / 32u) * width) * 8192u;
    const uint32_t block = k_block32[(y & 31u) / 8u][(x & 63u) / 8u];
    const uint32_t column = ((y & 7u) / 2u) * 64u +
        static_cast<uint32_t>(k_column32[y & 1u][x & 7u]) * 4u;
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(base) * 256u + page +
         block * 256u + column) & (k_vram_bytes - 1u)
    );
}

uint32_t c_ee_gs_state::psmct16_address(
    uint32_t base, uint32_t width, uint32_t x, uint32_t y,
    uint32_t psm)
{
    // 64x64 pixels per 8192-byte page, DBW in 64-pixel units.
    const uint64_t page =
        (static_cast<uint64_t>(x / 64u) +
         static_cast<uint64_t>(y / 64u) * width) * 8192u;
    const uint32_t block = psm == 0x0au ?
        k_block16s[(y & 63u) / 8u][(x & 63u) / 16u] :
        k_block16[(y & 63u) / 8u][(x & 63u) / 16u];
    const uint32_t column = ((y & 7u) / 2u) * 64u +
        static_cast<uint32_t>(k_column16[y & 1u][x & 15u]) * 2u;
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(base) * 256u + page +
         block * 256u + column) & (k_vram_bytes - 1u)
    );
}

uint32_t c_ee_gs_state::psmz32_address(
    uint32_t base, uint32_t width, uint32_t x, uint32_t y)
{
    // 64x32 pages, 8x8 blocks, shared 32-bit column table.
    const uint64_t page =
        (static_cast<uint64_t>(x / 64u) +
         static_cast<uint64_t>(y / 32u) * width) * 8192u;
    const uint32_t block = k_blockz32[(y & 31u) / 8u][(x & 63u) / 8u];
    const uint32_t column = ((y & 7u) / 2u) * 64u +
        static_cast<uint32_t>(k_column32[y & 1u][x & 7u]) * 4u;
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(base) * 256u + page +
         block * 256u + column) & (k_vram_bytes - 1u)
    );
}

uint32_t c_ee_gs_state::psmz16_address(
    uint32_t base, uint32_t width, uint32_t x, uint32_t y,
    uint32_t psm)
{
    // 64x64 pages, 16x8 blocks, shared 16-bit column table.
    const uint64_t page =
        (static_cast<uint64_t>(x / 64u) +
         static_cast<uint64_t>(y / 64u) * width) * 8192u;
    const uint32_t block = psm == 0x3au ?
        k_blockz16s[(y & 63u) / 8u][(x & 63u) / 16u] :
        k_blockz16[(y & 63u) / 8u][(x & 63u) / 16u];
    const uint32_t column = ((y & 7u) / 2u) * 64u +
        static_cast<uint32_t>(k_column16[y & 1u][x & 15u]) * 2u;
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(base) * 256u + page +
         block * 256u + column) & (k_vram_bytes - 1u)
    );
}

uint32_t c_ee_gs_state::psmt8_address(
    uint32_t base, uint32_t width, uint32_t x, uint32_t y)
{
    // Same 128x64 page, block, word and byte layout as the reference's
    // build_tables() / page_address() / write_pixel(PSMT8) path.
    // Its page stride truncates DBW*64/128, with a minimum of one page.
    const uint32_t pages_per_row = std::max(1u, width / 2u);
    const uint64_t page =
        (static_cast<uint64_t>(x / 128u) +
         static_cast<uint64_t>(y / 64u) * pages_per_row) * 8192u;

    const uint32_t block =
        k_block32[(y & 63u) / 16u][(x & 127u) / 16u];
    const uint32_t wx = x & 15u;
    const uint32_t inner_y = y & 15u;
    const uint32_t column = inner_y / 4u;
    const uint32_t wy = inner_y & 3u;
    const uint32_t table =
        ((wy & 2u) >> 1u) ^ ((y / 4u) & 1u);
    const uint32_t lane =
        ((wx & 8u) >> 2u) + ((wy & 2u) >> 1u);
    const uint32_t in_page =
        block * 256u + column * 64u +
        static_cast<uint32_t>(
            k_column_word[table][wy & 1u][wx & 7u]) * 4u + lane;

    return static_cast<uint32_t>(
        (static_cast<uint64_t>(base) * 256u + page + in_page) &
        (k_vram_bytes - 1u)
    );
}

uint32_t c_ee_gs_state::psmt4_nibble_address(
    uint32_t base, uint32_t width, uint32_t x, uint32_t y)
{
    // PSMT4 page_address(bp,bw,x,y,128,128) + g_off4[y&127][x&127].
    // The reference uses the normal PSMCT16 block table, NOT 16S.
    const uint32_t pages_per_row = std::max(1u, width / 2u);
    const uint64_t page =
        (static_cast<uint64_t>(x / 128u) +
         static_cast<uint64_t>(y / 128u) * pages_per_row) * 8192u;
    const uint32_t block =
        k_block16[(y & 127u) / 16u][(x & 127u) / 32u];

    const uint32_t wx = x & 31u;
    const uint32_t local_y = y & 15u;
    const uint32_t column = local_y / 4u;
    const uint32_t wy = local_y & 3u;
    const uint32_t nibble =
        ((wx & 0x18u) + ((wy & 2u) << 1u)) / 4u;
    const uint32_t table =
        ((wy & 2u) >> 1u) ^ (column & 1u);
    const uint32_t inner =
        column * 64u + block * 256u +
        static_cast<uint32_t>(
            k_column_word[table][wy & 1u][wx & 7u]) * 4u;

    const uint64_t address =
        (static_cast<uint64_t>(base) * 256u + page + inner) * 2u +
        nibble;
    return static_cast<uint32_t>(
        address & (2u * k_vram_bytes - 1u)
    );
}

void c_ee_gs_state::bind_committed_event_observer(
    void* user, t_committed_event_callback callback)
{
    m_committed_user = user;
    m_committed_callback = callback;
}

const std::vector<uint8_t>& c_ee_gs_state::vram() const
{
    return m_vram;
}

bool c_ee_gs_state::read_vram8(uint32_t offset, uint8_t& value) const
{
    if (offset >= k_vram_bytes)
    {
        trace_rejection(__func__, __LINE__);
        return false;
    }
    value = m_vram[offset];
    return true;
}

bool c_ee_gs_state::read_vram16(uint32_t offset, uint16_t& value) const
{
    if (offset > k_vram_bytes - 2u)
    {
        trace_rejection(__func__, __LINE__);
        return false;
    }
    value = static_cast<uint16_t>(
        static_cast<uint16_t>(m_vram[offset]) |
        static_cast<uint16_t>(m_vram[offset + 1u] << 8u)
    );
    return true;
}

bool c_ee_gs_state::read_vram32(uint32_t offset, uint32_t& value) const
{
    if (offset > k_vram_bytes - 4u)
    {
        trace_rejection(__func__, __LINE__);
        return false;
    }
    value = read_le32(m_vram.data() + offset);
    return true;
}

bool c_ee_gs_state::read_color_pixel(
    uint32_t psm, uint32_t base, uint32_t fbw,
    uint32_t x, uint32_t y, uint32_t& rgba
) const
{
    // The GS display circuit uses color formats only. Refuse indexed and
    // depth VRAM so a texture upload is never mislabelled a framebuffer.
    if (fbw > 63u || base > 0x3fffu ||
        x >= 2048u || y >= 2048u ||
        (psm != 0u && psm != 1u && psm != 2u && psm != 0x0au))
    {
        trace_rejection(__func__, __LINE__);
        return false;
    }
    // Match ps2-web-recomp's page_address(): a zero FBW has a minimum
    // page stride of one, not an invalid read. SDBZ writes FBW=0 to
    // DISPFB1/2 before its first VBlank. Preserve the actual register;
    // normalize only the swizzled VRAM addressing used for readback.
    const uint32_t stride = std::max(1u, fbw);
    const uint32_t raw = read_pixel(m_vram, psm, base, stride, x, y);
    uint32_t rgb = raw & 0x00ffffffu;
    if (psm == 2u || psm == 0x0au)
    {
        rgb = ((raw & 0x001fu) << 3u) |
              ((raw & 0x03e0u) << 6u) |
              ((raw & 0x7c00u) << 9u);
    }
    rgba = rgb | 0xff000000u;
    return true;
}

bool c_ee_gs_state::read_clut_entry(
    uint32_t slot, uint32_t& value) const
{
    if (slot >= m_state.m_clut.size() ||
        !m_state.m_clut_valid[slot])
    {
        trace_rejection(__func__, __LINE__);
        return false;
    }
    value = m_state.m_clut[slot];
    return true;
}

bool c_ee_gs_state::read_indexed_clut_value(
    uint32_t context, uint32_t x, uint32_t y,
    uint32_t& value) const
{
    if (context >= 2u || !m_state.m_written[0x06u + context])
    {
        trace_rejection(__func__, __LINE__);
        return false;
    }

    const uint64_t tex0 = m_state.m_registers[0x06u + context];
    const uint32_t bp = static_cast<uint32_t>(tex0 & 0x3fffu);
    const uint32_t bw = static_cast<uint32_t>((tex0 >> 14u) & 0x3fu);
    const uint32_t psm = static_cast<uint32_t>((tex0 >> 20u) & 0x3fu);
    const uint32_t csa = static_cast<uint32_t>((tex0 >> 56u) & 0x1fu);
    uint32_t slot = 0u;

    if (psm == 0x13u) // PSMT8
    {
        slot = m_vram[psmt8_address(bp, bw, x, y)];
    }
    else if (psm == 0x1bu) // PSMT8H: high byte of PSMCT32 word
    {
        const uint32_t address = psmct32_address(bp, bw, x, y);
        slot = m_vram[address + 3u];
    }
    else if (psm == 0x14u) // PSMT4
    {
        const uint32_t nibble = psmt4_nibble_address(bp, bw, x, y);
        const uint32_t index = (
            m_vram[nibble / 2u] >> ((nibble & 1u) * 4u)
        ) & 0x0fu;
        slot = (csa * 16u + index) & 511u;
    }
    else if (psm == 0x24u || psm == 0x2cu) // PSMT4HL / PSMT4HH
    {
        const uint32_t address = psmct32_address(bp, bw, x, y);
        const uint8_t upper = m_vram[address + 3u];
        const uint32_t index =
            (upper >> (psm == 0x24u ? 0u : 4u)) & 0x0fu;
        slot = (csa * 16u + index) & 511u;
    }
    else
    {
        trace_rejection(__func__, __LINE__);
        return false;
    }

    return read_clut_entry(slot, value);
}

bool c_ee_gs_state::read_image(uint8_t* data, uint32_t qwc)
{
    ee::c_runtime_profile_scope profile_scope(ee::e_runtime_profile_id::gs_read_image, qwc);
    if (data == nullptr || qwc == 0u || qwc > 0xffffu ||
        !m_state.m_written[0x53u] ||
        (m_state.m_registers[0x53u] & 3u) != 1u)
    {
        trace_rejection(__func__, __LINE__);
        return false;
    }

    const uint64_t buf = m_state.m_registers[0x50u];
    const uint64_t pos = m_state.m_registers[0x51u];
    const uint64_t reg = m_state.m_registers[0x52u];
    const uint32_t bp = static_cast<uint32_t>(buf & 0x3fffu);
    const uint32_t bw = static_cast<uint32_t>((buf >> 16u) & 0x3fu);
    const uint32_t psm = static_cast<uint32_t>((buf >> 24u) & 0x3fu);
    const uint32_t sx = static_cast<uint32_t>(pos & 0x7ffu);
    const uint32_t sy = static_cast<uint32_t>((pos >> 16u) & 0x7ffu);
    const uint32_t width = static_cast<uint32_t>(reg & 0xfffu);
    const uint32_t height = static_cast<uint32_t>((reg >> 32u) & 0xfffu);
    if (bw == 0u || !supported_psm(psm) ||
        width == 0u || height == 0u)
    {
        trace_rejection(__func__, __LINE__);
        return false;
    }

    uint32_t bits = 32u;
    switch (psm)
    {
    case 0x01u: // PSMCT24
    case 0x31u: // PSMZ24
        bits = 24u;
        break;
    case 0x02u: // PSMCT16
    case 0x0au: // PSMCT16S
    case 0x32u: // PSMZ16
    case 0x3au: // PSMZ16S
        bits = 16u;
        break;
    case 0x13u: // PSMT8
    case 0x1bu: // PSMT8H
        bits = 8u;
        break;
    case 0x14u: // PSMT4
    case 0x24u: // PSMT4HL
    case 0x2cu: // PSMT4HH
        bits = 4u;
        break;
    default:
        break;
    }

    const uint64_t total_pixels =
        static_cast<uint64_t>(width) * height;
    const uint64_t logical_bytes =
        (total_pixels * bits + 7u) / 8u;
    const uint64_t requested_bytes =
        static_cast<uint64_t>(qwc) * 16u;
    const uint64_t offset = m_state.m_download_bytes_read;
    const uint64_t next_offset = offset + requested_bytes;

    // A byte-oriented cursor naturally preserves 24-bit carry across
    // calls; 4-bit pixels always occupy low/high nibbles of each byte.
    // After the logical rectangle, all QWC padding bytes read as zero.
    for (uint64_t i = 0u; i < requested_bytes; ++i)
    {
        const uint64_t byte_offset = offset + i;
        uint8_t result = 0u;
        if (byte_offset < logical_bytes)
        {
            const uint64_t pixel = byte_offset * 8u / bits;
            const uint32_t x = (sx + static_cast<uint32_t>(
                pixel % width)) & 0x7ffu;
            const uint32_t y = (sy + static_cast<uint32_t>(
                pixel / width)) & 0x7ffu;
            const uint32_t value =
                read_pixel(m_vram, psm, bp, bw, x, y);
            if (bits == 4u)
            {
                const uint64_t second = pixel + 1u;
                uint32_t high = 0u;
                if (second < total_pixels)
                {
                    const uint32_t x2 =
                        (sx + static_cast<uint32_t>(second % width)) &
                        0x7ffu;
                    const uint32_t y2 =
                        (sy + static_cast<uint32_t>(second / width)) &
                        0x7ffu;
                    high = read_pixel(
                        m_vram, psm, bp, bw, x2, y2
                    );
                }
                result = static_cast<uint8_t>(
                    (value & 0x0fu) | ((high & 0x0fu) << 4u)
                );
            }
            else
            {
                const uint32_t byte_lane =
                    static_cast<uint32_t>(
                        byte_offset % (bits / 8u)
                    );
                result = static_cast<uint8_t>(
                    value >> (byte_lane * 8u)
                );
            }
        }
        data[static_cast<size_t>(i)] = result;
    }

    m_state.m_download_bytes_read = next_offset;
    m_state.m_download_qwc += qwc;
    m_state.m_download_active = next_offset < logical_bytes;
    return true;
}

bool c_ee_gs_state::dma_download(
    void* user, uint32_t channel, uint8_t* data, uint32_t qwc)
{
    if (user == nullptr || channel != 1u)
    {
        trace_rejection(__func__, __LINE__);
        return false;
    }
    return static_cast<c_ee_gs_state*>(user)->read_image(data, qwc);
}

void c_ee_gs_state::reset()
{
    m_state = {};
    std::fill(m_vram.begin(), m_vram.end(), 0u);
}

const s_ee_gs_state& c_ee_gs_state::state() const
{
    return m_state;
}

bool c_ee_gs_state::was_written(uint32_t reg) const
{
    return reg < m_state.m_written.size() && m_state.m_written[reg];
}

uint64_t c_ee_gs_state::register_value(uint32_t reg) const
{
    return reg < m_state.m_registers.size() ?
        m_state.m_registers[reg] : 0u;
}

const std::vector<uint8_t>& c_ee_gs_state::staged_vram_view(
    const std::vector<s_vram_write>& writes,
    std::vector<uint8_t>& shadow, size_t& applied) const
{
    // Committed VRAM is already the correct view until a transaction
    // stages its first write. Only allocate a shadow when needed.
    if (writes.empty())
    {
        return m_vram;
    }
    if (shadow.empty())
    {
        shadow = m_vram;
    }
    while (applied < writes.size())
    {
        apply_vram_write(shadow, writes[applied]);
        ++applied;
    }
    return shadow;
}

bool c_ee_gs_state::load_clut(
    s_ee_gs_state& state, uint64_t tex0,
    const std::vector<s_vram_write>& writes,
    std::vector<uint8_t>& shadow, size_t& applied) const
{
    ee::c_runtime_profile_scope profile_scope(ee::e_runtime_profile_id::gs_clut);
    const uint32_t cld = static_cast<uint32_t>((tex0 >> 61u) & 7u);
    if (cld == 0u || cld >= 6u)
    {
        return true;
    }

    const uint32_t cbp = static_cast<uint32_t>((tex0 >> 37u) & 0x3fffu);
    if ((cld == 4u && state.m_clut_cbp0 == cbp) ||
        (cld == 5u && state.m_clut_cbp1 == cbp))
    {
        return true;
    }

    const uint32_t psm = static_cast<uint32_t>((tex0 >> 20u) & 0x3fu);
    const uint32_t cpsm = static_cast<uint32_t>((tex0 >> 51u) & 0x0fu);
    const uint32_t csm = static_cast<uint32_t>((tex0 >> 55u) & 1u);
    const uint32_t csa = static_cast<uint32_t>((tex0 >> 56u) & 0x1fu);
    // A CLD-triggered load of a different indexed format or unknown
    // palette storage layout cannot yet be executed faithfully.
    if ((psm != 0x13u && psm != 0x14u &&
         psm != 0x1bu && psm != 0x24u && psm != 0x2cu) ||
        (cpsm != 0u && cpsm != 2u && cpsm != 0x0au))
    {
        trace_rejection(__func__, __LINE__);
        return false;
    }

    const bool eight = psm == 0x13u || psm == 0x1bu;
    const uint32_t entries = eight ? 256u : 16u;
    const uint32_t entry_bytes = cpsm == 0u ? 4u : 2u;
    const uint32_t texclut = static_cast<uint32_t>(
        state.m_registers[0x1cu]
    );
    const uint32_t cbw = csm ? (texclut & 0x3fu) : 1u;
    const uint32_t cou = (texclut >> 6u) & 0x3fu;
    const uint32_t cov = (texclut >> 12u) & 0x3ffu;

    // Read the exact VRAM contents visible at this TEX0/TEX2 write.
    // Both CLUT loads and local copies share one transaction-local
    // shadow, applying each staged write at most once to that view.
    const auto& vram = staged_vram_view(writes, shadow, applied);
    for (uint32_t i = 0u; i < entries; ++i)
    {
        uint32_t x = 0u;
        uint32_t y = 0u;
        if (csm != 0u) // CSM2: TEXCLUT.CBW/COU/COV
        {
            x = cou * 16u + i;
            y = cov;
        }
        else if (eight) // CSM1: swap index bits 3 and 4
        {
            const uint32_t j = (i & ~0x18u) |
                ((i & 0x08u) << 1u) | ((i & 0x10u) >> 1u);
            x = j & 15u;
            y = j >> 4u;
        }
        else // CSM1: 4-bit entries occupy an 8x2 rectangle
        {
            x = i & 7u;
            y = i >> 3u;
        }

        const uint32_t address = cpsm == 0u ?
            psmct32_address(cbp, std::max(1u, cbw), x, y) :
            psmct16_address(cbp, std::max(1u, cbw), x, y, cpsm);
        uint32_t value = 0u;
        for (uint32_t b = 0u; b < entry_bytes; ++b)
        {
            const uint32_t at = (address + b) & (k_vram_bytes - 1u);
            value |= static_cast<uint32_t>(vram[at]) << (8u * b);
        }

        const uint32_t slot = csm != 0u ?
            (csa * 16u + i) & 511u :
            eight ? i : ((csa & 15u) * 16u + i) & 255u;
        state.m_clut[slot] = value;
        state.m_clut_valid[slot] = true;
    }

    if (cld == 2u || cld == 4u)
    {
        state.m_clut_cbp0 = cbp;
    }
    else if (cld == 3u || cld == 5u)
    {
        state.m_clut_cbp1 = cbp;
    }
    ++state.m_clut_load_count;
    return true;
}

bool c_ee_gs_state::supported_psm(uint32_t psm)
{
    switch (psm)
    {
    case 0x00u: // PSMCT32
    case 0x01u: // PSMCT24
    case 0x02u: // PSMCT16
    case 0x0au: // PSMCT16S
    case 0x13u: // PSMT8
    case 0x14u: // PSMT4
    case 0x1bu: // PSMT8H
    case 0x24u: // PSMT4HL
    case 0x2cu: // PSMT4HH
    case 0x30u: // PSMZ32
    case 0x31u: // PSMZ24
    case 0x32u: // PSMZ16
    case 0x3au: // PSMZ16S
        return true;
    default:
        trace_rejection(__func__, __LINE__);
        return false;
    }
}

uint32_t c_ee_gs_state::read_pixel(
    const std::vector<uint8_t>& vram,
    uint32_t psm, uint32_t base, uint32_t width,
    uint32_t x, uint32_t y)
{
    if (psm == 0x14u)
    {
        const uint32_t nibble =
            psmt4_nibble_address(base, width, x, y);
        return (vram[nibble / 2u] >> ((nibble & 1u) * 4u)) & 0x0fu;
    }
    if (psm == 0x13u)
    {
        return vram[psmt8_address(base, width, x, y)];
    }

    uint32_t address = 0u;
    if (psm == 0x30u || psm == 0x31u)
    {
        address = psmz32_address(base, width, x, y);
    }
    else if (psm == 0x32u || psm == 0x3au)
    {
        address = psmz16_address(base, width, x, y, psm);
    }
    else if (psm == 0x02u || psm == 0x0au)
    {
        address = psmct16_address(base, width, x, y, psm);
    }
    else
    {
        address = psmct32_address(base, width, x, y);
    }

    if (psm == 0x1bu)
    {
        return vram[address + 3u];
    }
    if (psm == 0x24u || psm == 0x2cu)
    {
        return (vram[address + 3u] >>
                (psm == 0x24u ? 0u : 4u)) & 0x0fu;
    }
    if (psm == 0x02u || psm == 0x0au ||
        psm == 0x32u || psm == 0x3au)
    {
        return static_cast<uint32_t>(vram[address]) |
            (static_cast<uint32_t>(vram[address + 1u]) << 8u);
    }

    const uint32_t value = read_le32(vram.data() + address);
    return (psm == 0x01u || psm == 0x31u) ?
        (value & 0x00ffffffu) : value;
}

c_ee_gs_state::s_vram_write c_ee_gs_state::pixel_write(
    uint32_t psm, uint32_t base, uint32_t width,
    uint32_t x, uint32_t y, uint32_t value, uint8_t size)
{
    uint32_t address = 0u;
    uint8_t shift = 0u;
    if (psm == 0x14u)
    {
        const uint32_t nibble =
            psmt4_nibble_address(base, width, x, y);
        address = nibble / 2u;
        shift = (nibble & 1u) ? 4u : 0u;
    }
    else if (psm == 0x13u)
    {
        address = psmt8_address(base, width, x, y);
    }
    else if (psm == 0x1bu || psm == 0x24u || psm == 0x2cu)
    {
        address = psmct32_address(base, width, x, y) + 3u;
        shift = psm == 0x2cu ? 4u : 0u;
    }
    else if (psm == 0x30u || psm == 0x31u)
    {
        address = psmz32_address(base, width, x, y);
    }
    else if (psm == 0x32u || psm == 0x3au)
    {
        address = psmz16_address(base, width, x, y, psm);
    }
    else if (psm == 0x02u || psm == 0x0au)
    {
        address = psmct16_address(base, width, x, y, psm);
    }
    else
    {
        address = psmct32_address(base, width, x, y);
    }
    return {address, value, size, shift};
}

void c_ee_gs_state::apply_vram_write(
    std::vector<uint8_t>& vram, const s_vram_write& write)
{
    const uint32_t offset = write.offset();
    const uint32_t value = write.m_value;
    const uint32_t bytes = write.bytes();
    if (bytes == 0u)
    {
        const uint8_t mask =
            static_cast<uint8_t>(0x0fu << write.shift());
        vram[offset] = static_cast<uint8_t>(
            (vram[offset] & static_cast<uint8_t>(~mask)) |
            static_cast<uint8_t>((value & 0x0fu) << write.shift())
        );
    }
    else
    {
        for (uint32_t b = 0u; b < bytes; ++b)
        {
            const uint32_t at = (offset + b) & (k_vram_bytes - 1u);
            vram[at] = static_cast<uint8_t>(value >> (8u * b));
        }
    }
}

bool c_ee_gs_state::stage_local_copy(
    s_ee_gs_state& state, const std::vector<uint8_t>& source_vram,
    std::vector<s_vram_write>& writes)
{
    ee::c_runtime_profile_scope profile_scope(ee::e_runtime_profile_id::gs_local_copy);
    const uint64_t buf = state.m_registers[0x50u];
    const uint64_t pos = state.m_registers[0x51u];
    const uint64_t reg = state.m_registers[0x52u];

    const uint32_t sbp = static_cast<uint32_t>(buf & 0x3fffu);
    const uint32_t sbw = static_cast<uint32_t>((buf >> 16u) & 0x3fu);
    const uint32_t spsm = static_cast<uint32_t>((buf >> 24u) & 0x3fu);
    const uint32_t dbp = static_cast<uint32_t>((buf >> 32u) & 0x3fffu);
    const uint32_t dbw = static_cast<uint32_t>((buf >> 48u) & 0x3fu);
    const uint32_t dpsm = static_cast<uint32_t>((buf >> 56u) & 0x3fu);

    const uint32_t ssax = static_cast<uint32_t>(pos & 0x7ffu);
    const uint32_t ssay = static_cast<uint32_t>((pos >> 16u) & 0x7ffu);
    const uint32_t dsax = static_cast<uint32_t>((pos >> 32u) & 0x7ffu);
    const uint32_t dsay = static_cast<uint32_t>((pos >> 48u) & 0x7ffu);
    const uint32_t dir = static_cast<uint32_t>((pos >> 59u) & 3u);
    const uint32_t rrw = static_cast<uint32_t>(reg & 0xfffu);
    const uint32_t rrh = static_cast<uint32_t>((reg >> 32u) & 0xfffu);
    const uint64_t count = static_cast<uint64_t>(rrw) * rrh;
    if (sbw == 0u || dbw == 0u ||
        !supported_psm(spsm) || !supported_psm(dpsm) ||
        count == 0u || count > k_max_local_copy_pixels ||
        static_cast<uint64_t>(writes.size()) + count >
            k_max_local_copy_pixels)
    {
        trace_rejection(__func__, __LINE__);
        return false;
    }

    // The reference first reads all source pixels, then writes
    // destinations. Read directly from the frozen source view and
    // stage destination writes only: no live/overlapping source reads.
    // DIR reverses the source X/Y coordinate mapping, not destination
    // traversal order. Pixel values are copied raw, with destination
    // formats truncating or preserving lanes exactly like write_pixel().
    for (uint32_t y = 0u; y < rrh; ++y)
    {
        const uint32_t sy = (dir & 2u) != 0u ? rrh - 1u - y : y;
        for (uint32_t x = 0u; x < rrw; ++x)
        {
            const uint32_t sx = (dir & 1u) != 0u ? rrw - 1u - x : x;
            const uint32_t source_x = (ssax + sx) & 0x7ffu;
            const uint32_t source_y = (ssay + sy) & 0x7ffu;
            const uint32_t destination_x = (dsax + x) & 0x7ffu;
            const uint32_t destination_y = (dsay + y) & 0x7ffu;
            const uint32_t value = read_pixel(
                source_vram, spsm, sbp, sbw, source_x, source_y
            );
            uint8_t size = 4u;
            if (dpsm == 0x14u || dpsm == 0x24u || dpsm == 0x2cu)
            {
                size = 0u;
            }
            else if (dpsm == 0x13u || dpsm == 0x1bu)
            {
                size = 1u;
            }
            else if (dpsm == 0x02u || dpsm == 0x0au ||
                     dpsm == 0x32u || dpsm == 0x3au)
            {
                size = 2u;
            }
            else if (dpsm == 0x01u || dpsm == 0x31u)
            {
                size = 3u;
            }
            writes.push_back(pixel_write(
                dpsm, dbp, dbw, destination_x, destination_y,
                value, size
            ));
        }
    }
    ++state.m_local_copy_count;
    state.m_local_copy_pixels += count;
    return true;
}

bool c_ee_gs_state::stage_host_upload(
    s_ee_gs_state& state, std::vector<s_vram_write>& writes,
    const uint8_t* data, size_t byte_count)
{
    ee::c_runtime_profile_scope profile_scope(ee::e_runtime_profile_id::gs_host_upload, byte_count);
    if (!state.m_transfer_started ||
        !state.m_written[0x50u] ||
        !state.m_written[0x51u] ||
        !state.m_written[0x52u] ||
        !state.m_written[0x53u] ||
        (state.m_registers[0x53u] & 3u) != 0u ||
        data == nullptr || byte_count == 0u)
    {
        trace_rejection(__func__, __LINE__);
        return false;
    }

    const uint64_t buf = state.m_registers[0x50u];
    const uint64_t pos = state.m_registers[0x51u];
    const uint64_t reg_size = state.m_registers[0x52u];
    const uint32_t base =
        static_cast<uint32_t>((buf >> 32u) & 0x3fffu);
    const uint32_t width =
        static_cast<uint32_t>((buf >> 48u) & 0x3fu);
    const uint32_t psm =
        static_cast<uint32_t>((buf >> 56u) & 0x3fu);
    const uint32_t x =
        static_cast<uint32_t>((pos >> 32u) & 0x7ffu);
    const uint32_t y =
        static_cast<uint32_t>((pos >> 48u) & 0x7ffu);
    const uint32_t rrw = static_cast<uint32_t>(reg_size & 0xfffu);
    const uint32_t rrh =
        static_cast<uint32_t>((reg_size >> 32u) & 0xfffu);
    const uint32_t total_pixels = rrw * rrh;

    // Reject layout changes made after TRXDIR and malformed carries
    // rather than interpreting the payload as a different PSM.
    if (width == 0u || !supported_psm(psm) ||
        rrw == 0u || rrh == 0u ||
        state.m_transfer_pixels_written > total_pixels ||
        state.m_transfer_pending_size > 2u)
    {
        trace_rejection(__func__, __LINE__);
        return false;
    }

    const bool four_bit =
        psm == 0x14u || psm == 0x24u || psm == 0x2cu;
    const bool twenty_four_bit = psm == 0x01u || psm == 0x31u;
    if (!twenty_four_bit && state.m_transfer_pending_size != 0u)
    {
        trace_rejection(__func__, __LINE__);
        return false;
    }

    const uint8_t pixel_bytes = twenty_four_bit ? 3u :
        (psm == 0x13u || psm == 0x1bu) ? 1u :
        (psm == 0x02u || psm == 0x0au ||
         psm == 0x32u || psm == 0x3au) ? 2u : 4u;
    const uint32_t remaining =
        total_pixels - state.m_transfer_pixels_written;
    // IMAGE/HWREG payloads are bounded by consume(). Reserve only
    // completed pixels, excluding padding and an incomplete RGB24
    // carry. Geometric growth also handles multiple small chunks.
    const uint64_t available_pixels = four_bit ?
        static_cast<uint64_t>(byte_count) * 2u :
        (static_cast<uint64_t>(byte_count) +
            state.m_transfer_pending_size) / pixel_bytes;
    const size_t pixels = static_cast<size_t>(
        std::min<uint64_t>(available_pixels, remaining));
    if (pixels > writes.max_size() - writes.size())
    {
        trace_rejection(__func__, __LINE__);
        return false;
    }
    const size_t required = writes.size() + pixels;
    if (required > writes.capacity())
    {
        const size_t growth = std::min(
            writes.capacity() / 2u, writes.max_size() - writes.capacity());
        writes.reserve(std::max(required, writes.capacity() + growth));
    }

    // Reconstruct the raster cursor once per chunk. Incrementing
    // it avoids a division and remainder for every uploaded pixel,
    // including split rows and transfers wrapping at 11 bits.
    // Every supported swizzle is periodic over an 8 KiB page.
    // Precompute the exact in-page byte/nibble addresses once using
    // the reference scalar address functions. Keep the page stride
    // and 4 MiB wrap in the transfer cursor, not in the table.
    struct s_upload_layout
    {
        uint32_t m_x_shift = 0u;
        uint32_t m_y_shift = 0u;
        std::vector<uint16_t> m_offsets;
    };
    static const auto layouts = []
    {
        std::array<s_upload_layout, 64> result{};
        constexpr uint32_t formats[] = {
            0x00u, 0x01u, 0x02u, 0x0au, 0x13u, 0x14u, 0x1bu,
            0x24u, 0x2cu, 0x30u, 0x31u, 0x32u, 0x3au
        };
        for (uint32_t format : formats)
        {
            auto& layout = result[format];
            const bool indexed = format == 0x13u || format == 0x14u;
            const bool half = format == 0x02u || format == 0x0au ||
                format == 0x32u || format == 0x3au;
            layout.m_x_shift = indexed ? 7u : 6u;
            layout.m_y_shift = format == 0x14u ? 7u :
                (half || indexed) ? 6u : 5u;
            const uint32_t page_width = 1u << layout.m_x_shift;
            const uint32_t page_height = 1u << layout.m_y_shift;
            layout.m_offsets.resize(page_width * page_height);
            for (uint32_t py = 0u; py < page_height; ++py)
            {
                for (uint32_t px = 0u; px < page_width; ++px)
                {
                    const auto pixel = pixel_write(format, 0u, 2u, px, py, 0u, 0u);
                    // A page occupies 8192 bytes. One extra bit encodes
                    // a high-nibble write without expanding the table.
                    layout.m_offsets[py * page_width + px] =
                        static_cast<uint16_t>((pixel.offset() << 1u) |
                            (pixel.shift() >> 2u));
                }
            }
        }
        return result;
    }();
    const auto& layout = layouts[psm];
    const uint32_t x_mask = (1u << layout.m_x_shift) - 1u;
    const uint32_t y_mask = (1u << layout.m_y_shift) - 1u;
    const uint32_t page_stride = layout.m_x_shift == 7u ?
        std::max(1u, width / 2u) : width;
    uint32_t column = state.m_transfer_pixels_written % rrw;
    uint32_t row = state.m_transfer_pixels_written / rrw;
    uint32_t row_base = 0u;
    const uint16_t* row_offsets = nullptr;
    const auto update_row = [&]()
    {
        const uint32_t py = (y + row) & 0x7ffu;
        row_base = base * 256u + ((py >> layout.m_y_shift) * page_stride) * 8192u;
        row_offsets = layout.m_offsets.data() +
            ((py & y_mask) << layout.m_x_shift);
    };
    update_row();
    const auto stage_pixel = [&](uint32_t value, uint8_t size)
    {
        const uint32_t px = (x + column) & 0x7ffu;
        const uint32_t packed = row_offsets[px & x_mask];
        const uint32_t offset = (row_base +
            ((px >> layout.m_x_shift) * 8192u) + (packed >> 1u)) &
            (k_vram_bytes - 1u);
        writes.push_back({offset, value, size,
            static_cast<uint8_t>((packed & 1u) * 4u)});
        ++state.m_transfer_pixels_written;
        if (++column == rrw)
        {
            column = 0u;
            ++row;
            update_row();
        }
    };

    if (four_bit)
    {
        // GIF bytes store even-numbered pixels in the low nibble.
        // Raster order continues across odd-width rows without
        // adding per-row padding.
        for (size_t i = 0u; i < byte_count &&
             state.m_transfer_pixels_written < total_pixels; ++i)
        {
            stage_pixel(data[i] & 0x0fu, 0u);
            if (state.m_transfer_pixels_written < total_pixels)
            {
                stage_pixel((data[i] >> 4u) & 0x0fu, 0u);
            }
        }
    }
    else if (twenty_four_bit)
    {
        // A+D HWREG supplies eight bytes, not 16. In 24-bit modes
        // partial pixels can span HWREG, IMAGE and DMA calls.
        for (size_t i = 0u; i < byte_count &&
             state.m_transfer_pixels_written < total_pixels; ++i)
        {
            state.m_transfer_pending[
                state.m_transfer_pending_size++] = data[i];
            if (state.m_transfer_pending_size == 3u)
            {
                const uint32_t pixel =
                    static_cast<uint32_t>(state.m_transfer_pending[0]) |
                    (static_cast<uint32_t>(state.m_transfer_pending[1]) << 8u) |
                    (static_cast<uint32_t>(state.m_transfer_pending[2]) << 16u);
                stage_pixel(pixel, 3u);
                state.m_transfer_pending_size = 0u;
            }
        }
    }
    else
    {
        for (size_t i = 0u; i < pixels; ++i)
        {
            const size_t at = i * pixel_bytes;
            uint32_t value = data[at];
            if (pixel_bytes == 2u)
            {
                value |= static_cast<uint32_t>(data[at + 1u]) << 8u;
            }
            else if (pixel_bytes == 4u)
            {
                value = read_le32(data + at);
            }
            stage_pixel(value, pixel_bytes);
        }
    }
    return true;
}

void record_sprite_pair(s_ee_gs_state& state, uint64_t xyz1)
{
    const uint64_t prim = state.m_registers[0x00u];
    // PRMODECONT selects whether the primitive's own flags or PRMODE's
    // flags control rendering. The primitive type always comes from PRIM.
    const uint64_t effective =
        (state.m_registers[0x1au] & 1u) != 0u ? prim :
        (prim & 7u) | (state.m_registers[0x1bu] & ~7ull);
    const uint32_t context = static_cast<uint32_t>((effective >> 9u) & 1u);
    s_ee_gs_sprite_sample sample{};
    sample.m_index = ++state.m_total_sprite_pair_kicks;
    sample.m_prim = prim;
    sample.m_effective_prim = effective;
    sample.m_rgbaq = state.m_registers[0x01u];
    sample.m_xyz0 = state.m_pending_sprite_xyz0;
    sample.m_xyz1 = xyz1;
    sample.m_context = context;
    sample.m_xyoffset = state.m_registers[0x18u + context];
    sample.m_scissor = state.m_registers[0x40u + context];
    sample.m_frame = state.m_registers[0x4cu + context];
    sample.m_zbuf = state.m_registers[0x4eu + context];
    sample.m_test = state.m_registers[0x47u + context];
    if ((effective & (1u << 4u)) != 0u)
    {
        ++state.m_total_sprite_textured_kicks;
    }
    if ((sample.m_rgbaq & 0xffffffu) != 0u)
    {
        ++state.m_total_sprite_nonblack_rgb_kicks;
    }

    // Keep the beginning and trailing window, without allocation or
    // scanning the unbounded guest stream. consume() copies the entire
    // state transactionally; rejected DMA never commits either sample.
    const size_t last = static_cast<size_t>(
        (sample.m_index - 1u) % s_ee_gs_state::k_sprite_sample_capacity
    );
    state.m_recent_sprite_samples[last] = sample;
    if (sample.m_index <= s_ee_gs_state::k_sprite_sample_capacity)
    {
        state.m_first_sprite_samples[last] = sample;
    }
}

bool c_ee_gs_state::write_register(
    s_ee_gs_state& state, uint32_t reg, uint64_t value)
{
    // These registers have hardware side effects that a state-only
    // observer must NOT silently pretend to execute.
    if (reg >= state.m_registers.size())
    {
        trace_rejection(__func__, __LINE__);
        return false;
    }

    switch (reg)
    {
    case 0x00u: // PRIM
        state.m_vertex_count = 0u;
        state.m_pending_sprite_xyz0 = 0u;
        ++state.m_total_prim_writes;
        ++state.m_prim_type_counts[value & 7u];
        break;
    case 0x04u: // XYZF2
    case 0x05u: // XYZ2
        ++state.m_vertex_count;
        ++state.m_total_vertex_writes;
        ++state.m_total_vertex_kicks;
        if ((state.m_registers[0x00u] & 7u) == 6u)
        {
            if ((state.m_vertex_count & 1u) != 0u)
            {
                state.m_pending_sprite_xyz0 = value;
            }
            else
            {
                record_sprite_pair(state, value);
            }
        }
        break;
    case 0x0cu: // XYZF3 (ADC)
    case 0x0du: // XYZ3 (ADC)
        ++state.m_vertex_count;
        ++state.m_total_vertex_writes;
        ++state.m_total_adc_vertices;
        if ((state.m_registers[0x00u] & 7u) == 6u &&
            (state.m_vertex_count & 1u) != 0u)
        {
            state.m_pending_sprite_xyz0 = value;
        }
        break;
    case 0x01u: // RGBAQ
    case 0x02u: // ST
    case 0x03u: // UV
    case 0x06u: // TEX0_1
    case 0x07u: // TEX0_2
    case 0x08u: // CLAMP_1
    case 0x09u: // CLAMP_2
    case 0x0au: // FOG
    case 0x14u: // TEX1_1
    case 0x15u: // TEX1_2
    case 0x18u: // XYOFFSET_1
    case 0x19u: // XYOFFSET_2
    case 0x1au: // PRMODECONT
    case 0x1bu: // PRMODE
    case 0x1cu: // TEXCLUT
    case 0x22u: // SCANMSK
    case 0x34u: // MIPTBP1_1
    case 0x35u: // MIPTBP1_2
    case 0x36u: // MIPTBP2_1
    case 0x37u: // MIPTBP2_2
    case 0x3bu: // TEXA
    case 0x3du: // FOGCOL
    case 0x40u: // SCISSOR_1
    case 0x41u: // SCISSOR_2
    case 0x42u: // ALPHA_1
    case 0x43u: // ALPHA_2
    case 0x44u: // DIMX
    case 0x45u: // DTHE
    case 0x46u: // COLCLAMP
    case 0x47u: // TEST_1
    case 0x48u: // TEST_2
    case 0x49u: // PABE
    case 0x4au: // FBA_1
    case 0x4bu: // FBA_2
    case 0x4cu: // FRAME_1
    case 0x4du: // FRAME_2
    case 0x4eu: // ZBUF_1
    case 0x4fu: // ZBUF_2
    case 0x50u: // BITBLTBUF
    case 0x51u: // TRXPOS
    case 0x52u: // TRXREG
    case 0x54u: // HWREG, 8 bytes handled in consume()
        break;
    case 0x16u: // TEX2_1 modifies selected fields of TEX0_1
    case 0x17u: // TEX2_2 modifies selected fields of TEX0_2
    {
        // Exact bit mask from ps2-web-recomp/runtime/src/gs/gs.cpp.
        constexpr uint64_t k_tex2_mask = 0xffffffe003f00000ull;
        const uint32_t tex0 = 0x06u + (reg - 0x16u);
        state.m_registers[tex0] =
            (state.m_registers[tex0] & ~k_tex2_mask) |
            (value & k_tex2_mask);
        state.m_written[tex0] = true;
        break;
    }
    case 0x53u: // TRXDIR
        state.m_transfer_started = false;
        state.m_transfer_pixels_written = 0u;
        state.m_transfer_pending_size = 0u;
        state.m_download_active = false;
        state.m_download_bytes_read = 0u;
        state.m_download_qwc = 0u;
        if ((value & 3u) == 0u || (value & 3u) == 2u ||
            (value & 3u) == 1u)
        {
            // Validate the source for local-to-host downloads and
            // the destination for uploads/copies before committing.
            // A local copy executes immediately within consume().
            if (!state.m_written[0x50u] ||
                !state.m_written[0x51u] ||
                !state.m_written[0x52u])
            {
                trace_rejection(__func__, __LINE__);
                return false;
            }
            const uint64_t buf = state.m_registers[0x50u];
            const uint64_t reg_size = state.m_registers[0x52u];
            const uint32_t source_width =
                static_cast<uint32_t>((buf >> 16u) & 0x3fu);
            const uint32_t source_psm =
                static_cast<uint32_t>((buf >> 24u) & 0x3fu);
            const uint32_t dest_width =
                static_cast<uint32_t>((buf >> 48u) & 0x3fu);
            const uint32_t dest_psm =
                static_cast<uint32_t>((buf >> 56u) & 0x3fu);
            const uint32_t pixel_width =
                static_cast<uint32_t>(reg_size & 0xfffu);
            const uint32_t pixel_height =
                static_cast<uint32_t>((reg_size >> 32u) & 0xfffu);
            const bool local_copy = (value & 3u) == 2u;
            const uint64_t pixels =
                static_cast<uint64_t>(pixel_width) * pixel_height;
            const bool download = (value & 3u) == 1u;
            if (pixel_width == 0u || pixel_height == 0u ||
                (!download &&
                 (dest_width == 0u || !supported_psm(dest_psm))) ||
                ((local_copy || download) &&
                 (source_width == 0u || !supported_psm(source_psm))) ||
                (local_copy && pixels > k_max_local_copy_pixels))
            {
                trace_rejection(__func__, __LINE__);
                return false;
            }
            if ((value & 3u) == 0u)
            {
                state.m_image_data.clear();
                state.m_image_qwc = 0u;
                state.m_transfer_started = true;
            }
            else if (download)
            {
                state.m_download_active = true;
            }
        }
        break;
    case 0x3fu: // TEXFLUSH invalidates the renderer cache on GIF commit.
        return true;
    case 0x0eu: // GS ignores direct writes to these unmapped registers
    case 0x0fu:
        return true;
    default:
        // Reject SIGNAL, FINISH, LABEL and unknown
        // hardware operations; HWREG executes inside consume().
        trace_rejection(__func__, __LINE__);
        return false;
    }

    state.m_registers[reg] = value;
    state.m_written[reg] = true;
    return true;
}

bool c_ee_gs_state::write_packed(
    s_ee_gs_state& state, uint32_t reg, uint64_t low, uint64_t high)
{
    switch (reg)
    {
    case 0x00u: // PRIM
        return write_register(state, 0x00u, low & 0x7ffu);
    case 0x01u: // RGBAQ (four widened bytes plus current Q bits)
    {
        const uint64_t rgba =
            (low & 0xffu) |
            (((low >> 32u) & 0xffu) << 8u) |
            ((high & 0xffu) << 16u) |
            (((high >> 32u) & 0xffu) << 24u) |
            (static_cast<uint64_t>(state.m_q_bits) << 32u);
        return write_register(state, 0x01u, rgba);
    }
    case 0x02u: // STQ
    {
        if (!write_register(state, 0x02u, low))
        {
            trace_rejection(__func__, __LINE__);
            return false;
        }
        state.m_q_bits = static_cast<uint32_t>(high);
        return true;
    }
    case 0x03u: // UV (two masked 14-bit coordinates)
        return write_register(
            state, 0x03u,
            (low & 0x3fffu) |
            (((low >> 32u) & 0x3fffu) << 16u)
        );
    case 0x04u: // XYZF2, ADC bit selects XYZF3
    case 0x0cu: // XYZF3
    {
        const uint64_t xyzf =
            (low & 0xffffu) |
            (((low >> 32u) & 0xffffu) << 16u) |
            (((high >> 4u) & 0xffffffu) << 32u) |
            (((high >> 36u) & 0xffu) << 56u);
        const uint32_t dest = reg == 0x0cu ? 0x0cu :
            ((high >> 47u) & 1u) != 0u ? 0x0cu : 0x04u;
        return write_register(state, dest, xyzf);
    }
    case 0x05u: // XYZ2, ADC bit selects XYZ3
    case 0x0du: // XYZ3
    {
        const uint64_t xyz =
            (low & 0xffffu) |
            (((low >> 32u) & 0xffffu) << 16u) |
            ((high & 0xffffffffu) << 32u);
        const uint32_t dest = reg == 0x0du ? 0x0du :
            ((high >> 47u) & 1u) != 0u ? 0x0du : 0x05u;
        return write_register(state, dest, xyz);
    }
    case 0x0au: // FOG from W3 bits 4..11
        return write_register(
            state, 0x0au, ((high >> 36u) & 0xffu) << 56u
        );
    case 0x0eu: // A+D GS register address in low byte of high word
        return write_register(
            state, static_cast<uint32_t>(high & 0xffu), low
        );
    case 0x0fu: // NOP
        return true;
    default:
        // The remaining packed registers map directly to their GS
        // addresses in the ps2-web-recomp GS packed writer.
        return write_register(state, reg, low);
    }
}

bool c_ee_gs_state::consume(
    const s_ee_gif_event* events, size_t count,
    const uint8_t* data, uint32_t qwc)
{
    ee::c_runtime_profile_scope profile_scope(ee::e_runtime_profile_id::gs_transaction, count);
    if ((count != 0u && events == nullptr) ||
        (qwc != 0u && data == nullptr) ||
        qwc > k_max_graphics_batch_qwc)
    {
        trace_rejection(__func__, __LINE__);
        return false;
    }

    // A local snapshot makes every accepted DMA event batch atomic.
    s_ee_gs_state next = m_state;
    // Keep scratch storage across transactions, but never retain its
    // logical contents. Release exceptionally large journals to bound
    // the persistent allocation (at most 32 MiB plus the 4 MiB shadow).
    struct s_scratch_scope
    {
        std::vector<s_vram_write>& m_writes;
        std::vector<uint8_t>& m_shadow;
        bool m_active = true;

        void finish()
        {
            if (m_active)
            {
                m_writes.clear();
                m_shadow.clear();
                if (m_writes.capacity() > k_max_local_copy_pixels)
                {
                    std::vector<s_vram_write>().swap(m_writes);
                }
                m_active = false;
            }
        }

        ~s_scratch_scope()
        {
            finish();
        }
    };
    auto& writes = m_write_scratch;
    auto& shadow_vram = m_shadow_scratch;
    // Establish an empty logical transaction independently of cleanup
    // on any earlier exit. Capacity, not previous contents, is reused.
    writes.clear();
    shadow_vram.clear();
    s_scratch_scope scratch{writes, shadow_vram};
    const uint64_t bytes = static_cast<uint64_t>(qwc) * 16u;

    // Shared, lazy transactional VRAM view. CLUT loads and local
    // copies see only writes preceding the current GIF event.
    size_t shadow_applied = 0u;
    const auto copy_local = [&]() -> bool
    {
        const auto& source = staged_vram_view(writes, shadow_vram, shadow_applied);
        return stage_local_copy(next, source, writes);
    };

    // A+D HWREG writes are 8-byte host-to-local image chunks. They
    // preserve the same pending-pixel state as GIF IMAGE/IMAGE2, and
    // are staged alongside other GS writes for atomic DMA rollback.
    const auto feed_hwreg = [&](uint64_t value) -> bool
    {
        uint8_t packed[8]{};
        for (uint32_t b = 0u; b < 8u; ++b)
        {
            packed[b] = static_cast<uint8_t>(value >> (b * 8u));
        }
        if (!stage_host_upload(next, writes, packed, sizeof(packed)))
        {
            trace_rejection(__func__, __LINE__);
            return false;
        }
        ++next.m_hwreg_write_count;
        next.m_hwreg_bytes += sizeof(packed);
        return true;
    };

    // TEX0 and TEX2 load CLUT entries at the register write, not at
    // the end of the GIF batch or on every subsequent VRAM write.
    const auto refresh_clut = [&](uint32_t reg) -> bool
    {
        uint32_t tex0_reg = 0u;
        if (reg == 0x06u || reg == 0x07u)
        {
            tex0_reg = reg;
        }
        else if (reg == 0x16u || reg == 0x17u)
        {
            tex0_reg = 0x06u + (reg - 0x16u);
        }
        else
        {
            return true;
        }
        return load_clut(next, next.m_registers[tex0_reg], writes,
            shadow_vram, shadow_applied);
    };

    for (size_t i = 0; i < count; ++i)
    {
        const s_ee_gif_event& e = events[i];
        const uint64_t offset = e.m_byte_offset;
        if (offset >= bytes)
        {
            trace_rejection(__func__, __LINE__);
            return false;
        }

        switch (e.m_type)
        {
        case e_ee_gif_event_type::tag:
            if ((offset & 15u) != 0u || offset + 16u > bytes)
            {
                trace_rejection(__func__, __LINE__);
                return false;
            }
            next.m_q_bits = 0x3f800000u;
            ++next.m_tag_count;
            ++next.m_tag_format_counts[(e.m_word0 >> 58u) & 3u];
            break;
        case e_ee_gif_event_type::primitive:
            if ((offset & 15u) != 0u ||
                offset + 16u > bytes ||
                e.m_word0 > 0x7ffu ||
                !write_register(next, 0x00u, e.m_word0))
            {
                trace_rejection(__func__, __LINE__);
                return false;
            }
            break;
        case e_ee_gif_event_type::packed:
            if ((offset & 15u) != 0u ||
                offset + 16u > bytes ||
                e.m_register > 0xfu ||
                !write_packed(
                    next, e.m_register, e.m_word0, e.m_word1
                ) ||
                (e.m_register == 0x0eu &&
                 (e.m_word1 & 0xffu) == 0x53u &&
                 (e.m_word0 & 3u) == 2u &&
                 !copy_local()) ||
                (e.m_register == 0x0eu &&
                 (e.m_word1 & 0xffu) == 0x54u &&
                 !feed_hwreg(e.m_word0)) ||
                !refresh_clut(
                    e.m_register == 0x0eu ?
                    static_cast<uint32_t>(e.m_word1 & 0xffu) :
                    e.m_register
                ))
            {
                trace_rejection(__func__, __LINE__);
                return false;
            }
            ++next.m_total_packed_events;
            ++next.m_packed_register_counts[e.m_register];
            if (e.m_register == 0x0eu)
            {
                ++next.m_total_ad_events;
                const uint32_t reg = static_cast<uint32_t>(e.m_word1 & 0xffu);
                if (reg < next.m_ad_register_counts.size())
                {
                    ++next.m_ad_register_counts[reg];
                }
            }
            break;
        case e_ee_gif_event_type::reglist:
            if ((offset & 7u) != 0u ||
                offset + 8u > bytes ||
                e.m_register > 0xfu ||
                !write_register(next, e.m_register, e.m_word0) ||
                !refresh_clut(e.m_register))
            {
                trace_rejection(__func__, __LINE__);
                return false;
            }
            ++next.m_total_reglist_events;
            break;
        case e_ee_gif_event_type::image:
        {
            const uint64_t length =
                static_cast<uint64_t>(e.m_qwc) * 16u;
            // IMAGE data without a configured host-to-local transfer
            // cannot safely be claimed as received by a GS device.
            if ((offset & 15u) != 0u ||
                e.m_qwc == 0u ||
                offset + length > bytes ||
                !next.m_written[0x50u] ||
                !next.m_written[0x51u] ||
                !next.m_written[0x52u] ||
                !next.m_written[0x53u] ||
                (next.m_registers[0x53u] & 3u) != 0u ||
                !next.m_transfer_started ||
                length > k_max_image_bytes -
                    next.m_image_data.size())
            {
                trace_rejection(__func__, __LINE__);
                return false;
            }
            next.m_image_data.insert(
                next.m_image_data.end(),
                data + static_cast<size_t>(offset),
                data + static_cast<size_t>(offset + length)
            );
            next.m_image_qwc += e.m_qwc;
            ++next.m_total_image_events;
            next.m_total_image_qwc += e.m_qwc;

            if (!stage_host_upload(
                    next, writes, data + static_cast<size_t>(offset),
                    static_cast<size_t>(length)))
            {
                trace_rejection(__func__, __LINE__);
                return false;
            }
            // Padding after the final pixel is captured but not written.
            break;
        }
        default:
            trace_rejection(__func__, __LINE__);
            return false;
        }
    }

    // Commit only after every event has passed validation, including
    // TRXDIR=2. Reuse the same byte/nibble application as the frozen
    // local-copy view to keep single-batch ordering deterministic.
    for (const s_vram_write& write : writes)
    {
        apply_vram_write(m_vram, write);
    }
    if (count != 0u)
    {
        ++next.m_committed_gif_batches;
    }
    m_state = std::move(next);
    // Return scratch before invoking an external observer. A callback
    // can synchronously start another transaction on this same GS.
    scratch.finish();
    if (m_committed_callback != nullptr)
    {
        // Observer receives the already validated guest event stream.
        // It has no success/failure return into the GIF or DMAC.
        m_committed_callback(
            m_committed_user, events, count, data, qwc);
    }
    return true;
}

bool c_ee_gs_state::gif_events(
    void* user, const s_ee_gif_event* events, size_t count,
    const uint8_t* data, uint32_t qwc)
{
    if (user == nullptr)
    {
        trace_rejection(__func__, __LINE__);
        return false;
    }
    return static_cast<c_ee_gs_state*>(user)->consume(
        events, count, data, qwc
    );
}
} // namespace ee
