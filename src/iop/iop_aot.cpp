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

c_iop_aot::~c_iop_aot()
{
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
    add_modules(modules(), count());
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
}
} // namespace iop
