// Adapted from ps2-web-recomp runtime/src/ipu/ipu.cpp (MIT, see
// LICENSE.upstream in this directory). VLC tables in tables.hpp are from
// Play! (BSD-2-Clause, see NOTICE). Behaviour is kept identical; global state
// became members and DMA/INTC go through this runtime's device models.

#include "ee_ipu.h"

#include "ee_dmac.h"
#include "ee_intc.h"
#include "tables.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace ee
{
namespace
{
using namespace ps2::ipu::tables;

constexpr uint32_t k_ecd = 0x4000u;
constexpr uint32_t k_scd = 0x8000u;
constexpr uint32_t k_reset = 0x40000000u;
constexpr size_t k_fifo_bytes = 128u;

constexpr uint8_t k_default_intra[64] = {
    8,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83,
};

constexpr uint8_t k_non_linear_scale[32] = {
    0,  1,  2,  3,  4,  5,  6,  7,  8,  10, 12, 14, 16, 18, 20, 22,
    24, 28, 32, 36, 40, 44, 48, 52, 56, 64, 72, 80, 88, 96, 104, 112,
};

struct s_entry
{
    uint8_t length = 0u;
    uint32_t value = 0u;
};

struct s_lookup
{
    unsigned bits = 0u;
    std::vector<s_entry> entries;
};

struct s_dct_entry
{
    uint8_t length = 0u;
    uint8_t run = 0u;
    int16_t level = 0;
};

struct s_dct_lookup
{
    unsigned bits = 0u;
    std::vector<s_dct_entry> entries;
};

template <size_t N>
s_lookup build(const Vlc (&table)[N])
{
    s_lookup lookup;
    for (const Vlc& e : table) lookup.bits = std::max<unsigned>(lookup.bits, e.length);
    lookup.entries.resize(size_t{1} << lookup.bits);
    for (const Vlc& e : table)
    {
        const unsigned shift = lookup.bits - e.length;
        for (uint32_t i = 0; i < (1u << shift); ++i)
            lookup.entries[(static_cast<uint32_t>(e.code) << shift) | i] = {e.length, e.value};
    }
    return lookup;
}

template <size_t N>
s_dct_lookup build(const Dct (&table)[N])
{
    s_dct_lookup lookup;
    for (const Dct& e : table) lookup.bits = std::max<unsigned>(lookup.bits, e.length);
    lookup.entries.resize(size_t{1} << lookup.bits);
    for (const Dct& e : table)
    {
        const unsigned shift = lookup.bits - e.length;
        for (uint32_t i = 0; i < (1u << shift); ++i)
            lookup.entries[(static_cast<uint32_t>(e.code) << shift) | i] = {e.length, e.run, e.level};
    }
    return lookup;
}

enum e_table : int
{
    k_mbai, k_mb_i, k_mb_p, k_mb_b, k_motion, k_dmv, k_dc_luma, k_dc_chroma, k_cbp,
};

struct s_tables
{
    s_lookup vlc[9] = {
        build(kMbAddressIncrement), build(kMbTypeI), build(kMbTypeP), build(kMbTypeB),
        build(kMotionCode), build(kDmVector), build(kDcSizeLuma), build(kDcSizeChroma),
        build(kCodedBlockPattern),
    };
    s_dct_lookup dct0 = build(kDctTable0);
    s_dct_lookup dct1 = build(kDctTable1);
    double cosines[8][8];

    s_tables()
    {
        for (int x = 0; x < 8; ++x)
            for (int u = 0; u < 8; ++u)
                cosines[x][u] = (u ? 0.5 : 0.5 / std::sqrt(2.0)) *
                    std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0);
    }
};

const s_tables& tables()
{
    static const s_tables t;
    return t;
}

enum e_command : uint32_t
{
    k_bclr, k_idec, k_bdec, k_vdec, k_fdec, k_setiq, k_setvq, k_csc, k_pack, k_setth,
};

void idct(const int* in, int16_t* out)
{
    const auto& c = tables().cosines;
    double rows[8][8];
    for (int v = 0; v < 8; ++v)
        for (int x = 0; x < 8; ++x)
        {
            double s = 0;
            for (int u = 0; u < 8; ++u) s += c[x][u] * in[v * 8 + u];
            rows[v][x] = s;
        }
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
        {
            double s = 0;
            for (int v = 0; v < 8; ++v) s += c[y][v] * rows[v][x];
            out[y * 8 + x] = static_cast<int16_t>(
                std::clamp(static_cast<int>(std::floor(s + 0.5)), -256, 255));
        }
}
} // namespace

c_ee_ipu::c_ee_ipu()
{
    reset();
    // SDBZ_TRACE_IPU=N logs the first N commands and DMA starts.
    if (const char* trace_env = std::getenv("SDBZ_TRACE_IPU"))
    {
        m_trace_limit = std::strtoull(trace_env, nullptr, 10);
    }
}

void c_ee_ipu::trace(const char* event, uint32_t value)
{
    if (m_traced >= m_trace_limit)
    {
        return;
    }
    ++m_traced;
    std::fprintf(stderr,
        "[ipu] %s 0x%08X result=0x%08X ctrl=0x%08X busy=%d decoding=%d "
        "starved=%d in=%zu bit=%zu out=%zu phase=%d\n",
        event, value, m_result, m_ctrl, m_busy ? 1 : 0, m_decoding ? 1 : 0,
        m_starved ? 1 : 0, m_in.size(), m_bit, m_out.size() - m_out_pos, m_phase);
}

bool c_ee_ipu::pull()
{
    uint8_t q[16];
    if (m_dmac == nullptr || !m_dmac->ipu_pull(q)) return false;
    m_in.insert(m_in.end(), q, q + 16);
    return true;
}

bool c_ee_ipu::have(size_t bits)
{
    while (m_in.size() * 8 < m_bit + bits)
        if (!pull()) return false;
    return true;
}

size_t c_ee_ipu::available() const
{
    return m_in.size() * 8 > m_bit ? m_in.size() * 8 - m_bit : 0;
}

uint32_t c_ee_ipu::bits_at(size_t bit, unsigned count) const
{
    if (!count) return 0;
    uint64_t v = 0;
    const size_t byte = bit >> 3;
    for (size_t i = 0; i < 8; ++i) v = (v << 8) | (byte + i < m_in.size() ? m_in[byte + i] : 0u);
    return static_cast<uint32_t>((v << (bit & 7u)) >> (64 - count));
}

uint32_t c_ee_ipu::peek(unsigned count)
{
    if (!have(count)) throw s_need_data{};
    return bits_at(m_bit, count);
}

uint32_t c_ee_ipu::get(unsigned count)
{
    const uint32_t v = peek(count);
    m_bit += count;
    return v;
}

void c_ee_ipu::skip(unsigned count)
{
    if (!have(count)) throw s_need_data{};
    m_bit += count;
}

uint32_t c_ee_ipu::peek_soft(unsigned count, size_t& available_bits)
{
    have(count);
    available_bits = available();
    return bits_at(m_bit, count);
}

uint32_t c_ee_ipu::decode_vlc(int table)
{
    const s_lookup& lookup = tables().vlc[table];
    size_t avail;
    const s_entry& e = lookup.entries[peek_soft(lookup.bits, avail)];
    if (!e.length)
    {
        if (avail < lookup.bits) throw s_need_data{};
        throw s_bad_code{};
    }
    if (e.length > avail) throw s_need_data{};
    m_bit += e.length;
    return e.value;
}

uint32_t c_ee_ipu::peek_now() const
{
    const unsigned n = static_cast<unsigned>(std::min<size_t>(32, available()));
    return n ? bits_at(m_bit, n) << (32 - n) : 0u;
}

void c_ee_ipu::commit()
{
    const size_t drop = std::min(m_bit / 128 * 16, m_in.size() / 16 * 16);
    m_in.erase(m_in.begin(), m_in.begin() + static_cast<std::ptrdiff_t>(drop));
    m_bit -= drop * 8;
}

void c_ee_ipu::top_up()
{
    while (m_in.size() < k_fifo_bytes && pull()) {}
}

int c_ee_ipu::read_dc_diff(int channel)
{
    const unsigned size = decode_vlc(channel ? k_dc_chroma : k_dc_luma) & 0xFFFFu;
    if (!size) return 0;
    const int v = static_cast<int>(get(size));
    return v < (1 << (size - 1)) ? v + 1 - (1 << size) : v;
}

void c_ee_ipu::read_coefficient(bool table_one, int& run, int& level)
{
    const s_dct_lookup& lookup = table_one ? tables().dct1 : tables().dct0;
    size_t avail;
    const s_dct_entry& e = lookup.entries[peek_soft(lookup.bits, avail)];
    if (!e.length)
    {
        if (avail < lookup.bits) throw s_need_data{};
        throw s_bad_code{};
    }
    if (e.length > avail) throw s_need_data{};
    m_bit += e.length;
    if (e.run == kEscape)
    {
        run = static_cast<int>(get(6));
        if (mpeg1())
        {
            int v = static_cast<int>(get(8));
            if (v == 0) v = static_cast<int>(get(8));
            else if (v == 128) v = static_cast<int>(get(8)) - 256;
            else if (v > 128) v -= 256;
            level = v;
        }
        else
        {
            const int v = static_cast<int>(get(12));
            level = v & 0x800 ? v - 0x1000 : v;
        }
        return;
    }
    run = e.run;
    level = get(1) ? -e.level : e.level;
}

void c_ee_ipu::decode_block(int16_t* out, int channel, bool intra, uint32_t qsc)
{
    int coef[64] = {};
    int index = 0;
    const bool table_one = intra && intra_vlc_one();
    if (intra)
    {
        m_dc[channel] = static_cast<int16_t>(m_dc[channel] + read_dc_diff(channel));
        coef[0] = m_dc[channel];
        index = 1;
    }
    else if (peek(1))
    {
        coef[0] = get(2) & 1u ? -1 : 1;
        index = 1;
    }
    for (;;)
    {
        if (index)
        {
            if (table_one ? peek(4) == 6u : peek(2) == 2u)
            {
                skip(table_one ? 4 : 2);
                break;
            }
        }
        int run = 0, level = 0;
        read_coefficient(table_one, run, level);
        index += run;
        if (index >= 64) throw s_bad_code{};
        coef[index++] = level;
    }
    const uint8_t* scan = alternate_scan() ? kAlternate : kZigzag;
    const uint8_t* weights = intra ? m_intra_iq : m_nonintra_iq;
    const int scale = mpeg1() || !nonlinear_q()
        ? static_cast<int>(qsc) * 2 : k_non_linear_scale[qsc & 31u];
    int f[64] = {};
    int sum = 0;
    for (int k = 0; k < 64; ++k)
    {
        const int v = coef[k];
        const int r = scan[k];
        int x;
        if (intra && k == 0)
        {
            x = v * (8 >> dc_precision());
        }
        else
        {
            if (!v) continue;
            const int sign = v > 0 ? 1 : -1;
            x = intra ? (v * weights[r] * scale * 2) / 32
                      : ((2 * v + sign) * weights[r] * scale) / 32;
            if (mpeg1() && x && !(x & 1)) x -= sign;
        }
        x = std::clamp(x, -2048, 2047);
        f[r] = x;
        sum += x;
    }
    if (!mpeg1() && !(sum & 1)) f[63] += f[63] & 1 ? -1 : 1;
    idct(f, out);
}

void c_ee_ipu::decode_macroblock(int16_t* raw, bool intra, bool field,
                                 bool dc_reset, uint32_t qsc)
{
    const unsigned cbp = intra ? 0x3Fu : decode_vlc(k_cbp) & 0xFFu;
    if (dc_reset) m_dc[0] = m_dc[1] = m_dc[2] = static_cast<int16_t>(128 << dc_precision());
    int16_t blocks[6][64] = {};
    for (int b = 0; b < 6; ++b)
        if (cbp & (32u >> b)) decode_block(blocks[b], b < 4 ? 0 : b - 3, intra, qsc);
    for (int i = 0; i < 8; ++i)
    {
        const int top = field ? 2 * i : i;
        const int bottom = field ? 2 * i + 1 : i + 8;
        std::memcpy(raw + top * 16, blocks[0] + i * 8, 16);
        std::memcpy(raw + top * 16 + 8, blocks[1] + i * 8, 16);
        std::memcpy(raw + bottom * 16, blocks[2] + i * 8, 16);
        std::memcpy(raw + bottom * 16 + 8, blocks[3] + i * 8, 16);
    }
    std::memcpy(raw + 256, blocks[4], 128);
    std::memcpy(raw + 320, blocks[5], 128);
}

void c_ee_ipu::convert(const uint8_t* raw, bool rgb16)
{
    uint8_t pixels[1024];
    size_t size = 0;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
        {
            const int lum = 149 * std::max(0, raw[y * 16 + x] - 16);
            const int cb = raw[256 + (y / 2) * 8 + x / 2] - 128;
            const int cr = raw[320 + (y / 2) * 8 + x / 2] - 128;
            const int r = std::clamp((lum + 204 * cr + 64) >> 7, 0, 255);
            const int g = std::clamp((lum - 50 * cb - 104 * cr + 64) >> 7, 0, 255);
            const int b = std::clamp((lum + 258 * cb + 64) >> 7, 0, 255);
            int a = 0x80;
            if (r < m_th0 && g < m_th0 && b < m_th0) a = 0;
            else if (r < m_th1 && g < m_th1 && b < m_th1) a = 0x40;
            if (rgb16)
            {
                const uint16_t p = static_cast<uint16_t>(
                    (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | (a & 0x80 ? 0x8000 : 0));
                std::memcpy(pixels + size, &p, 2);
                size += 2;
            }
            else
            {
                const uint8_t p[4] = {static_cast<uint8_t>(r), static_cast<uint8_t>(g),
                                      static_cast<uint8_t>(b), static_cast<uint8_t>(a)};
                std::memcpy(pixels + size, p, 4);
                size += 4;
            }
        }
    m_out.insert(m_out.end(), pixels, pixels + size);
}

void c_ee_ipu::emit_raw16(const int16_t* raw)
{
    const auto* bytes = reinterpret_cast<const uint8_t*>(raw);
    m_out.insert(m_out.end(), bytes, bytes + 768);
}

bool c_ee_ipu::start_code_follows()
{
    size_t avail;
    const uint32_t next = peek_soft(8, avail);
    return avail >= 8 && next == 0;
}

template <typename t_body>
bool c_ee_ipu::atomic(t_body&& body)
{
    const size_t bit = m_bit;
    int16_t dc[3];
    std::memcpy(dc, m_dc, sizeof dc);
    const size_t out = m_out.size();
    try
    {
        body();
        return true;
    }
    catch (const s_need_data&)
    {
        ++m_stalls;
        m_bit = bit;
        std::memcpy(m_dc, dc, sizeof dc);
        m_out.resize(out);
        return false;
    }
}

bool c_ee_ipu::run_idec()
{
    const bool dtd = (m_command >> 24) & 1u;
    const bool rgb16 = (m_command >> 27) & 1u;
    for (;;)
    {
        bool ok = true;
        switch (m_phase)
        {
        case 0:
            ok = atomic([&] { skip(m_command & 0x3Fu); });
            if (ok) m_phase = 1;
            break;
        case 1:
            ok = atomic([&] {
                const uint32_t type = decode_vlc(k_mb_i) & 0xFFFFu;
                const bool field = dtd && get(1);
                uint32_t qsc = m_qsc;
                if (type & 0x10u) qsc = get(5);
                int16_t raw[384];
                decode_macroblock(raw, true, field, m_first, qsc);
                uint8_t raw8[384];
                for (int i = 0; i < 384; ++i)
                    raw8[i] = static_cast<uint8_t>(std::clamp<int>(raw[i], 0, 255));
                convert(raw8, rgb16);
                m_qsc = qsc;
            });
            if (ok)
            {
                m_first = false;
                m_phase = 2;
            }
            break;
        case 2:
            ok = atomic([&] {
                size_t avail;
                const uint32_t next = peek_soft(8, avail);
                if (avail < 8) throw s_need_data{};
                if (next == 0)
                {
                    m_bit = (m_bit + 7) & ~size_t{7};
                    m_phase = 3;
                    return;
                }
                uint32_t v;
                do v = decode_vlc(k_mbai) & 0xFFFFu;
                while (v == 0x22u || v == 0x23u);
                m_phase = 1;
            });
            break;
        case 3:
            ok = atomic([&] {
                for (;;)
                {
                    size_t avail;
                    const uint32_t code = peek_soft(24, avail);
                    if (avail < 24 || code == 1)
                    {
                        m_phase = 4;
                        return;
                    }
                    if (code != 0) throw s_bad_code{};
                    m_bit += 8;
                }
            });
            break;
        default:
            m_ctrl |= k_scd;
            return true;
        }
        if (!ok) return false;
        commit();
    }
}

bool c_ee_ipu::run_command()
{
    const uint32_t cmd = m_command;
    switch (cmd >> 28)
    {
    case k_bclr:
        m_in.clear();
        m_bit = cmd & 0x7Fu;
        return true;
    case k_idec:
        return run_idec();
    case k_bdec:
    {
        const bool ok = atomic([&] {
            skip(cmd & 0x3Fu);
            int16_t raw[384];
            decode_macroblock(raw, (cmd >> 27) & 1u, (cmd >> 25) & 1u,
                              (cmd >> 26) & 1u, (cmd >> 16) & 0x1Fu);
            emit_raw16(raw);
        });
        if (!ok) return false;
        commit();
        if (start_code_follows())
        {
            m_ctrl |= k_scd;
            m_interrupt = false;
        }
        return true;
    }
    case k_vdec:
    {
        const bool ok = atomic([&] {
            skip(cmd & 0x3Fu);
            int table = k_mbai;
            switch ((cmd >> 26) & 3u)
            {
            case 0: table = k_mbai; break;
            case 1:
                switch (picture_type())
                {
                case 1: table = k_mb_i; break;
                case 2: table = k_mb_p; break;
                case 3: table = k_mb_b; break;
                default: throw s_bad_code{};
                }
                break;
            case 2: table = k_motion; break;
            default: table = k_dmv; break;
            }
            m_result = decode_vlc(table);
        });
        if (ok) commit();
        return ok;
    }
    case k_fdec:
    {
        const bool ok = atomic([&] {
            skip(cmd & 0x3Fu);
            m_result = peek(32);
        });
        if (ok) commit();
        return ok;
    }
    case k_setiq:
    {
        uint8_t* matrix = (cmd >> 27) & 1u ? m_nonintra_iq : m_intra_iq;
        uint8_t values[64];
        const bool ok = atomic([&] {
            skip(cmd & 0x3Fu);
            for (uint8_t& v : values) v = static_cast<uint8_t>(get(8));
        });
        if (!ok) return false;
        for (int k = 0; k < 64; ++k) matrix[kZigzag[k]] = values[k];
        commit();
        return true;
    }
    case k_setvq:
    {
        uint16_t values[16];
        const bool ok = atomic([&] {
            skip(cmd & 0x3Fu);
            for (uint16_t& v : values) v = static_cast<uint16_t>(get(16));
        });
        if (!ok) return false;
        std::memcpy(m_vqclut, values, sizeof values);
        commit();
        return true;
    }
    case k_csc:
        while (m_count)
        {
            uint8_t raw[384];
            const bool ok = atomic([&] {
                for (uint8_t& v : raw) v = static_cast<uint8_t>(get(8));
                convert(raw, (cmd >> 27) & 1u);
            });
            if (!ok) return false;
            commit();
            --m_count;
        }
        return true;
    case k_setth:
        m_th0 = static_cast<uint16_t>(cmd & 0x1FFu);
        m_th1 = static_cast<uint16_t>((cmd >> 16) & 0x1FFu);
        return true;
    default:
        std::fprintf(stderr, "[ipu] command 0x%08X is not implemented\n", cmd);
        return true;
    }
}

void c_ee_ipu::drain()
{
    while (m_out_pos + 16 <= m_out.size() && m_dmac != nullptr &&
           m_dmac->ipu_push(m_out.data() + m_out_pos))
    {
        m_out_pos += 16;
        ++m_out_qwords;
    }
    if (m_out_pos == m_out.size())
    {
        m_out.clear();
        m_out_pos = 0;
    }
}

void c_ee_ipu::step()
{
    if (m_busy && m_decoding)
    {
        try
        {
            m_starved = !run_command();
            if (!m_starved) m_decoding = false;
        }
        catch (const s_bad_code&)
        {
            ++m_bad_codes;
            std::fprintf(stderr, "[ipu] bad code in command 0x%08X\n", m_command);
            m_ctrl |= k_ecd;
            m_decoding = false;
            m_busy = false;
        }
    }
    drain();
    if (m_busy && !m_decoding && m_out.empty())
    {
        m_busy = false;
        if (m_interrupt && m_intc != nullptr) (void)m_intc->raise(k_intc_cause);
    }
    top_up();
}

void c_ee_ipu::reset()
{
    m_in.clear();
    m_out.clear();
    m_bit = m_out_pos = 0;
    m_ctrl = m_result = m_command = 0;
    m_last = -1;
    m_busy = m_decoding = false;
    std::memcpy(m_intra_iq, k_default_intra, 64);
    std::fill(std::begin(m_nonintra_iq), std::end(m_nonintra_iq), 16);
    std::memset(m_vqclut, 0, sizeof m_vqclut);
    m_th0 = m_th1 = 0;
    std::memset(m_dc, 0, sizeof m_dc);
}

uint32_t c_ee_ipu::read32(uint32_t address)
{
    switch (address & 0xFFFFFFFCu)
    {
    case 0x10002000u:
        return m_last == k_vdec || m_last == k_fdec ? m_result : peek_now();
    case 0x10002004u:
        return m_busy ? 0x80000000u : 0u;
    case 0x10002010u:
    {
        const uint32_t ifc = m_starved ? 0u
            : static_cast<uint32_t>(std::min<size_t>(8, m_in.size() / 16));
        const uint32_t ofc = static_cast<uint32_t>(
            std::min<size_t>(8, (m_out.size() - m_out_pos) / 16));
        return (m_ctrl & ~0x800000FFu) | (m_busy ? 0x80000000u : 0u) | ifc | (ofc << 4);
    }
    case 0x10002020u:
    {
        if (m_starved) return 0;
        const uint32_t bp = static_cast<uint32_t>(m_bit & 127u);
        uint32_t ifc = static_cast<uint32_t>(
            m_in.size() / 16 - std::min(m_in.size() / 16, m_bit / 128));
        uint32_t fp = 0;
        if (bp && ifc)
        {
            fp = 1;
            --ifc;
        }
        return (fp << 16) | (std::min<uint32_t>(ifc, 8) << 8) | bp;
    }
    case 0x10002030u:
        return m_busy ? 0u : peek_now();
    case 0x10002034u:
        return m_busy || available() < 32 ? 0x80000000u : 0u;
    default:
        return 0;
    }
}

void c_ee_ipu::write32(uint32_t address, uint32_t value)
{
    switch (address)
    {
    case 0x10002000u:
        m_ctrl &= ~(k_ecd | k_scd);
        m_starved = false;
        ++m_commands[value >> 28];
        m_command = value;
        m_last = static_cast<int>(value >> 28);
        m_busy = m_decoding = m_interrupt = true;
        m_phase = 0;
        m_qsc = (value >> 16) & 0x1Fu;
        m_first = true;
        m_count = value & 0x7FFu;
        step();
        trace("cmd", value);
        break;
    case 0x10002010u:
        if (value & k_reset)
        {
            m_starved = false;
            m_in.clear();
            m_out.clear();
            m_bit = m_out_pos = 0;
            m_busy = m_decoding = false;
            m_th0 = m_th1 = 0;
        }
        m_ctrl = (m_ctrl & ~0x3FFF0000u) | (value & 0x3FFF0000u);
        break;
    default:
        if (address >= k_in_fifo && address < k_in_fifo + 0x10u)
        {
            const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
            m_in.insert(m_in.end(), bytes, bytes + 4);
            step();
        }
        break;
    }
}

void c_ee_ipu::write_fifo(const uint8_t* qword)
{
    m_in.insert(m_in.end(), qword, qword + 16);
    step();
}

void c_ee_ipu::read_fifo(uint8_t* qword)
{
    if (m_out_pos + 16 <= m_out.size())
    {
        std::memcpy(qword, m_out.data() + m_out_pos, 16);
        m_out_pos += 16;
    }
    else
    {
        std::memset(qword, 0, 16);
    }
    step();
}

void c_ee_ipu::dma_started(uint32_t channel)
{
    step();
    trace(channel == 3u ? "dma3" : "dma4", channel);
}

bool c_ee_ipu::mmio_read(void* user, uint32_t address, uint32_t width, s_ee_u128* value)
{
    if (user == nullptr || value == nullptr)
    {
        return false;
    }
    c_ee_ipu* ipu = static_cast<c_ee_ipu*>(user);
    if (address >= k_register_base && address < k_register_base + 0x40u)
    {
        if (width == 8u && (address & 7u) == 0u)
        {
            const uint64_t low = ipu->read32(address);
            const uint64_t high = ipu->read32(address + 4u);
            value->m_lo = low | (high << 32u);
            value->m_hi = 0u;
            return true;
        }
        if (width <= 4u)
        {
            const uint32_t word = ipu->read32(address & ~3u);
            value->m_lo = word >> ((address & 3u) * 8u);
            value->m_hi = 0u;
            return true;
        }
        return false;
    }
    if (address >= k_out_fifo && address < k_out_fifo + 0x10u && width == 16u)
    {
        uint8_t bytes[16];
        ipu->read_fifo(bytes);
        std::memcpy(&value->m_lo, bytes, 8);
        std::memcpy(&value->m_hi, bytes + 8, 8);
        return true;
    }
    if (address == k_out_fifo && width == 8u)
    {
        // A 64-bit LD pair: the low half pops a qword, +8 returns the rest.
        ipu->read_fifo(ipu->m_out_half);
        std::memcpy(&value->m_lo, ipu->m_out_half, 8);
        value->m_hi = 0u;
        return true;
    }
    if (address == k_out_fifo + 8u && width == 8u)
    {
        std::memcpy(&value->m_lo, ipu->m_out_half + 8, 8);
        value->m_hi = 0u;
        return true;
    }
    return false;
}

bool c_ee_ipu::mmio_write(void* user, uint32_t address, uint32_t width, const s_ee_u128* value)
{
    if (user == nullptr || value == nullptr)
    {
        return false;
    }
    c_ee_ipu* ipu = static_cast<c_ee_ipu*>(user);
    if (address >= k_register_base && address < k_register_base + 0x40u)
    {
        if ((width == 4u || width == 8u) && (address & 3u) == 0u)
        {
            ipu->write32(address, static_cast<uint32_t>(value->m_lo));
            return true;
        }
        return false;
    }
    if (address >= k_in_fifo && address < k_in_fifo + 0x10u)
    {
        if (width == 16u)
        {
            uint8_t bytes[16];
            std::memcpy(bytes, &value->m_lo, 8);
            std::memcpy(bytes + 8, &value->m_hi, 8);
            ipu->write_fifo(bytes);
            return true;
        }
        if (width == 8u)
        {
            // Games commonly feed the FIFO with two 64-bit SD stores
            // (0x10007010 then 0x10007018); bytes are appended in order.
            const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value->m_lo);
            ipu->m_in.insert(ipu->m_in.end(), bytes, bytes + 8);
            ipu->step();
            return true;
        }
        if (width == 4u)
        {
            ipu->write32(address, static_cast<uint32_t>(value->m_lo));
            return true;
        }
    }
    return false;
}

} // namespace ee
