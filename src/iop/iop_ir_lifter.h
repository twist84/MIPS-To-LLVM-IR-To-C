#pragma once

// Ahead-of-time R3000A (IOP) to LLVM IR translation of one relocated,
// import-linked IRX .text image. The emitted function follows the ABI in
// iop_aot_abi.h and mirrors c_cpu::step semantics exactly (32-bit GPRs, no
// load delay slots). Instructions it does not model return
// IOP_AOT_INTERPRET with the precise resume state so the interpreter runs
// them: SYSCALL, BREAK, COP0, LWL/LWR/SWL/SWR, overflow and trap
// exceptions, memory faults, undefined encodings and branch-in-delay-slot.

#include <cstdint>
#include <string>
#include <vector>

namespace iop
{
struct s_iop_ir_module
{
    std::string m_name;
    uint32_t m_base = 0u;            // physical IOP address of .text
    std::vector<uint8_t> m_text;     // relocated, linked bytes
};

// Text of a complete LLVM IR module defining the external function
// `symbol` (i32 (ptr, i32)). Shared helpers are internal to the module.
std::string iop_lift_module_ir(const s_iop_ir_module& module,
                               const std::string& symbol);

// Experimental split lowering: each sorted, disjoint list contains ordinary
// resume PCs; delay slots still read the original linked image. Outputs
// one IR module per region plus a module-entry dispatcher. Unowned PCs
// fall back, and transfers between regions return to the CPU scheduler.
// On invalid input neither output is modified. No descriptor ABI change.
bool iop_lift_regions_ir(const s_iop_ir_module& module,
    const std::vector<std::vector<uint32_t>>& regions,
    const std::string& symbol, std::vector<std::string>* code,
    std::string* dispatcher);

// Text of the table module: exports iop_aot_module_count/iop_aot_modules
// for the given (module, symbol, hash) list.
struct s_iop_ir_table_entry
{
    std::string m_name;
    uint32_t m_base = 0u;
    uint32_t m_size = 0u;
    uint64_t m_hash = 0u;
    std::string m_symbol;
};
std::string iop_table_module_ir(const std::vector<s_iop_ir_table_entry>& entries,
                                bool windows_exports);
} // namespace iop
