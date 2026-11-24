#pragma once

// ABI shared by the IOP runtime and ahead-of-time recompiled IRX modules
// (tools/iop_recomp.py). Plain C layout: generated code is compiled
// separately into a host library and loaded at runtime.
//
// A translated module covers the relocated, import-linked .text of one IRX
// at the exact IOP load base it was captured from. The runtime activates it
// only when the bytes in IOP RAM hash to the same value, so a different load
// base or a modified image falls back to the R3000 interpreter.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct s_iop_aot_context
{
    uint32_t* m_gpr;        // 32 guest GPRs (r0 must stay zero)
    uint32_t* m_hi;
    uint32_t* m_lo;
    uint8_t* m_ram;         // 2 MiB IOP RAM
    void* m_cpu;            // opaque c_cpu for slow-path accesses
    // Non-RAM or faulting accesses. Return non-zero on success.
    int (*m_read)(void* cpu, uint32_t address, uint32_t width,
                  uint32_t* value);
    int (*m_write)(void* cpu, uint32_t address, uint32_t width,
                   uint32_t value);
    uint32_t m_budget;      // instruction budget for this call
    // Outputs: instructions retired and the architectural resume state.
    uint32_t m_executed;
    uint32_t m_pc;
    uint32_t m_next_pc;
    uint32_t m_in_delay;
    uint32_t m_branch_pc;
} s_iop_aot_context;

// Return values of a module entry.
enum
{
    // Leave translated code; resume (pc, next_pc) in the dispatcher.
    IOP_AOT_EXIT = 0,
    // Interpret exactly the instruction at pc (syscall, COP0, overflow,
    // memory fault, unsupported encoding), then return to the dispatcher.
    IOP_AOT_INTERPRET = 1
};

typedef uint32_t (*t_iop_aot_entry)(s_iop_aot_context* context, uint32_t pc);

typedef struct s_iop_aot_module
{
    const char* m_name;
    uint32_t m_base;        // first translated byte (module .text start)
    uint32_t m_size;        // translated bytes
    uint64_t m_hash;        // FNV-1a 64 of the translated bytes
    t_iop_aot_entry m_entry;
} s_iop_aot_module;

// Exported by a translated library.
typedef uint32_t (*t_iop_aot_module_count)(void);
typedef const s_iop_aot_module* (*t_iop_aot_modules)(void);

#ifdef __cplusplus
}
#endif
