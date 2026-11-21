#pragma once

// Captures read-only provenance for an actual stopped IOP R3000 thread.
// A captured fault is never treated as guest success or an emulated IRQ.

#include "iop_kernel.h"

#include <array>
#include <cstdint>
#include <cstdio>

namespace iop
{

struct s_iop_fault_word
{
    uint32_t m_address = 0u;
    uint32_t m_opcode = 0u;
    bool m_readable = false;
};

class c_iop_guest_fault_probe final
{
public:
    static constexpr size_t k_window_words = 7u;

    bool capture(const c_kernel& kernel, const s_kernel_run_result& result);
    bool captured() const;
    void write_text(FILE* output) const;
    void write_json(FILE* output) const;

    uint32_t thread_id() const;
    uint32_t guest_pc() const;
    uint32_t entry() const;
    uint32_t recent_count() const;
    const std::array<s_iop_fault_word, k_window_words>& pc_words() const;

private:
    static uint32_t matching_module(const c_kernel& kernel, uint32_t address);
    static void capture_window(const c_kernel& kernel, uint32_t center,
        std::array<s_iop_fault_word, k_window_words>& out);
    static void write_words_json(FILE* output,
        const std::array<s_iop_fault_word, k_window_words>& words);
    static void write_words_text(FILE* output, const char* name,
        const std::array<s_iop_fault_word, k_window_words>& words);

    s_kernel_run_result m_fault{};
    s_kernel_thread m_thread{};
    s_memory_fault m_memory_fault{};
    uint32_t m_pc_module = 0u;
    uint32_t m_entry_module = 0u;
    uint32_t m_ra_module = 0u;
    std::array<s_iop_fault_word, k_window_words> m_pc_words{};
    std::array<s_iop_fault_word, k_window_words> m_entry_words{};
    std::array<s_iop_fault_word, k_window_words> m_ra_words{};
    std::array<s_iop_fault_word, k_window_words> m_sp_words{};
    bool m_captured = false;
};

} // namespace iop
