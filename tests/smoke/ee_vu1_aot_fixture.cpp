#include "ee_vu1_aot.h"

#include <cstdio>
#include <cstring>
#include <filesystem>

namespace
{
constexpr uint32_t k_start = 0x3ff8u;
constexpr uint32_t k_lower_nop = 0x8000033cu;

void pair(ee::t_vu1_code& code, uint32_t pc, uint32_t lo, uint32_t hi)
{
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        code[pc + i] = static_cast<uint8_t>(lo >> (i * 8u));
        code[pc + 4u + i] = static_cast<uint8_t>(hi >> (i * 8u));
    }
}

uint32_t upper(uint32_t op, uint32_t fd, uint32_t fs,
    uint32_t ft, uint32_t mask)
{
    return op | (fd << 6u) | (fs << 11u) | (ft << 16u) | (mask << 21u);
}

ee::t_vu1_code program()
{
    ee::t_vu1_code code{};
    const uint32_t add_vi =
        0x80000030u | (2u << 16u) | (1u << 11u) | (3u << 6u);
    pair(code, k_start, add_vi, upper(0x28u, 4u, 2u, 3u, 15u));
    // Same-pair LQ reads memory while the upper consumes the old VF4.
    pair(code, 0u, 0x01e40001u, upper(0x2au, 5u, 4u, 3u, 15u) | 0x40000000u);
    // E delay slot stores the old VF6 before the simultaneous upper
    // writeback. Native execution must publish these data bytes too.
    pair(code, 8u, (1u << 25u) | (15u << 21u) | (1u << 16u) |
        (6u << 11u), upper(0x22u, 6u, 5u, 0u, 15u));
    return code;
}

struct s_outcome
{
    ee::s_ee_vu1_state m_state{};
    ee::s_ee_vu1_issue m_issue{};
    ee::t_vu1_code m_data{};
    std::vector<uint8_t> m_path1;
    bool m_ok = false;
};

s_outcome run(ee::t_vu1_code code, ee::c_ee_vu1_aot* backend)
{
    ee::c_vu1_aot_binding binding(backend);
    s_outcome result{};
    auto& s = result.m_state;
    s.m_initialized = true;
    s.m_vf[0][3] = 0x3f800000u;
    s.m_vf[2] = {0x3f800000u, 0x80000001u, 0x7fffffffu, 0xbf800000u};
    s.m_vf[3] = {0x40000000u, 0x80000000u, 0x7f7fffffu, 0x3f800000u};
    s.m_vi[1] = 0xfffeu;
    s.m_vi[2] = 5u;
    s.m_i = 0x40000000u;
    s.m_acc = {11u, 22u, 33u, 44u};
    s.m_vf[6] = {0x12345678u, 0xabcdef01u, 0xdeadbeefu, 0x01020304u};
    s.m_q = 0x3f800000u;
    s.m_p = 123u;
    s.m_r = 456u;
    s.m_mac = 0x1234u;
    s.m_status = 0x30u;
    s.m_clip = 0x123456u;
    result.m_data.fill(0x5au);
    result.m_path1 = {1u, 2u, 3u, 4u};
    result.m_ok = ee::c_ee_vu1::run(s, code, result.m_data, k_start, 123u,
        456u, result.m_path1, &result.m_issue);
    return result;
}

bool same(const s_outcome& a, const s_outcome& b)
{
    const auto& x = a.m_state;
    const auto& y = b.m_state;
    const auto& i = a.m_issue;
    const auto& j = b.m_issue;
    return a.m_ok && b.m_ok && x.m_vf == y.m_vf && x.m_vi == y.m_vi &&
        x.m_acc == y.m_acc && x.m_i == y.m_i && x.m_q == y.m_q &&
        x.m_p == y.m_p && x.m_r == y.m_r && x.m_mac == y.m_mac &&
        x.m_status == y.m_status && x.m_clip == y.m_clip &&
        x.m_tpc == y.m_tpc && x.m_initialized == y.m_initialized &&
        i.m_steps == j.m_steps && i.m_cycles == j.m_cycles &&
        i.m_pc == j.m_pc && i.m_lower == j.m_lower && i.m_upper == j.m_upper &&
        i.m_kick_reject == j.m_kick_reject && a.m_data == b.m_data &&
        a.m_path1 == b.m_path1;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        return 2;
    }
    auto code = program();
    ee::c_ee_vu1_aot backend;
    std::string error;
    if (std::strcmp(argv[1], "capture") == 0)
    {
        if (!backend.enable_capture(argv[2], &error))
        {
            return 1;
        }
        const auto baseline = run(code, nullptr);
        if (!same(run(code, &backend), baseline) ||
            !same(run(code, &backend), baseline))
        {
            return 1;
        }
        // Unsupported debug control must be captured but never compiled.
        auto unsupported = code;
        pair(unsupported, k_start, k_lower_nop, 0x100002ffu);
        backend.capture(unsupported, k_start);
        backend.capture(code, k_start);
        size_t files = 0u;
        for (const auto& item : std::filesystem::directory_iterator(argv[2]))
        {
            if (item.path().extension() == ".bin")
            {
                ++files;
            }
        }
        return files == 2u ? 0 : 1;
    }
    if (std::strcmp(argv[1], "run") != 0 ||
        backend.open_directory(argv[2], &error) != 1u || !error.empty())
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    if (!same(run(code, &backend), run(code, nullptr)) ||
        backend.native_runs() != 1u)
    {
        return 1;
    }
    // Unreached MPG changes are irrelevant. Replacing a reached word
    // forces fallback, including changes across the wrap boundary.
    auto unrelated = code;
    unrelated[0x100u] ^= 1u;
    if (!same(run(unrelated, &backend), run(unrelated, nullptr)) ||
        backend.native_runs() != 2u)
    {
        return 1;
    }
    auto replaced = code;
    pair(replaced, 0u, k_lower_nop, upper(0x28u, 5u, 4u, 3u, 15u) | 0x40000000u);
    if (!same(run(replaced, &backend), run(replaced, nullptr)) ||
        backend.fallbacks() != 1u || backend.native_runs() != 2u ||
        !same(run(code, &backend), run(code, nullptr)) ||
        backend.native_runs() != 3u)
    {
        return 1;
    }
    std::puts("VU1 AOT native dispatch, exact-byte invalidation and fallback: PASS");
    return 0;
}
