#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace iop
{
class c_hw;
// The PS2 IOP is an R3000A (MIPS I), separate from the EE R5900.
// This standalone, fail-closed interpreter is the execution foundation
// for the IOP kernel/IRX loader. It does not start an IOP or set SIF flags.
enum class e_step_result
{
    completed,
    syscall,
    breakpoint,
    unsupported,
    memory_fault,
    overflow,
    budget_exhausted
};

struct s_cpu_state
{
    uint32_t m_gpr[32]{};
    uint32_t m_hi = 0u;
    uint32_t m_lo = 0u;
    uint32_t m_pc = 0u;
    uint32_t m_next_pc = 4u;
    uint32_t m_cop0_status = 0u;
    uint32_t m_cop0_cause = 0u;
    uint32_t m_cop0_epc = 0u;
    uint32_t m_cop0_prid = 0u;
    // Delay-slot identity survives context switches and allows BD/EPC
    // to describe guest exceptions at the precise branch boundary.
    bool m_in_delay_slot = false;
    uint32_t m_branch_pc = 0u;
    // Host-only bounded guest instruction history, preserved across IOP
    // thread switches. Does not feed COP0, guest RAM or virtual cycles.
    static constexpr uint32_t k_recent_pc_capacity = 8u;
    uint32_t m_recent_pc[k_recent_pc_capacity]{};
    uint32_t m_recent_opcode[k_recent_pc_capacity]{};
    uint32_t m_recent_next = 0u;
    uint32_t m_recent_count = 0u;
};

struct s_run_result
{
    e_step_result m_result = e_step_result::completed;
    uint32_t m_instructions = 0u;
    uint32_t m_pc = 0u;
    uint32_t m_opcode = 0u;
};

// Diagnostic evidence from the guest address that failed its final
// memory read/write. Not an architectural COP0 BadVAddr register.
struct s_memory_fault
{
    uint32_t m_address = 0u;
    uint32_t m_width = 0u;
    bool m_write = false;
    bool m_valid = false;
};

class c_cpu
{
public:
    static constexpr uint32_t k_ram_size = 2u * 1024u * 1024u;
    // Retail IOP maps the 2 MiB backing RAM four times in the first
    // 8 MiB of physical address space (including KSEG0/KSEG1 mirrors).
    static constexpr uint32_t k_ram_mirror_size = 8u * 1024u * 1024u;
    static constexpr uint32_t k_rom0_size = 4u * 1024u * 1024u;
    static constexpr uint32_t k_rom0_physical = 0x1fc00000u;
    static constexpr uint32_t k_rom0_reset_pc = 0xbfc00000u;

    c_cpu();
    void reset(uint32_t entry = 0u);
    void bind_hardware(c_hw* hardware);

    // The 4 MiB primary PS2 BIOS ROM is read-only at physical
    // 0x1fc00000, KSEG0 and KSEG1. No firmware is provided by the
    // emulator; the caller supplies a legal local ROM dump.
    bool install_rom0(const std::vector<uint8_t>& image);
    bool has_rom0() const;
    bool reset_rom0();

    const s_cpu_state& state() const;
    s_cpu_state& state();

    uint64_t cycles() const;
    const s_memory_fault& last_memory_fault() const;
    bool read8(uint32_t address, uint8_t* value) const;
    bool write8(uint32_t address, uint8_t value);
    bool read32(uint32_t address, uint32_t* value) const;
    bool write32(uint32_t address, uint32_t value);
    // Hardware DMA reads and writes *actual* IOP RAM, never MMIO or
    // unbacked ROM. Preflight the complete range before copying.
    bool dma_read(uint32_t address, uint8_t* data, uint32_t size) const;
    bool dma_write(uint32_t address, const uint8_t* data, uint32_t size);

    // Exactly one guest instruction, with architectural branch-delay slots.
    // Exceptions and unsupported instructions leave PC at the faulting word.
    e_step_result step(uint32_t* opcode = nullptr);
    s_run_result run(uint32_t instruction_budget);

    // After a syscall is explicitly serviced by a future IOP kernel, the
    // kernel may advance past its instruction. Never do so automatically.
    void resume_syscall();

private:
    bool read(uint32_t address, uint32_t width, uint32_t* value) const;
    bool write(uint32_t address, uint32_t width, uint32_t value);
    bool fail_memory(uint32_t address, uint32_t width, bool write) const;
    void take_exception(uint32_t cause_code);

    c_hw* m_hardware = nullptr;
    std::vector<uint8_t> m_ram;
    std::vector<uint8_t> m_rom0;
    s_cpu_state m_state{};
    mutable s_memory_fault m_last_memory_fault{};
    uint64_t m_cycles = 0u;
    bool m_guest_exceptions = false;
};
} // namespace iop
