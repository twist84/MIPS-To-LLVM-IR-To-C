#include "ee_vu1_aot.h"
#include "ee_vu1_worker.h"

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
    // MULAx writes ACC; following MADD consumes it.
    pair(code, k_start, add_vi, upper(0x3cu, 0x18u >> 2u, 2u, 3u, 15u));
    // Same-pair LQ reads memory while the upper consumes the old VF4.
    pair(code, 0u, 0x01e40001u, upper(0x29u, 5u, 4u, 3u, 15u) | 0x40000000u);
    // E delay slot stores the old VF6 before the simultaneous upper
    // writeback. Native execution must publish these data bytes too.
    pair(code, 8u, (1u << 25u) | (15u << 21u) | (1u << 16u) |
        (6u << 11u), upper(0x3cu, 0x14u >> 2u, 5u, 6u, 15u));
    return code;
}

ee::t_vu1_code branch_program()
{
    ee::t_vu1_code code{};
    const auto branch = [](uint32_t major, uint32_t is, uint32_t it, uint32_t imm)
    {
        return (major << 25u) | (it << 16u) | (is << 11u) | (imm & 0x7ffu);
    };
    constexpr uint32_t k_upper = 0x000002ffu;
    // BAL across the micro-memory boundary; its link is VI6=2049.
    pair(code, k_start, branch(0x21u, 0u, 6u, 4u),
        upper(0x3fu, 0x1fu >> 2u, 2u, 3u, 0u));
    // A memory write in the delay slot must be transactional even when
    // the later JALR target is not in the statically compiled graph.
    pair(code, 0u, (1u << 25u) | (15u << 21u) | (1u << 16u) |
        (6u << 11u), upper(0x3eu, 0x2eu >> 2u, 2u, 3u, 15u));
    pair(code, 8u, branch(0x20u, 0u, 0u, 6u), k_upper | 0x40000000u);
    // Q producer in E delay slot is drained before completion.
    pair(code, 16u, 0x80000000u | (3u << 16u) | (2u << 11u) |
        ((0x38u >> 2u) << 6u) | 0x3cu, k_upper);
    pair(code, 0x20u, branch(0x25u, 2u, 8u, 0u), k_upper);
    // A successful XGKICK in the JALR delay slot must be rolled back
    // if the later indirect target is unknown.
    pair(code, 0x28u, 0x80000000u | (1u << 11u) |
        ((0x6cu >> 2u) << 6u) | 0x3cu, k_upper);
    pair(code, 0x30u, k_lower_nop, k_upper | 0x40000000u);
    pair(code, 0x38u, k_lower_nop, k_upper);
    return code;
}

ee::t_vu1_code efu_program()
{
    ee::t_vu1_code code{};
    uint32_t pc = k_start;
    const auto append = [&](uint32_t lo, uint32_t hi = 0x000002ffu)
    {
        pair(code, pc, lo, hi);
        pc = (pc + 8u) & 0x3fffu;
    };
    const auto special = [](uint32_t index, uint32_t fs, uint32_t ft, uint32_t mask)
    {
        return 0x80000000u | (mask << 21u) | (ft << 16u) |
            (fs << 11u) | ((index >> 2u) << 6u) | 0x3cu | (index & 3u);
    };
    constexpr uint32_t ops[] = {
        0x70u, 0x71u, 0x72u, 0x73u, 0x74u, 0x75u, 0x76u,
        0x78u, 0x79u, 0x7au, 0x7cu, 0x7du, 0x7eu
    };
    uint32_t slot = 0x100u;
    for (uint32_t op : ops)
    {
        // Finite, signed zero, exponent-255 and zero vector cases.
        for (uint32_t source : {2u, 3u, 4u})
        {
            append(special(op, source, 0u, 0u));
            append(special(0x64u, 0u, 16u, 15u)); // previous P
            append(special(0x7bu, 0u, 0u, 0u)); // WAITP
            append(special(0x64u, 0u, 17u, 15u));
            append((1u << 25u) | (15u << 21u) | (16u << 11u) | slot++);
            append((1u << 25u) | (15u << 21u) | (17u << 11u) | slot++);
        }
    }
    // Producers serialize, Q and P overlap, and E drains both.
    append(special(0x73u, 2u, 0u, 0u));
    append(special(0x70u, 3u, 0u, 0u));
    append(special(0x38u, 2u, 3u, 0u), 0x400002ffu);
    append(special(0x7eu, 3u, 0u, 0u));
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

s_outcome run(ee::t_vu1_code code, ee::c_ee_vu1_aot* backend,
    bool branching = false, ee::c_ee_vu1_worker* worker = nullptr,
    bool aligned = false)
{
    ee::c_vu1_aot_binding binding(backend);
    s_outcome result{};
    auto& s = result.m_state;
    s.m_initialized = true;
    s.m_vf[0][3] = 0x3f800000u;
    s.m_vf[2] = {0x3f800000u, 0x80000001u, 0x7fffffffu, 0xbf800000u};
    s.m_vf[3] = {0x40000000u, 0x80000000u, 0x7f7fffffu, 0x3f800000u};
    s.m_vi[1] = 0xfffeu;
    s.m_vi[2] = branching ? 1u : 5u;
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
    if (branching)
    {
        // The preceding SQ writes VF6 to quadword 1022; supply an EOP
        // IMAGE tag there and a single payload qword at 1023.
        s.m_vf[6] = {0x00008001u, 0x08000000u, 0u, 0u};
    }
    result.m_path1 = {1u, 2u, 3u, 4u};
    if (worker != nullptr || aligned)
    {
        result.m_path1.resize(16u, 0x5au);
    }
    if (worker != nullptr)
    {
        auto job = std::make_unique<ee::s_ee_vu1_job>();
        job->m_resident->m_state = s;
        job->m_resident->m_code = code;
        job->m_resident->m_data = result.m_data;
        job->m_path1.swap(result.m_path1);
        job->m_address = k_start;
        job->m_top = 123u;
        job->m_itop = 456u;
        if (!worker->submit(job))
        {
            return result;
        }
        job = worker->wait_take();
        if (job == nullptr)
        {
            return result;
        }
        result.m_state = job->m_resident->m_state;
        result.m_data = job->m_resident->m_data;
        result.m_path1.swap(job->m_path1);
        result.m_issue = job->m_issue;
        result.m_ok = job->m_accepted && !job->m_host_failed &&
            job->m_compiled_pairs == job->m_issue.m_steps;
    }
    else
    {
        result.m_ok = ee::c_ee_vu1::run(s, code, result.m_data, k_start, 123u,
            456u, result.m_path1, &result.m_issue);
    }
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
        const auto branching = branch_program();
        if (!same(run(branching, &backend, true), run(branching, nullptr, true)))
        {
            return 1;
        }
        const auto efu = efu_program();
        if (!same(run(efu, &backend), run(efu, nullptr)))
        {
            return 1;
        }
        size_t files = 0u;
        for (const auto& item : std::filesystem::directory_iterator(argv[2]))
        {
            if (item.path().extension() == ".bin")
            {
                ++files;
            }
        }
        return files == 4u ? 0 : 1;
    }
    if (std::strcmp(argv[1], "run") != 0 ||
        backend.open_directory(argv[2], &error) != 3u || !error.empty())
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
    const auto branching = branch_program();
    if (!same(run(branching, &backend, true), run(branching, nullptr, true)) ||
        backend.native_runs() != 4u)
    {
        return 1;
    }
    // A dynamic entry that writes memory and later jumps to an unknown
    // target must leave caller state and data entirely unchanged.
    ee::s_ee_vu1_state candidate{};
    candidate.m_initialized = true;
    candidate.m_vf[0][3] = 0x3f800000u;
    candidate.m_vf[6] = {0x00008001u, 0x08000000u, 0u, 0u};
    candidate.m_vi[1] = 0xfffeu;
    candidate.m_vi[2] = 0x100u;
    const auto original = candidate;
    ee::t_vu1_code memory{};
    memory.fill(0xa5u);
    const auto original_memory = memory;
    ee::s_ee_vu1_issue issue{};
    std::vector<uint8_t> path1{1u, 2u, 3u, 4u};
    const auto original_path1 = path1;
    if (backend.try_run(candidate, branching, memory, k_start, 0u, 0u, path1, issue) ||
        candidate.m_vi != original.m_vi || candidate.m_vf != original.m_vf ||
        candidate.m_acc != original.m_acc ||
        candidate.m_i != original.m_i || candidate.m_tpc != original.m_tpc ||
        candidate.m_mac != original.m_mac || candidate.m_status != original.m_status ||
        candidate.m_clip != original.m_clip || candidate.m_q != original.m_q ||
        memory != original_memory || path1 != original_path1 || issue.m_steps != 0u ||
        backend.native_runs() != 4u || backend.fallbacks() != 2u)
    {
        return 1;
    }
    // A malformed tag also rejects without publishing any bytes.
    candidate = original;
    candidate.m_vi[2] = 1u;
    candidate.m_vf[6] = {0x0000ffffu, 0x08000000u, 0u, 0u};
    const auto malformed = candidate;
    if (backend.try_run(candidate, branching, memory, k_start, 0u, 0u, path1, issue) ||
        candidate.m_vf != malformed.m_vf || candidate.m_vi != malformed.m_vi ||
        candidate.m_clip != malformed.m_clip || memory != original_memory ||
        path1 != original_path1 || backend.fallbacks() != 3u)
    {
        return 1;
    }
    // An unrelated change is safe for the dynamic switch too. A
    // changed emitted delay slot still forces the exact-byte fallback.
    const auto misses = std::filesystem::path(argv[2]) / "misses";
    if (!backend.enable_capture(misses.string(), &error, true))
    {
        return 1;
    }
    auto modified = branching;
    modified[0x100u] ^= 1u;
    if (!same(run(modified, &backend, true), run(modified, nullptr, true)) ||
        backend.fallbacks() != 3u || backend.native_runs() != 5u ||
        !std::filesystem::is_empty(misses))
    {
        return 1;
    }
    pair(modified, 0x28u, k_lower_nop, 0x000002ffu);
    if (!same(run(modified, &backend, true), run(modified, nullptr, true)) ||
        backend.fallbacks() != 4u || backend.native_runs() != 5u ||
        std::filesystem::is_empty(misses))
    {
        return 1;
    }
    candidate = original;
    if (backend.try_run(candidate, branching, memory, 0x100u, 0u, 0u, path1, issue))
    {
        return 1;
    }
    candidate.m_initialized = false;
    if (backend.try_run(candidate, branching, memory, k_start, 0u, 0u, path1, issue) ||
        backend.fallback_count(ee::e_vu1_aot_fallback::no_entry) != 1u ||
        backend.fallback_count(ee::e_vu1_aot_fallback::code_mismatch) != 2u ||
        backend.fallback_count(ee::e_vu1_aot_fallback::invalid_state) != 1u ||
        backend.fallback_count(ee::e_vu1_aot_fallback::native_rejected) != 2u ||
        backend.fallbacks() != 6u)
    {
        return 1;
    }
    const auto efu = efu_program();
    if (!same(run(efu, &backend), run(efu, nullptr)) || backend.native_runs() != 6u)
    {
        return 1;
    }
    // Native libraries and mutable capture/counter state are exclusively
    // owned by the persistent worker, not the main thread's TLS binding.
    auto owned = std::make_unique<ee::c_ee_vu1_aot>();
    error.clear();
    if (owned->open_directory(argv[2], &error) != 3u || !error.empty())
    {
        return 1;
    }
    ee::c_ee_vu1_worker worker(std::move(owned), true);
    if (!same(run(code, nullptr, false, &worker),
              run(code, nullptr, false, nullptr, true)) ||
        !same(run(branching, nullptr, true, &worker),
              run(branching, nullptr, true, nullptr, true)) ||
        !same(run(efu, nullptr, false, &worker),
              run(efu, nullptr, false, nullptr, true)))
    {
        return 1;
    }
    std::puts("VU1 AOT native dispatch, scalar pipelines, PATH1 rollback and worker parity: PASS");
    return 0;
}
