#include "ee_vu1_aot.h"
#include "ee_vu1_ir.h"
#include "ee_runtime_profile.h"

#include <algorithm>
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
    size_t loaded = 0u;
    for (const auto& item : paths)
    {
        std::string message;
        if (open(item.string(), &message))
        {
            ++loaded;
        }
        else if (error != nullptr)
        {
            *error += (error->empty() ? "" : "; ") + message;
        }
    }
    return loaded;
}

bool c_ee_vu1_aot::enable_capture(const std::string& path, std::string* error)
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
    t_vu1_code& data, uint32_t pc, uint32_t top, uint32_t itop, s_ee_vu1_issue& issue)
{
    pc &= 0x3ff8u;
    // A compiled entry assumes initialized constant registers. If a
    // caller supplies a noncanonical state, the interpreter owns it.
    if (state.m_initialized && state.m_vi[0] == 0u &&
        state.m_vf[0] == std::array<uint32_t, 4>{0u, 0u, 0u, 0x3f800000u})
    {
        for (const auto& program : m_programs[pc >> 3u])
        {
            if (!exact_match(code, pc, program.m_code))
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
            uint8_t* data_ptr = program.m_writes_data ?
                staged_data.data() : data.data();
            std::array<uint32_t, 128> vf{};
            static_assert(sizeof(vf) == sizeof(next.m_vf), "VF ABI");
            std::memcpy(vf.data(), next.m_vf.data(), sizeof(vf));
            uint32_t steps = 0u;
            uint32_t cycles = 0u;
            uint32_t last_pc = 0u;
            c_runtime_profile_scope scope(e_runtime_profile_id::vu1_compiled,
                0u, &steps);
            steps = program.m_entry(next.m_vi.data(), &next.m_i,
                &next.m_tpc, top, itop, vf.data(), next.m_q, &next.m_mac,
                &next.m_status, data_ptr, &cycles, &last_pc, next.m_acc.data(), &next.m_clip);
            if (steps < 2u || steps > c_ee_vu1::k_max_steps ||
                cycles < steps || cycles > steps * 4u ||
                last_pc >= c_ee_vu1::k_memory_size || (last_pc & 7u) != 0u ||
                next.m_tpc >= c_ee_vu1::k_memory_size || (next.m_tpc & 7u) != 0u ||
                (program.m_steps != 0u &&
                    (steps != program.m_steps || cycles != program.m_cycles ||
                     next.m_tpc != program.m_next_pc ||
                     last_pc != ((program.m_next_pc - 8u) & 0x3fffu))) ||
                next.m_vi[0] != 0u || vf[0] != 0u || vf[1] != 0u ||
                vf[2] != 0u || vf[3] != 0x3f800000u)
            {
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
    return false;
}

void c_ee_vu1_aot::write_json(FILE* output) const
{
    std::fprintf(output,
        "{\"enabled\":true,\"programs\":%zu,\"native_runs\":%llu,"
        "\"native_pairs\":%llu,\"interpreter_fallbacks\":%llu,"
        "\"captures\":%llu,\"capture_errors\":%llu}",
        program_count(), static_cast<unsigned long long>(m_native_runs),
        static_cast<unsigned long long>(m_native_pairs),
        static_cast<unsigned long long>(m_fallbacks),
        static_cast<unsigned long long>(m_captures),
        static_cast<unsigned long long>(m_capture_errors));
}
} // namespace ee
