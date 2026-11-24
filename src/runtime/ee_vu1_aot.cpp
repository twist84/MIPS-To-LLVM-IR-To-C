#include "ee_vu1_aot.h"
#include "ee_vu1_ir.h"
#include "ee_runtime_profile.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace ee
{
namespace
{
// Diagnostic identity of the on-disk binary, not an authenticity check.
bool native_file_fingerprint(const std::filesystem::path& path, uint64_t& hash)
{
    std::ifstream input(path, std::ios::binary);
    std::array<char, 16384> bytes{};
    hash = UINT64_C(14695981039346656037);
    for (; input; )
    {
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        const auto count = input.gcount();
        for (std::streamsize i = 0; i < count; ++i)
        {
            hash ^= static_cast<uint8_t>(bytes[static_cast<size_t>(i)]);
            hash *= UINT64_C(1099511628211);
        }
    }
    return input.eof() && !input.bad();
}

void close_library(void* library)
{
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(library));
#else
    dlclose(library);
#endif
}

uint32_t word(const uint8_t* data)
{
    uint32_t value = 0u;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

bool exact_match(const t_vu1_code& code, uint32_t pc,
    const std::vector<uint8_t>& program)
{
    size_t offset = 0u;
    while (offset < program.size())
    {
        const size_t count = std::min(program.size() - offset,
            code.size() - pc);
        if (std::memcmp(code.data() + pc, program.data() + offset, count) != 0)
        {
            return false;
        }
        offset += count;
        pc = 0u;
    }
    return true;
}
struct s_native_kick_context
{
    const t_vu1_code& m_data;
    std::vector<uint8_t>& m_path1;
    s_ee_vu1_issue m_issue{};
};

uint32_t native_kick(void* context, uint32_t address)
{
    auto& kick = *static_cast<s_native_kick_context*>(context);
    return c_ee_vu1::kick(kick.m_data, address, kick.m_path1, kick.m_issue) ? 1u : 0u;
}
uint64_t native_efu(uint32_t index, uint32_t component, uint32_t x,
    uint32_t y, uint32_t z, uint32_t w)
{
    uint32_t value = 0u;
    uint32_t latency = 0u;
    if (!c_ee_vu1::efu({x, y, z, w}, index, component, value, latency))
    {
        return 0u;
    }
    return (static_cast<uint64_t>(latency) << 32u) | value;
}
} // namespace

c_ee_vu1_aot::~c_ee_vu1_aot()
{
    for (void* library : m_libraries)
    {
        close_library(library);
    }
}

bool c_ee_vu1_aot::open(const std::string& path, std::string* error)
{
#if defined(_WIN32)
    void* library = LoadLibraryA(path.c_str());
    const auto symbol = [library](const char* name)
    {
        return GetProcAddress(static_cast<HMODULE>(library), name);
    };
#else
    void* library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    const auto symbol = [library](const char* name)
    {
        return dlsym(library, name);
    };
#endif
    const auto reject = [&](const char* reason)
    {
        if (error != nullptr)
        {
            *error = std::string(reason) + ": " + path;
        }
        if (library != nullptr)
        {
            close_library(library);
        }
        return false;
    };
    if (library == nullptr)
    {
        return reject("cannot load VU1 AOT library");
    }
    const auto info = reinterpret_cast<t_vu1_aot_info>(symbol("ee_vu1_aot_info"));
    const auto bytes = reinterpret_cast<t_vu1_aot_code>(symbol("ee_vu1_aot_code"));
    const auto entry = reinterpret_cast<t_vu1_aot_entry>(symbol("ee_vu1_program"));
    if (info == nullptr || bytes == nullptr || entry == nullptr)
    {
        return reject("missing VU1 AOT exports");
    }
    const uint32_t* metadata = info();
    if (metadata == nullptr || metadata[0] != k_vu1_aot_abi)
    {
        return reject("VU1 AOT ABI mismatch");
    }
    const uint32_t start = metadata[1];
    const uint32_t steps = metadata[2];
    const uint32_t cycles = metadata[3];
    const uint32_t next_pc = metadata[4];
    const uint8_t* code_bytes = bytes();
    const bool dynamic = steps == 0u;
    if (start >= c_ee_vu1::k_memory_size || (start & 7u) != 0u ||
        (dynamic ? (cycles != 0u || next_pc != 0u) :
            (steps < 2u || steps > 2049u || cycles < steps ||
             cycles > steps * 4u || next_pc != ((start + steps * 8u) & 0x3fffu))) ||
        code_bytes == nullptr)
    {
        return reject("invalid VU1 AOT metadata");
    }

    // Validate the exact encoded program with the current lifter. Reject
    // stale coverage or timing metadata even if the ABI version matches.
    t_vu1_code image{};
    const uint32_t size = dynamic ? c_ee_vu1::k_memory_size : steps * 8u;
    for (uint32_t i = 0u; i < size; ++i)
    {
        image[(start + i) & 0x3fffu] = code_bytes[i];
    }
    s_program program{};
    program.m_steps = steps;
    program.m_cycles = cycles;
    program.m_next_pc = next_pc;
    program.m_code.assign(code_bytes, code_bytes + size);
    program.m_entry = entry;
    const auto lifted = vu1_lift_program_ir(image, start);
    if (lifted.m_ir.empty() || lifted.m_steps != steps ||
        lifted.m_cycles != cycles || lifted.m_next_pc != next_pc ||
        !exact_match(image, start, program.m_code))
    {
        return reject("VU1 AOT code or timing validation failed");
    }
    program.m_writes_data = lifted.m_writes_data;
    if (dynamic)
    {
        // The embedded bytes are rotated by start. Derive ranges from
        // the exact graph used to generate the switch, not a hash or
        // a guessed microprogram extent. Coalesce adjacent pairs.
        for (uint32_t index = 0u; index < lifted.m_reached.size(); ++index)
        {
            if (!lifted.m_reached.test(index))
            {
                continue;
            }
            const uint32_t pc = index * 8u;
            const uint32_t offset = (pc - start) & 0x3fffu;
            if (!program.m_ranges.empty())
            {
                auto& last = program.m_ranges.back();
                if (last.m_pc + last.m_size == pc &&
                    last.m_offset + last.m_size == offset)
                {
                    last.m_size += 8u;
                    continue;
                }
            }
            program.m_ranges.push_back({pc, offset, 8u});
        }
        if (program.m_ranges.empty())
        {
            return reject("empty VU1 AOT dynamic code guard");
        }
    }
    m_programs[start >> 3u].push_back(std::move(program));
    m_libraries.push_back(library);
    return true;
}

size_t c_ee_vu1_aot::open_directory(const std::string& path, std::string* error)
{
#if defined(_WIN32)
    const char* suffix = ".dll";
#else
    const char* suffix = ".so";
#endif
    std::vector<std::filesystem::path> paths;
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(path, ec))
    {
        if (item.path().extension() == suffix)
        {
            paths.push_back(std::filesystem::absolute(item.path(), ec));
            if (ec)
            {
                break;
            }
        }
    }
    if (ec)
    {
        if (error != nullptr)
        {
            *error = ec.message();
        }
        return 0u;
    }
    std::sort(paths.begin(), paths.end());
    std::fprintf(stderr, "VU1 AOT directory: %s\n", path.c_str());
    size_t loaded = 0u;
    for (const auto& item : paths)
    {
        std::string message;
        if (open(item.string(), &message))
        {
            ++loaded;
            uint64_t hash = 0u;
            if (native_file_fingerprint(item, hash))
            {
                std::fprintf(stderr, "VU1 AOT library: %s fnv1a64=%016llx\n",
                    item.filename().string().c_str(), static_cast<unsigned long long>(hash));
            }
            else
            {
                std::fprintf(stderr, "VU1 AOT library: %s fingerprint unavailable\n",
                    item.filename().string().c_str());
            }
        }
        else if (error != nullptr)
        {
            *error += (error->empty() ? "" : "; ") + message;
        }
    }
    return loaded;
}

bool c_ee_vu1_aot::enable_capture(const std::string& path, std::string* error,
    bool misses_only)
{
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec)
    {
        if (error != nullptr)
        {
            *error = ec.message();
        }
        return false;
    }
    m_capture_path = path;
    m_capture_misses_only = misses_only;
    m_has_code = false;
    m_captured_entries.reset();
    return true;
}

void c_ee_vu1_aot::capture(const t_vu1_code& code, uint32_t pc)
{
    // At most 32 MiB of new captures per process. Full image identity
    // is checked before reusing a seen-entry bit or an existing file.
    if (m_capture_path.empty() || m_captures >= 2048u ||
        m_capture_errors >= 16u)
    {
        return;
    }
    pc &= 0x3ff8u;
    if (!m_has_code || m_last_code != code)
    {
        m_last_code = code;
        m_has_code = true;
        m_captured_entries.reset();
        m_capture_hash = UINT64_C(14695981039346656037);
        for (uint8_t byte : code)
        {
            m_capture_hash ^= byte;
            m_capture_hash *= UINT64_C(1099511628211);
        }
    }
    if (m_captured_entries.test(pc >> 3u))
    {
        return;
    }
    char name[64];
    std::snprintf(name, sizeof(name), "%04x-%016llx", pc,
        static_cast<unsigned long long>(m_capture_hash));
    for (uint32_t collision = 0u; collision < 16u; ++collision)
    {
        const auto path = std::filesystem::path(m_capture_path) /
            (std::string(name) + "-" + std::to_string(collision) + ".bin");
        std::error_code ec;
        const bool exists = std::filesystem::exists(path, ec);
        if (ec)
        {
            break;
        }
        if (exists)
        {
            t_vu1_code previous{};
            std::ifstream input(path, std::ios::binary);
            if (input.read(reinterpret_cast<char*>(previous.data()), previous.size()) &&
                input.peek() == std::char_traits<char>::eof() && previous == code)
            {
                m_captured_entries.set(pc >> 3u);
                return;
            }
            continue;
        }
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(code.data()), code.size());
        output.close();
        if (output)
        {
            ++m_captures;
            m_captured_entries.set(pc >> 3u);
            return;
        }
        break;
    }
    ++m_capture_errors;
}

bool c_ee_vu1_aot::try_run(s_ee_vu1_state& state, const t_vu1_code& code,
    t_vu1_code& data, uint32_t pc, uint32_t top, uint32_t itop,
    std::vector<uint8_t>& path1, s_ee_vu1_issue& issue)
{
    pc &= 0x3ff8u;
    const auto& candidates = m_programs[pc >> 3u];
    auto reason = e_vu1_aot_fallback::invalid_state;
    // A compiled entry assumes initialized constant registers. If a
    // caller supplies a noncanonical state, the interpreter owns it.
    if (state.m_initialized && state.m_vi[0] == 0u &&
        state.m_vf[0] == std::array<uint32_t, 4>{0u, 0u, 0u, 0x3f800000u})
    {
        reason = candidates.empty() ? e_vu1_aot_fallback::no_entry :
            e_vu1_aot_fallback::code_mismatch;
        for (const auto& program : candidates)
        {
            const bool matches = program.m_steps != 0u ?
                exact_match(code, pc, program.m_code) :
                std::all_of(program.m_ranges.begin(), program.m_ranges.end(),
                    [&](const s_code_range& range)
                    {
                        return std::memcmp(code.data() + range.m_pc,
                            program.m_code.data() + range.m_offset,
                            range.m_size) == 0;
                    });
            if (!matches)
            {
                continue;
            }
            // Keep candidate state and any memory stores private until
            // successful completion. Read-only programs avoid the copy.
            auto next = state;
            t_vu1_code staged_data;
            if (program.m_writes_data)
            {
                staged_data = data;
            }
            auto& native_data = program.m_writes_data ? staged_data : data;
            uint8_t* data_ptr = native_data.data();
            // The shared XGKICK parser only appends. Roll back the
            // appended suffix on any native failure, including a later
            // unknown jump or malformed GIFtag. Existing PATH1 bytes
            // still count toward the same aggregate budget.
            const size_t path1_before = path1.size();
            s_native_kick_context kick{native_data, path1, {}};
            std::array<uint32_t, 128> vf{};
            static_assert(sizeof(vf) == sizeof(next.m_vf), "VF ABI");
            std::memcpy(vf.data(), next.m_vf.data(), sizeof(vf));
            uint32_t steps = 0u;
            uint32_t cycles = 0u;
            uint32_t last_pc = 0u;
            c_runtime_profile_scope scope(e_runtime_profile_id::vu1_compiled,
                0u, &steps);
            steps = program.m_entry(next.m_vi.data(), &next.m_i,
                &next.m_tpc, top, itop, vf.data(), &next.m_q, &next.m_mac,
                &next.m_status, data_ptr, &cycles, &last_pc, next.m_acc.data(), &next.m_clip, native_kick, &kick,
                &next.m_p, &next.m_r, native_efu);
            if (steps < 2u || steps > c_ee_vu1::k_max_steps ||
                cycles < steps || cycles > steps * 64u ||
                last_pc >= c_ee_vu1::k_memory_size || (last_pc & 7u) != 0u ||
                next.m_tpc >= c_ee_vu1::k_memory_size || (next.m_tpc & 7u) != 0u ||
                (program.m_steps != 0u &&
                    (steps != program.m_steps || cycles != program.m_cycles ||
                     next.m_tpc != program.m_next_pc ||
                     last_pc != ((program.m_next_pc - 8u) & 0x3fffu))) ||
                next.m_vi[0] != 0u || vf[0] != 0u || vf[1] != 0u ||
                vf[2] != 0u || vf[3] != 0x3f800000u ||
                kick.m_issue.m_kick_reject != e_ee_vu1_kick_reject::none)
            {
                path1.resize(path1_before);
                reason = e_vu1_aot_fallback::native_rejected;
                break;
            }
            std::memcpy(next.m_vf.data(), vf.data(), sizeof(vf));
            state = next;
            if (program.m_writes_data)
            {
                data = staged_data;
            }
            issue.m_steps = steps;
            issue.m_cycles = cycles;
            issue.m_pc = last_pc;
            issue.m_lower = word(code.data() + issue.m_pc);
            issue.m_upper = word(code.data() + issue.m_pc + 4u);
            ++m_native_runs;
            m_native_pairs += steps;
            return true;
        }
    }
    ++m_fallbacks;
    ++m_fallback_reasons[static_cast<size_t>(reason)];
    ++m_fallback_pcs[pc >> 3u];
    if (m_capture_misses_only &&
        (reason == e_vu1_aot_fallback::no_entry ||
         reason == e_vu1_aot_fallback::code_mismatch))
    {
        capture(code, pc);
    }
    return false;
}

void c_ee_vu1_aot::write_json(FILE* output) const
{
    std::fprintf(output,
        "{\"enabled\":true,\"programs\":%zu,\"native_runs\":%llu,"
        "\"native_pairs\":%llu,\"interpreter_fallbacks\":%llu,"
        "\"captures\":%llu,\"capture_errors\":%llu,"
        "\"fallback_reasons\":{\"no_entry\":%llu,\"code_mismatch\":%llu,"
        "\"invalid_state\":%llu,\"native_rejected\":%llu},"
        "\"fallback_pcs\":[",
        program_count(), static_cast<unsigned long long>(m_native_runs),
        static_cast<unsigned long long>(m_native_pairs),
        static_cast<unsigned long long>(m_fallbacks),
        static_cast<unsigned long long>(m_captures),
        static_cast<unsigned long long>(m_capture_errors),
        static_cast<unsigned long long>(fallback_count(e_vu1_aot_fallback::no_entry)),
        static_cast<unsigned long long>(fallback_count(e_vu1_aot_fallback::code_mismatch)),
        static_cast<unsigned long long>(fallback_count(e_vu1_aot_fallback::invalid_state)),
        static_cast<unsigned long long>(fallback_count(e_vu1_aot_fallback::native_rejected)));
    bool first = true;
    for (uint32_t index = 0u; index < m_fallback_pcs.size(); ++index)
    {
        if (m_fallback_pcs[index] != 0u)
        {
            std::fprintf(output, "%s{\"pc\":%u,\"count\":%llu}",
                first ? "" : ",", index * 8u,
                static_cast<unsigned long long>(m_fallback_pcs[index]));
            first = false;
        }
    }
    std::fputs("]}", output);
}
} // namespace ee
