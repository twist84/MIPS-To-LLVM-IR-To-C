// Derived from ps2-web-recomp:SDBZ runtime/src/vu_micro.cpp
// (Samir Nuri, 2026, MIT). See ps2_webgpu/gs/LICENSE.upstream.
// This first batch intentionally rejects unported instructions and timing
// dependencies instead of silently treating them as NOPs.
#include "ee_vu1.h"
#include "ee_vu1_aot.h"
#include "ee_runtime_profile.h"
#include "ee_graphics_batch.h"
#include "ee_vu1_fp.h"
#include "ee_vu1_dependency.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ee
{
namespace
{
uint32_t read32(const uint8_t* bytes)
{
    uint32_t result = 0u;
    std::memcpy(&result, bytes, sizeof(result));
    return result;
}

uint64_t read64(const uint8_t* bytes)
{
    uint64_t result = 0u;
    std::memcpy(&result, bytes, sizeof(result));
    return result;
}

float to_float(uint32_t bits)
{
    // Reference VU input normalisation: flush denormals, saturate INF/NaN.
    const uint32_t exponent = bits & 0x7f800000u;
    if (exponent == 0u)
    {
        bits &= 0x80000000u;
    }
    else if (exponent == 0x7f800000u)
    {
        bits = (bits & 0x80000000u) | 0x7f7fffffu;
    }
    float result = 0.0f;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

uint32_t to_bits(float value)
{
    uint32_t result = 0u;
    std::memcpy(&result, &value, sizeof(result));
    const uint32_t exponent = result & 0x7f800000u;
    if (exponent == 0u)
    {
        result &= 0x80000000u;
    }
    else if (exponent == 0x7f800000u)
    {
        result = (result & 0x80000000u) | 0x7f7fffffu;
    }
    return result;
}

void store_vi(s_ee_vu1_state& state, uint32_t index, uint32_t value)
{
    if (index != 0u)
    {
        state.m_vi[index] = static_cast<uint16_t>(value);
    }
}

uint32_t qword_offset(uint32_t address)
{
    return (address & 0x3ffu) * 16u;
}

// XGKICK walks actual GIFtags from VU1 RAM until an EOP tag. A missing EOP
// or oversized chain is an error, never a fabricated empty PATH1 transfer.
bool xgkick(const std::array<uint8_t, 16384>& data,
            uint32_t address, std::vector<uint8_t>& output,
            s_ee_vu1_issue& issue)
{
    ee::c_runtime_profile_scope profile_scope(ee::e_runtime_profile_id::vu1_xgkick);
    const uint32_t start = qword_offset(address);
    uint32_t count = 0u;
    bool finished = false;
    const auto reject = [&](e_ee_vu1_kick_reject reason, uint32_t at,
                            uint64_t tag, uint32_t packet_qwc)
    {
        issue.m_kick_reject = reason;
        issue.m_kick_address = address & 0x3ffu;
        issue.m_kick_tag_address = at / 16u;
        issue.m_kick_tag = tag;
        issue.m_kick_packet_qwc = packet_qwc;
        issue.m_kick_staged_qwc = static_cast<uint32_t>(output.size() / 16u);
        return false;
    };
    uint32_t last_at = 0u;
    uint64_t last_tag = 0u;
    for (; count < 1024u && !finished;)
    {
        const uint32_t at = qword_offset(address + count);
        const uint64_t tag = read64(data.data() + at);
        last_at = at;
        last_tag = tag;
        const uint32_t nloop = static_cast<uint32_t>(tag & 0x7fffu);
        const uint32_t flg = static_cast<uint32_t>((tag >> 58u) & 3u);
        uint32_t nreg = static_cast<uint32_t>((tag >> 60u) & 15u);
        if (nreg == 0u)
        {
            nreg = 16u;
        }
        const uint32_t body = flg == 0u ? nloop * nreg :
            flg == 1u ? (nloop * nreg + 1u) / 2u : nloop;
        if (body + 1u > 1024u - count)
        {
            return reject(e_ee_vu1_kick_reject::packet_qwc_limit,
                          at, tag, count + body + 1u);
        }
        // Only tags need inspection. Every body is a consecutive
        // range of qwords in the same circular VU RAM.
        count += body + 1u;
        finished = (tag & 0x8000u) != 0u;
    }
    if (!finished)
    {
        return reject(e_ee_vu1_kick_reject::missing_eop, last_at, last_tag,
                      count);
    }
    // PATH1 accumulates output from many independent kicks and MSCNT
    // continuations in the same staged VIF transaction. Its aggregate
    // budget must agree with the GIF/GS consumer, not VU1 memory size.
    constexpr size_t k_max_batch_bytes =
        static_cast<size_t>(k_max_graphics_batch_qwc) * 16u;
    const size_t bytes = static_cast<size_t>(count) * 16u;
    if (output.size() > k_max_batch_bytes ||
        bytes > k_max_batch_bytes - output.size())
    {
        return reject(e_ee_vu1_kick_reject::batch_qwc_limit, last_at, last_tag,
                      count);
    }
    // After complete validation, append the packet directly from RAM.
    // At most one wrap is possible under the single-kick qword limit.
    const size_t first = std::min(bytes, data.size() - start);
    output.insert(output.end(), data.data() + start,
                  data.data() + start + first);
    if (bytes != first)
    {
        output.insert(output.end(), data.data(), data.data() + bytes - first);
    }
    return true;
}

struct s_upper_write
{
    std::array<uint32_t, 4> m_values{};
    uint32_t m_overflow = 0u;
    uint32_t m_underflow = 0u;
    uint32_t m_product_sticky = 0u;
    uint32_t m_target = 0u;
    uint32_t m_mask = 0u;
    uint32_t m_clip = 0u;
    bool m_acc = false;
    bool m_flags = false;
    bool m_has_clip = false;
    bool m_writes = false;
};

// VU FMAC and status calculations ported from vu_micro.cpp::set_flags.
void set_mac_flags(s_ee_vu1_state& state,
                   const std::array<uint32_t, 4>& value, uint32_t mask,
                   uint32_t overflow, uint32_t underflow, uint32_t product_sticky)
{
    uint32_t mac = 0u;
    for (uint32_t lane = 0u; lane < 4u; ++lane)
    {
        if ((mask & (8u >> lane)) == 0u)
        {
            continue;
        }
        const uint32_t bit = 3u - lane;
        const uint32_t x = value[lane];
        if ((x & 0x7f800000u) == 0u)
        {
            mac |= 1u << bit;
        }
        if ((x & 0x80000000u) != 0u)
        {
            mac |= 1u << (bit + 4u);
        }
        if ((underflow & (1u << bit)) != 0u)
        {
            mac |= 1u << (bit + 8u);
        }
        if ((overflow & (1u << bit)) != 0u)
        {
            mac |= 1u << (bit + 12u);
        }
    }
    state.m_mac = mac;
    uint32_t status = (state.m_status & 0xff0u) | product_sticky;
    if ((mac & 0x000fu) != 0u) { status |= 1u; }
    if ((mac & 0x00f0u) != 0u) { status |= 2u; }
    if ((mac & 0x0f00u) != 0u) { status |= 4u; }
    if ((mac & 0xf000u) != 0u) { status |= 8u; }
    status |= (status & 15u) << 6u;
    state.m_status = (status & ~0x30u) | (state.m_status & 0x30u);
}

// All FMAC mathematics and special-index mappings below are ported from
// ps2-web-recomp's reference upper()/decode_upper(). No Q-dependent opcode
// is enabled before the Q-latency pipeline is implemented.
bool upper(const s_ee_vu1_state& state, uint32_t hi,
           s_upper_write& write)
{
    namespace fp = vu1_fp;
    const uint32_t funct = hi & 63u;
    const bool special = funct >= 0x3cu;
    const uint32_t index = (((hi >> 6u) & 31u) << 2u) | (funct & 3u);
    const uint32_t op = special ? index : funct;
    const uint32_t mask = (hi >> 21u) & 15u;
    const uint32_t fs = (hi >> 11u) & 31u;
    const uint32_t ft = (hi >> 16u) & 31u;
    const uint32_t fd = (hi >> 6u) & 31u;

    if (special && op == 0x2fu)
    {
        return true; // Canonical upper NOP.
    }
    if (op > 0x2fu || (!special && funct >= 0x30u))
    {
        return false; // Unknown opcode or Q-latency dependency.
    }

    const bool conversion = special && op >= 0x10u && op <= 0x17u;
    const bool abs_op = special && op == 0x1du;
    const bool clip_op = special && op == 0x1fu;
    const bool cross = op == 0x2eu;
    const bool uses_acc = special && !conversion && !abs_op &&
        !clip_op && op != 0x2fu;
    const bool is_minmax =
        (op >= 0x10u && op <= 0x17u && !conversion) ||
        op == 0x1du || op == 0x1fu || op == 0x2bu || op == 0x2fu;
    const bool is_arithmetic = !conversion && !abs_op &&
        !clip_op && !is_minmax;

    if (clip_op)
    {
        const int32_t limit = fp::key(state.m_vf[ft][3] & fp::kAbs);
        uint32_t flags = 0u;
        for (uint32_t lane = 0u; lane < 3u; ++lane)
        {
            const int32_t x = fp::key(state.m_vf[fs][lane]);
            if (x > limit) { flags |= 1u << (lane * 2u); }
            if (x < -limit) { flags |= 2u << (lane * 2u); }
        }
        write.m_clip = ((state.m_clip << 6u) | flags) & 0xffffffu;
        write.m_has_clip = true;
        return true;
    }

    write.m_mask = cross ? 0xeu : mask;
    write.m_acc = uses_acc;
    write.m_target = special && (conversion || abs_op) ? ft : fd;
    write.m_flags = is_arithmetic;
    write.m_writes = true;
    static constexpr int k_fractions[4]{0, 4, 12, 15};
    const int frac = k_fractions[op & 3u];

    for (uint32_t lane = 0u; lane < 4u; ++lane)
    {
        if ((write.m_mask & (8u >> lane)) == 0u)
        {
            continue;
        }
        const uint32_t lhs = state.m_vf[fs][cross
            ? (lane == 0u ? 1u : lane == 1u ? 2u : 0u) : lane];
        const uint32_t rhs_cross = state.m_vf[ft][
            lane == 0u ? 2u : lane == 1u ? 0u : 1u];
        const bool uses_q = op == 0x1cu || op == 0x20u ||
            op == 0x21u || op == 0x24u || op == 0x25u;
        const bool uses_i = op == 0x1du || op == 0x1eu ||
            op == 0x1fu || op == 0x22u || op == 0x23u ||
            op == 0x26u || op == 0x27u;
        const uint32_t rhs =
            op <= 0x1bu ? state.m_vf[ft][funct & 3u] :
            uses_q ? state.m_q :
            uses_i ? state.m_i : state.m_vf[ft][lane];
        const uint32_t acc = state.m_acc[lane];
        uint32_t result = 0u;
        vu_fmac_fp::s_fmac_result arithmetic{};
        const uint32_t lane_bit = 8u >> lane;
        const bool latched = (state.m_acc_overflow_mask & lane_bit) != 0u;

        if (conversion)
        {
            if (op <= 0x13u)
            {
                result = fp::from_fixed(
                    static_cast<int32_t>(lhs), frac);
            }
            else
            {
                result = fp::to_fixed(lhs, frac);
            }
        }
        else if (abs_op)
        {
            result = lhs & fp::kAbs;
        }
        else if (cross)
        {
            if (special)
            {
                arithmetic = vu_fmac_fp::mul(lhs, rhs_cross);
            }
            else
            {
                const auto mac = vu_fmac_fp::mac(acc, lhs, rhs_cross, true, latched);
                arithmetic = mac.m_final;
                write.m_product_sticky |= mac.m_sticky;
            }
        }
        else if (op <= 0x03u || op == 0x20u ||
                 op == 0x22u || op == 0x28u)
        {
            arithmetic = vu_fmac_fp::add(lhs, rhs, false);
        }
        else if ((op >= 0x04u && op <= 0x07u) ||
                 op == 0x24u || op == 0x26u || op == 0x2cu)
        {
            arithmetic = vu_fmac_fp::add(lhs, rhs, true);
        }
        else if ((op >= 0x08u && op <= 0x0bu) ||
                 op == 0x21u || op == 0x23u || op == 0x29u)
        {
            const auto mac = vu_fmac_fp::mac(acc, lhs, rhs, false, latched);
            arithmetic = mac.m_final;
            write.m_product_sticky |= mac.m_sticky;
        }
        else if ((op >= 0x0cu && op <= 0x0fu) ||
                 op == 0x25u || op == 0x27u || op == 0x2du)
        {
            const auto mac = vu_fmac_fp::mac(acc, lhs, rhs, true, latched);
            arithmetic = mac.m_final;
            write.m_product_sticky |= mac.m_sticky;
        }
        else if ((op >= 0x10u && op <= 0x13u) ||
                 op == 0x1du || op == 0x2bu)
        {
            result = fp::max(lhs, rhs);
        }
        else if ((op >= 0x14u && op <= 0x17u) ||
                 op == 0x1fu || op == 0x2fu)
        {
            result = fp::min(lhs, rhs);
        }
        else if ((op >= 0x18u && op <= 0x1cu) ||
                 op == 0x1eu || op == 0x2au)
        {
            arithmetic = vu_fmac_fp::mul(lhs, rhs);
        }
        else
        {
            return false;
        }
        if (is_arithmetic)
        {
            result = arithmetic.m_bits;
            if (arithmetic.m_overflow) { write.m_overflow |= lane_bit; }
            if (arithmetic.m_underflow) { write.m_underflow |= lane_bit; }
        }
        write.m_values[lane] = result;
    }
    return true;
}

// Flag reads are four microinstruction cycles behind the latest writes.
// Adapted from ps2-web-recomp runtime/include/ps2/vu_ops.hpp::FlagPipe.
struct s_flag_pipe
{
    uint32_t m_t = 16u;
    uint32_t m_stalls = 0u;
    uint32_t m_q_value = 0u;
    uint32_t m_q_ready = 0u;
    uint32_t m_q_flags = 0u;
    uint32_t m_p_value = 0u;
    uint32_t m_p_ready = 0u;
    bool m_q_pending = false;
    bool m_p_pending = false;

    uint32_t now() const
    {
        return m_t + m_stalls;
    }

    void sync_q(s_ee_vu1_state& state)
    {
        if (m_q_pending && now() >= m_q_ready)
        {
            state.m_q = m_q_value;
            state.m_status = vu_scalar_fp::update_status(state.m_status, m_q_flags);
            m_q_pending = false;
        }
    }

    void wait_q(s_ee_vu1_state& state)
    {
        if (m_q_pending)
        {
            if (now() < m_q_ready)
            {
                m_stalls += m_q_ready - now();
            }
            sync_q(state);
        }
    }

    void schedule_q(s_ee_vu1_state& state, uint32_t value, uint32_t latency,
                    uint32_t flags)
    {
        wait_q(state);
        m_q_value = value;
        m_q_flags = flags;
        m_q_ready = now() + latency;
        m_q_pending = true;
    }

    void sync_p(s_ee_vu1_state& state)
    {
        if (m_p_pending && now() >= m_p_ready)
        {
            state.m_p = m_p_value;
            m_p_pending = false;
        }
    }

    void wait_p(s_ee_vu1_state& state)
    {
        if (m_p_pending)
        {
            if (now() < m_p_ready)
            {
                m_stalls += m_p_ready - now();
            }
            sync_p(state);
        }
    }

    void schedule_p(s_ee_vu1_state& state, uint32_t value, uint32_t latency)
    {
        wait_p(state);
        m_p_value = value;
        m_p_ready = now() + latency;
        m_p_pending = true;
    }
    uint32_t m_mac_ts[8]{};
    uint32_t m_mac_values[8]{};
    uint32_t m_status_values[8]{};
    uint32_t m_clip_ts[8]{};
    uint32_t m_clip_values[8]{};
    uint32_t m_mac_head = 0u;
    uint32_t m_clip_head = 0u;

    void start(const s_ee_vu1_state& state)
    {
        for (uint32_t i = 0u; i < 8u; ++i)
        {
            m_mac_values[i] = state.m_mac;
            m_status_values[i] = state.m_status;
            m_clip_values[i] = state.m_clip;
        }
    }

    void set_mac(uint32_t mac, uint32_t status)
    {
        const uint32_t i = m_mac_head++ & 7u;
        m_mac_ts[i] = m_t;
        m_mac_values[i] = mac;
        m_status_values[i] = status;
    }

    void set_clip(uint32_t value, uint32_t at)
    {
        const uint32_t i = m_clip_head++ & 7u;
        m_clip_ts[i] = at;
        m_clip_values[i] = value;
    }

    void force_mac(uint32_t mac, uint32_t status)
    {
        set_mac(mac, status);
        m_mac_ts[(m_mac_head - 1u) & 7u] = m_t - 4u;
    }

    uint32_t mac_slot() const
    {
        for (uint32_t k = 1u; k <= 8u; ++k)
        {
            const uint32_t i = (m_mac_head - k) & 7u;
            if (m_mac_ts[i] + 4u <= m_t)
            {
                return i;
            }
        }
        return (m_mac_head - 8u) & 7u;
    }

    uint32_t mac() const
    {
        return m_mac_values[mac_slot()];
    }

    uint32_t status(const s_ee_vu1_state& state) const
    {
        // The FMAC delay does not hide already retired FDIV flags.
        return (m_status_values[mac_slot()] & 0x3cfu) | (state.m_status & 0xc30u);
    }

    uint32_t clip() const
    {
        for (uint32_t k = 1u; k <= 8u; ++k)
        {
            const uint32_t i = (m_clip_head - k) & 7u;
            if (m_clip_ts[i] + 4u <= now())
            {
                return m_clip_values[i];
            }
        }
        return m_clip_values[(m_clip_head - 8u) & 7u];
    }
};

bool branch_delay_exempt(uint32_t lower)
{
    switch (lower >> 25u)
    {
    case 0x10u: case 0x12u: case 0x13u: case 0x14u:
    case 0x16u: case 0x17u: case 0x18u: case 0x1au:
    case 0x1bu: case 0x1cu: case 0x21u: case 0x25u:
        return true;
    default:
        return false;
    }
}

// Reference EFU arithmetic from vu_ops.hpp::efu; pipeline latencies from
// the VU manual. P retains its previous value until the producer retires.
bool efu_result(const std::array<uint32_t, 4>& source, uint32_t index,
                uint32_t fsf, uint32_t& value, uint32_t& latency)
{
    const float x = to_float(source[0]);
    const float y = to_float(source[1]);
    const float z = to_float(source[2]);
    const float scalar = to_float(source[fsf]);
    const float sum = x * x + y * y + z * z;
    float result = 0.0f;

    switch (index)
    {
    case 0x70u: result = sum; latency = 11u; break; // ESADD
    case 0x71u:
        result = sum != 0.0f ? 1.0f / sum : 3.4e38f;
        latency = 18u;
        break; // ERSADD
    case 0x72u: result = std::sqrt(sum); latency = 18u; break; // ELENG
    case 0x73u:
        result = sum != 0.0f ? 1.0f / std::sqrt(sum) : 3.4e38f;
        latency = 24u;
        break; // ERLENG
    case 0x74u: result = std::atan2(y, x); latency = 54u; break;
    case 0x75u: result = std::atan2(z, x); latency = 54u; break;
    case 0x76u:
        result = x + y + z + to_float(source[3]);
        latency = 12u;
        break; // ESUM
    case 0x78u: result = std::sqrt(std::fabs(scalar)); latency = 12u; break;
    case 0x79u:
    {
        const float root = std::sqrt(std::fabs(scalar));
        result = root != 0.0f ? 1.0f / root : 3.4e38f;
        latency = 18u;
        break;
    }
    case 0x7au:
        result = scalar != 0.0f ? 1.0f / scalar : 3.4e38f;
        latency = 12u;
        break;
    case 0x7cu: result = std::sin(scalar); latency = 29u; break;
    case 0x7du: result = std::atan(scalar); latency = 54u; break;
    case 0x7eu: result = std::exp(-scalar); latency = 44u; break;
    default: return false;
    }
    value = to_bits(result);
    return true;
}

bool lower(s_ee_vu1_state& state, uint32_t lo, uint32_t pc,
           std::array<uint8_t, 16384>& data, uint32_t top, uint32_t itop,
           const std::array<uint16_t, 16>& branch_vi,
           std::vector<uint8_t>& path1_bytes,
           uint32_t& branch_target, bool& branched,
           s_flag_pipe& pipe, int32_t& fsset, s_ee_vu1_issue& issue)
{
    const uint32_t major = lo >> 25u;
    const uint32_t dest = (lo >> 21u) & 15u;
    const uint32_t it = (lo >> 16u) & 15u;
    const uint32_t is = (lo >> 11u) & 15u;
    const uint32_t id = (lo >> 6u) & 15u;
    const uint32_t ft = (lo >> 16u) & 31u;
    const uint32_t fs = (lo >> 11u) & 31u;
    const int32_t imm11 = static_cast<int32_t>(lo << 21u) >> 21u;
    const uint32_t target = pc + 8u +
        static_cast<uint32_t>(imm11) * 8u;

    if (major == 0x40u)
    {
        const uint32_t funct = lo & 63u;
        if (funct < 0x3cu)
        {
            switch (funct)
            {
            case 0x30u: store_vi(state, id, state.m_vi[is] + state.m_vi[it]); return true;
            case 0x31u: store_vi(state, id, state.m_vi[is] - state.m_vi[it]); return true;
            case 0x32u:
                store_vi(state, it, state.m_vi[is] +
                    (static_cast<int32_t>(((lo >> 6u) & 31u) << 27u) >> 27u));
                return true;
            case 0x34u: store_vi(state, id, state.m_vi[is] & state.m_vi[it]); return true;
            case 0x35u: store_vi(state, id, state.m_vi[is] | state.m_vi[it]); return true;
            default: return false;
            }
        }
        const uint32_t index = (((lo >> 6u) & 31u) << 2u) | (funct & 3u);
        switch (index)
        {
        case 0x30u: // MOVE
        case 0x31u: // MR32
            if (ft != 0u)
            {
                const auto source = state.m_vf[fs];
                for (uint32_t lane = 0u; lane < 4u; ++lane)
                {
                    if ((dest & (8u >> lane)) != 0u)
                    {
                        state.m_vf[ft][lane] = source[
                            index == 0x30u ? lane : (lane + 1u) & 3u];
                    }
                }
            }
            return true;
        case 0x34u: // LQI
        case 0x36u: // LQD
        {
            if (index == 0x36u)
            {
                store_vi(state, is, state.m_vi[is] - 1u);
            }
            const uint8_t* bytes = data.data() + qword_offset(state.m_vi[is]);
            if (ft != 0u)
            {
                for (uint32_t lane = 0u; lane < 4u; ++lane)
                {
                    if ((dest & (8u >> lane)) != 0u)
                    {
                        state.m_vf[ft][lane] = read32(bytes + lane * 4u);
                    }
                }
            }
            if (index == 0x34u)
            {
                store_vi(state, is, state.m_vi[is] + 1u);
            }
            return true;
        }
        case 0x35u: // SQI
        case 0x37u: // SQD
        {
            if (index == 0x37u)
            {
                store_vi(state, it, state.m_vi[it] - 1u);
            }
            uint8_t* bytes = data.data() + qword_offset(state.m_vi[it]);
            for (uint32_t lane = 0u; lane < 4u; ++lane)
            {
                if ((dest & (8u >> lane)) != 0u)
                {
                    const uint32_t value = state.m_vf[fs][lane];
                    std::memcpy(bytes + lane * 4u, &value, sizeof(value));
                }
            }
            if (index == 0x35u)
            {
                store_vi(state, it, state.m_vi[it] + 1u);
            }
            return true;
        }
        case 0x38u: // DIV
        {
            const uint32_t fsf = (lo >> 21u) & 3u;
            const uint32_t ftf = (lo >> 23u) & 3u;
            const auto result = vu1_fp::div(
                state.m_vf[fs][fsf], state.m_vf[ft][ftf]);
            pipe.schedule_q(state, result.value, 7u, result.current_flags);
            return true;
        }
        case 0x39u: // SQRT
        {
            const uint32_t ftf = (lo >> 23u) & 3u;
            const uint32_t raw = state.m_vf[ft][ftf];
            pipe.schedule_q(state, vu1_fp::sqrt(raw), 7u,
                vu_scalar_fp::current_flags(0u, raw, true, false));
            return true;
        }
        case 0x3au: // RSQRT
        {
            const uint32_t fsf = (lo >> 21u) & 3u;
            const uint32_t ftf = (lo >> 23u) & 3u;
            const auto result = vu1_fp::rsqrt(
                state.m_vf[fs][fsf], state.m_vf[ft][ftf]);
            pipe.schedule_q(state, result.value, 13u, result.current_flags);
            return true;
        }
        case 0x3bu: pipe.wait_q(state); return true; // WAITQ
        case 0x3cu: // MTIR
            store_vi(state, it, state.m_vf[fs][(lo >> 21u) & 3u]);
            return true;
        case 0x3du: // MFIR
        {
            const uint32_t value = static_cast<uint32_t>(
                static_cast<int32_t>(static_cast<int16_t>(state.m_vi[is])));
            if (ft != 0u)
            {
                for (uint32_t lane = 0u; lane < 4u; ++lane)
                {
                    if ((dest & (8u >> lane)) != 0u)
                    {
                        state.m_vf[ft][lane] = value;
                    }
                }
            }
            return true;
        }
        case 0x3eu: // ILWR
        {
            const uint8_t* bytes = data.data() + qword_offset(state.m_vi[is]);
            for (uint32_t lane = 0u; lane < 4u; ++lane)
            {
                if ((dest & (8u >> lane)) != 0u)
                {
                    store_vi(state, it, read32(bytes + lane * 4u) & 0xffffu);
                    break;
                }
            }
            return true;
        }
        case 0x3fu: // ISWR
        {
            uint8_t* bytes = data.data() + qword_offset(state.m_vi[is]);
            const uint32_t value = state.m_vi[it];
            for (uint32_t lane = 0u; lane < 4u; ++lane)
            {
                if ((dest & (8u >> lane)) != 0u)
                {
                    std::memcpy(bytes + lane * 4u, &value, sizeof(value));
                }
            }
            return true;
        }
        case 0x40u: // RNEXT
            state.m_r = ((state.m_r << 1u) |
                (((state.m_r >> 4u) ^ (state.m_r >> 22u)) & 1u)) & 0x7fffffu;
            [[fallthrough]];
        case 0x41u: // RGET
            if (ft != 0u)
            {
                for (uint32_t lane = 0u; lane < 4u; ++lane)
                {
                    if ((dest & (8u >> lane)) != 0u)
                    {
                        state.m_vf[ft][lane] =
                            (state.m_r & 0x7fffffu) | vu1_fp::kOne;
                    }
                }
            }
            return true;
        case 0x42u: // RINIT
            state.m_r = state.m_vf[fs][(lo >> 21u) & 3u] & 0x7fffffu;
            return true;
        case 0x43u: // RXOR
            state.m_r = (state.m_r ^
                state.m_vf[fs][(lo >> 21u) & 3u]) & 0x7fffffu;
            return true;
        case 0x64u: // MFP reads the previous P until the EFU completes
            if (ft != 0u)
            {
                for (uint32_t lane = 0u; lane < 4u; ++lane)
                {
                    if ((dest & (8u >> lane)) != 0u)
                    {
                        state.m_vf[ft][lane] = state.m_p;
                    }
                }
            }
            return true;
        case 0x7bu: pipe.wait_p(state); return true; // WAITP
        default:
        {
            uint32_t value = 0u;
            uint32_t latency = 0u;
            if (!efu_result(state.m_vf[fs], index, (lo >> 21u) & 3u,
                            value, latency))
            {
                return false;
            }
            pipe.schedule_p(state, value, latency);
            return true;
        }
        case 0x68u: store_vi(state, it, top); return true;
        case 0x69u: store_vi(state, it, itop); return true;
        case 0x6cu: return xgkick(data, state.m_vi[is], path1_bytes, issue);
        }
    }

    if (major == 0x00u || major == 0x01u)
    {
        uint8_t* bytes = data.data() + qword_offset(
            state.m_vi[major == 0u ? is : it] +
            static_cast<uint32_t>(imm11));
        if (major == 0u)
        {
            if (ft != 0u)
            {
                for (uint32_t lane = 0u; lane < 4u; ++lane)
                {
                    if ((dest & (8u >> lane)) != 0u)
                    {
                        state.m_vf[ft][lane] = read32(bytes + lane * 4u);
                    }
                }
            }
        }
        else
        {
            for (uint32_t lane = 0u; lane < 4u; ++lane)
            {
                if ((dest & (8u >> lane)) != 0u)
                {
                    const uint32_t value = state.m_vf[fs][lane];
                    std::memcpy(bytes + lane * 4u, &value, sizeof(value));
                }
            }
        }
        return true;
    }

    // ILW/ISW use signed 11-bit quadword offsets, unlike the unsigned
    // immediate arithmetic below. VU1 memory wraps at 1024 quadwords.
    // VI values are 16-bit; ISW zero-extends each selected 32-bit lane.
    if (major == 0x04u || major == 0x05u)
    {
        uint8_t* bytes = data.data() + qword_offset(
            state.m_vi[is] + static_cast<uint32_t>(imm11));
        for (uint32_t lane = 0u; lane < 4u; ++lane)
        {
            if ((dest & (8u >> lane)) != 0u)
            {
                if (major == 0x04u)
                {
                    // Selected lanes are processed in XYZW order.
                    store_vi(state, it, read32(bytes + lane * 4u) & 0xffffu);
                }
                else
                {
                    const uint32_t value = state.m_vi[it];
                    std::memcpy(bytes + lane * 4u, &value, sizeof(value));
                }
            }
        }
        return true;
    }

    switch (major)
    {
    case 0x08u:
        store_vi(state, it, state.m_vi[is] +
            (((lo >> 10u) & 0x7800u) | (lo & 0x7ffu)));
        return true;
    case 0x09u:
        store_vi(state, it, state.m_vi[is] -
            (((lo >> 10u) & 0x7800u) | (lo & 0x7ffu)));
        return true;
    case 0x10u: // FCEQ
        store_vi(state, 1u,
            (pipe.clip() & 0xffffffu) == (lo & 0xffffffu) ? 1u : 0u);
        return true;
    case 0x11u: // FCSET; visible immediately to subsequent flag reads
        state.m_clip = lo & 0xffffffu;
        pipe.set_clip(state.m_clip, pipe.m_t - 4u);
        return true;
    case 0x12u: // FCAND
        store_vi(state, 1u, (pipe.clip() & lo & 0xffffffu) != 0u ? 1u : 0u);
        return true;
    case 0x13u: // FCOR
        store_vi(state, 1u,
            ((pipe.clip() | lo) & 0xffffffu) == 0xffffffu ? 1u : 0u);
        return true;
    case 0x14u: // FSEQ
        store_vi(state, it,
            (pipe.status(state) & 0xfffu) ==
            (((lo >> 10u) & 0x800u) | (lo & 0x7ffu)) ? 1u : 0u);
        return true;
    case 0x15u: // FSSET sticky flags update after same-cycle upper
        fsset = static_cast<int32_t>(
            (((lo >> 10u) & 0x800u) | (lo & 0x7ffu)) & 0xfc0u);
        return true;
    case 0x16u: // FSAND
        store_vi(state, it, pipe.status(state) &
            (((lo >> 10u) & 0x800u) | (lo & 0x7ffu)));
        return true;
    case 0x17u: // FSOR
        store_vi(state, it, (pipe.status(state) & 0xfffu) |
            (((lo >> 10u) & 0x800u) | (lo & 0x7ffu)));
        return true;
    case 0x18u: // FMEQ
        store_vi(state, it, (pipe.mac() & 0xffffu) ==
            state.m_vi[is] ? 1u : 0u);
        return true;
    case 0x1au: // FMAND
        store_vi(state, it, pipe.mac() & state.m_vi[is]);
        return true;
    case 0x1bu: // FMOR
        store_vi(state, it, (pipe.mac() & 0xffffu) |
            state.m_vi[is]);
        return true;
    case 0x1cu: // FCGET
        store_vi(state, it, pipe.clip() & 0xfffu);
        return true;
    case 0x20u: branch_target = target; branched = true; return true;
    case 0x21u:
        store_vi(state, it, (pc + 16u) / 8u);
        branch_target = target;
        branched = true;
        return true;
    case 0x24u: branch_target = state.m_vi[is] * 8u; branched = true; return true;
    case 0x25u:
        branch_target = state.m_vi[is] * 8u;
        store_vi(state, it, (pc + 16u) / 8u);
        branched = true;
        return true;
    case 0x28u: branched = branch_vi[is] == branch_vi[it]; break;
    case 0x29u: branched = branch_vi[is] != branch_vi[it]; break;
    case 0x2cu: branched = static_cast<int16_t>(branch_vi[is]) < 0; break;
    case 0x2du: branched = static_cast<int16_t>(branch_vi[is]) > 0; break;
    case 0x2eu: branched = static_cast<int16_t>(branch_vi[is]) <= 0; break;
    case 0x2fu: branched = static_cast<int16_t>(branch_vi[is]) >= 0; break;
    default: return false;
    }
    if (branched)
    {
        branch_target = target;
    }
    return true;
}
} // namespace

bool c_ee_vu1::kick(const std::array<uint8_t, k_memory_size>& data,
    uint32_t address, std::vector<uint8_t>& output, s_ee_vu1_issue& issue)
{
    return xgkick(data, address, output, issue);
}

bool c_ee_vu1::efu(const std::array<uint32_t, 4>& source, uint32_t index,
    uint32_t component, uint32_t& value, uint32_t& latency)
{
    return component < 4u && efu_result(source, index, component, value, latency);
}

struct s_ee_vu1_execution
{
    explicit s_ee_vu1_execution(const s_ee_vu1_state& state, uint32_t address)
        : m_pc(address & 0x3ff8u), m_npc((m_pc + 8u) & 0x3fffu),
          m_previous_vi(state.m_vi)
    {
        m_pipe.start(state);
    }

    uint32_t m_pc;
    uint32_t m_npc;
    std::array<uint16_t, 16> m_previous_vi;
    bool m_previous_lower = false;
    bool m_ending = false;
    int32_t m_fsset = -1;
    s_flag_pipe m_pipe{};
    s_ee_vu1_dependency m_dependencies{};
};

static void initialize_vu1(s_ee_vu1_state& state)
{
    if (!state.m_initialized)
    {
        state = {};
        state.m_vf[0][3] = 0x3f800000u;
        state.m_initialized = true;
    }
}

std::shared_ptr<s_ee_vu1_execution> c_ee_vu1::begin_execution(
    s_ee_vu1_state& state, uint32_t address)
{
    if ((address & 7u) != 0u)
    {
        return {};
    }
    initialize_vu1(state);
    state.m_tpc = address & 0x3ff8u;
    return std::make_shared<s_ee_vu1_execution>(state, address);
}

bool c_ee_vu1::run(
    s_ee_vu1_state& state,
    std::array<uint8_t, k_memory_size>& code,
    std::array<uint8_t, k_memory_size>& data,
    uint32_t address, uint32_t top, uint32_t itop,
    std::vector<uint8_t>& path1_bytes, s_ee_vu1_issue* issue)
{
    if (issue == nullptr || (address & 7u) != 0u)
    {
        return false;
    }
    *issue = {};
    c_runtime_profile_scope profile_scope(e_runtime_profile_id::vu1_run, 0u, &issue->m_steps);
    initialize_vu1(state);

    if (g_vu1_aot != nullptr)
    {
        g_vu1_aot->capture_on_entry(code, address);
        if (g_vu1_aot->try_run(state, code, data, address, top, itop, path1_bytes, *issue))
        {
            return true;
        }
    }
    s_ee_vu1_execution execution(state, address);
    return run_slice(execution, state, code, data, top, itop,
                     k_max_steps, path1_bytes, issue) ==
        e_ee_vu1_slice::completed;
}

e_ee_vu1_slice c_ee_vu1::run_slice(
    s_ee_vu1_execution& execution, s_ee_vu1_state& state,
    std::array<uint8_t, k_memory_size>& code,
    std::array<uint8_t, k_memory_size>& data,
    uint32_t top, uint32_t itop, uint32_t pair_budget,
    std::vector<uint8_t>& path1_bytes, s_ee_vu1_issue* issue)
{
    if (issue == nullptr || pair_budget == 0u || pair_budget > k_max_steps ||
        execution.m_pipe.now() > 0x7f000000u)
    {
        return e_ee_vu1_slice::rejected;
    }
    *issue = {};
    auto& pc = execution.m_pc;
    auto& npc = execution.m_npc;
    auto& previous_vi = execution.m_previous_vi;
    auto& previous_lower = execution.m_previous_lower;
    auto& ending = execution.m_ending;
    auto& fsset = execution.m_fsset;
    auto& pipe = execution.m_pipe;
    auto& dependencies = execution.m_dependencies;
    const uint32_t slice_start = pipe.now();
    // The decode cache remains thread-local and checks the exact words.
    static thread_local c_ee_vu1_decode_cache decode_cache;

    for (uint32_t step = 0u; step < pair_budget; ++step)
    {
        const uint32_t lo = read32(code.data() + pc);
        const uint32_t hi = read32(code.data() + pc + 4u);
        issue->m_pc = pc;
        issue->m_lower = lo;
        issue->m_upper = hi;
        issue->m_steps = step + 1u;

        // D/T debug interrupts require guest-visible delivery, not NOPs.
        if ((hi & 0x18000000u) != 0u)
        {
            return e_ee_vu1_slice::rejected;
        }

        ++pipe.m_t;
        const s_ee_vu1_use& use = decode_cache.lookup(pc, lo, hi);
        const uint32_t issued = dependencies.issue(use, pipe.now());
        if (issued > pipe.now())
        {
            pipe.m_stalls += issued - pipe.now();
        }
        pipe.sync_q(state);
        pipe.sync_p(state);
        s_upper_write pending{};
        if (!upper(state, hi, pending))
        {
            return e_ee_vu1_slice::rejected;
        }

        const auto before = state.m_vi;
        const uint32_t current = pc;
        pc = npc;
        npc = (npc + 8u) & 0x3fffu;

        if ((hi & 0x80000000u) != 0u)
        {
            state.m_i = lo;
        }
        else
        {
            uint32_t target = 0u;
            bool branched = false;
            if (!lower(state, lo, current, data, top, itop,
                       previous_lower ? previous_vi : state.m_vi,
                       path1_bytes, target, branched, pipe, fsset, *issue))
            {
                return e_ee_vu1_slice::rejected;
            }
            if (branched)
            {
                npc = target & 0x3fffu;
            }
        }

        dependencies.retire(use, issued);
        if (pending.m_writes)
        {
            if (pending.m_acc || pending.m_target != 0u)
            {
                auto& target = pending.m_acc
                    ? state.m_acc : state.m_vf[pending.m_target];
                for (uint32_t lane = 0u; lane < 4u; ++lane)
                {
                    if ((pending.m_mask & (8u >> lane)) != 0u)
                    {
                        target[lane] = pending.m_values[lane];
                    }
                }
            }
            if (pending.m_flags)
            {
                if (pending.m_acc)
                {
                    state.m_acc_overflow_mask = (state.m_acc_overflow_mask & ~pending.m_mask) |
                        pending.m_overflow;
                }
                set_mac_flags(state, pending.m_values, pending.m_mask,
                    pending.m_overflow, pending.m_underflow, pending.m_product_sticky);
                pipe.set_mac(state.m_mac, state.m_status);
            }
        }
        if (fsset >= 0)
        {
            state.m_status = (state.m_status & 0x3fu) |
                static_cast<uint32_t>(fsset);
            pipe.force_mac(state.m_mac, state.m_status);
            fsset = -1;
        }
        if (pending.m_has_clip)
        {
            state.m_clip = pending.m_clip;
            pipe.set_clip(state.m_clip, pipe.now());
        }
        state.m_vi[0] = 0u;
        state.m_vf[0] = {0u, 0u, 0u, 0x3f800000u};
        previous_vi = before;
        previous_lower = (hi & 0x80000000u) == 0u &&
            !branch_delay_exempt(lo);
        issue->m_cycles = pipe.now() - slice_start;

        if (ending)
        {
            pipe.wait_q(state);
            pipe.wait_p(state);
            issue->m_cycles = pipe.now() - slice_start;
            state.m_tpc = pc;
            return e_ee_vu1_slice::completed;
        }
        if ((hi & 0x40000000u) != 0u)
        {
            ending = true; // E-bit takes effect AFTER the delay-slot pair.
        }
    }
    state.m_tpc = pc;
    return e_ee_vu1_slice::pending; // Keep branch, flag, Q/P and E-bit state.
}
} // namespace ee
