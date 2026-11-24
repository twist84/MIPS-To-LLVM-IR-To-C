#pragma once

#include "iop_cpu.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace iop
{
enum class e_irx_load_result
{
    success,
    invalid_elf,
    invalid_bounds,
    invalid_module,
    unsupported_relocation,
    unresolved_import,
    invalid_import_target,
    insufficient_ram,
    invalid_export,
    duplicate_export
};

struct s_irx_export
{
    std::string m_library;
    uint16_t m_ordinal = 0u;
    uint32_t m_entry = 0u;
};

struct s_irx_module
{
    std::string m_name;
    uint32_t m_base = 0u;
    uint32_t m_size = 0u;
    // .text bytes from the IRX iopmod header (code starts at m_base).
    uint32_t m_text_size = 0u;
    uint32_t m_entry = 0u;
    uint32_t m_gp = 0u;
    std::vector<s_irx_export> m_exports;
};

// The registered IOP kernel/IRX libraries will supply import targets.
// An absent resolver never invents HLE implementations.
using t_irx_import_resolver = std::function<bool(
    const std::string& library, uint16_t ordinal, uint32_t* target
)>;

// Called after relocation and export validation, before any IOP RAM is
// modified. The kernel refuses collisions with already registered exports.
using t_irx_export_validator = std::function<bool(
    const std::vector<s_irx_export>& exports
)>;

// Import failures keep their actual guest library and ordinal.
struct s_irx_link_diagnostic
{
    e_irx_load_result m_result = e_irx_load_result::invalid_module;
    uint32_t m_entry_offset = 0u;
    uint32_t m_gp_offset = 0u;
    uint32_t m_image_size = 0u;
    std::string m_unresolved_library;
    uint16_t m_unresolved_ordinal = 0u;
};

// Load one Sony IOP ELF32 MIPS IRX at an explicitly allocated, aligned IOP
// RAM base. All parsing, relocation and import linking are performed on a
// private staging buffer. On failure, guest memory and output stay unchanged.
//
// No module thread is started, no syscalls are serviced, and no EE SIF
// status is altered. Allocation/scheduling are owned by the future kernel.
e_irx_load_result load_irx(
    c_cpu& cpu,
    const std::vector<uint8_t>& bytes,
    uint32_t base,
    s_irx_module* module,
    const t_irx_import_resolver& resolver = {},
    const t_irx_export_validator& validate_exports = {},
    s_irx_link_diagnostic* diagnostic = nullptr
);
} // namespace iop
