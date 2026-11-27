// VU1 register dependency timing adapted from ps2-web-recomp:SDBZ
// runtime/include/ps2/vu_ops.hpp (Samir Nuri, 2026, MIT).
// License: src/runtime/ps2_webgpu/gs/LICENSE.upstream
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace ee
{
struct s_ee_vu1_use
{
    std::array<uint8_t, 4> m_reg{};
    std::array<uint8_t, 4> m_mask{};
    std::array<uint8_t, 2> m_wreg{};
    std::array<uint8_t, 2> m_wmask{};
    uint32_t m_n = 0u;
    uint32_t m_wn = 0u;
    uint16_t m_vi_reads = 0u;
    uint32_t m_vi_load = 0u;

    // Expand the decoded masks once, when populating the microcode
    // cache. Each VF lane is encoded as register * 4 + lane.
    std::array<uint8_t, 16> m_read_lanes{};
    std::array<uint8_t, 8> m_write_lanes{};
    std::array<uint8_t, 15> m_vi_indices{};
    uint8_t m_read_lane_count = 0u;
    uint8_t m_write_lane_count = 0u;
    uint8_t m_vi_index_count = 0u;

    constexpr void read_vi(uint32_t register_index)
    {
        register_index &= 15u;
        if (register_index != 0u)
        {
            const uint16_t bit = static_cast<uint16_t>(1u << register_index);
            if ((m_vi_reads & bit) == 0u)
            {
                m_vi_indices[m_vi_index_count++] =
                    static_cast<uint8_t>(register_index);
                m_vi_reads |= bit;
            }
        }
    }

    constexpr void read(uint32_t register_index, uint32_t mask)
    {
        if (register_index != 0u && mask != 0u && m_n < m_reg.size())
        {
            m_reg[m_n] = static_cast<uint8_t>(register_index);
            m_mask[m_n++] = static_cast<uint8_t>(mask);
            for (uint32_t lane = 0u; lane < 4u; ++lane)
            {
                if ((mask & (8u >> lane)) != 0u)
                {
                    m_read_lanes[m_read_lane_count++] =
                        static_cast<uint8_t>(register_index * 4u + lane);
                }
            }
        }
    }

    constexpr void write(uint32_t register_index, uint32_t mask)
    {
        if (register_index != 0u && mask != 0u && m_wn < m_wreg.size())
        {
            m_wreg[m_wn] = static_cast<uint8_t>(register_index);
            m_wmask[m_wn++] = static_cast<uint8_t>(mask);
            for (uint32_t lane = 0u; lane < 4u; ++lane)
            {
                if ((mask & (8u >> lane)) != 0u)
                {
                    m_write_lanes[m_write_lane_count++] =
                        static_cast<uint8_t>(register_index * 4u + lane);
                }
            }
        }
    }
};

struct s_ee_vu1_dependency
{
    std::array<std::array<uint32_t, 4>, 32> m_vf_ready{};
    std::array<uint32_t, 16> m_vi_ready{};

    static constexpr s_ee_vu1_use decode(uint32_t lo, uint32_t hi)
    {
        s_ee_vu1_use result{};
        const uint32_t dest = (hi >> 21u) & 15u;
        const uint32_t ft = (hi >> 16u) & 31u;
        const uint32_t fs = (hi >> 11u) & 31u;
        const uint32_t fd = (hi >> 6u) & 31u;
        const uint32_t bc = 8u >> (hi & 3u);
        const uint32_t funct = hi & 63u;

        if (funct < 0x3cu)
        {
            if (funct < 0x1cu)
            {
                result.read(fs, dest);
                result.read(ft, bc);
            }
            else if (funct < 0x28u)
            {
                result.read(fs, dest);
            }
            else if (funct == 0x2eu)
            {
                result.read(fs, 0xeu);
                result.read(ft, 0xeu);
            }
            else
            {
                result.read(fs, dest);
                result.read(ft, dest);
            }
            result.write(fd, funct == 0x2eu ? 0xeu : dest);
        }
        else
        {
            const uint32_t index = (((hi >> 6u) & 31u) << 2u) |
                (funct & 3u);
            if (index < 0x10u || (index >= 0x18u && index < 0x1cu))
            {
                result.read(fs, dest);
                result.read(ft, bc);
            }
            else if (index < 0x18u || index == 0x1du)
            {
                result.read(fs, dest);
                result.write(ft, dest);
            }
            else if (index == 0x1fu)
            {
                result.read(fs, 0xeu);
                result.read(ft, 1u);
            }
            else if (index < 0x28u)
            {
                result.read(fs, dest);
            }
            else if (index == 0x2eu)
            {
                result.read(fs, 0xeu);
                result.read(ft, 0xeu);
            }
            else if (index < 0x2fu)
            {
                result.read(fs, dest);
                result.read(ft, dest);
            }
        }

        if ((hi & 0x80000000u) != 0u)
        {
            return result;
        }

        const uint32_t ldest = (lo >> 21u) & 15u;
        const uint32_t lft = (lo >> 16u) & 31u;
        const uint32_t lfs = (lo >> 11u) & 31u;
        const uint32_t fsf = 8u >> ((lo >> 21u) & 3u);
        const uint32_t ftf = 8u >> ((lo >> 23u) & 3u);
        const uint32_t it = (lo >> 16u) & 15u;
        const uint32_t is = (lo >> 11u) & 15u;
        const uint32_t major = lo >> 25u;

        if (major == 0x00u || major == 0x08u || major == 0x09u ||
            major == 0x18u || major == 0x1au || major == 0x1bu ||
            major == 0x24u || major == 0x25u ||
            (major >= 0x2cu && major <= 0x2fu))
        {
            result.read_vi(is);
        }
        if (major == 0x01u)
        {
            result.read_vi(it);
        }
        if (major == 0x04u || major == 0x05u ||
            major == 0x28u || major == 0x29u)
        {
            result.read_vi(is);
        }
        if (major == 0x05u || major == 0x28u || major == 0x29u)
        {
            result.read_vi(it);
        }
        if (major == 0x04u)
        {
            result.m_vi_load = it;
        }
        if (major == 0x40u && (lo & 0x3cu) != 0x3cu)
        {
            const uint32_t function = lo & 63u;
            if (function == 0x30u || function == 0x31u ||
                function == 0x32u || function == 0x34u || function == 0x35u)
            {
                result.read_vi(is);
            }
            if (function == 0x30u || function == 0x31u ||
                function == 0x34u || function == 0x35u)
            {
                result.read_vi(it);
            }
        }
        if (major == 0x40u && (lo & 0x3cu) == 0x3cu)
        {
            const uint32_t index = (((lo >> 6u) & 31u) << 2u) |
                (lo & 3u);
            if (index == 0x34u || index == 0x36u ||
                index == 0x3du || index == 0x3eu ||
                index == 0x3fu || index == 0x6cu)
            {
                result.read_vi(is);
            }
            if (index == 0x35u || index == 0x37u || index == 0x3fu)
            {
                result.read_vi(it);
            }
            if (index == 0x3eu)
            {
                result.m_vi_load = it;
            }
        }

        if (major == 0x00u)
        {
            result.write(lft, ldest);
        }
        else if (major == 0x01u)
        {
            result.read(lfs, ldest);
        }
        else if (major == 0x40u && (lo & 0x3cu) == 0x3cu)
        {
            const uint32_t index = (((lo >> 6u) & 31u) << 2u) |
                (lo & 3u);
            switch (index)
            {
            case 0x30u:
                result.read(lfs, ldest);
                result.write(lft, ldest);
                break;
            case 0x31u:
                result.read(lfs, ((ldest >> 1u) | (ldest << 3u)) & 15u);
                result.write(lft, ldest);
                break;
            case 0x34u:
            case 0x36u:
            case 0x3du:
            case 0x40u:
            case 0x41u:
            case 0x64u:
                result.write(lft, ldest);
                break;
            case 0x35u:
            case 0x37u:
                result.read(lfs, ldest);
                break;
            case 0x38u:
            case 0x3au:
                result.read(lfs, fsf);
                result.read(lft, ftf);
                break;
            case 0x39u:
                result.read(lft, ftf);
                break;
            case 0x3cu:
            case 0x42u:
            case 0x43u:
                result.read(lfs, fsf);
                break;
            default:
                if (index >= 0x70u)
                {
                    result.read(lfs, 15u);
                }
                break;
            }
        }
        return result;
    }

    constexpr uint32_t issue(const s_ee_vu1_use& use, uint32_t next_cycle) const
    {
        uint32_t cycle = next_cycle;
        for (uint32_t index = 0u; index < use.m_read_lane_count; ++index)
        {
            const uint32_t lane = use.m_read_lanes[index];
            cycle = std::max(cycle, m_vf_ready[lane >> 2u][lane & 3u]);
        }
        for (uint32_t index = 0u; index < use.m_vi_index_count; ++index)
        {
            cycle = std::max(cycle, m_vi_ready[use.m_vi_indices[index]]);
        }
        return cycle;
    }

    constexpr void retire(const s_ee_vu1_use& use, uint32_t cycle)
    {
        for (uint32_t index = 0u; index < use.m_write_lane_count; ++index)
        {
            const uint32_t lane = use.m_write_lanes[index];
            m_vf_ready[lane >> 2u][lane & 3u] = cycle + 4u;
        }
        if (use.m_vi_load != 0u)
        {
            m_vi_ready[use.m_vi_load] = cycle + 4u;
        }
    }
};

// One entry per aligned microinstruction address. The exact instruction
// words, not an address or a hash alone, identify cached metadata. A
// staged MPG upload, rollback, or different VIF instance can reuse a PC
// with different code; comparing both words makes that reuse safe.
// No guest state, scoreboard timestamps or execution result is cached.
class c_ee_vu1_decode_cache
{
public:
    const s_ee_vu1_use& lookup(uint32_t pc, uint32_t lo, uint32_t hi)
    {
        auto& entry = m_entries[(pc >> 3u) & 0x7ffu];
        if (!entry.m_valid || entry.m_lower != lo || entry.m_upper != hi)
        {
            entry.m_use = s_ee_vu1_dependency::decode(lo, hi);
            entry.m_lower = lo;
            entry.m_upper = hi;
            entry.m_valid = true;
        }
        return entry.m_use;
    }

private:
    struct s_entry
    {
        s_ee_vu1_use m_use{};
        uint32_t m_lower = 0u;
        uint32_t m_upper = 0u;
        bool m_valid = false;
    };

    std::array<s_entry, 2048> m_entries{};
};
} // namespace ee
