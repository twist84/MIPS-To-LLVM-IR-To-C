#pragma once

// Runtime side of ahead-of-time recompiled IRX modules (see iop_aot_abi.h).

#include "iop_aot_abi.h"

#include <cstdint>
#include <string>
#include <vector>

namespace iop
{
class c_cpu;

uint64_t iop_aot_hash(const uint8_t* bytes, size_t size);

class c_iop_aot
{
public:
    c_iop_aot() = default;
    ~c_iop_aot();
    c_iop_aot(const c_iop_aot&) = delete;
    c_iop_aot& operator=(const c_iop_aot&) = delete;

    // Load one translated module library (may be called repeatedly).
    // Returns false (with a message) on failure.
    bool open(const std::string& path, std::string* error);
    // Load every library (.dll/.so) in a directory, one per IRX module.
    // Returns the number loaded; failures are appended to *error.
    size_t open_directory(const std::string& directory, std::string* error);
    // Register modules directly (tests and statically linked translations).
    void add_modules(const s_iop_aot_module* modules, uint32_t count);

    // Re-check every translated module against current IOP RAM. Call after
    // each module load: only byte-identical images become active.
    void refresh(const c_cpu& cpu);

    // Active module covering pc, or nullptr.
    const s_iop_aot_module* find(uint32_t pc) const
    {
        const uint32_t physical = pc & 0x1fffffffu;
        if ((pc & 0xe0000000u) != 0u && (pc & 0xe0000000u) != 0x80000000u &&
            (pc & 0xe0000000u) != 0xa0000000u)
        {
            return nullptr;
        }
        for (const s_iop_aot_module* module : m_active)
        {
            if (physical - module->m_base < module->m_size)
            {
                return module;
            }
        }
        return nullptr;
    }

    size_t module_count() const { return m_modules.size(); }
    size_t active_count() const { return m_active.size(); }
    uint64_t entries() const { return m_entries; }
    uint64_t instructions() const { return m_instructions; }
    void add_time(double milliseconds) { m_milliseconds += milliseconds; }
    double milliseconds() const { return m_milliseconds; }
    double refresh_milliseconds() const { return m_refresh_milliseconds; }
    void record(uint32_t executed)
    {
        ++m_entries;
        m_instructions += executed;
    }

private:
    std::vector<void*> m_libraries;
    std::vector<const s_iop_aot_module*> m_modules;
    std::vector<const s_iop_aot_module*> m_active;
    uint64_t m_entries = 0u;
    uint64_t m_instructions = 0u;
    double m_milliseconds = 0.0;
    double m_refresh_milliseconds = 0.0;
};
} // namespace iop
