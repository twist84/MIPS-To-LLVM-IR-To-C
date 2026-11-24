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
