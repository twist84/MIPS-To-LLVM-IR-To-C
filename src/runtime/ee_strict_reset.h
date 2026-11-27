#pragma once

#include "ee_strict_rom0.h"
#include "ee_strict_branch.h"

namespace ee
{
// EE Core User's Manual v6.0 p.78: IC=010 (16 KiB), DC=001
// (8 KiB), EC=0 are fixed. K0 is undefined on reset; choose 0,
// matching the reference reset profile. All enable bits reset clear.
inline constexpr uint32_t k_ee_reset_config_fixed = 0x00000440u;
inline constexpr uint32_t k_ee_reset_config_writable = 0x00073007u;
inline constexpr uint32_t k_ee_reset_config_readonly = 0x70000fc0u;

inline constexpr bool ee_strict_reset_config_write_supported(uint32_t value)
{
    const uint32_t k0 = value & 7u;
    return (value & ~(k_ee_reset_config_writable | k_ee_reset_config_readonly)) == 0u &&
        (k0 == 0u || k0 == 2u || k0 == 3u || k0 == 7u);
}

inline constexpr uint32_t ee_strict_reset_config_value(uint32_t value)
{
    return k_ee_reset_config_fixed | (value & k_ee_reset_config_writable);
}

static_assert(ee_strict_reset_config_value(0u) == 0x440u);
static_assert(ee_strict_reset_config_value(0x73003u) == 0x73443u);
static_assert(ee_strict_reset_config_value(0x70000fc0u) == 0x440u);
static_assert(ee_strict_reset_config_write_supported(0x73443u));
static_assert(!ee_strict_reset_config_write_supported(1u));
static_assert(!ee_strict_reset_config_write_supported(0x80000000u));

// Minimal isolated reset-prefix experiment. These are only the COP0
// reset values relevant to the supported initial instruction (EE Core
// User's Manual v6.0, 4.2.1). Unspecified state is initialized to zero
// for reproducibility, not asserted as hardware power-on contents.
// No cache, device, counter clock or live runtime reset is implied.
inline constexpr s_ee_cpu_state ee_make_strict_reset_prefix_state()
{
    s_ee_cpu_state cpu{};
    cpu.m_pc = 0xbfc00000u;
    cpu.m_exit_reason = static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH);
    cpu.m_cop0[COP0_Status] = STATUS_ERL | STATUS_BEV;
    cpu.m_cop0[COP0_Random] = 47u;
    cpu.m_cop0[COP0_Config] = k_ee_reset_config_fixed;
    // Reference-compatible R5900 model/revision (PCSX2 cpuReset).
    // Not a claim that all physical EE revisions have this low byte.
    cpu.m_cop0[COP0_PRId] = 0x00002e20u;
    return cpu;
}

enum class e_ee_reset_prefix_result : uint8_t
{
    COMMITTED,
    UNSAFE_BOUNDARY,
    FETCH_UNAVAILABLE,
    UNSUPPORTED_OPCODE,
    BUDGET_EXHAUSTED,
    DATA_UNAVAILABLE
};

struct s_ee_reset_prefix_step
{
    e_ee_reset_prefix_result m_result = e_ee_reset_prefix_result::UNSAFE_BOUNDARY;
    uint32_t m_opcode = 0u;
    uint32_t m_retired = 0u;
    // On refusal, identify the actual unsupported instruction or slot,
    // not just the branch restart PC. Data address is meaningful only
    // for DATA_UNAVAILABLE.
    uint32_t m_detail_pc = 0u;
    uint32_t m_data_address = 0u;
};

// Only the reset-entry MTC0 zero,Status is supported. Its source is
// architectural zero, so no undefined power-on GPR is consumed.
// Other ERL instructions remain rejected by the ordinary executors.
// Rejection leaves the CPU unchanged. The caller owns isolated state;
// this function does not emulate COP0 pipeline hazards or elapsed cycles.
inline constexpr s_ee_reset_prefix_step ee_execute_strict_reset_prefix(
    s_ee_cpu_state& cpu, const s_ee_strict_rom0_view& rom)
{
    using result = e_ee_reset_prefix_result;
    if (cpu.m_pc != 0xbfc00000u ||
        cpu.m_exit_reason != static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH) ||
        cpu.m_cop0[COP0_Status] != (STATUS_ERL | STATUS_BEV) ||
        cpu.m_cop0[COP0_Config] != k_ee_reset_config_fixed ||
        cpu.m_cop0[COP0_Random] != 47u ||
        cpu.m_cop0[COP0_Wired] != 0u)
    {
        return {result::UNSAFE_BOUNDARY};
    }
    const c_ee_tlb tlb{};
    const auto fetch = ee_plan_instruction_fetch(tlb, cpu.m_pc,
        cpu.m_cop0[COP0_Status], cpu.m_cop0[COP0_Config],
        cpu.m_cop0[COP0_Context], cpu.m_cop0[COP0_EntryHi]);
    const uint8_t* code = rom.fetch_span(fetch);
    if (code == nullptr)
    {
        return {result::FETCH_UNAVAILABLE};
    }
    const uint32_t opcode = ee_read_instruction_word(code);
    if (opcode != 0x40806000u)
    {
        return {result::UNSUPPORTED_OPCODE, opcode};
    }
    // Architectural effect of the observed MTC0 zero,Status.
    cpu.m_cop0[COP0_Status] = 0u;
    cpu.m_pc += 4u;
    return {result::COMMITTED, opcode, 1u};
}

// Explicit caller-owned, isolated main RAM backing for the reset copy.
// The physical bus planner excludes aliases, ROM and device windows;
// only canonical uncached main RAM instruction fetch is permitted.
struct s_ee_reset_ram_view
{
    uint8_t* m_bytes = nullptr;
    size_t m_size = 0u;

    constexpr const uint8_t* fetch_span(const s_ee_fetch_plan& fetch) const
    {
        const uint32_t address = fetch.m_access.m_physical_address;
        if (m_bytes == nullptr ||
            fetch.m_route != e_ee_fetch_route::CACHE_BYPASS_PHYSICAL ||
            fetch.m_access.m_status != e_ee_access_status::PHYSICAL ||
            fetch.m_access.m_cache_mode != 2u || (fetch.m_pc & 3u) != 0u ||
            fetch.m_pc < 0xa0000000u || fetch.m_pc >= 0xa2000000u ||
            address != fetch.m_pc - 0xa0000000u ||
            ee_classify_physical_span(address, 4u, m_size) != e_ee_bus_region::RAM)
        {
            return nullptr;
        }
        return m_bytes + address;
    }

    constexpr uint8_t* write_span(const s_ee_bus_span_plan& bus) const
    {
        if (m_bytes == nullptr || bus.m_region != e_ee_bus_region::RAM ||
            bus.m_cache_policy != e_ee_data_cache_policy::UNCACHED ||
            ee_classify_physical_span(bus.m_address, bus.m_width, m_size) != e_ee_bus_region::RAM)
        {
            return nullptr;
        }
        return m_bytes + bus.m_address;
    }

    constexpr const uint8_t* read_span(const s_ee_bus_span_plan& bus) const
    {
        return write_span(bus);
    }
};

// Fetch from the supplied live backing, never from a ROM image's
// source bytes as a substitute for missing RAM.
inline constexpr const uint8_t* ee_fetch_strict_reset_word(
    const s_ee_fetch_plan& fetch, const s_ee_strict_rom0_view& rom,
    const s_ee_reset_ram_view& ram = {})
{
    return fetch.m_pc >= 0xbfc00000u && fetch.m_pc < 0xc0000000u
        ? rom.fetch_span(fetch) : ram.fetch_span(fetch);
}

static_assert(s_ee_reset_ram_view{}.fetch_span({}) == nullptr);

// Explicit diagnostic transport for the EE SIO TX byte register.
// This models only ordered byte delivery to a caller-provided sink,
// not serial timing, FIFO fullness, receive state or interrupts.
struct s_ee_reset_sio_tx
{
    uint8_t* m_bytes = nullptr;
    size_t m_capacity = 0u;
    size_t m_count = 0u;

    constexpr bool write(const s_ee_bus_span_plan& bus, uint32_t address, uint8_t value)
    {
        if (address != 0xb000f180u || bus.m_address != 0x1000f180u ||
            bus.m_region != e_ee_bus_region::DEVICE ||
            bus.m_cache_policy != e_ee_data_cache_policy::UNCACHED ||
            bus.m_width != 1u || m_bytes == nullptr || m_count >= m_capacity)
        {
            return false;
        }
        m_bytes[m_count++] = value;
        return true;
    }
};

// Opt-in device transport for the isolated reset probe.
// Admit only canonical uncached VIF resets/FIFOs and INTC_MASK.
// Callbacks must accept the real device access or refuse.
struct s_ee_reset_mmio
{
    using t_write = bool (*)(void*, uint32_t, uint32_t, const s_ee_u128*);
    void* m_user = nullptr;
    t_write m_write = nullptr;
    using t_read = bool (*)(void*, uint32_t, uint32_t, s_ee_u128*);
    t_read m_read = nullptr;

    constexpr bool read(const s_ee_bus_span_plan& bus, uint32_t address,
        s_ee_u128& value) const
    {
        return m_read != nullptr && bus.m_region == e_ee_bus_region::DEVICE &&
            bus.m_cache_policy == e_ee_data_cache_policy::UNCACHED &&
            bus.m_width == 4u && address == 0xb000f010u &&
            bus.m_address == 0x1000f010u &&
            m_read(m_user, bus.m_address, bus.m_width, &value);
    }

    constexpr bool write(const s_ee_bus_span_plan& bus, uint32_t address,
        const s_ee_u128& value) const
    {
        const bool reset = bus.m_width == 4u && static_cast<uint32_t>(value.m_lo) == 1u &&
            (address == 0xb0003810u || address == 0xb0003c10u);
        const bool fifo = bus.m_width == 16u &&
            (address == 0xb0004000u || address == 0xb0005000u);
        const bool intc = bus.m_width == 4u && address == 0xb000f010u;
        return m_write != nullptr && bus.m_region == e_ee_bus_region::DEVICE &&
            bus.m_cache_policy == e_ee_data_cache_policy::UNCACHED &&
            (reset || fifo || intc) && bus.m_address == address - 0xa0000000u &&
            m_write(m_user, bus.m_address, bus.m_width, &value);
    }
};

namespace strict_reset_detail
{
inline constexpr uint64_t read_gpr(const s_ee_cpu_state& cpu, uint32_t reg)
{
    return reg == 0u ? 0u : cpu.m_gpr[reg].m_lo;
}

inline constexpr uint64_t sign_extend_word(uint32_t word)
{
    return (word & 0x80000000u) != 0u
        ? uint64_t{word} | 0xffffffff00000000ull : uint64_t{word};
}

// Only CU0/CU1/CU2 enable bits are supported in post-reset Status.
// They do not change kernel mode, enable interrupts or enter exception
// levels. Other Status bits and COP0 timing/hazards remain unmodeled.
inline constexpr bool status_supported(uint32_t status)
{
    return (status & ~0x70000000u) == 0u;
}
static_assert(status_supported(0u));
static_assert(status_supported(0x70000000u));
static_assert(!status_supported(STATUS_ERL));
static_assert(!status_supported(STATUS_BEV));
static_assert(!status_supported(0x70000001u));
static_assert(!status_supported(0x80000000u));

// No memory, branches, clocks or general COP0 side effects.
inline constexpr bool execute_register_word(s_ee_cpu_state& cpu, uint32_t opcode,
    c_ee_tlb* persistent_tlb)
{
    const uint32_t primary = opcode >> 26u;
    const uint32_t rs = (opcode >> 21u) & 31u;
    const uint32_t rt = (opcode >> 16u) & 31u;
    uint32_t dest = rt;
    const uint32_t imm = opcode & 0xffffu;
    uint64_t value = 0u;
    switch (primary)
    {
    case 0u:
    {
        if (opcode == 0u || opcode == 0x0000040fu || opcode == 0x0000000fu)
        {
            // NOP / canonical SYNC.P / SYNC.L. All transfers in this
            // isolated executor finish synchronously; there is no
            // outstanding pipeline, cache or store-buffer work to drain.
            // This is not a live runtime barrier implementation.
            return true;
        }
        dest = (opcode >> 11u) & 31u;
        const uint32_t function = opcode & 0x3fu;
        if (function == 0u || function == 2u || function == 3u)
        {
            // SLL/SRL/SRA consume only the low word, sign-extend the
            // result to 64 bits, and require the reserved rs field zero.
            if (rs != 0u)
            {
                return false;
            }
            const uint32_t shift = (opcode >> 6u) & 31u;
            const uint32_t source = static_cast<uint32_t>(read_gpr(cpu, rt));
            uint32_t shifted = 0u;
            if (function == 0u)
            {
                shifted = source << shift;
            }
            else
            {
                shifted = source >> shift;
                if (function == 3u && shift != 0u && (source & 0x80000000u) != 0u)
                {
                    shifted |= 0xffffffffu << (32u - shift);
                }
            }
            value = sign_extend_word(shifted);
            break;
        }
        switch (opcode & 0x7ffu) // includes shamt: reject reserved encodings
        {
        case 0x21u: // ADDU: low 32-bit wrapping sum
            value = sign_extend_word(static_cast<uint32_t>(read_gpr(cpu, rs)) +
                                     static_cast<uint32_t>(read_gpr(cpu, rt)));
            break;
        case 0x24u: // AND: both low 64-bit operands
            value = read_gpr(cpu, rs) & read_gpr(cpu, rt);
            break;
        case 0x25u: // OR: both low 64-bit operands
            value = read_gpr(cpu, rs) | read_gpr(cpu, rt);
            break;
        case 0x27u: // NOR: both low 64-bit operands
            value = ~(read_gpr(cpu, rs) | read_gpr(cpu, rt));
            break;
        case 0x2bu: // SLTU: unsigned comparison of both low 64-bit halves
            value = read_gpr(cpu, rs) < read_gpr(cpu, rt) ? 1u : 0u;
            break;
        default:
            return false;
        }
        break;
    }
    case 0x10u:
        if (persistent_tlb != nullptr)
        {
            // Reuse the bounded TLB register array and its index/mask
            // validation. No translated access or Random evolution is
            // implied by programming an entry.
            if (opcode == 0x42000002u) // TLBWI
            {
                return persistent_tlb->write(cpu.m_cop0[COP0_Index],
                    cpu.m_cop0[COP0_PageMask], cpu.m_cop0[COP0_EntryHi],
                    cpu.m_cop0[COP0_EntryLo0], cpu.m_cop0[COP0_EntryLo1]);
            }
            if (opcode == 0x42000001u) // TLBR
            {
                s_ee_tlb_entry entry{};
                if (!persistent_tlb->read(cpu.m_cop0[COP0_Index], &entry))
                {
                    return false;
                }
                cpu.m_cop0[COP0_PageMask] = entry.m_page_mask;
                cpu.m_cop0[COP0_EntryHi] = entry.m_entry_hi;
                cpu.m_cop0[COP0_EntryLo0] = entry.m_entry_lo0;
                cpu.m_cop0[COP0_EntryLo1] = entry.m_entry_lo1;
                return true;
            }
            const uint32_t reg = (opcode >> 11u) & 31u;
            const uint32_t form = opcode & 0xffe007ffu;
            if ((form == 0x40800000u || form == 0x40000000u) &&
                (reg == COP0_Index || reg == COP0_PageMask || reg == COP0_EntryHi ||
                 reg == COP0_EntryLo0 || reg == COP0_EntryLo1))
            {
                // Match the existing bounded RAM interpreter's staging
                // register storage policy; TLBWI validates index/mask.
                if (form == 0x40800000u)
                {
                    cpu.m_cop0[reg] = static_cast<uint32_t>(read_gpr(cpu, rt));
                }
                else if (rt != 0u)
                {
                    cpu.m_gpr[rt].m_lo = sign_extend_word(cpu.m_cop0[reg]);
                }
                return true;
            }
        }
        if ((opcode & 0xffe0ffffu) == 0x40806000u) // canonical MTC0 rt,Status
        {
            const uint32_t status = static_cast<uint32_t>(read_gpr(cpu, rt));
            if (!status_supported(status))
            {
                return false;
            }
            cpu.m_cop0[COP0_Status] = status;
            return true;
        }
        if ((opcode & 0xffe0ffffu) == 0x40803000u) // MTC0 rt,Wired
        {
            const uint32_t wired = static_cast<uint32_t>(read_gpr(cpu, rt));
            if (wired >= c_ee_tlb::k_entry_count)
            {
                return false;
            }
            cpu.m_cop0[COP0_Wired] = wired;
            cpu.m_cop0[COP0_Random] = c_ee_tlb::k_entry_count - 1u;
            return true;
        }
        if ((opcode & 0xffe0ffffu) == 0x40808000u) // MTC0 rt,Config
        {
            const uint32_t config = static_cast<uint32_t>(read_gpr(cpu, rt));
            if (!ee_strict_reset_config_write_supported(config))
            {
                return false;
            }
            cpu.m_cop0[COP0_Config] = ee_strict_reset_config_value(config);
            return true;
        }
        if ((opcode & 0xffe0ffffu) == 0x40008000u) // MFC0 rt,Config
        {
            value = sign_extend_word(cpu.m_cop0[COP0_Config]);
        }
        else if ((opcode & 0xffe0ffffu) == 0x40003000u) // MFC0 rt,Wired
        {
            value = sign_extend_word(cpu.m_cop0[COP0_Wired]);
        }
        // Random reads remain unsupported: no elapsed-cycle model.
        else if ((opcode & 0xffe0ffffu) == 0x40007800u)
        {
            value = sign_extend_word(cpu.m_cop0[COP0_PRId]);
        }
        else
        {
            return false;
        }
        break;
    case 0x0bu: // SLTIU: unsigned low 64-bit comparison, signed immediate
        value = read_gpr(cpu, rs) <
            sign_extend_word(imm | ((imm & 0x8000u) != 0u ? 0xffff0000u : 0u)) ? 1u : 0u;
        break;
    case 0x0cu: // ANDI
        value = read_gpr(cpu, rs) & imm;
        break;
    case 0x0du: // ORI
        value = read_gpr(cpu, rs) | imm;
        break;
    case 0x0fu: // LUI
        if (rs != 0u)
        {
            return false;
        }
        value = sign_extend_word(imm << 16u);
        break;
    case 0x09u: // ADDIU (32-bit wrapping, sign-extended result)
        value = sign_extend_word(static_cast<uint32_t>(read_gpr(cpu, rs)) +
            (imm | ((imm & 0x8000u) != 0u ? 0xffff0000u : 0u)));
        break;
    default:
        return false;
    }
    if (dest != 0u)
    {
        cpu.m_gpr[dest].m_lo = value;
    }
    return true;
}

// LBU/LW/LQ read ROM or explicitly backed uncached RAM; SB/SW/SQ write RAM.
// SB to the exact SIO TX register additionally requires an explicit sink.
// Validate even with register zero. A refusal is diagnostic only;
// this reset probe does not deliver architectural guest faults.
inline constexpr s_ee_reset_prefix_step execute_rom_word(
    s_ee_cpu_state& cpu, const s_ee_strict_rom0_view& rom,
    const c_ee_tlb& tlb, uint32_t pc, uint32_t opcode,
    const s_ee_reset_ram_view& ram, s_ee_reset_sio_tx* sio,
    c_ee_tlb* persistent_tlb, const s_ee_reset_mmio* mmio)
{
    using result = e_ee_reset_prefix_result;
    const uint32_t primary = opcode >> 26u;
    const bool quad = primary == 0x1eu || primary == 0x1fu;
    const bool store = primary == 0x28u || primary == 0x2bu || primary == 0x1fu;
    if (primary != 0x24u && primary != 0x23u && primary != 0x1eu && !store)
    {
        return {execute_register_word(cpu, opcode, persistent_tlb) ? result::COMMITTED :
            result::UNSUPPORTED_OPCODE, opcode, 0u, pc};
    }
    const uint32_t rs = (opcode >> 21u) & 31u;
    const uint32_t rt = (opcode >> 16u) & 31u;
    const uint32_t imm = opcode & 0xffffu;
    const uint32_t address = static_cast<uint32_t>(read_gpr(cpu, rs)) +
        (imm | ((imm & 0x8000u) != 0u ? 0xffff0000u : 0u));
    const bool word = primary == 0x23u || primary == 0x2bu;
    const auto kind = quad ? e_ee_data_access_kind::GPR_QUADWORD :
        word ? e_ee_data_access_kind::WORD : e_ee_data_access_kind::BYTE;
    const auto plan = ee_plan_data_access(tlb, kind,
        store, address, cpu.m_cop0[COP0_Status], cpu.m_cop0[COP0_Config],
        cpu.m_cop0[COP0_Context], cpu.m_cop0[COP0_EntryHi]);
    const auto bus = ee_plan_data_bus_span(plan.m_access, plan.m_width,
        cpu.m_cop0[COP0_Config], ram.m_bytes != nullptr ? ram.m_size : 0u);
    // Restrict RAM accesses to canonical KSEG1, with no alias or
    // speculative fallback. ROM remains separately read-only.
    const uint32_t access_address = plan.m_virtual_address;
    const bool ram_address = access_address >= 0xa0000000u && access_address < 0xa2000000u &&
        bus.m_address == access_address - 0xa0000000u;
    if (store)
    {
        if (!plan.m_fault.m_valid && sio != nullptr &&
            sio->write(bus, address, static_cast<uint8_t>(read_gpr(cpu, rt))))
        {
            return {result::COMMITTED, opcode, 0u, pc};
        }
        const s_ee_u128 value{read_gpr(cpu, rt), rt == 0u ? 0u : cpu.m_gpr[rt].m_hi};
        if (!plan.m_fault.m_valid && mmio != nullptr &&
            mmio->write(bus, access_address, value))
        {
            return {result::COMMITTED, opcode, 0u, pc};
        }
        uint8_t* target = plan.m_fault.m_valid || !ram_address ? nullptr : ram.write_span(bus);
        if (target == nullptr)
        {
            return {result::DATA_UNAVAILABLE, opcode, 0u, pc, address};
        }
        for (uint32_t i = 0u; i < plan.m_width; ++i)
        {
            const uint64_t half = i < 8u ? value.m_lo : value.m_hi;
            target[i] = static_cast<uint8_t>(half >> ((i & 7u) * 8u));
        }
        return {result::COMMITTED, opcode, 0u, pc};
    }
    s_ee_u128 device_value{};
    if (!plan.m_fault.m_valid && mmio != nullptr &&
        mmio->read(bus, access_address, device_value))
    {
        if (rt != 0u)
        {
            cpu.m_gpr[rt].m_lo = sign_extend_word(static_cast<uint32_t>(device_value.m_lo));
        }
        return {result::COMMITTED, opcode, 0u, pc};
    }
    const uint8_t* bytes = nullptr;
    if (!plan.m_fault.m_valid)
    {
        if (ram_address)
        {
            bytes = ram.read_span(bus);
        }
        else if (access_address >= 0xbfc00000u && access_address < 0xc0000000u)
        {
            bytes = rom.read_span(bus);
        }
    }
    if (bytes == nullptr)
    {
        return {result::DATA_UNAVAILABLE, opcode, 0u, pc, address};
    }
    if (rt != 0u)
    {
        if (quad)
        {
            s_ee_u128 value{};
            for (uint32_t i = 0u; i < 8u; ++i)
            {
                value.m_lo |= static_cast<uint64_t>(bytes[i]) << (i * 8u);
                value.m_hi |= static_cast<uint64_t>(bytes[i + 8u]) << (i * 8u);
            }
            cpu.m_gpr[rt] = value;
        }
        else
        {
            cpu.m_gpr[rt].m_lo = primary == 0x23u
                ? sign_extend_word(ee_read_instruction_word(bytes))
                : static_cast<uint64_t>(*bytes);
        }
    }
    return {result::COMMITTED, opcode, 0u, pc};
}
} // namespace strict_reset_detail

// Isolated post-Status-clear ROM and uncached RAM execution. The caller
// owns all backing/state and excludes pending clock/device effects.
// BEQ/BNE/J/JAL/JR and supported register/ROM/RAM transfer slots are
// a single bounded transaction. A refused slot changes no state.
// No guest fault is invented by refusal.
inline constexpr s_ee_reset_prefix_step ee_execute_strict_rom_register_step(
    s_ee_cpu_state& cpu, const s_ee_strict_rom0_view& rom, uint32_t budget,
    const s_ee_reset_ram_view& ram = {}, s_ee_reset_sio_tx* sio = nullptr,
    c_ee_tlb* persistent_tlb = nullptr, const s_ee_reset_mmio* mmio = nullptr)
{
    using result = e_ee_reset_prefix_result;
    if (cpu.m_exit_reason != static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH) ||
        !strict_reset_detail::status_supported(cpu.m_cop0[COP0_Status]))
    {
        return {result::UNSAFE_BOUNDARY};
    }
    if (budget == 0u)
    {
        return {result::BUDGET_EXHAUSTED};
    }
    const c_ee_tlb empty_tlb{};
    const c_ee_tlb& tlb = persistent_tlb != nullptr ? *persistent_tlb : empty_tlb;
    const auto fetch = [&](uint32_t pc)
    {
        return ee_fetch_strict_reset_word(ee_plan_instruction_fetch(tlb, pc,
            cpu.m_cop0[COP0_Status], cpu.m_cop0[COP0_Config],
            cpu.m_cop0[COP0_Context], cpu.m_cop0[COP0_EntryHi]), rom, ram);
    };
    const uint8_t* code = fetch(cpu.m_pc);
    if (code == nullptr)
    {
        return {result::FETCH_UNAVAILABLE, 0u, 0u, cpu.m_pc};
    }
    const uint32_t opcode = ee_read_instruction_word(code);
    const uint32_t primary = opcode >> 26u;
    const bool jr = ee_strict_jr_supported(opcode);
    const bool direct_jump = primary == 2u || primary == 3u;
    const bool link = primary == 3u;
    if (primary == 4u || primary == 5u || jr || direct_jump)
    {
        if (budget < 2u)
        {
            return {result::BUDGET_EXHAUSTED, opcode};
        }
        const uint32_t pc = cpu.m_pc;
        const bool equal = strict_reset_detail::read_gpr(cpu, (opcode >> 21u) & 31u) ==
            strict_reset_detail::read_gpr(cpu, (opcode >> 16u) & 31u);
        const bool taken = primary == 4u ? equal : !equal;
        const uint32_t imm = opcode & 0xffffu;
        const uint32_t disp = (imm | ((imm & 0x8000u) != 0u ? 0xffff0000u : 0u)) << 2u;
        // Sample the indirect target before the delay slot. Its
        // alignment and backing are checked only on the next fetch.
        const uint32_t next = jr
            ? static_cast<uint32_t>(strict_reset_detail::read_gpr(cpu, (opcode >> 21u) & 31u))
            : direct_jump ? ee_strict_direct_jump_target(pc, opcode)
            : taken ? pc + 4u + disp : pc + 8u;
        const uint8_t* slot = fetch(pc + 4u);
        if (slot == nullptr)
        {
            return {result::FETCH_UNAVAILABLE, 0u, 0u, pc + 4u};
        }
        // The link must be visible to the delay slot. This isolated
        // executor has only host refusals, so restore it on rejection.
        const uint64_t old_link = cpu.m_gpr[31].m_lo;
        if (link)
        {
            cpu.m_gpr[31].m_lo = ee_strict_link_address(pc);
        }
        const auto action = strict_reset_detail::execute_rom_word(cpu, rom, tlb,
            pc + 4u, ee_read_instruction_word(slot), ram, sio, persistent_tlb, mmio);
        if (action.m_result != result::COMMITTED)
        {
            if (link)
            {
                cpu.m_gpr[31].m_lo = old_link;
            }
            return action;
        }
        cpu.m_pc = next;
        return {result::COMMITTED, opcode, 2u};
    }
    const auto action = strict_reset_detail::execute_rom_word(cpu, rom, tlb,
        cpu.m_pc, opcode, ram, sio, persistent_tlb, mmio);
    if (action.m_result != result::COMMITTED)
    {
        return action;
    }
    cpu.m_pc += 4u;
    return {result::COMMITTED, opcode, 1u};
}

static_assert(strict_reset_detail::sign_extend_word(0xbfc0012cu) == 0xffffffffbfc0012cull);
static_assert(ee_make_strict_reset_prefix_state().m_cop0[COP0_PRId] == 0x2e20u);
static_assert(ee_make_strict_reset_prefix_state().m_pc == 0xbfc00000u);
static_assert(ee_make_strict_reset_prefix_state().m_cop0[COP0_Status] ==
              (STATUS_ERL | STATUS_BEV));
static_assert(ee_make_strict_reset_prefix_state().m_cop0[COP0_Random] == 47u);
static_assert(ee_make_strict_reset_prefix_state().m_cop0[COP0_Config] == 0x440u);

} // namespace ee
