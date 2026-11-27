#include "iop_aot.h"
#include "iop_cpu.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <cstdio>
#include <cstddef>

// iop_ir_lifter.cpp addresses the context by these byte offsets.
static_assert(offsetof(s_iop_aot_context, m_gpr) == 0, "aot abi");
static_assert(offsetof(s_iop_aot_context, m_hi) == 8, "aot abi");
static_assert(offsetof(s_iop_aot_context, m_lo) == 16, "aot abi");
static_assert(offsetof(s_iop_aot_context, m_ram) == 24, "aot abi");
static_assert(offsetof(s_iop_aot_context, m_cpu) == 32, "aot abi");
static_assert(offsetof(s_iop_aot_context, m_read) == 40, "aot abi");
static_assert(offsetof(s_iop_aot_context, m_write) == 48, "aot abi");
static_assert(offsetof(s_iop_aot_context, m_budget) == 56, "aot abi");
static_assert(offsetof(s_iop_aot_context, m_executed) == 60, "aot abi");
static_assert(offsetof(s_iop_aot_context, m_pc) == 64, "aot abi");
static_assert(offsetof(s_iop_aot_context, m_next_pc) == 68, "aot abi");
static_assert(offsetof(s_iop_aot_context, m_in_delay) == 72, "aot abi");
static_assert(offsetof(s_iop_aot_context, m_branch_pc) == 76, "aot abi");
static_assert(sizeof(s_iop_aot_module) == 32, "aot abi");

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace iop
{
uint64_t iop_aot_hash(const uint8_t* bytes, size_t size)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t index = 0; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

void c_iop_aot::record_exit(const s_iop_aot_module& module, uint32_t entry,
                           const s_iop_aot_context& context, uint32_t reason,
                           uint32_t requested_budget)
{
    const t_exit_key key{module.m_base, module.m_hash, entry, context.m_pc,
                         reason, context.m_in_delay};
    auto it = m_exit_counts.find(key);
    if (it == m_exit_counts.end())
    {
        constexpr size_t k_max_exit_keys = 262144u;
        if (m_exit_counts.size() >= k_max_exit_keys)
        {
            ++m_exit_dropped;
            return;
        }
        it = m_exit_counts.emplace(key, s_exit_counts{}).first;
    }
    s_exit_counts& counts = it->second;
    ++counts.m_entries;
    counts.m_retired += context.m_executed;
    counts.m_budget_exhausted += context.m_executed >= context.m_budget ? 1u : 0u;
    counts.m_horizon_limited += context.m_budget < requested_budget ? 1u : 0u;
    counts.m_zero_progress += context.m_executed == 0u ? 1u : 0u;
}

c_iop_aot::~c_iop_aot()
{
    if (!m_exit_counts.empty() || m_exit_dropped != 0u)
    {
        std::fprintf(stderr, "IOP EXIT PROFILE: keys=%zu dropped=%llu\n",
                     m_exit_counts.size(), static_cast<unsigned long long>(m_exit_dropped));
        for (const auto& [key, counts] : m_exit_counts)
        {
            const auto& [base, hash, entry, resume, reason, delay] = key;
            std::fprintf(stderr,
                "IOP EXIT ROW: base=%08x hash=%016llx entry=%08x resume=%08x "
                "reason=%u delay=%u entries=%llu retired=%llu budget=%llu "
                "horizon=%llu zero=%llu\n",
                base, static_cast<unsigned long long>(hash), entry, resume, reason, delay,
                static_cast<unsigned long long>(counts.m_entries),
                static_cast<unsigned long long>(counts.m_retired),
                static_cast<unsigned long long>(counts.m_budget_exhausted),
                static_cast<unsigned long long>(counts.m_horizon_limited),
                static_cast<unsigned long long>(counts.m_zero_progress));
        }
    }
    for (void* library : m_libraries)
    {
#if defined(_WIN32)
        FreeLibrary(static_cast<HMODULE>(library));
#else
        dlclose(library);
#endif
    }
}

size_t c_iop_aot::open_directory(const std::string& directory, std::string* error)
{
#if defined(_WIN32)
    const char* suffix = ".dll";
#else
    const char* suffix = ".so";
#endif
    std::vector<std::filesystem::path> paths;
    std::error_code code;
    for (const auto& entry : std::filesystem::directory_iterator(directory, code))
    {
        if (entry.path().extension() == suffix)
        {
            paths.push_back(entry.path());
        }
    }
    std::sort(paths.begin(), paths.end());
    size_t loaded = 0u;
    for (const auto& path : paths)
    {
        std::string message;
        if (open(path.string(), &message))
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

bool c_iop_aot::open(const std::string& path, std::string* error)
{
#if defined(_WIN32)
    HMODULE library = LoadLibraryA(path.c_str());
    const auto lookup = [library](const char* name)
    {
        return reinterpret_cast<void*>(GetProcAddress(library, name));
    };
#else
    void* library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    const auto lookup = [library](const char* name)
    {
        return dlsym(library, name);
    };
#endif
    if (library == nullptr)
    {
        if (error != nullptr)
        {
            *error = "cannot load IOP AOT library " + path;
        }
        return false;
    }
    const auto count = reinterpret_cast<t_iop_aot_module_count>(
        lookup("iop_aot_module_count"));
    const auto modules = reinterpret_cast<t_iop_aot_modules>(
        lookup("iop_aot_modules"));
    if (count == nullptr || modules == nullptr)
    {
        if (error != nullptr)
        {
            *error = "IOP AOT library has no module table: " + path;
        }
#if defined(_WIN32)
        FreeLibrary(library);
#else
        dlclose(library);
#endif
        return false;
    }
    m_libraries.push_back(library);
    const auto* descriptors = modules();
    const uint32_t module_count = count();
    add_modules(descriptors, module_count);
    const auto chain = reinterpret_cast<t_iop_aot_entry>(
        lookup("iop_aot_chain_entry_v1"));
    const auto chain_pc = reinterpret_cast<t_iop_aot_module_count>(
        lookup("iop_aot_chain_entry_pc_v1"));
    if (chain != nullptr && chain_pc != nullptr && module_count == 1u)
    {
        const uint32_t pc = chain_pc();
        if ((pc & 3u) == 0u && pc - descriptors->m_base < descriptors->m_size)
        {
            m_chain_entries.emplace(descriptors, std::make_pair(pc, chain));
        }
    }
    return true;
}

void c_iop_aot::add_modules(const s_iop_aot_module* modules, uint32_t count)
{
    for (uint32_t index = 0; index < count; ++index)
    {
        if (modules[index].m_entry != nullptr && modules[index].m_size != 0u)
        {
            m_modules.push_back(&modules[index]);
        }
    }
}

void c_iop_aot::refresh(const c_cpu& cpu)
{
    const auto started = std::chrono::steady_clock::now();
    struct s_timer
    {
        std::chrono::steady_clock::time_point m_start;
        double* m_total;
        ~s_timer()
        {
            *m_total += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - m_start).count();
        }
    } timer{started, &m_refresh_milliseconds};
    m_active.clear();
    m_active_chain_entries.clear();
    std::vector<uint8_t> bytes;
    for (const s_iop_aot_module* module : m_modules)
    {
        if (static_cast<uint64_t>(module->m_base) + module->m_size >
            c_cpu::k_ram_size)
        {
            continue;
        }
        bytes.resize(module->m_size);
        if (!cpu.dma_read(module->m_base, bytes.data(), module->m_size))
        {
            continue;
        }
        if (iop_aot_hash(bytes.data(), bytes.size()) == module->m_hash)
        {
            m_active.push_back(module);
        }
    }
    for (const auto* module : m_active)
    {
        const auto chain = m_chain_entries.find(module);
        if (chain == m_chain_entries.end())
        {
            continue;
        }
        bool overlap = false;
        for (const auto* other : m_active)
        {
            if (other != module &&
                static_cast<uint64_t>(module->m_base) < static_cast<uint64_t>(other->m_base) + other->m_size &&
                static_cast<uint64_t>(other->m_base) < static_cast<uint64_t>(module->m_base) + module->m_size)
            {
                overlap = true;
                break;
            }
        }
        if (!overlap)
        {
            m_active_chain_entries.emplace(module, chain->second);
        }
    }
}
} // namespace iop
