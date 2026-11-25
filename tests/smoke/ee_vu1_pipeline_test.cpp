#include "ee_vu1.h"
#include "ee_graphics_batch.h"
#include "ee_vu1_fp.h"
#include "ee_vu1_dependency.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
constexpr uint32_t k_lower_nop = 0x8000033cu;
constexpr uint32_t k_upper_nop = 0x000002ffu;
constexpr uint32_t k_upper_end = k_upper_nop | 0x40000000u;

struct s_pair
{
    uint32_t m_lower = k_lower_nop;
    uint32_t m_upper = k_upper_nop;
};

bool check(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "VU1 Batch C: %s\n", message);
    }
    return condition;
}

uint32_t special(uint32_t index, uint32_t it = 0u,
                 uint32_t is = 0u, uint32_t mask = 0u)
{
    return 0x80000000u | ((mask & 15u) << 21u) |
        ((it & 31u) << 16u) | ((is & 31u) << 11u) |
        ((index >> 2u) << 6u) | 0x3cu | (index & 3u);
}

uint32_t upper(uint32_t op, uint32_t fd, uint32_t fs,
               uint32_t ft, uint32_t mask)
{
    return (mask << 21u) | (ft << 16u) |
        (fs << 11u) | (fd << 6u) | op;
}

bool run(ee::s_ee_vu1_state& state, const std::vector<s_pair>& pairs,
         std::array<uint8_t, ee::c_ee_vu1::k_memory_size>& data,
         ee::s_ee_vu1_issue& issue)
{
    std::array<uint8_t, ee::c_ee_vu1::k_memory_size> code{};
    for (size_t i = 0u; i < pairs.size(); ++i)
    {
        std::memcpy(code.data() + i * 8u, &pairs[i].m_lower, 4u);
        std::memcpy(code.data() + i * 8u + 4u, &pairs[i].m_upper, 4u);
    }
    std::vector<uint8_t> path1;
    return ee::c_ee_vu1::run(
        state, code, data, 0u, 0u, 0u, path1, &issue) &&
        path1.empty();
}
} // namespace

int main()
{
    std::array<uint8_t, ee::c_ee_vu1::k_memory_size> data{};
    ee::s_ee_vu1_issue issue{};

    // MAX/MINI use raw sign-magnitude ordering, including signed zero,
    // denormals and exponent-255 words. CLIP retains its separate key.
    {
        ee::s_ee_vu1_state extrema{};
        extrema.m_initialized = true;
        extrema.m_vf[2] = {0x80000000u, 1u, 0x80000001u, 0x7f800000u};
        extrema.m_vf[3] = {0u, 2u, 0x80000002u, 0x7f800001u};
        if (!check(run(extrema, {
                       {k_lower_nop, upper(0x2bu, 4u, 2u, 3u, 15u)},
                       {k_lower_nop, upper(0x2fu, 5u, 3u, 2u, 15u)},
                       {k_lower_nop, k_upper_end},
                       {k_lower_nop, k_upper_nop}
                   }, data, issue) &&
                   extrema.m_vf[4] == std::array<uint32_t, 4>{0u, 2u, 0x80000001u, 0x7f800001u} &&
                   extrema.m_vf[5] == std::array<uint32_t, 4>{0x80000000u, 1u, 0x80000002u, 0x7f800000u},
                   "MAX/MINI raw word selection"))
        {
            return 1;
        }
    }

    // Q retains its previous value until the FDIV completes; WAITQ
    // advances the cycle clock before the next upper reads the new Q.
    ee::s_ee_vu1_state q{};
    q.m_initialized = true;
    q.m_q = 0x3f800000u; // 1.0
    q.m_vf[2][0] = 0x40c00000u; // 6.0
    q.m_vf[3][0] = 0x40000000u; // 2.0
    const uint32_t addq_4 = upper(0x20u, 4u, 2u, 0u, 8u);
    const uint32_t addq_5 = upper(0x20u, 5u, 2u, 0u, 8u);
    if (!check(run(q, {
                   {special(0x38u, 3u, 2u), k_upper_nop},
                   {k_lower_nop, addq_4},
                   {special(0x3bu), k_upper_nop},
                   {k_lower_nop, addq_5},
                   {k_lower_nop, k_upper_end},
                   {k_lower_nop, k_upper_nop}
               }, data, issue) &&
               q.m_vf[4][0] == 0x40e00000u && // 6 + old 1
               q.m_vf[5][0] == 0x41100000u && // 6 + new 3
               q.m_q == 0x40400000u &&
               issue.m_steps == 6u && issue.m_cycles == 11u,
               "DIV, Q-old/Q-new, WAITQ or latency"))
    {
        return 1;
    }

    // SQRT and RSQRT compute PS2 reference bit-exact Q, including
    // the 13-cycle RSQRT latency, while preserving an old Q before WAITQ.
    ee::s_ee_vu1_state roots{};
    roots.m_initialized = true;
    roots.m_vf[2][0] = 0x40c00000u; // 6.0
    roots.m_vf[3][0] = 0x40800000u; // 4.0
    if (!check(run(roots, {
                   {special(0x39u, 3u), k_upper_nop},
                   {special(0x3bu), k_upper_nop},
                   {special(0x3au, 3u, 2u), k_upper_nop},
                   {special(0x3bu), k_upper_end},
                   {k_lower_nop, k_upper_nop}
               }, data, issue) &&
               roots.m_q == 0x40400000u && // 6 / sqrt(4)
               issue.m_cycles >= 22u,
               "SQRT/RSQRT and Q pipeline synchronization"))
    {
        return 1;
    }

    // Directed Q cases: physical VU0 flag observations and shared
    // signed-digit arithmetic model. Native O0/O3 parity is covered by
    // the q case of ee_vu1_ir_fixture.
    struct s_q_case
    {
        uint32_t m_op;
        uint32_t m_fs;
        uint32_t m_ft;
        uint32_t m_q;
        uint32_t m_flags;
    };
    constexpr s_q_case q_cases[] =
    {
        {0x38u, 0u, 0u, 0x7fffffffu, 0x10u},
        {0x38u, 0x3f800000u, 0x80000000u, 0xffffffffu, 0x20u},
        {0x39u, 0u, 0xbf800000u, 0x3f800000u, 0x10u},
        {0x39u, 0u, 0x3f80efd3u, 0x3f8077b1u, 0u},
        {0x3au, 0xff3cf542u, 0x80000000u, 0xffffffffu, 0x30u},
        {0x3au, 0x7fffffffu, 0x7fffffffu, 0x5fb504f2u, 0u},
        {0x38u, 0x3f800000u, 0x3fffffffu, 0x3f000001u, 0u},
    };
    for (const auto& entry : q_cases)
    {
        ee::s_ee_vu1_state state{};
        state.m_initialized = true;
        state.m_vf[2][0] = entry.m_fs;
        state.m_vf[3][0] = entry.m_ft;
        state.m_status = 0x430u;
        const uint32_t expected_status = 0x400u | entry.m_flags | (entry.m_flags << 6u);
        if (!check(run(state, {
                       {special(entry.m_op, 3u, 2u), k_upper_nop},
                       {special(0x3bu), k_upper_end},
                       {k_lower_nop, k_upper_nop}
                   }, data, issue) &&
                   state.m_q == entry.m_q && state.m_status == expected_status,
                   "Q result, current I/D replacement and sticky preservation"))
        {
            return 1;
        }
    }

    // FDIV exceptions are invisible until the Q result retires.
    // FSAND must observe them without any intervening upper FMAC.
    {
        ee::s_ee_vu1_state state{};
        state.m_initialized = true;
        state.m_vf[2][0] = 0x3f800000u;
        const auto fsand = [](uint32_t it)
        {
            return (0x16u << 25u) | (it << 16u) | 0x200430u;
        };
        const uint32_t fsset_zero = 0x15u << 25u;
        if (!check(run(state, {
                       {special(0x38u, 0u, 2u), k_upper_nop}, // 1 / 0
                       {fsand(1u), k_upper_nop},
                       {special(0x3bu), k_upper_nop},
                       {fsand(2u), k_upper_nop},
                       {special(0x38u, 2u, 2u), k_upper_nop}, // 1 / 1
                       {fsand(3u), k_upper_nop},
                       {k_lower_nop, k_upper_nop},
                       {k_lower_nop, k_upper_nop},
                       {k_lower_nop, k_upper_nop},
                       {k_lower_nop, k_upper_nop},
                       {k_lower_nop, k_upper_nop},
                       {fsand(4u), k_upper_nop}, // natural retirement
                       {fsset_zero, k_upper_nop},
                       {fsand(5u), k_upper_end},
                       {k_lower_nop, k_upper_nop}
                   }, data, issue) &&
                   state.m_vi[1] == 0u && state.m_vi[2] == 0x820u &&
                   state.m_vi[3] == 0x820u && state.m_vi[4] == 0x800u &&
                   state.m_vi[5] == 0u && state.m_q == 0x3f800000u &&
                   state.m_status == 0u,
                   "FDIV status visibility before/after WAITQ, natural retirement and FSSET"))
        {
            return 1;
        }
    }

    // Result flags must distinguish an exact finite maximum from overflow;
    // cancellation and multiplication underflow both set MAC Z/U.
    {
        ee::s_ee_vu1_state state{};
        state.m_initialized = true;
        state.m_vf[2] = {0x7fffffffu, 0x00800000u, 0x3f800000u, 0u};
        state.m_vf[3] = {0x3f800000u, 0x3f000000u, 0x3fffffffu, 0u};
        if (!check(run(state, {
                       {k_lower_nop, upper(0x2au, 4u, 2u, 3u, 14u)},
                       {k_lower_nop, k_upper_end},
                       {k_lower_nop, k_upper_nop}
                   }, data, issue) &&
                   state.m_vf[4][0] == 0x7fffffffu &&
                   state.m_vf[4][1] == 0u &&
                   state.m_vf[4][2] == 0x3ffffffeu &&
                   state.m_mac == 0x0404u && state.m_status == 0x145u,
                   "FMAC true range flags and asymmetric multiplication"))
        {
            return 1;
        }
    }

    // Hardware word-level EFU checks for the shared interpreter/AOT
    // arithmetic. Exercise normal inputs, full-range exponent 255, and
    // zero denominators without host IEEE infinity or NaN conversion.
    {
        struct s_efu_case
        {
            uint32_t m_index;
            uint32_t m_latency;
            uint32_t m_regular;
            uint32_t m_max;
            uint32_t m_zero;
        };
        const std::array<uint32_t, 4> regular{
            0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u};
        const std::array<uint32_t, 4> maximum{
            0x7fffffffu, 0x7fffffffu, 0x7fffffffu, 0x7fffffffu};
        const std::array<uint32_t, 4> zero{};
        const s_efu_case cases[] =
        {
            {0x70u, 11u, 0x41600000u, 0x7fffffffu, 0u},
            {0x71u, 18u, 0x3d924925u, 0u, 0x7fffffffu},
            {0x72u, 18u, 0x406f7750u, 0x5fb504f3u, 0u},
            {0x73u, 24u, 0x3e88d677u, 0x1f3504f3u, 0x7fffffffu},
            {0x76u, 12u, 0x41200000u, 0x7fffffffu, 0u},
            {0x78u, 12u, 0x3fddb3d7u, 0x5fb504f3u, 0u},
            {0x79u, 18u, 0x3f13cd3au, 0x1f3504f3u, 0x7fffffffu},
            {0x7au, 12u, 0x3eaaaaabu, 0u, 0x7fffffffu}
        };
        for (const auto& test : cases)
        {
            uint32_t bits = 0u;
            uint32_t latency = 0u;
            if (!check(ee::c_ee_vu1::efu(regular, test.m_index, 2u, bits, latency) &&
                       bits == test.m_regular && latency == test.m_latency &&
                       ee::c_ee_vu1::efu(maximum, test.m_index, 2u, bits, latency) &&
                       bits == test.m_max && latency == test.m_latency &&
                       ee::c_ee_vu1::efu(zero, test.m_index, 2u, bits, latency) &&
                       bits == test.m_zero && latency == test.m_latency,
                       "EFU full-range FMAC/root/quotient arithmetic"))
            {
                return 1;
            }
        }
    }

    // Transcendental checks cover known hardware values, without
    // asserting that every internal polynomial step is characterized.
    {
        uint32_t bits = 0u;
        uint32_t latency = 0u;
        const std::array<uint32_t, 4> regular{
            0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u};
        const std::array<uint32_t, 4> one{
            0x3f800000u, 0x3f800000u, 0x3f800000u, 0x3f800000u};
        const std::array<uint32_t, 4> maximum{
            0x7fffffffu, 0x7fffffffu, 0x7fffffffu, 0x7fffffffu};
        const std::array<uint32_t, 4> zero{};
        if (!check(ee::c_ee_vu1::efu(one, 0x7du, 2u, bits, latency) &&
                   bits == 0x3f490fdau && latency == 54u &&
                   ee::c_ee_vu1::efu(regular, 0x74u, 2u, bits, latency) &&
                   bits == 0x3f8db70bu && latency == 54u &&
                   ee::c_ee_vu1::efu(regular, 0x75u, 2u, bits, latency) &&
                   bits == 0x3f9fe0bau && latency == 54u &&
                   ee::c_ee_vu1::efu(one, 0x7cu, 2u, bits, latency) &&
                   bits == 0x3f576aa4u && latency == 29u &&
                   ee::c_ee_vu1::efu(maximum, 0x7cu, 2u, bits, latency) &&
                   bits == 0x7fd76aa2u && latency == 29u &&
                   ee::c_ee_vu1::efu(regular, 0x7eu, 2u, bits, latency) &&
                   bits == 0x3d4bed7fu && latency == 44u &&
                   ee::c_ee_vu1::efu(zero, 0x7eu, 2u, bits, latency) &&
                   bits == 0x3f800000u && latency == 44u,
                   "EFU polynomial hardware regression"))
        {
            return 1;
        }
    }

    // The EFU producer updates P after ESUM latency. MFP must first
    // observe old P and then new P only after WAITP.
    ee::s_ee_vu1_state p{};
    p.m_initialized = true;
    p.m_p = 0x3f800000u; // old 1.0
    p.m_vf[2] = {0x40000000u, 0x40400000u, 0x40800000u, 0x40a00000u};
    if (!check(run(p, {
                   {special(0x76u, 0u, 2u), k_upper_nop},
                   {special(0x64u, 6u, 0u, 8u), k_upper_nop},
                   {special(0x7bu), k_upper_nop},
                   {special(0x64u, 7u, 0u, 8u), k_upper_nop},
                   {k_lower_nop, k_upper_end},
                   {k_lower_nop, k_upper_nop}
               }, data, issue) &&
               p.m_vf[6][0] == 0x3f800000u &&
               p.m_vf[7][0] == 0x41600000u && // ESUM 14
               p.m_p == 0x41600000u &&
               issue.m_cycles >= 14u,
               "ESUM/MFP/WAITP old/new P timing"))
    {
        return 1;
    }

    // LQI and LQD implement post-increment/pre-decrement in VI,
    // including loading the same data vector after the reverse step.
    ee::s_ee_vu1_state memory{};
    memory.m_initialized = true;
    memory.m_vi[1] = 7u;
    memory.m_vf[2] = {10u, 20u, 30u, 40u};
    if (!check(run(memory, {
                   {special(0x35u, 1u, 2u, 15u), k_upper_nop}, // SQI
                   {special(0x36u, 3u, 1u, 15u), k_upper_end}, // LQD
                   {k_lower_nop, k_upper_nop}
               }, data, issue) &&
               memory.m_vi[1] == 7u &&
               memory.m_vf[3] == memory.m_vf[2],
               "SQI/LQD address update and vector data"))
    {
        return 1;
    }

    // SDBZ MSCNT rejected this exact ISW.x VI8, -1(VI4) encoding.
    // Exercise negative offsets, RAM wrap, zero extension, mask
    // preservation, ILW load latency and VI0 write suppression.
    {
        ee::s_ee_vu1_state integer{};
        integer.m_initialized = true;
        integer.m_vi[4] = 0u;
        integer.m_vi[8] = 0xabcd;
        std::array<uint32_t, 4> initial{0xfeed1234u, 0x12345678u,
                                       0x87654321u, 0xaabbccddu};
        std::memcpy(data.data() + 0x3ff0u, initial.data(), 16u);
        const uint32_t ilw_x = (0x04u << 25u) | (8u << 21u) |
            (9u << 16u) | (4u << 11u) | 0x7ffu;
        const uint32_t isw_y = (0x05u << 25u) | (4u << 21u) |
            (9u << 16u) | (4u << 11u) | 1u;
        if (!check(run(integer, {
                       {0x0b0827ffu, k_upper_nop},
                       {ilw_x, k_upper_nop},
                       {isw_y, k_upper_nop},
                       {ilw_x & ~(15u << 16u), k_upper_end},
                       {k_lower_nop, k_upper_nop}
                    }, data, issue), "SDBZ ISW/ILW microprogram rejected"))
        {
            return 1;
        }
        std::array<uint32_t, 4> actual{};
        uint32_t stored_y = 0u;
        std::memcpy(actual.data(), data.data() + 0x3ff0u, 16u);
        std::memcpy(&stored_y, data.data() + 20u, 4u);
        if (!check(actual[0] == 0xabcdu &&
                   actual[1] == initial[1] && actual[2] == initial[2] &&
                   actual[3] == initial[3] && integer.m_vi[9] == 0xabcdu &&
                   integer.m_vi[0] == 0u && stored_y == 0xabcdu &&
                   issue.m_cycles >= 8u,
                   "ISW/ILW addressing, lane mask, zero extension or load stall"))
        {
            return 1;
        }
    }

    // RINIT/RNEXT/RGET use the reference 23-bit LFSR, and MFIR
    // sign-extends the VI source into the selected VF lanes.
    ee::s_ee_vu1_state rand{};
    rand.m_initialized = true;
    rand.m_vf[2][0] = 0x00123456u;
    rand.m_vi[4] = 0xff80u;
    if (!check(run(rand, {
                   {special(0x42u, 0u, 2u), k_upper_nop},
                   {special(0x40u, 5u, 0u, 8u), k_upper_nop},
                   {special(0x41u, 6u, 0u, 8u), k_upper_nop},
                   {special(0x3du, 7u, 4u, 8u), k_upper_end},
                   {k_lower_nop, k_upper_nop}
               }, data, issue) &&
               rand.m_r == (((0x123456u << 1u) |
                   (((0x123456u >> 4u) ^ (0x123456u >> 22u)) & 1u)) & 0x7fffffu) &&
               rand.m_vf[5][0] == (rand.m_r | ee::vu1_fp::kOne) &&
               rand.m_vf[6][0] == rand.m_vf[5][0] &&
               rand.m_vf[7][0] == 0xffffff80u,
               "RINIT/RNEXT/RGET or MFIR sign extension"))
    {
        return 1;
    }

    // A dependent VF read stalls until its lane is available; the
    // unrelated W lane does not force unnecessary stalls.
    ee::s_ee_vu1_state hazard{};
    hazard.m_initialized = true;
    hazard.m_vf[2][0] = 0x3f800000u;
    hazard.m_vf[3][0] = 0x40000000u;
    if (!check(run(hazard, {
                   {k_lower_nop, upper(0x28u, 4u, 2u, 3u, 8u)},
                   {k_lower_nop, upper(0x2au, 5u, 4u, 3u, 8u) | 0x40000000u},
                   {k_lower_nop, k_upper_nop}
               }, data, issue) &&
               hazard.m_vf[5][0] == 0x40c00000u &&
               issue.m_steps == 3u && issue.m_cycles >= 6u,
               "FMAC vector RAW dependency stalls"))
    {
        return 1;
    }

    // Compare the expanded scoreboard lists with the original mask
    // scan across deterministic instruction words, varying readiness
    // timestamps and both immediate and ordinary lower instructions.
    {
        uint32_t random = 0x6a09e667u;
        const auto next = [&]()
        {
            random = random * 1664525u + 1013904223u;
            return random;
        };
        ee::c_ee_vu1_decode_cache cache;
        for (uint32_t sample = 0u; sample < 8192u; ++sample)
        {
            const uint32_t lo = next();
            const uint32_t hi = next();
            const auto& use = cache.lookup((sample & 0x7ffu) * 8u, lo, hi);
            ee::s_ee_vu1_dependency scoreboard{};
            for (auto& reg : scoreboard.m_vf_ready)
            {
                for (auto& lane : reg)
                {
                    lane = next() & 127u;
                }
            }
            for (auto& reg : scoreboard.m_vi_ready)
            {
                reg = next() & 127u;
            }
            auto reference = scoreboard;
            const uint32_t start = next() & 127u;
            uint32_t expected = start;
            for (uint32_t i = 0u; i < use.m_n; ++i)
            {
                for (uint32_t lane = 0u; lane < 4u; ++lane)
                {
                    if ((use.m_mask[i] & (8u >> lane)) != 0u)
                    {
                        expected = std::max(expected,
                            reference.m_vf_ready[use.m_reg[i]][lane]);
                    }
                }
            }
            for (uint32_t reg = 1u; reg < 16u; ++reg)
            {
                if ((use.m_vi_reads & (1u << reg)) != 0u)
                {
                    expected = std::max(expected, reference.m_vi_ready[reg]);
                }
            }
            const uint32_t actual = scoreboard.issue(use, start);
            for (uint32_t i = 0u; i < use.m_wn; ++i)
            {
                for (uint32_t lane = 0u; lane < 4u; ++lane)
                {
                    if ((use.m_wmask[i] & (8u >> lane)) != 0u)
                    {
                        reference.m_vf_ready[use.m_wreg[i]][lane] =
                            expected + 4u;
                    }
                }
            }
            if (use.m_vi_load != 0u)
            {
                reference.m_vi_ready[use.m_vi_load] = expected + 4u;
            }
            scoreboard.retire(use, actual);
            if (!check(actual == expected &&
                       scoreboard.m_vf_ready == reference.m_vf_ready &&
                       scoreboard.m_vi_ready == reference.m_vi_ready,
                       "expanded scoreboard differs from mask scan"))
            {
                return 1;
            }
        }
        // Exercise the full representable metadata capacity, including
        // duplicates and suppressed writes to the constant registers.
        ee::s_ee_vu1_use full{};
        for (uint32_t reg = 0u; reg < 16u; ++reg)
        {
            full.read_vi(reg);
            full.read_vi(reg);
        }
        for (uint32_t reg = 1u; reg <= 4u; ++reg)
        {
            full.read(reg, 15u);
            full.write(reg, 15u);
        }
        if (!check(full.m_vi_index_count == 15u &&
                   full.m_read_lane_count == 16u &&
                   full.m_write_lane_count == 8u,
                   "expanded scoreboard capacity or VI deduplication"))
        {
            return 1;
        }
    }

    // A resident decode cache must not retain stale dependency metadata
    // when MPG replaces either half of the instruction at the same PC.
    // Check the load and vector masks against a fresh decode, then revisit
    // the original pair (as after an uncommitted VIF transaction).
    {
        ee::c_ee_vu1_decode_cache cache;
        const uint32_t ilw = (0x04u << 25u) | (8u << 21u) |
            (9u << 16u) | (4u << 11u);
        const uint32_t add_x = upper(0x28u, 4u, 2u, 3u, 8u);
        const uint32_t add_w = upper(0x28u, 4u, 2u, 3u, 1u);
        const s_pair pairs[] = {
            {k_lower_nop, add_x},
            {k_lower_nop, add_x},
            {ilw, add_x},
            {ilw, add_w},
            {ilw, add_w | 0x80000000u},
            {k_lower_nop, add_x}
        };
        for (const auto& pair : pairs)
        {
            const auto expected = ee::s_ee_vu1_dependency::decode(
                pair.m_lower, pair.m_upper);
            const auto& actual = cache.lookup(
                0x3ff8u, pair.m_lower, pair.m_upper);
            if (!check(actual.m_reg == expected.m_reg &&
                       actual.m_mask == expected.m_mask &&
                       actual.m_wreg == expected.m_wreg &&
                       actual.m_wmask == expected.m_wmask &&
                       actual.m_n == expected.m_n &&
                       actual.m_wn == expected.m_wn &&
                       actual.m_vi_reads == expected.m_vi_reads &&
                       actual.m_vi_load == expected.m_vi_load,
                       "VU1 decode cache reused stale metadata"))
            {
                return 1;
            }
        }
    }

    // Reuse the same micro-PCs with a dependent X lane, then replace
    // the consumer with an unrelated W lane. The scoreboard is fresh
    // on each run, and the changed upper word must remove the stall.
    {
        const uint32_t producer = upper(0x28u, 4u, 2u, 3u, 8u);
        const uint32_t consumer_x =
            upper(0x2au, 5u, 4u, 3u, 8u) | 0x40000000u;
        const uint32_t consumer_w =
            upper(0x2au, 5u, 4u, 3u, 1u) | 0x40000000u;
        ee::s_ee_vu1_state initial{};
        initial.m_initialized = true;
        initial.m_vf[2][0] = 0x3f800000u;
        initial.m_vf[3][0] = 0x40000000u;
        initial.m_vf[3][3] = 0x40000000u;
        initial.m_vf[4][3] = 0x40400000u;
        for (uint32_t consumer : {consumer_x, consumer_w, consumer_x})
        {
            auto resident = initial;
            if (!check(run(resident, {
                           {k_lower_nop, producer},
                           {k_lower_nop, consumer},
                           {k_lower_nop, k_upper_nop}
                       }, data, issue) &&
                       issue.m_steps == 3u &&
                       issue.m_cycles == (consumer == consumer_x ? 6u : 3u) &&
                       resident.m_vf[5][consumer == consumer_x ? 0u : 3u] ==
                           0x40c00000u,
                       "VU1 replacement microcode dependency timing"))
            {
                return 1;
            }
        }
    }

    // Unsupported debug/interrupt upper bits still fail closed.
    std::array<uint8_t, ee::c_ee_vu1::k_memory_size> raw_code{};
    const uint32_t debug_upper = k_upper_nop | 0x10000000u;
    std::memcpy(raw_code.data(), &k_lower_nop, 4u);
    std::memcpy(raw_code.data() + 4u, &debug_upper, 4u);
    std::vector<uint8_t> output{};
    if (!check(!ee::c_ee_vu1::run(
                   hazard, raw_code, data, 0u, 0u, 0u, output, &issue) &&
               output.empty() && issue.m_steps == 1u,
               "unimplemented VU debug interrupt silently completed"))
    {
        return 1;
    }

    // The exact SDBZ XGKICK VI3 encoding, with previously staged
    // output at the obsolete 1 MiB limit. The packet wraps at VU RAM
    // end. The independent single-kick safety bound is retained.
    {
        using reject = ee::e_ee_vu1_kick_reject;
        std::array<uint8_t, ee::c_ee_vu1::k_memory_size> code{}, memory{};
        const uint32_t instructions[] = {
            0x80001efcu, k_upper_end, k_lower_nop, k_upper_nop
        };
        std::memcpy(code.data(), instructions, sizeof(instructions));
        const uint64_t tag = 0x1000000000008001ull;
        std::memcpy(memory.data() + 0x3ff0u, &tag, sizeof(tag));
        memory[0] = 0x5au;
        ee::s_ee_vu1_state kick{};
        kick.m_initialized = true;
        kick.m_vi[3] = 0x3ffu;
        std::vector<uint8_t> output(0x100000u, 0u);
        const auto execute = [&]()
        {
            return ee::c_ee_vu1::run(kick, code, memory, 0u, 0u, 0u,
                                     output, &issue);
        };
        if (!check(execute() && output.size() == 0x100020u &&
                   output[0x100010u] == 0x5au &&
                   issue.m_kick_reject == reject::none,
                   "valid XGKICK rejected at obsolete aggregate cap"))
        {
            return 1;
        }
        output.resize(static_cast<size_t>(ee::k_max_graphics_batch_qwc) * 16u - 16u);
        const size_t before = output.size();
        if (!check(!execute() && output.size() == before &&
                   issue.m_kick_reject == reject::batch_qwc_limit &&
                   issue.m_kick_address == 0x3ffu &&
                   issue.m_kick_packet_qwc == 2u &&
                   issue.m_kick_staged_qwc == ee::k_max_graphics_batch_qwc - 1u,
                   "XGKICK aggregate rejection missing diagnostic or leaked bytes"))
        {
            return 1;
        }
        // A mixed-format packet crosses the RAM boundary after
        // its first tag. Verify all payload bytes, not just its length.
        memory.fill(0xa5u);
        const uint64_t packed = 0x2000000000000001ull;
        // FLG=1, NREG=3, NLOOP=3: ceil(9/2)=5 body qwords.
        const uint64_t reglist = 0x3400000000000003ull;
        const uint64_t image = 0x0800000000008002ull;
        std::memcpy(memory.data() + 0x3ff0u, &packed, sizeof(packed));
        std::memcpy(memory.data() + 32u, &reglist, sizeof(reglist));
        std::memcpy(memory.data() + 128u, &image, sizeof(image));
        output.assign(16u, 0x77u);
        std::vector<uint8_t> expected = output;
        expected.insert(expected.end(), memory.end() - 16, memory.end());
        expected.insert(expected.end(), memory.begin(), memory.begin() + 176);
        if (!check(execute() && output == expected,
                   "mixed-format wrapped XGKICK payload differs"))
        {
            return 1;
        }

        // A valid single IMAGE tag can describe all of circular RAM.
        // No bytes may be lost or duplicated when the packet wraps.
        const uint64_t full = 0x08000000000083ffull;
        std::memcpy(memory.data() + 0x3ff0u, &full, sizeof(full));
        output.clear();
        expected.assign(memory.end() - 16, memory.end());
        expected.insert(expected.end(), memory.begin(), memory.end() - 16);
        if (!check(execute() && output == expected,
                   "full circular XGKICK payload differs"))
        {
            return 1;
        }
        output.clear();
        const uint64_t oversize = 0x1000000000008400ull;
        std::memcpy(memory.data() + 0x3ff0u, &oversize, sizeof(oversize));
        if (!check(!execute() && output.empty() &&
                   issue.m_kick_reject == reject::packet_qwc_limit &&
                   issue.m_kick_tag_address == 0x3ffu &&
                   issue.m_kick_tag == oversize && issue.m_kick_packet_qwc == 1025u,
                   "XGKICK oversized packet not diagnosed"))
        {
            return 1;
        }
        memory.fill(0u);
        if (!check(!execute() && output.empty() &&
                   issue.m_kick_reject == reject::missing_eop &&
                   issue.m_kick_packet_qwc == 1024u,
                   "unterminated XGKICK packet not diagnosed"))
        {
            return 1;
        }
    }

    std::puts("VU1 Q/P, lower specials and dependencies: PASS");
    return 0;
}
