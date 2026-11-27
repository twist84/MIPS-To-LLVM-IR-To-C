#pragma once

#include "ee_strict_reset.h"
#include "ee_strict_gs_timing.h"
#include "ee_vsync_flags.h"
#include "../tools/ee_host_timers.h"
#include "../tools/ee_iop_clock.h"
#include "../iop/iop_kernel.h"
#include "../iop/iop_sif_mmio_link.h"
#include "../iop/iop_sif0_dma_transport.h"
#include "../iop/iop_sif1_dma_transport.h"

#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ee
{
// The pilot owns a reset domain, not a resumed snapshot. Bus-clock timers,
// INTC and bounded synchronous SPR DMA are connected; guest exception
// entry is bounded to Level 1 interrupts and synchronous SYSCALL.
enum class e_ee_firmware_boundary : uint8_t
{
    RETIRED,
    HOST_REFUSAL,
    BUDGET,
    TIMER_DEADLINE,
    INTERRUPT_ENTRY,
    DEVICE_FAULT,
    SYSCALL_ENTRY
};

struct s_ee_firmware_step
{
    e_ee_firmware_boundary m_boundary = e_ee_firmware_boundary::HOST_REFUSAL;
    s_ee_reset_prefix_step m_instruction{};
    bool m_timer_serviced = false;
    uint32_t m_interrupt_vector = 0u;
    bool m_cop0_compare_crossed = false;
    bool m_dma_completed = false;
    bool m_vblank_started = false;
    bool m_vblank_ended = false;
    uint32_t m_iop_executed = 0u;
};

// Reference policy reserves two slots for supported branches and their delay
// slots, and one for ordinary instructions. This is not validated R5900
// timing, and no host dispatch/yield contributes a slot.
inline constexpr uint32_t ee_firmware_reference_slots(uint32_t opcode)
{
    const uint32_t primary = opcode >> 26u;
    return primary == 2u || primary == 3u || ee_strict_conditional_branch_supported(opcode) ||
        ee_strict_jr_supported(opcode) ||
        ee_strict_jalr_supported(opcode) ? 2u : 1u;
}

static_assert(ee_firmware_reference_slots(0u) == 1u);
static_assert(ee_firmware_reference_slots(0x10000001u) == 2u);
static_assert(ee_firmware_reference_slots(0x0ff00010u) == 2u);
static_assert(ee_firmware_reference_slots(0x03e00008u) == 2u);
static_assert(ee_firmware_reference_slots(0x0320f809u) == 2u);
static_assert(ee_firmware_reference_slots(0x1bc00063u) == 2u);
static_assert(ee_firmware_reference_slots(0x04800002u) == 2u);
static_assert(ee_firmware_reference_slots(0x04810002u) == 2u);
static_assert(ee_firmware_reference_slots(0x1bc10063u) == 1u);

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
    // deadline (65536 ticks at the reference NTSC line cadence).
    // Permit several fields. Guest HBlank programming is admitted only
    // with an explicit reference scanout configuration.
    static constexpr uint64_t k_slot_limit = 1u << 24u;
    static_assert(k_slot_limit <
        65536ull * ((c_ee_scanout_clock::k_ee_clock_hz * 1001ull / 60000ull) * 2ull / 525ull));

    explicit c_ee_strict_firmware_pilot(std::vector<uint8_t> rom)
        : m_rom(std::move(rom)), m_memory(k_ram_size)
    {
        if (!m_runtime.m_ee_cycle_scheduler.install_reference_compatibility_source())
        {
            std::abort();
        }
        if (m_memory.physical_ram_span(0u, k_ram_size) == nullptr)
        {
            std::abort();
        }
        m_hardware.bind_cpu(&m_cpu);
        m_hardware.bind_memory(&m_memory);
        m_runtime.m_vu0_state = &m_vu0;
        m_vif0.bind_vu0(&m_vu0);
        m_hardware.bind_vif0(&m_vif0);
        m_hardware.bind_vif1(&m_vif1);
        m_hardware.bind_gs(&m_gs);
        m_gif_path3.bind_event_sink(&m_gs, &c_ee_gs_state::gif_events);
        m_hardware.bind_device_transfer(this, &vif_dma);
    }

    c_ee_strict_firmware_pilot(const c_ee_strict_firmware_pilot&) = delete;
    c_ee_strict_firmware_pilot& operator=(const c_ee_strict_firmware_pilot&) = delete;

    // Select an explicit machine video configuration at reset, not a
    // guest SetGsCrt syscall or an invented completed field. Re-anchor
    // the reset Timer3 schedule to the selected line cadence at cycle 0.
    bool configure_scanout(uint32_t mode, uint32_t interlace)
    {
        if (m_retired != 0u || m_hardware.scanout().enabled() ||
            (mode != 2u && mode != 3u) || interlace > 1u ||
            !m_hardware.set_gs_crt(interlace, mode, 0u, 0u))
        {
            return false;
        }
        const s_ee_u128 reset_mode{0x183u, 0u};
        if (!c_ee_host_timers::mmio_write(&m_timers, 0x10001810u, 4u, &reset_mode))
        {
            std::abort();
        }
        return true;
    }

    uint64_t next_scanout_event() { return m_hardware.scanout().next_event_cycle(); }
    uint64_t fields_started() { return m_hardware.scanout().fields_started(); }
    uint64_t fields_ended() { return m_hardware.scanout().fields_ended(); }
    uint64_t gs_csr() { return m_hardware.scanout().csr(); }
    uint32_t gs_reset_count() const { return m_gs_reset_count; }
    uint32_t gif_reset_count() const { return m_gif_reset_count; }
    uint32_t gs_upper_count() const { return m_gs_upper_count; }
    uint64_t gs_siglblid() const { return m_gs.siglblid(); }
    uint32_t gs_crt_mode() const { return m_hardware.gs_crt_mode(); }
    const s_ee_strict_gs_timing& gs_timing() const { return m_gs_timing; }
    const s_ee_gs_display_registers& gs_display_registers() const
    {
        return m_hardware.gs_display_registers();
    }

    // Install a second reset domain before either processor executes.
    // Only immutable ROM input is supplied; no guest RAM/register
    // prepopulation or HLE service bootstrap.
    bool attach_iop(const std::vector<uint8_t>& rom, bool synchronized = false)
    {
        if (m_retired != 0u || m_iop != nullptr)
        {
            return false;
        }
        auto iop = std::make_unique<iop::c_kernel>();
        if (!iop->start_rom0_boot(rom))
        {
            return false;
        }
        m_iop = std::move(iop);
        m_iop_synchronized = synchronized;
        if (synchronized && m_iop_clock.observe(0u) != c_ee_iop_clock::e_result::accepted)
        {
            std::abort();
        }
        iop::c_sif_mmio_link::bind(m_iop->hardware(), m_hardware.sif());
        m_sif0 = std::make_unique<iop::c_sif0_dma_transport>(
            m_memory, m_hardware.dmac(), m_iop->cpu(), m_iop->hardware());
        m_sif1 = std::make_unique<iop::c_sif1_dma_transport>(
            m_memory, m_hardware.dmac(), m_iop->cpu(), m_iop->hardware());
        return true;
    }

    // Explicit independent IOP slice, not an EE-to-IOP clock mapping.
    // Calls are serialized between EE transactions. The next EE step
    // samples resulting interrupt levels before fetching any opcode.
    std::optional<iop::s_kernel_run_result> advance_iop(uint32_t budget)
    {
        if (m_iop == nullptr || m_iop_synchronized || budget == 0u || budget > k_slot_limit || sif_faulted())
        {
            return std::nullopt;
        }
        const auto run = m_iop->run(budget, budget);
        if (run.m_stop != iop::e_kernel_stop::budget_exhausted)
        {
            m_iop_stopped = true;
        }
        return run;
    }

    const iop::s_cpu_state* iop_state() const
    {
        return m_iop != nullptr ? &m_iop->cpu().state() : nullptr;
    }
    const std::optional<iop::s_kernel_run_result>& iop_last_run() const { return m_iop_last_run; }
    uint32_t sif_register_value(uint32_t id)
    {
        uint32_t value = 0u;
        (void)m_hardware.sif().get_reg(id, &value);
        return value;
    }
    bool delay_slot_pending() const { return m_delay_slot_pending; }
    uint64_t iop_cycles() const { return m_iop != nullptr ? m_iop->cycles() : 0u; }
    uint64_t iop_debt() const { return m_iop_clock.due(); }
    uint64_t next_iop_event() const { return m_iop_clock.next_cycle(); }
    uint32_t iop_irq_status() const
    {
        return m_iop != nullptr ? m_iop->hardware().scheduler_checkpoint().m_irq_stat : 0u;
    }

    bool sif_faulted() const
    {
        return m_iop_stopped || (m_sif0 != nullptr && m_sif0->faulted()) ||
            (m_sif1 != nullptr && m_sif1->faulted());
    }
    uint64_t sif0_delivered() const { return m_sif0 != nullptr ? m_sif0->delivered_bytes() : 0u; }
    uint64_t sif1_delivered() const { return m_sif1 != nullptr ? m_sif1->delivered_bytes() : 0u; }
    uint32_t sif1_fifo_words() const { return m_sif1 != nullptr ? m_sif1->fifo_words() : 0u; }

    std::span<const uint8_t> serial_output() const
    {
        return {m_sio_bytes.data(), m_sio.m_count};
    }
    uint32_t instruction_cache_lines() const { return m_instruction_cache.resident_count(); }
    uint32_t data_cache_lines() const { return m_data_cache.resident_count(); }
    uint32_t data_cache_dirty_lines() const { return m_data_cache.dirty_count(); }
    s_ee_fetch_plan current_fetch_plan() const
    {
        return ee_plan_instruction_fetch(m_memory.tlb(), m_cpu.m_pc,
            m_cpu.m_cop0[COP0_Status], m_cpu.m_cop0[COP0_Config],
            m_cpu.m_cop0[COP0_Context], m_cpu.m_cop0[COP0_EntryHi]);
    }
    s_ee_tlb_lookup current_tlb_lookup() const
    {
        return m_memory.tlb().lookup(m_cpu.m_pc, m_cpu.m_cop0[COP0_EntryHi] & 0xffu, false);
    }
    const s_ee_cpu_state& cpu() const { return m_cpu; }
    static constexpr size_t k_ram_size = 0x02000000u;
    std::span<const uint8_t> ram() const { return {m_memory.read_span(0u, k_ram_size), k_ram_size}; }
    uint32_t dmac_control() const { return m_dmac_control; }
    uint32_t dmac_status() { return m_hardware.dmac().status(); }
    uint32_t dmac_started() { return m_hardware.dmac().started_channels(); }
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
        if (sif_faulted())
        {
            return {boundary::DEVICE_FAULT};
        }
        if (budget != 0u && m_retired != 0u && !m_delay_slot_pending && ee_interrupts_enabled(&m_cpu))
        {
            const uint32_t vector = ee_check_interrupts(&m_cpu, &m_runtime, m_cpu.m_pc, 0u);
            return {boundary::INTERRUPT_ENTRY, {}, false, vector};
        }
        const s_ee_strict_rom0_view rom{m_rom.data(), m_rom.size()};
        auto* active_cache = m_iop_synchronized &&
            m_cold_cache.instruction_count() == c_ee_reset_cold_cache::k_instruction_lines
                ? &m_instruction_cache : nullptr;
        c_ee_reset_instruction_cache::c_transaction cache_transaction(active_cache);
        auto* active_data_cache = m_iop_synchronized &&
            m_cold_cache.data_count() == c_ee_reset_cold_cache::k_data_lines
                ? &m_data_cache : nullptr;
        c_ee_reset_data_cache::c_transaction data_transaction(active_data_cache);
        const s_ee_reset_ram_view ram{m_memory.physical_ram_span(0u, k_ram_size),
            k_ram_size, m_iop_synchronized, &cache_transaction, &data_transaction};
        const auto fetch = ee_plan_instruction_fetch(m_memory.tlb(), m_cpu.m_pc,
            m_cpu.m_cop0[COP0_Status], m_cpu.m_cop0[COP0_Config],
            m_cpu.m_cop0[COP0_Context], m_cpu.m_cop0[COP0_EntryHi]);
        const uint8_t* code = ee_fetch_strict_reset_word(fetch, rom, ram);
        const uint32_t opcode = code != nullptr ? ee_read_instruction_word(code) : 0u;
        const uint32_t natural_slots = m_delay_slot_pending ? 1u : ee_firmware_reference_slots(opcode);
        const auto deadline = [this](uint64_t end)
        {
            return m_timers.next_event() < end || next_compare_event() < end ||
                next_scanout_event() < end ||
                (m_iop_synchronized && next_iop_event() < end);
        };
        // Split only in the synchronized domain when a real boundary or
        // caller budget requires it. Unsplit and isolated behavior remains
        // unchanged. The branch is retired before the interior event.
        bool split = m_iop_synchronized && natural_slots == 2u &&
            (budget == 1u || deadline(cycle() + 2u));
        // A synchronous trap in a slot needs a committed branch and its
        // saved restart PC. Probe with a separate, discarded transaction
        // so a slot line cannot become resident on branch retirement.
        if (m_iop_synchronized && natural_slots == 2u && !split && budget >= 2u)
        {
            c_ee_reset_instruction_cache::c_transaction probe(active_cache);
            const s_ee_reset_ram_view probe_ram{m_memory.physical_ram_span(0u, k_ram_size),
                k_ram_size, true, &probe};
            const auto slot_fetch = ee_plan_instruction_fetch(m_memory.tlb(), m_cpu.m_pc + 4u,
                m_cpu.m_cop0[COP0_Status], m_cpu.m_cop0[COP0_Config],
                m_cpu.m_cop0[COP0_Context], m_cpu.m_cop0[COP0_EntryHi]);
            const auto* slot = ee_fetch_strict_reset_word(slot_fetch, rom, probe_ram);
            split = slot != nullptr && syscall_word(ee_read_instruction_word(slot));
        }
        const uint32_t slots = split ? 1u : natural_slots;
        if (budget < slots || slots > k_slot_limit - cycle())
        {
            return {boundary::BUDGET, {result::BUDGET_EXHAUSTED, opcode}};
        }

        const uint64_t end = cycle() + slots;
        // The reference executor commits branch+slot atomically. Refuse
        // an intervening deadline instead of processing it late or
        // committing a partial branch. An event exactly at end is legal.
        if (deadline(end) ||
            (m_gs_timing.m_stage == 5u && next_scanout_event() <= end))
        {
            return {boundary::TIMER_DEADLINE};
        }

        // Status/ERET in a delay slot is outside this atomic timing
        // contract. Preflight before the reference executor can link.
        if (slots == 2u)
        {
            const uint32_t slot_pc = m_cpu.m_pc + 4u;
            const auto slot_fetch = ee_plan_instruction_fetch(m_memory.tlb(), slot_pc,
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

        // SYSCALL is a precise, non-retiring synchronous exception,
        // not a successful instruction or an HLE request. The reference
        // clock counts committed retirements only; exception latency is
        // not calibrated. A successfully fetched trapping word is real
        // cache activity, unlike an unsupported host refusal.
        if (m_retired != 0u && code != nullptr && syscall_word(opcode) &&
            strict_reset_detail::scheduled_status_supported(m_cpu.m_cop0[COP0_Status]))
        {
            const uint32_t pc = m_cpu.m_pc;
            const uint32_t vector = ee_raise_exception(&m_cpu, &m_runtime, EX_Sys,
                pc, m_delay_slot_pending ? 1u : 0u, 0u);
            m_delay_slot_pending = false;
            m_delay_target = 0u;
            cache_transaction.commit();
            return {boundary::SYSCALL_ENTRY, {result::COMMITTED, opcode, 0u, pc}, false, vector};
        }

        const uint32_t previous_exit = m_cpu.m_exit_reason;
        // A previously accepted interrupt is an owned dispatcher handoff,
        // not an HLE callback. Preserve its exit marker on a host refusal.
        if (previous_exit == static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_EXCEPTION))
        {
            m_cpu.m_exit_reason = static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH);
        }
        m_branch_transaction = slots == 2u;
        const s_ee_reset_mmio mmio{this, &write, &read, true, true, true, m_iop != nullptr,
            m_hardware.scanout().enabled()};
        s_ee_reset_prefix_step action{};
        if (m_retired == 0u)
        {
            action = ee_execute_strict_reset_prefix(m_cpu, rom);
        }
        else if (m_delay_slot_pending)
        {
            action = execute_pending_slot(rom, ram, code, opcode, mmio);
        }
        else if (split && code != nullptr)
        {
            action = execute_branch_only(opcode);
        }
        else if (code != nullptr && owned_control(opcode))
        {
            action = execute_control(opcode);
        }
        else
        {
            action = ee_execute_strict_rom_register_step(m_cpu, rom, budget, ram,
                &m_sio, &m_memory.tlb(), &mmio, &m_cold_cache, true);
        }
        m_branch_transaction = false;
        if (action.m_result != result::COMMITTED)
        {
            m_cpu.m_exit_reason = previous_exit;
            m_dma_start_address = 0u;
            // No time or device service on a host refusal.
            return {boundary::HOST_REFUSAL, action};
        }

        // Only committed supported instructions reach this point. A mismatch
        // is an internal contract violation, not a retryable guest refusal.
        if (action.m_retired != slots)
        {
            std::abort();
        }
        cache_transaction.commit();
        data_transaction.commit();
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
        // A prevalidated normal SPR/VIF DMA start is a synchronous reference
        // transaction at the retired store boundary, before IRQ sampling.
        // No asynchronous latency/arbitration or fake completion is used.
        bool dma_completed = false;
        if (m_dma_start_address != 0u)
        {
            const uint32_t channel = dma_channel_bit(m_dma_start_address);
            const bool sif = sif_dma_register(m_dma_start_address);
            const s_ee_u128 value{m_dma_start_value, 0u};
            if (!c_ee_dmac::mmio_write(&m_hardware.dmac(), m_dma_start_address, 4u, &value))
            {
                std::abort();
            }
            dma_completed = (m_hardware.dmac().started_channels() & channel) == 0u;
            if (!sif && !dma_completed)
            {
                std::abort(); // prevalidated synchronous transfer contract
            }
            m_dma_start_address = 0u;
        }
        const bool due = m_timers.next_event() <= timestamp;
        m_timers.service();
        bool vblank_started = false;
        bool vblank_ended = false;
        if (m_hardware.scanout().enabled() && next_scanout_event() <= timestamp)
        {
            const auto serviced = m_runtime.m_ee_cycle_scheduler.service_scanout(
                m_hardware, m_vsync_flags, m_memory);
            if (serviced == e_ee_scheduled_event_result::INVALID_TIMESTAMP_OR_DELIVERY)
            {
                std::abort();
            }
            for (const uint8_t edge : m_hardware.scanout().delivered_vblank_edges())
            {
                // Raw IOP hardware IRQ latches, not HLE callbacks. The
                // PCSX2 IopCounters reference uses start IRQ0/end IRQ11.
                if (m_iop_synchronized && !m_iop->hardware().raise_interrupt(edge != 0u ? 0u : 11u))
                {
                    std::abort();
                }
                if (edge != 0u)
                {
                    vblank_started = true; // scanout raised INTC 2
                }
                else
                {
                    vblank_ended = true;
                    // ps2-web-recomp hw.cpp: VBlank end raises INTC 3.
                    if (!m_hardware.intc().raise(3u))
                    {
                        std::abort();
                    }
                }
            }
        }
        // Deterministic same-timestamp order: EE store/DMA, timers,
        // scanout edges, one due IOP instruction, then EE IRQ sampling.
        // At most one IOP slot is due under the branch preflight rule.
        uint32_t iop_executed = 0u;
        if (m_iop_synchronized)
        {
            if (m_iop_clock.observe(timestamp) != c_ee_iop_clock::e_result::accepted ||
                m_iop_clock.due() > 1u)
            {
                std::abort();
            }
            if (m_iop_clock.due() != 0u && !sif_faulted())
            {
                const auto run = m_iop->run(1u, 1u);
                m_iop_last_run = run;
                if (run.m_executed > 1u || !m_iop_clock.consume(run.m_executed))
                {
                    std::abort();
                }
                iop_executed = static_cast<uint32_t>(run.m_executed);
                if (run.m_stop != iop::e_kernel_stop::budget_exhausted || iop_executed != 1u)
                {
                    m_iop_stopped = true;
                }
            }
        }
        const bool ip7_before = (m_cpu.m_cop0[COP0_Cause] & (1u << 15u)) != 0u;
        ee_cop0_clock_sync(&m_cpu, &m_runtime);
        const bool compare_crossed = !ip7_before &&
            (m_cpu.m_cop0[COP0_Cause] & (1u << 15u)) != 0u;
        // Compatibility policy: Status is effective at its retirement
        // boundary; no calibrated MTC0 pipeline hazard is claimed.
        // Match the reference branch-boundary IRQ policy: hardware
        // progresses at the interior timestamp, but asynchronous EE
        // interrupts are sampled after the delay slot, not between the
        // branch and slot. This is not a calibrated issue/IRQ model.
        const uint32_t vector = m_delay_slot_pending ? 0u :
            ee_check_interrupts(&m_cpu, &m_runtime, m_cpu.m_pc, 0u);
        return {boundary::RETIRED, action, due, vector, compare_crossed, dma_completed,
            vblank_started, vblank_ended, iop_executed};
    }

private:
    // Only the scheduler-owned synchronized path can expose this
    // intermediate state. Target and link are sampled once; retrying a
    // refused delay-slot instruction never reexecutes the branch.
    s_ee_reset_prefix_step execute_branch_only(uint32_t opcode)
    {
        using result = e_ee_reset_prefix_result;
        const uint32_t pc = m_cpu.m_pc;
        if (!strict_reset_detail::scheduled_status_supported(m_cpu.m_cop0[COP0_Status]))
        {
            return {result::UNSAFE_BOUNDARY, opcode, 0u, pc};
        }
        const uint32_t primary = opcode >> 26u;
        const uint64_t lhs = strict_reset_detail::read_gpr(m_cpu, (opcode >> 21u) & 31u);
        const uint64_t rhs = strict_reset_detail::read_gpr(m_cpu, (opcode >> 16u) & 31u);
        if (ee_strict_jr_supported(opcode) || ee_strict_jalr_supported(opcode))
        {
            m_delay_target = static_cast<uint32_t>(
                strict_reset_detail::read_gpr(m_cpu, (opcode >> 21u) & 31u));
            const uint32_t link_reg = ee_strict_jalr_supported(opcode) ? (opcode >> 11u) & 31u : 0u;
            if (link_reg != 0u)
            {
                m_cpu.m_gpr[link_reg].m_lo = ee_strict_link_address(pc);
            }
        }
        else if (primary == 2u || primary == 3u)
        {
            m_delay_target = ee_strict_direct_jump_target(pc, opcode);
            if (primary == 3u)
            {
                m_cpu.m_gpr[31].m_lo = ee_strict_link_address(pc);
            }
        }
        else if (ee_strict_conditional_branch_supported(opcode))
        {
            m_delay_target = ee_strict_conditional_branch_target(pc, opcode, lhs, rhs);
        }
        else
        {
            return {result::UNSUPPORTED_OPCODE, opcode, 0u, pc};
        }
        m_delay_slot_pending = true;
        m_cpu.m_pc = pc + 4u;
        return {result::COMMITTED, opcode, 1u, pc};
    }

    s_ee_reset_prefix_step execute_pending_slot(const s_ee_strict_rom0_view& rom,
        const s_ee_reset_ram_view& ram, const uint8_t* code, uint32_t opcode,
        const s_ee_reset_mmio& mmio)
    {
        using result = e_ee_reset_prefix_result;
        const uint32_t pc = m_cpu.m_pc;
        if (code == nullptr)
        {
            return {result::FETCH_UNAVAILABLE, 0u, 0u, pc};
        }
        if (owned_control(opcode) || ee_firmware_reference_slots(opcode) != 1u)
        {
            return {result::UNSUPPORTED_OPCODE, opcode, 0u, pc};
        }
        const auto action = strict_reset_detail::execute_rom_word(m_cpu, rom, m_memory.tlb(),
            pc, opcode, ram, &m_sio, &m_memory.tlb(), &mmio, &m_cold_cache);
        if (action.m_result != result::COMMITTED)
        {
            return action;
        }
        m_cpu.m_pc = m_delay_target;
        m_delay_slot_pending = false;
        return {result::COMMITTED, opcode, 1u, pc};
    }

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

    static constexpr bool epc_write(uint32_t opcode)
    {
        return (opcode & 0xffe0ffffu) == 0x40807000u;
    }

    static constexpr bool clock_read(uint32_t opcode)
    {
        const uint32_t canonical = opcode & 0xffe0ffffu;
        return canonical == 0x40004800u || canonical == 0x40005800u ||
            canonical == 0x40006800u || canonical == 0x40007000u;
    }

    static constexpr bool syscall_word(uint32_t opcode)
    {
        // SPECIAL function 0x0c; the optional 20-bit code is ignored
        // by architectural exception entry.
        return (opcode & 0xfc00003fu) == 0x0000000cu;
    }

    static constexpr bool owned_control(uint32_t opcode)
    {
        return status_write(opcode) || clock_write(opcode) ||
            epc_write(opcode) || clock_read(opcode) || opcode == k_eret || syscall_word(opcode);
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
        else if (epc_write(opcode))
        {
            // EPC is a full 32-bit guest register. Target validity is
            // checked on subsequent instruction fetch, not at MTC0.
            ee_cop0_write(&m_cpu, COP0_EPC, static_cast<uint32_t>(
                strict_reset_detail::read_gpr(m_cpu, (opcode >> 16u) & 31u)));
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

    static constexpr uint32_t k_d_ctrl = 0x1000e000u;
    static constexpr uint32_t k_dma_channel_mask = 3u | (1u << 8u) | (1u << 9u);

    static constexpr bool spr_register(uint32_t address)
    {
        const uint32_t base = address & ~0xffu;
        const uint32_t offset = address & 0xffu;
        return (base == c_ee_dmac::k_from_spr_base || base == c_ee_dmac::k_to_spr_base) &&
            (offset == 0u || offset == 0x10u || offset == 0x20u || offset == 0x80u);
    }

    static constexpr bool vif_dma_register(uint32_t address)
    {
        const uint32_t base = address & ~0xffu;
        const uint32_t offset = address & 0xffu;
        return (base == c_ee_dmac::k_vif0_base || base == c_ee_dmac::k_vif1_base) &&
            (offset == 0u || offset == 0x10u || offset == 0x20u);
    }

    static constexpr bool sif_dma_register(uint32_t address)
    {
        const uint32_t base = address & ~0xffu;
        const uint32_t offset = address & 0xffu;
        return (base == c_ee_dmac::k_sif0_base || base == c_ee_dmac::k_sif1_base) &&
            (offset == 0u || offset == 0x10u || offset == 0x20u);
    }

    static constexpr uint32_t dma_channel_bit(uint32_t address)
    {
        switch (address & ~0xffu)
        {
        case c_ee_dmac::k_vif0_base: return 1u << 0u;
        case c_ee_dmac::k_vif1_base: return 1u << 1u;
        case c_ee_dmac::k_sif0_base: return 1u << 5u;
        case c_ee_dmac::k_sif1_base: return 1u << 6u;
        case c_ee_dmac::k_from_spr_base: return 1u << 8u;
        case c_ee_dmac::k_to_spr_base: return 1u << 9u;
        default: return 0u;
        }
    }

    static constexpr bool vif_command_supported(uint32_t word)
    {
        // Only complete, single-word commands, no VU starts, path
        // traffic, partial commands or interrupt stalls. MARK.I alone
        // is the production parser's explicitly non-stalling IRQ.
        const uint32_t op = (word >> 24u) & 0x7fu;
        return (word & 0x00ff0000u) == 0u &&
            ((op == 7u) || ((word & 0x80000000u) == 0u &&
             (op == 0u || op == 1u || op == 4u || op == 5u)));
    }

    static bool vif_packet_supported(const uint8_t* data, uint32_t qwc)
    {
        if (data == nullptr || qwc == 0u || qwc > 1024u)
        {
            return false;
        }
        for (uint32_t i = 0u; i < qwc * 4u; ++i)
        {
            if (!vif_command_supported(ee_read_instruction_word(data + i * 4u)))
            {
                return false;
            }
        }
        return true;
    }

    static bool vif_dma(void* user, uint32_t channel, const uint8_t* data, uint32_t qwc)
    {
        auto& self = *static_cast<c_ee_strict_firmware_pilot*>(user);
        if (channel > 1u || !vif_packet_supported(data, qwc))
        {
            return false;
        }
        return (channel == 0u ? self.m_vif0 : self.m_vif1).consume(data, qwc);
    }

    static constexpr bool sif_register(uint32_t address)
    {
        return address >= c_ee_sif::k_mscom_address && address <= c_ee_sif::k_smflag_address &&
            (address & 15u) == 0u;
    }

    c_ee_vif1* vif_register(uint32_t address)
    {
        const uint32_t offset = address & 0x3ffu;
        switch (offset)
        {
        case 0u: case 0x10u: case 0x20u: case 0x30u:
        case 0x40u: case 0x50u: case 0x80u: case 0x90u:
            break;
        default: return nullptr;
        }
        const uint32_t base = address & ~0x3ffu;
        if (base == c_ee_vif1::k_vif0_register_base)
        {
            return &m_vif0;
        }
        if (base == c_ee_vif1::k_register_base)
        {
            return &m_vif1;
        }
        return nullptr;
    }

    bool prepare_dma_start(uint32_t address, uint32_t word)
    {
        // Normal-mode STR only. Dedicated scratchpad channels have
        // fixed direction; accept the reference DIR bit without inventing
        // chain/interleave/tag behavior. Reject disabled DMAE instead of
        // pretending to model queued starts while the controller is off.
        const bool vif = vif_dma_register(address);
        const bool sif = sif_dma_register(address);
        if (m_dmac_control != 1u ||
            (m_hardware.dmac().started_channels() & dma_channel_bit(address)) != 0u ||
            (sif && m_iop == nullptr) ||
            (!sif && ((word & ~0x101u) != 0u || (vif && word != 0x101u))))
        {
            return false;
        }
        s_ee_u128 madr{}, qwc{};
        auto& dmac = m_hardware.dmac();
        if (!c_ee_dmac::mmio_read(&dmac, address + 0x10u, 4u, &madr) ||
            !c_ee_dmac::mmio_read(&dmac, address + 0x20u, 4u, &qwc))
        {
            return false;
        }
        if (sif && address == c_ee_dmac::k_sif0_base)
        {
            // Destination chain comes from genuine IOP DMA9, not EE
            // TADR or host memory. Do not complete an unpaired receiver.
            if ((word != 0x184u && word != 0x104u) || qwc.m_lo != 0u)
            {
                return false;
            }
            m_dma_start_address = address;
            m_dma_start_value = word;
            return true;
        }
        if (sif && word != 0x101u)
        {
            return false; // only normal forward SIF1 source in this batch
        }
        // Limit the pilot to nonzero bounded transfers with no alias or
        // physical span overflow. The production engine validates again.
        const uint64_t bytes = qwc.m_lo * 16u;
        if (qwc.m_lo == 0u || qwc.m_lo > 1024u || (madr.m_lo & 15u) != 0u ||
            madr.m_lo >= k_ram_size || bytes > k_ram_size - madr.m_lo ||
            !m_memory.contains(static_cast<uint32_t>(madr.m_lo), static_cast<size_t>(bytes)))
        {
            return false;
        }
        if (vif && !vif_packet_supported(m_memory.read_span(
                static_cast<uint32_t>(madr.m_lo), static_cast<size_t>(bytes)),
                static_cast<uint32_t>(qwc.m_lo)))
        {
            return false;
        }
        m_dma_start_address = address;
        m_dma_start_value = word;
        return true;
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
        if (self.m_hardware.scanout().enabled() &&
            (address == c_ee_hw_bus::k_gs_csr || address == c_ee_hw_bus::k_gs_csr + 4u))
        {
            return c_ee_hw_bus::mmio_read(&self.m_hardware, address, width, value);
        }
        if (address == k_d_ctrl)
        {
            *value = {self.m_dmac_control, 0u};
            return true;
        }
        if (address == c_ee_dmac::k_stat_address || spr_register(address) || vif_dma_register(address) ||
            (self.m_iop != nullptr && sif_dma_register(address)))
        {
            return c_ee_dmac::mmio_read(&self.m_hardware.dmac(), address, width, value);
        }
        if (sif_register(address))
        {
            return c_ee_sif::mmio_read(&self.m_hardware.sif(), address, width, value);
        }
        if (auto* vif = self.vif_register(address))
        {
            // FBRST is a write-only command; do not synthesize readback.
            return (address & 0x3ffu) != 0x10u &&
                c_ee_vif1::mmio_read(vif, address, width, value);
        }
        uint32_t index = 0u;
        uint32_t offset = 0u;
        if (!timer_register(address, index, offset) ||
            ((self.m_timers.current_mode(index) & 3u) == 3u &&
             !self.m_hardware.scanout().enabled()))
        {
            return false;
        }
        return c_ee_host_timers::mmio_read(&self.m_timers, address, width, value);
    }

    static bool write(void* user, uint32_t address, uint32_t width, const s_ee_u128* value)
    {
        auto& self = *static_cast<c_ee_strict_firmware_pilot*>(user);
        if (self.m_branch_transaction || value == nullptr)
        {
            return false;
        }
        if (self.m_hardware.scanout().enabled() &&
            s_ee_reset_mmio::gs_timing_write_address(address + 0xa0000000u, width))
        {
            // Stage the physical values, validating the entire mode tuple.
            // Only the actual release-SINT write commits the clock change.
            if (self.m_hardware.scanout().fields_started() != 0u ||
                self.next_scanout_event() <= self.cycle() + 1u)
            {
                return false;
            }
            auto next = self.m_gs_timing;
            if (!next.write(address, value->m_lo, self.gs_display_registers().m_smode2))
            {
                return false;
            }
            if (next.m_stage == 6u && !self.m_timers.reconfigure_cold_scanout(
                next.m_smode2 & 1u, next.m_mode, (next.m_smode2 >> 1u) & 1u))
            {
                return false;
            }
            self.m_gs_timing = next;
            return true;
        }
        if (self.m_hardware.scanout().enabled() &&
            s_ee_reset_mmio::gs_display_write_address(address + 0xa0000000u, width))
        {
            if ((address & ~4u) == 0x12000020u &&
                self.m_gs_timing.m_stage > 0u && self.m_gs_timing.m_stage < 6u)
            {
                return false;
            }
            // Latch the exact guest value using the shared GS model.
            // No synthesized timing change, draw, field or IRQ.
            return c_ee_hw_bus::mmio_write(&self.m_hardware, address, width, value);
        }
        if (width == 16u && (address == c_ee_vif1::k_vif0_fifo_base ||
                            address == c_ee_vif1::k_fifo_base))
        {
            const uint32_t words[] = {static_cast<uint32_t>(value->m_lo),
                static_cast<uint32_t>(value->m_lo >> 32u), static_cast<uint32_t>(value->m_hi),
                static_cast<uint32_t>(value->m_hi >> 32u)};
            for (const uint32_t word : words)
            {
                if (!vif_command_supported(word))
                {
                    return false;
                }
            }
            auto& vif = address == c_ee_vif1::k_vif0_fifo_base ? self.m_vif0 : self.m_vif1;
            return c_ee_vif1::mmio_write(&vif, address, width, value);
        }
        if (width != 4u)
        {
            return false;
        }
        if (address == c_ee_intc::k_mask_address || address == c_ee_intc::k_stat_address)
        {
            return c_ee_intc::mmio_write(&self.m_hardware.intc(), address, width, value);
        }
        if (address == c_ee_hw_bus::k_gs_csr)
        {
            if (static_cast<uint32_t>(value->m_lo) == 0x200u)
            {
                // Same bounded cold-GS reset as the isolated ROM0
                // probe. No GIF/VRAM work is connected to this pilot.
                // Do not reset the bus, scanout epoch, INTC or clocks.
                if (self.m_gs_reset_count != 0u ||
                    self.m_hardware.scanout().fields_started() != 0u ||
                    self.m_hardware.dmac().started_channels() != 0u)
                {
                    return false;
                }
                self.m_gs.reset();
                ++self.m_gs_reset_count;
                return true;
            }
            // Only CSR.VSINT W1C outside the cold reset command.
            return self.m_hardware.scanout().enabled() &&
                (static_cast<uint32_t>(value->m_lo) & ~8u) == 0u &&
                c_ee_hw_bus::mmio_write(&self.m_hardware, address, width, value);
        }
        if (address == c_ee_hw_bus::k_gs_csr + 4u)
        {
            if (static_cast<uint32_t>(value->m_lo) != 0u ||
                self.m_gs_reset_count != 1u || self.m_gs_upper_count != 0u)
            {
                return false;
            }
            ++self.m_gs_upper_count;
            return true; // reserved upper word, no control bits
        }
        const uint32_t dmac_word = static_cast<uint32_t>(value->m_lo);
        if (address == 0x10003000u)
        {
            // Cold GIF reset only. Reset the actual parser, not GS
            // VRAM, scanout, or DMA. No queued or live path is admitted.
            if (!self.m_hardware.scanout().enabled() || dmac_word != 1u ||
                self.m_gif_reset_count != 0u ||
                self.m_hardware.dmac().started_channels() != 0u ||
                self.m_hardware.dmac().gif_path3_masked() ||
                self.m_gif_path3.state().m_active ||
                self.m_gif_path3.counters().m_accepted_batches != 0u)
            {
                return false;
            }
            self.m_gif_path3.reset();
            ++self.m_gif_reset_count;
            return true;
        }
        if (address == k_d_ctrl)
        {
            if ((dmac_word & ~1u) != 0u ||
                (dmac_word == 0u && self.m_hardware.dmac().started_channels() != 0u))
            {
                return false;
            }
            self.m_dmac_control = dmac_word;
            return true;
        }
        if (address == c_ee_dmac::k_stat_address)
        {
            const uint32_t channels = k_dma_channel_mask | (self.m_iop != nullptr ? 0x60u : 0u);
            const uint32_t supported = channels | (channels << 16u);
            // Other channel sources and bus error state are not connected.
            if ((dmac_word & ~supported) != 0u)
            {
                return false;
            }
            return c_ee_dmac::mmio_write(&self.m_hardware.dmac(), address, width, value);
        }
        if (spr_register(address) || vif_dma_register(address) ||
            (self.m_iop != nullptr && sif_dma_register(address)))
        {
            if ((self.m_hardware.dmac().started_channels() & dma_channel_bit(address)) != 0u)
            {
                return false; // no cancellation or active register overwrite
            }
            if ((address & 0xffu) == 0u)
            {
                if ((dmac_word & c_ee_dmac::k_chcr_str) != 0u)
                {
                    return self.prepare_dma_start(address, dmac_word);
                }
                if ((dmac_word & ~1u) != 0u)
                {
                    return false;
                }
            }
            return c_ee_dmac::mmio_write(&self.m_hardware.dmac(), address, width, value);
        }
        if (sif_register(address))
        {
            // IOP-owned SMCOM is read-only, SMFLAG W1C. No IOP
            // producer or readiness is manufactured by this path.
            return c_ee_sif::mmio_write(&self.m_hardware.sif(), address, width, value);
        }
        if (auto* vif = self.vif_register(address))
        {
            const uint32_t offset = address & 0x3ffu;
            if ((offset == 0x10u && (dmac_word & ~9u) == 0u) ||
                (offset == 0x20u && (dmac_word & ~7u) == 0u) ||
                (offset == 0x30u && (dmac_word & ~0xffffu) == 0u))
            {
                return c_ee_vif1::mmio_write(vif, address, width, value);
            }
            return false;
        }
        uint32_t index = 0u;
        uint32_t offset = 0u;
        if (!timer_register(address, index, offset))
        {
            return false;
        }
        if (offset == 0x10u)
        {
            // No gating or reserved bits. HBlank uses the production
            // reference integer line divider only with a fixed CRT mode.
            const uint32_t word = static_cast<uint32_t>(value->m_lo);
            if ((word & ~0xfc3u) != 0u ||
                ((word & 3u) == 3u && !self.m_hardware.scanout().enabled()))
            {
                return false;
            }
        }
        else if ((self.m_timers.current_mode(index) & 3u) == 3u &&
                 !self.m_hardware.scanout().enabled())
        {
            return false;
        }
        // Service at the already-published instruction-start timestamp.
        // Only a successful callback can mutate device state.
        return c_ee_host_timers::mmio_write(&self.m_timers, address, width, value);
    }

    s_ee_cpu_state m_cpu = ee_make_strict_reset_prefix_state();
    const std::vector<uint8_t> m_rom;
    ee_memory m_memory;
    c_ee_reset_cold_cache m_cold_cache{};
    c_ee_reset_instruction_cache m_instruction_cache{};
    c_ee_reset_data_cache m_data_cache{};
    // The bounded serial sink records only committed guest SB accesses.
    // Capacity exhaustion is a refusal, never a silently discarded byte.
    std::vector<uint8_t> m_sio_bytes = std::vector<uint8_t>(65536u, 0u);
    s_ee_reset_sio_tx m_sio{m_sio_bytes.data(), m_sio_bytes.size(), 0u};
    s_ee_runtime m_runtime{};
    s_ee_vu0_state m_vu0{};
    c_ee_vif1 m_vif0{};
    c_ee_vif1 m_vif1{};
    c_ee_gs_state m_gs{};
    c_ee_gif_path3 m_gif_path3{};
    uint32_t m_gif_reset_count = 0u;
    s_ee_strict_gs_timing m_gs_timing{};
    uint32_t m_gs_reset_count = 0u;
    uint32_t m_gs_upper_count = 0u;
    c_ee_hw_bus m_hardware{};
    c_ee_vsync_flags m_vsync_flags{};
    c_ee_host_timers m_timers{&m_runtime, &m_hardware};
    uint64_t m_retired = 0u;
    uint32_t m_dmac_control = 0u;
    uint32_t m_dma_start_address = 0u;
    uint32_t m_dma_start_value = 0u;
    bool m_branch_transaction = false;
    std::optional<iop::s_kernel_run_result> m_iop_last_run;
    bool m_iop_stopped = false;
    bool m_delay_slot_pending = false;
    uint32_t m_delay_target = 0u;
    bool m_iop_synchronized = false;
    c_ee_iop_clock m_iop_clock{};
    // Destruction order detaches FIFO callbacks before either endpoint.
    std::unique_ptr<iop::c_kernel> m_iop;
    std::unique_ptr<iop::c_sif0_dma_transport> m_sif0;
    std::unique_ptr<iop::c_sif1_dma_transport> m_sif1;
};
} // namespace ee
