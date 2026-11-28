#pragma once

#include "ee_vu1.h"

#include <bitset>
#include <cstdio>
#include <string>

namespace ee
{
// ABI changes invalidate native VU1 libraries, never the EE IR cache.
inline constexpr uint32_t k_vu1_aot_abi = 20u; // Rebuild captured libraries for full-width VU memory transfers.
using t_vu1_aot_kick = uint32_t (*)(void*, uint32_t);
using t_vu1_aot_efu = uint64_t (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
using t_vu1_aot_entry = uint32_t (*)(uint16_t*, uint32_t*, uint32_t*,
    uint32_t, uint32_t, uint32_t*, uint32_t*, uint32_t*, uint32_t*, uint8_t*, uint32_t*, uint32_t*, uint32_t*, uint32_t*,
    t_vu1_aot_kick, void*, uint32_t*, uint32_t*, t_vu1_aot_efu, uint32_t*);
using t_vu1_aot_info = const uint32_t* (*)();
using t_vu1_aot_code = const uint8_t* (*)();
using t_vu1_aot_mul_profile = void (*)(uint64_t*);
using t_vu1_code = std::array<uint8_t, c_ee_vu1::k_memory_size>;

enum class e_vu1_aot_fallback : uint32_t
{
    no_entry,
    code_mismatch,
    invalid_state,
    native_rejected,
    count
};

class c_ee_vu1_aot
{
public:
    c_ee_vu1_aot() = default;
    ~c_ee_vu1_aot();
    c_ee_vu1_aot(const c_ee_vu1_aot&) = delete;
    c_ee_vu1_aot& operator=(const c_ee_vu1_aot&) = delete;

    bool open(const std::string& path, std::string* error);
    size_t open_directory(const std::string& path, std::string* error);
    bool enable_capture(const std::string& path, std::string* error,
        bool misses_only = false);

    // Capture observes attempted programs, including later VIF rollbacks.
    // This is host-only; it never mutates guest code or execution state.
    void capture(const t_vu1_code& code, uint32_t pc);
    void capture_on_entry(const t_vu1_code& code, uint32_t pc)
    {
        if (!m_capture_misses_only)
        {
            capture(code, pc);
        }
    }
    bool try_run(s_ee_vu1_state& state, const t_vu1_code& code,
        t_vu1_code& data, uint32_t pc, uint32_t top, uint32_t itop,
        std::vector<uint8_t>& path1, s_ee_vu1_issue& issue);
    void write_json(FILE* output) const;
    uint64_t native_runs() const { return m_native_runs; }
    uint64_t fallbacks() const { return m_fallbacks; }
    uint64_t fallback_count(e_vu1_aot_fallback reason) const
    {
        return m_fallback_reasons[static_cast<size_t>(reason)];
    }
    size_t program_count() const { return m_libraries.size(); }

private:
    struct s_code_range
    {
        uint32_t m_pc;
        uint32_t m_offset;
        uint32_t m_size;
    };

    struct s_program
    {
        uint32_t m_steps = 0u;
        uint32_t m_cycles = 0u;
        uint32_t m_next_pc = 0u;
        bool m_writes_data = false;
        std::vector<uint8_t> m_code;
        std::vector<s_code_range> m_ranges;
        t_vu1_aot_entry m_entry = nullptr;
        t_vu1_aot_mul_profile m_mul_profile = nullptr;
        // Host diagnostics only; ownership follows the AOT backend.
        size_t m_library_index = 0u;
        uint64_t m_code_hash = 0u;
        uint64_t m_attempts = 0u;
        uint64_t m_attempted_pairs = 0u;
        uint64_t m_native_runs = 0u;
        uint64_t m_native_pairs = 0u;
        uint64_t m_profiled_attempts = 0u;
        uint64_t m_profiled_pairs = 0u;
        uint64_t m_compiled_exclusive_ns = 0u;
    };
    std::array<std::vector<s_program>, 2048> m_programs;
    std::vector<void*> m_libraries;
    std::string m_capture_path;
    t_vu1_code m_last_code{};
    std::bitset<2048> m_captured_entries;
    bool m_has_code = false;
    bool m_capture_misses_only = false;
    uint64_t m_capture_hash = 0u;
    uint64_t m_native_runs = 0u;
    uint64_t m_native_pairs = 0u;
    uint64_t m_fallbacks = 0u;
    std::array<uint64_t, static_cast<size_t>(e_vu1_aot_fallback::count)> m_fallback_reasons{};
    std::array<uint64_t, 2048> m_fallback_pcs{};
    uint64_t m_captures = 0u;
    uint64_t m_capture_errors = 0u;
};

// The synchronous VIF runner owns a thread-local binding; tests can
// install one without changing any guest state or global device object.
inline thread_local c_ee_vu1_aot* g_vu1_aot = nullptr;

class c_vu1_aot_binding
{
public:
    explicit c_vu1_aot_binding(c_ee_vu1_aot* backend)
        : m_previous(g_vu1_aot)
    {
        g_vu1_aot = backend;
    }
    ~c_vu1_aot_binding()
    {
        g_vu1_aot = m_previous;
    }
    c_vu1_aot_binding(const c_vu1_aot_binding&) = delete;
    c_vu1_aot_binding& operator=(const c_vu1_aot_binding&) = delete;
private:
    c_ee_vu1_aot* m_previous;
};

inline void write_vu1_aot_json(FILE* output)
{
    if (g_vu1_aot != nullptr)
    {
        g_vu1_aot->write_json(output);
    }
    else
    {
        std::fputs("{\"enabled\":false}", output);
    }
}
} // namespace ee
