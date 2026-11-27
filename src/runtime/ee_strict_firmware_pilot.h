#pragma once

#include "ee_strict_reset.h"
#include "../tools/ee_host_timers.h"

#include <cstdlib>
#include <utility>
#include <vector>

namespace ee
{
// The pilot owns a reset domain, not a resumed snapshot. Only the bus-clock
// timer/INTC subset is connected; other devices and guest exception entry
// remain deliberately outside this checkpoint.
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
        if (m_timers.next_event() < end)
        {
            return {boundary::TIMER_DEADLINE};
        }

        m_branch_transaction = slots == 2u;
        const s_ee_reset_mmio mmio{this, &write, &read, true};
        const auto action = m_retired == 0u
            ? ee_execute_strict_reset_prefix(m_cpu, rom)
            : ee_execute_strict_rom_register_step(m_cpu, rom, budget, ram,
                nullptr, &m_tlb, &mmio, &m_cold_cache);
        m_branch_transaction = false;
        if (action.m_result != result::COMMITTED)
        {
            // The strict executor has host refusals, not guest exception
            // delivery. Do not advance time or service a device on refusal.
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
        // INTC can assert Cause.IP2, but no interrupt entry occurs in H1.
        const bool due = m_timers.next_event() <= timestamp;
        m_timers.service();
        return {boundary::RETIRED, action, due};
    }

private:
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
