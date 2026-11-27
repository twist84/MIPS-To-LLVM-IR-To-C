#pragma once
#include "ee_state.h"
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>

namespace ee {

// Result of recompiling one guest function.
struct s_recompiled_fn
{
    std::string name;                  // Guest symbol / display name.
    std::string host_symbol;            // Unique native symbol keyed by guest entry VA.
    uint32_t vaddr = 0;
    uint32_t size = 0;                 // bytes decoded
    uint32_t insn_count = 0;
    uint32_t supported_count = 0;      // instructions lifted to a known op
    uint32_t unsupported_count = 0;    // instructions with no lift
    uint32_t block_count = 0;
    // Guest PCs accepted by the generated function's entry switch. The host
    // dispatcher uses these to resume a translated function at any CFG block.
    std::vector<uint32_t> block_pcs;
    std::string ir_path;               // emitted LLVM IR text file
};

// Result of recompiling a whole ELF.
struct s_recompiled_elf
{
    std::string path;
    uint32_t entry_point = 0;
    uint16_t machine = 0;
    uint32_t segment_count = 0;
    uint32_t function_count = 0;       // functions actually emitted
    uint32_t functions_discovered = 0; // total entries found (before cap)
    // Primary totals: function-scoped (union of CFG-reachable instructions).
    uint32_t total_insns = 0;
    uint32_t total_supported = 0;
    uint32_t total_unsupported = 0;
    // Secondary: full linear executable-segment sweep (includes data noise).
    uint32_t linear_insns = 0;
    uint32_t linear_supported = 0;
    uint32_t linear_unsupported = 0;
    uint32_t function_scoped_pcs = 0; // unique PCs covered by CFGs
    std::vector<s_recompiled_fn> functions;
    // Opcode coverage (function-scoped): opcode -> {total, supported}
    std::vector<std::pair<uint16_t, std::pair<uint32_t, uint32_t>>> coverage;
    // Linear sweep opcode coverage (same shape).
    std::vector<std::pair<uint16_t, std::pair<uint32_t, uint32_t>>> linear_coverage;
    // Sub-op histograms for multi-funct primaries (function-scoped).
    // Key: (opcode<<8)|funct  or for COP1 (opcode<<16)|(fmt<<8)|funct
    // Stored as opcode, sub, total, supported for JSON simplicity.
    struct s_sub_cov {
        uint16_t opcode = 0;
        uint16_t sub = 0;      // funct or REGIMM rt or COP1 fmt
        uint16_t sub2 = 0;     // COP1 funct when sub is fmt
        uint32_t total = 0;
        uint32_t supported = 0;
    };
    std::vector<s_sub_cov> sub_coverage;
};

// Find the translated function whose entry switch accepts guest_pc.
// Returns nullptr when the PC is outside all emitted CFGs.
const s_recompiled_fn* find_recompiled_function(
    const s_recompiled_elf& compiled,
    uint32_t guest_pc);

// Emission controls for ee_recomp.
struct s_recomp_options
{
    // Emit at most this many functions (0 = unlimited).
    uint32_t max_functions = 0;
    // If non-zero, emit only the function at this guest VA (ignores max_functions
    // for other entries; still discovers all entries for CFG bounds).
    uint32_t only_function = 0;
    // Explicit batch of executable guest entry addresses. Loads ELF/discovery
    // once and emits each requested entry to a separate IR module.
    // Mutually exclusive with only_function and max_functions.
    std::vector<uint32_t> only_functions;
    // LLVM optimisation level hint (0-3); currently logged only.
    int opt_level = 0;
    // Bounded parallel emission for explicit batches only; ownership is serial.
    uint32_t ir_jobs = 1;
    // Reuse a verified full-ELF discovery snapshot when emitting one function.
    // The snapshot is tied to the exact source ELF and discovery format.
    std::string discovery_cache_path;
};

// Recompile functions discovered in `elf_path` into per-function LLVM IR
// text files plus coverage metadata. Returns false (fills `error`) if the ELF
// cannot be loaded.
//
// With default options every discovered function is emitted. Use
// opts.max_functions or opts.only_function to cap emission for testing.
bool ee_recomp(const char* elf_path,
               const char* output_dir,
               s_recompiled_elf* result,
               std::string* error = nullptr,
               const s_recomp_options& opts = {});

struct s_ee_recomp_session_state;
class c_ee_recomp_session
{
public:
    c_ee_recomp_session();
    ~c_ee_recomp_session();
    bool recompile(const char* elf_path, const char* output_dir,
                   s_recompiled_elf* result, std::string* error,
                   const s_recomp_options& opts);

private:
    std::unique_ptr<s_ee_recomp_session_state> m_state;
};

} // namespace ee
