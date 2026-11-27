#pragma once

#include "ee_strict_reset.h"
#include "../tools/ee_host_timers.h"

#include <cstdlib>
#include <limits>
#include <utility>
#include <vector>

namespace ee
{
// The pilot owns a reset domain, not a resumed snapshot. Only the bus-clock
// timer/INTC subset is connected; guest exception entry is bounded to
// Level 1 interrupts. Other devices remain disconnected.
enum class e_ee_firmware_boundary : uint8_t
{
    RETIRED,
    HOST_REFUSAL,
    BUDGET,
    TIMER_DEADLINE
};

struct s_ee_firmware_step
{
    e_ee_firmware_boundary m_boundary = e_ee_firmware_boundary::HOST_REFUSAL;
    s_ee_reset_prefix_step m_instruction{};
    bool m_timer_serviced = false;
    uint32_t m_interrupt_vector = 0u;
    bool m_cop0_compare_crossed = false;
};

// Reference policy reserves two slots for supported branches and their delay
// slots, and one for ordinary instructions. This is not validated R5900
// timing, and no host dispatch/yield contributes a slot.
inline constexpr uint32_t ee_firmware_reference_slots(uint32_t opcode)
{
    const uint32_t primary = opcode >> 26u;
    return primary == 2u || primary == 3u || primary == 4u ||
        primary == 5u || ee_strict_jr_supported(opcode) ? 2u : 1u;
}

static_assert(ee_firmware_reference_slots(0u) == 1u);
static_assert(ee_firmware_reference_slots(0x10000001u) == 2u);
static_assert(ee_firmware_reference_slots(0x0ff00010u) == 2u);
static_assert(ee_firmware_reference_slots(0x03e00008u) == 2u);

// Count advances modulo 2^32 under the existing compatibility clock.
// Starting at equality is not a new compare event until a full wrap.
// An already asserted IP7 is a level, not a second interrupt edge.
inline constexpr uint64_t ee_firmware_compare_distance(uint32_t count, uint32_t compare)
{
    const uint32_t distance = compare - count;
    return distance == 0u ? (1ull << 32u) : distance;
}

static_assert(ee_firmware_compare_distance(0u, 0u) == (1ull << 32u));
static_assert(ee_firmware_compare_distance(0xfffffffeu, 0u) == 2u);
static_assert(ee_firmware_compare_distance(12u, 14u) == 2u);

class c_ee_strict_firmware_pilot
{
public:
    // Keep the pilot below the unconfigured reset Timer3 HBlank compare
    // deadline (65536 ticks at the reference NTSC line cadence). Until
    // real scanout is connected, do not admit guest HBlank programming.
    static constexpr uint64_t k_slot_limit = 1u << 20u;
    static_assert(k_slot_limit <
        65536ull * ((c_ee_scanout_clock::k_ee_clock_hz * 1001ull / 60000ull) * 2ull / 525ull));

    explicit c_ee_strict_firmware_pilot(std::vector<uint8_t> rom)
        : m_rom(std::move(rom)), m_ram(0x02000000u, 0u)
    {
        if (!m_runtime.m_ee_cycle_scheduler.install_reference_compatibility_source())
        {
            std::abort();
        }
        m_hardware.bind_cpu(&m_cpu);
    }

    c_ee_strict_firmware_pilot(const c_ee_strict_firmware_pilot&) = delete;
    c_ee_strict_firmware_pilot& operator=(const c_ee_strict_firmware_pilot&) = delete;

    const s_ee_cpu_state& cpu() const { return m_cpu; }
    const std::vector<uint8_t>& ram() const { return m_ram; }
    uint64_t retired() const { return m_retired; }
    uint64_t cycle() const { return m_runtime.m_ee_cycle_scheduler.current_cycle(); }
    e_ee_cycle_source source() const { return m_runtime.m_ee_cycle_scheduler.source(); }
    uint64_t next_timer_event() const { return m_timers.next_event(); }
    uint64_t next_compare_event() const
    {
        if ((m_cpu.m_cop0[COP0_Cause] & (1u << 15u)) != 0u)
        {
            return std::numeric_limits<uint64_t>::max();
        }
        return cycle() + ee_firmware_compare_distance(
            m_cpu.m_cop0[COP0_Count], m_cpu.m_cop0[COP0_Compare]);
    }
    uint32_t timer_mode(uint32_t index) const { return m_timers.current_mode(index); }
    uint32_t timer_count(uint32_t index) const { return m_timers.current_count(index); }
    uint32_t intc_status() { return m_hardware.intc().status(); }
    uint32_t intc_mask() { return m_hardware.intc().mask(); }

    s_ee_firmware_step step(uint32_t budget)
    {
        using boundary = e_ee_firmware_boundary;
        using result = e_ee_reset_prefix_result;
        const s_ee_strict_rom0_view rom{m_rom.data(), m_rom.size()};
        const s_ee_reset_ram_view ram{m_ram.data(), m_ram.size()};
        const auto fetch = ee_plan_instruction_fetch(m_tlb, m_cpu.m_pc,
            m_cpu.m_cop0[COP0_Status], m_cpu.m_cop0[COP0_Config],
            m_cpu.m_cop0[COP0_Context], m_cpu.m_cop0[COP0_EntryHi]);
        const uint8_t* code = ee_fetch_strict_reset_word(fetch, rom, ram);
        const uint32_t opcode = code != nullptr ? ee_read_instruction_word(code) : 0u;
        const uint32_t slots = ee_firmware_reference_slots(opcode);
        if (budget < slots || slots > k_slot_limit - cycle())
        {
            return {boundary::BUDGET, {result::BUDGET_EXHAUSTED, opcode}};
        }

        const uint64_t end = cycle() + slots;
        // The reference executor commits branch+slot atomically. Refuse
        // an intervening deadline instead of processing it late or
        // committing a partial branch. An event exactly at end is legal.
        if (m_timers.next_event() < end || next_compare_event() < end)
        {
            return {boundary::TIMER_DEADLINE};
        }

        // Status/ERET in a delay slot is outside this atomic timing
        // contract. Preflight before the reference executor can link.
        if (slots == 2u)
        {
            const uint32_t slot_pc = m_cpu.m_pc + 4u;
            const auto slot_fetch = ee_plan_instruction_fetch(m_tlb, slot_pc,
                m_cpu.m_cop0[COP0_Status], m_cpu.m_cop0[COP0_Config],
                m_cpu.m_cop0[COP0_Context], m_cpu.m_cop0[COP0_EntryHi]);
            const uint8_t* slot = ee_fetch_strict_reset_word(slot_fetch, rom, ram);
            if (slot != nullptr)
            {
                const uint32_t word = ee_read_instruction_word(slot);
                if (owned_control(word))
                {
                    return {boundary::HOST_REFUSAL,
                        {result::UNSUPPORTED_OPCODE, word, 0u, slot_pc}};
                }
            }
        }

        const uint32_t previous_exit = m_cpu.m_exit_reason;
        // A previously accepted interrupt is an owned dispatcher handoff,
        // not an HLE callback. Preserve its exit marker on a host refusal.
        if (previous_exit == static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_EXCEPTION))
        {
            m_cpu.m_exit_reason = static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH);
        }
        m_branch_transaction = slots == 2u;
        const s_ee_reset_mmio mmio{this, &write, &read, true};
        s_ee_reset_prefix_step action{};
        if (m_retired == 0u)
        {
            action = ee_execute_strict_reset_prefix(m_cpu, rom);
        }
        else if (code != nullptr && owned_control(opcode))
        {
            action = execute_control(opcode);
        }
        else
        {
            action = ee_execute_strict_rom_register_step(m_cpu, rom, budget, ram,
                nullptr, &m_tlb, &mmio, &m_cold_cache, true);
        }
        m_branch_transaction = false;
        if (action.m_result != result::COMMITTED)
        {
            m_cpu.m_exit_reason = previous_exit;
            // No time or device service on a host refusal.
            return {boundary::HOST_REFUSAL, action};
        }

        // Only committed supported instructions reach this point. A mismatch
        // is an internal contract violation, not a retryable guest refusal.
        if (action.m_retired != slots)
        {
            std::abort();
        }
        m_retired += action.m_retired;
        m_runtime.m_reference_instruction_slots += slots;
        uint64_t timestamp = 0u;
        if (!ee_reference_compatibility_cycle(m_runtime, timestamp) ||
            timestamp != end || !m_runtime.m_ee_cycle_scheduler.publish_cycle(timestamp))
        {
            std::abort();
        }

        // Effects of the instruction/slot are committed first; then service
        // due timers (production timer model uses ascending channel order).
        const bool due = m_timers.next_event() <= timestamp;
        m_timers.service();
        const bool ip7_before = (m_cpu.m_cop0[COP0_Cause] & (1u << 15u)) != 0u;
        ee_cop0_clock_sync(&m_cpu, &m_runtime);
        const bool compare_crossed = !ip7_before &&
            (m_cpu.m_cop0[COP0_Cause] & (1u << 15u)) != 0u;
        // Compatibility policy: Status is effective at its retirement
        // boundary; no calibrated MTC0 pipeline hazard is claimed.
        // An atomic branch has completed its slot, so EPC is its next PC
        // and BD is clear. Intra-branch deadlines still refuse above.
        const uint32_t vector = ee_check_interrupts(&m_cpu, &m_runtime, m_cpu.m_pc, 0u);
        return {boundary::RETIRED, action, due, vector, compare_crossed};
    }

private:
    static constexpr uint32_t k_eret = 0x42000018u;

    static constexpr bool status_write(uint32_t opcode)
    {
        return (opcode & 0xffe0ffffu) == 0x40806000u;
    }

    static constexpr bool clock_write(uint32_t opcode)
    {
        const uint32_t canonical = opcode & 0xffe0ffffu;
        return canonical == 0x40804800u || canonical == 0x40805800u;
    }

    static constexpr bool clock_read(uint32_t opcode)
    {
        const uint32_t canonical = opcode & 0xffe0ffffu;
        return canonical == 0x40004800u || canonical == 0x40005800u ||
            canonical == 0x40006800u || canonical == 0x40007000u;
    }

    static constexpr bool owned_control(uint32_t opcode)
    {
        return status_write(opcode) || clock_write(opcode) ||
            clock_read(opcode) || opcode == k_eret;
    }

    s_ee_reset_prefix_step execute_control(uint32_t opcode)
    {
        using result = e_ee_reset_prefix_result;
        const uint32_t pc = m_cpu.m_pc;
        const uint32_t status = m_cpu.m_cop0[COP0_Status];
        if (!strict_reset_detail::scheduled_status_supported(status))
        {
            return {result::UNSAFE_BOUNDARY, opcode, 0u, pc};
        }
        if (opcode == k_eret)
        {
            if ((status & STATUS_EXL) == 0u)
            {
                return {result::UNSUPPORTED_OPCODE, opcode, 0u, pc};
            }
            (void)ee_exception_return(&m_cpu);
        }
        else if (clock_write(opcode))
        {
            // All previous elapsed time is committed. The shared helper
            // writes the core-global clock and acknowledges IP7 on Compare.
            const uint32_t reg = (opcode >> 11u) & 31u;
            const uint32_t value = static_cast<uint32_t>(
                strict_reset_detail::read_gpr(m_cpu, (opcode >> 16u) & 31u));
            ee_cop0_clock_write(&m_cpu, &m_runtime, reg, value);
            m_cpu.m_pc += 4u;
        }
        else if (clock_read(opcode))
        {
            const uint32_t reg = (opcode >> 11u) & 31u;
            const uint32_t rt = (opcode >> 16u) & 31u;
            if (rt != 0u)
            {
                m_cpu.m_gpr[rt].m_lo = strict_reset_detail::sign_extend_word(m_cpu.m_cop0[reg]);
            }
            m_cpu.m_pc += 4u;
        }
        else
        {
            const uint32_t requested = static_cast<uint32_t>(
                strict_reset_detail::read_gpr(m_cpu, (opcode >> 16u) & 31u));
            if (!strict_reset_detail::scheduled_status_supported(requested))
            {
                return {result::UNSUPPORTED_OPCODE, opcode, 0u, pc};
            }
            // All preceding committed time was synchronized at the previous
            // boundary. The common COP0 write owns the register update.
            ee_cop0_write(&m_cpu, COP0_Status, requested);
            m_cpu.m_pc += 4u;
        }
        return {result::COMMITTED, opcode, 1u, pc};
    }

    static bool timer_register(uint32_t address, uint32_t& index, uint32_t& offset)
    {
        if (address < 0x10000000u || address >= 0x10002000u)
        {
            return false;
        }
        index = (address >> 11u) & 3u;
        offset = address & 0x7ffu;
        return offset == 0u || offset == 0x10u || offset == 0x20u;
    }

    static bool read(void* user, uint32_t address, uint32_t width, s_ee_u128* value)
    {
        auto& self = *static_cast<c_ee_strict_firmware_pilot*>(user);
        // MMIO in an atomic branch slot needs a separate issue-time contract.
        // Refuse before the device callback, so nothing leaks on rollback.
        if (self.m_branch_transaction || width != 4u || value == nullptr)
        {
            return false;
        }
        if (address == c_ee_intc::k_mask_address || address == c_ee_intc::k_stat_address)
        {
            return c_ee_intc::mmio_read(&self.m_hardware.intc(), address, width, value);
        }
        uint32_t index = 0u;
        uint32_t offset = 0u;
        if (!timer_register(address, index, offset) ||
            (self.m_timers.current_mode(index) & 3u) == 3u)
        {
            return false;
        }
        return c_ee_host_timers::mmio_read(&self.m_timers, address, width, value);
    }

    static bool write(void* user, uint32_t address, uint32_t width, const s_ee_u128* value)
    {
        auto& self = *static_cast<c_ee_strict_firmware_pilot*>(user);
        if (self.m_branch_transaction || width != 4u || value == nullptr)
        {
            return false;
        }
        if (address == c_ee_intc::k_mask_address || address == c_ee_intc::k_stat_address)
        {
            return c_ee_intc::mmio_write(&self.m_hardware.intc(), address, width, value);
        }
        uint32_t index = 0u;
        uint32_t offset = 0u;
        if (!timer_register(address, index, offset))
        {
            return false;
        }
        if (offset == 0x10u)
        {
            // No gating, HBlank, or reserved bits in this pilot.
            const uint32_t word = static_cast<uint32_t>(value->m_lo);
            if ((word & ~0xfc3u) != 0u || (word & 3u) == 3u)
            {
                return false;
            }
        }
        else if ((self.m_timers.current_mode(index) & 3u) == 3u)
        {
            return false;
        }
        // Service at the already-published instruction-start timestamp.
        // Only a successful callback can mutate device state.
        return c_ee_host_timers::mmio_write(&self.m_timers, address, width, value);
    }

    s_ee_cpu_state m_cpu = ee_make_strict_reset_prefix_state();
    const std::vector<uint8_t> m_rom;
    std::vector<uint8_t> m_ram;
    c_ee_tlb m_tlb{};
    c_ee_reset_cold_cache m_cold_cache{};
    s_ee_runtime m_runtime{};
    c_ee_hw_bus m_hardware{};
    c_ee_host_timers m_timers{&m_runtime, &m_hardware};
    uint64_t m_retired = 0u;
    bool m_branch_transaction = false;
};
} // namespace ee
