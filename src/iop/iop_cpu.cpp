#include "iop_cpu.h"
#include "iop_aot.h"
#include "iop_hw.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace iop
{
namespace
{
int32_t sign16(uint32_t value)
{
    return static_cast<int16_t>(value & 0xffffu);
}

bool signed_sum(uint32_t left, uint32_t right, uint32_t* result)
{
    const int64_t sum = static_cast<int64_t>(static_cast<int32_t>(left)) +
                        static_cast<int64_t>(static_cast<int32_t>(right));
    const bool valid =
        sum >= std::numeric_limits<int32_t>::min() &&
        sum <= std::numeric_limits<int32_t>::max();
    if (valid && result != nullptr)
    {
        *result = static_cast<uint32_t>(sum);
    }
    return valid;
}

bool signed_difference(uint32_t left, uint32_t right, uint32_t* result)
{
    const int64_t difference =
        static_cast<int64_t>(static_cast<int32_t>(left)) -
        static_cast<int64_t>(static_cast<int32_t>(right));
    const bool valid =
        difference >= std::numeric_limits<int32_t>::min() &&
        difference <= std::numeric_limits<int32_t>::max();
    if (valid && result != nullptr)
    {
        *result = static_cast<uint32_t>(difference);
    }
    return valid;
}
} // namespace

c_cpu::c_cpu()
    : m_ram(k_ram_size, 0u)
{
    reset();
}

void c_cpu::reset(uint32_t entry)
{
    m_state = {};
    m_last_memory_fault = {};
    m_state.m_pc = entry;
    m_state.m_next_pc = entry + 4u;
    m_cycles = 0u;
    m_guest_exceptions = false;
    std::fill(m_ram.begin(), m_ram.end(), 0u);
    if (m_hardware != nullptr)
    {
        m_hardware->reset();
    }
}

void c_cpu::bind_hardware(c_hw* hardware)
{
    m_hardware = hardware;
}

bool c_cpu::install_rom0(const std::vector<uint8_t>& image)
{
    if (image.size() != k_rom0_size)
    {
        return false;
    }
    // Installing a new ROM never modifies the current IOP RAM or CPU.
    // It is persistent across IOP reset, just like hardware ROM0.
    m_rom0 = image;
    return true;
}

bool c_cpu::has_rom0() const
{
    return m_rom0.size() == k_rom0_size;
}

bool c_cpu::reset_rom0()
{
    if (!has_rom0())
    {
        return false;
    }
    reset(k_rom0_reset_pc);
    // PCSX2 R3000A reset: COP0 enabled, BEV and TS set, PRid 0x1f.
    m_state.m_cop0_status = 0x10900000u;
    m_state.m_cop0_prid = 0x0000001fu;
    // A real ROM has its own exception handlers; generic IRX tests
    // remain fail-closed until a guest ROM0 reset has occurred.
    m_guest_exceptions = true;
    return true;
}

const s_cpu_state& c_cpu::state() const
{
    return m_state;
}

s_cpu_state& c_cpu::state()
{
    return m_state;
}

uint64_t c_cpu::cycles() const
{
    return m_cycles;
}

const s_memory_fault& c_cpu::last_memory_fault() const
{
    return m_last_memory_fault;
}

bool c_cpu::fail_memory(
    uint32_t address, uint32_t width, bool write
) const
{
    m_last_memory_fault = {address, width, write, true};
    return false;
}

bool c_cpu::read(uint32_t address, uint32_t width, uint32_t* value) const
{
    if (value == nullptr || (width != 1u && width != 2u && width != 4u))
    {
        return fail_memory(address, width, false);
    }

    // The R3000 cache-control register lives above KSEG1 and does
    // not use the ordinary 29-bit physical bus mask.
    if (address == c_hw::k_cache_control)
    {
        if (m_hardware != nullptr &&
            m_hardware->read(address, width, value))
        {
            return true;
        }
        return fail_memory(address, width, false);
    }
    // RAM, ROM and ordinary MMIO are accessed through physical or
    // direct-mapped KSEG0/KSEG1, never arbitrary KSEG2/TLB aliases.
    const uint32_t region = address & 0xe0000000u;
    if (region != 0u && region != 0x80000000u &&
        region != 0xa0000000u)
    {
        return fail_memory(address, width, false);
    }
    const uint32_t physical = address & 0x1fffffffu;
    if ((address & (width - 1u)) != 0u)
    {
        return fail_memory(address, width, false);
    }

    const uint8_t* source = nullptr;
    uint32_t offset = 0u;
    // The standard IOP has 2 MiB of RAM repeated in the first
    // 8 MiB of physical space. This applies to instruction fetches
    // as well as data reads, without aliasing MMIO, ROM or KSEG2.
    if (physical < k_ram_mirror_size)
    {
        source = m_ram.data();
        offset = physical & (k_ram_size - 1u);
    }
    else if (has_rom0() && physical >= k_rom0_physical &&
             static_cast<uint64_t>(physical - k_rom0_physical) + width <=
                 k_rom0_size)
    {
        source = m_rom0.data();
        offset = physical - k_rom0_physical;
    }
    else
    {
        if (m_hardware != nullptr &&
            m_hardware->read(physical, width, value))
        {
            return true;
        }
        return fail_memory(address, width, false);
    }

    uint32_t result = 0u;
    for (uint32_t index = 0u; index < width; ++index)
    {
        result |= static_cast<uint32_t>(
            source[offset + index]
        ) << (index * 8u);
    }
    *value = result;
    return true;
}

bool c_cpu::write(uint32_t address, uint32_t width, uint32_t value)
{
    if (width != 1u && width != 2u && width != 4u)
    {
        return fail_memory(address, width, true);
    }
    if (address == c_hw::k_cache_control)
    {
        if (m_hardware != nullptr &&
            m_hardware->write(address, width, value))
        {
            return true;
        }
        return fail_memory(address, width, true);
    }

    const uint32_t region = address & 0xe0000000u;
    if (region != 0u && region != 0x80000000u &&
        region != 0xa0000000u)
    {
        return fail_memory(address, width, true);
    }
    const uint32_t physical = address & 0x1fffffffu;
    if ((address & (width - 1u)) != 0u)
    {
        return fail_memory(address, width, true);
    }
    if (physical >= k_ram_mirror_size)
    {
        if (m_hardware != nullptr &&
            m_hardware->write(physical, width, value))
        {
            return true;
        }
        return fail_memory(address, width, true);
    }

    const uint32_t offset = physical & (k_ram_size - 1u);
    for (uint32_t index = 0u; index < width; ++index)
    {
        m_ram[offset + index] =
            static_cast<uint8_t>(value >> (index * 8u));
    }
    return true;
}

bool c_cpu::read8(uint32_t address, uint8_t* value) const
{
    uint32_t raw = 0u;
    if (value == nullptr || !read(address, 1u, &raw))
    {
        return false;
    }
    *value = static_cast<uint8_t>(raw);
    return true;
}

bool c_cpu::write8(uint32_t address, uint8_t value)
{
    return write(address, 1u, value);
}

bool c_cpu::read32(uint32_t address, uint32_t* value) const
{
    return read(address, 4u, value);
}

bool c_cpu::write32(uint32_t address, uint32_t value)
{
    return write(address, 4u, value);
}

bool c_cpu::dma_read(
    uint32_t address, uint8_t* data, uint32_t size
) const
{
    const uint32_t physical = address & 0x1fffffffu;
    if (data == nullptr || size == 0u || physical > k_ram_size ||
        size > k_ram_size - physical)
    {
        return false;
    }
    std::copy(m_ram.begin() + physical,
              m_ram.begin() + physical + size, data);
    return true;
}

bool c_cpu::dma_write(
    uint32_t address, const uint8_t* data, uint32_t size
)
{
    const uint32_t physical = address & 0x1fffffffu;
    if (data == nullptr || size == 0u || physical > k_ram_size ||
        size > k_ram_size - physical)
    {
        return false;
    }
    std::copy(data, data + size, m_ram.begin() + physical);
    return true;
}

void c_cpu::resume_syscall()
{
    m_state.m_pc = m_state.m_next_pc;
    m_state.m_next_pc += 4u;
    m_state.m_in_delay_slot = false;
    m_state.m_branch_pc = 0u;
}

void c_cpu::take_exception(uint32_t cause_code)
{
    const uint32_t status = m_state.m_cop0_status;
    const bool in_delay_slot = m_state.m_in_delay_slot;
    m_state.m_cop0_epc = in_delay_slot
        ? m_state.m_branch_pc : m_state.m_pc;
    m_state.m_cop0_cause =
        (m_state.m_cop0_cause & 0x0000ff00u) |
        (cause_code << 2u) |
        (in_delay_slot ? 0x80000000u : 0u);
    // MIPS I KU/IE status stack: push IEc/KUc into IEp/KUp.
    m_state.m_cop0_status =
        (status & ~0x3fu) | ((status << 2u) & 0x3fu);
    m_state.m_pc = (status & 0x00400000u) != 0u
        ? 0xbfc00180u : 0x80000080u;
    m_state.m_next_pc = m_state.m_pc + 4u;
    m_state.m_in_delay_slot = false;
    m_state.m_branch_pc = 0u;
}

e_step_result c_cpu::step(uint32_t* opcode)
{
    // Each guest instruction owns its own fault evidence. Successful
    // reads/writes do not leave a stale failure from a previous step.
    m_last_memory_fault = {};
    // IOP peripheral IRQ reaches COP0 through IP2; preserve software
    // pending bits and defer delivery until an executed delay slot ends.
    if (m_hardware != nullptr)
    {
        if (m_hardware->pending_interrupts() != 0u)
        {
            m_state.m_cop0_cause |= 0x0400u;
        }
        else
        {
            m_state.m_cop0_cause &= ~0x0400u;
        }
    }
    if (m_guest_exceptions && !m_state.m_in_delay_slot &&
        (m_state.m_cop0_status & 1u) != 0u &&
        (m_state.m_cop0_status & m_state.m_cop0_cause & 0xff00u) != 0u)
    {
        take_exception(0u);
    }

    uint32_t word = 0u;
    if (!read32(m_state.m_pc, &word))
    {
        return e_step_result::memory_fault;
    }
    if (opcode != nullptr)
    {
        *opcode = word;
    }

    const uint32_t pc = m_state.m_pc;
    // Capture even a fail-closed unsupported opcode. The circular history
    // belongs to the saved IOP guest thread state and is not CPU-visible.
    const uint32_t history_slot = m_state.m_recent_next;
    m_state.m_recent_pc[history_slot] = pc;
    m_state.m_recent_opcode[history_slot] = word;
    m_state.m_recent_next =
        (history_slot + 1u) % s_cpu_state::k_recent_pc_capacity;
    if (m_state.m_recent_count < s_cpu_state::k_recent_pc_capacity)
    {
        ++m_state.m_recent_count;
    }
    const uint32_t op = word >> 26;
    const uint32_t rs = (word >> 21) & 31u;
    const uint32_t rt = (word >> 16) & 31u;
    const uint32_t rd = (word >> 11) & 31u;
    const uint32_t shamt = (word >> 6) & 31u;
    const uint32_t funct = word & 63u;
    const uint32_t immediate = word & 0xffffu;
    const uint32_t left = m_state.m_gpr[rs];
    const uint32_t right = m_state.m_gpr[rt];
    const uint32_t next = m_state.m_next_pc;
    uint32_t next_after_delay = next + 4u;
    uint32_t value = 0u;
    bool has_delay_slot = false;
    const auto guest_trap = [&](uint32_t code)
    {
        take_exception(code);
        m_state.m_gpr[0] = 0u;
        ++m_cycles;
        if (m_hardware != nullptr)
        {
            m_hardware->advance_cycles(1u);
        }
        return e_step_result::completed;
    };

    const auto assign = [&](uint32_t index, uint32_t assigned)
    {
        if (index != 0u)
        {
            m_state.m_gpr[index] = assigned;
        }
    };
    const auto branch = [&](bool taken)
    {
        has_delay_slot = true;
        if (taken)
        {
            next_after_delay = next +
                (static_cast<uint32_t>(sign16(immediate)) << 2u);
        }
    };
    const auto read_word = [&](uint32_t address, uint32_t width,
                               uint32_t* destination)
    {
        return read(address, width, destination);
    };
    const uint32_t address = left +
        static_cast<uint32_t>(sign16(immediate));

    switch (op)
    {
    case 0u:
        switch (funct)
        {
        case 0x00u: assign(rd, right << shamt); break;
        case 0x02u: assign(rd, right >> shamt); break;
        case 0x03u:
            assign(rd, static_cast<uint32_t>(
                static_cast<int32_t>(right) >> shamt
            ));
            break;
        case 0x04u: assign(rd, right << (left & 31u)); break;
        case 0x06u: assign(rd, right >> (left & 31u)); break;
        case 0x07u:
            assign(rd, static_cast<uint32_t>(
                static_cast<int32_t>(right) >> (left & 31u)
            ));
            break;
        case 0x08u:
            has_delay_slot = true;
            next_after_delay = left;
            break;
        case 0x09u:
            assign(rd, pc + 8u);
            has_delay_slot = true;
            next_after_delay = left;
            break;
        case 0x0cu:
            return m_guest_exceptions ? guest_trap(8u) :
                e_step_result::syscall;
        case 0x0du: return e_step_result::breakpoint;
        case 0x10u: assign(rd, m_state.m_hi); break;
        case 0x11u: m_state.m_hi = left; break;
        case 0x12u: assign(rd, m_state.m_lo); break;
        case 0x13u: m_state.m_lo = left; break;
        case 0x18u:
        {
            const int64_t product =
                static_cast<int64_t>(static_cast<int32_t>(left)) *
                static_cast<int64_t>(static_cast<int32_t>(right));
            const uint64_t bits = static_cast<uint64_t>(product);
            m_state.m_lo = static_cast<uint32_t>(bits);
            m_state.m_hi = static_cast<uint32_t>(bits >> 32u);
            break;
        }
        case 0x19u:
        {
            const uint64_t product =
                static_cast<uint64_t>(left) *
                static_cast<uint64_t>(right);
            m_state.m_lo = static_cast<uint32_t>(product);
            m_state.m_hi = static_cast<uint32_t>(product >> 32u);
            break;
        }
        case 0x1au:
        {
            const int32_t numerator = static_cast<int32_t>(left);
            const int32_t denominator = static_cast<int32_t>(right);
            if (denominator == 0)
            {
                m_state.m_lo = numerator < 0 ? 1u : 0xffffffffu;
                m_state.m_hi = left;
            }
            else if (numerator == std::numeric_limits<int32_t>::min() &&
                     denominator == -1)
            {
                m_state.m_lo = left;
                m_state.m_hi = 0u;
            }
            else
            {
                m_state.m_lo = static_cast<uint32_t>(
                    numerator / denominator
                );
                m_state.m_hi = static_cast<uint32_t>(
                    numerator % denominator
                );
            }
            break;
        }
        case 0x1bu:
            m_state.m_lo = right == 0u ? 0xffffffffu : left / right;
            m_state.m_hi = right == 0u ? left : left % right;
            break;
        case 0x20u:
            if (!signed_sum(left, right, &value))
            {
                return m_guest_exceptions ? guest_trap(12u) :
                    e_step_result::overflow;
            }
            assign(rd, value);
            break;
        case 0x21u: assign(rd, left + right); break;
        case 0x22u:
            if (!signed_difference(left, right, &value))
            {
                return m_guest_exceptions ? guest_trap(12u) :
                    e_step_result::overflow;
            }
            assign(rd, value);
            break;
        case 0x23u: assign(rd, left - right); break;
        case 0x24u: assign(rd, left & right); break;
        case 0x25u: assign(rd, left | right); break;
        case 0x26u: assign(rd, left ^ right); break;
        case 0x27u: assign(rd, ~(left | right)); break;
        case 0x2au:
            assign(rd, static_cast<int32_t>(left) <
                       static_cast<int32_t>(right) ? 1u : 0u);
            break;
        case 0x2bu: assign(rd, left < right ? 1u : 0u); break;
        case 0x30u:
            if (static_cast<int32_t>(left) >= static_cast<int32_t>(right))
            {
                return m_guest_exceptions ? guest_trap(13u) :
                    e_step_result::unsupported;
            }
            break;
        case 0x31u:
            if (left >= right)
            {
                return m_guest_exceptions ? guest_trap(13u) :
                    e_step_result::unsupported;
            }
            break;
        case 0x32u:
            if (static_cast<int32_t>(left) < static_cast<int32_t>(right))
            {
                return m_guest_exceptions ? guest_trap(13u) :
                    e_step_result::unsupported;
            }
            break;
        case 0x33u:
            if (left < right)
            {
                return m_guest_exceptions ? guest_trap(13u) :
                    e_step_result::unsupported;
            }
            break;
        case 0x34u:
            if (left == right)
            {
                return m_guest_exceptions ? guest_trap(13u) :
                    e_step_result::unsupported;
            }
            break;
        case 0x36u:
            if (left != right)
            {
                return m_guest_exceptions ? guest_trap(13u) :
                    e_step_result::unsupported;
            }
            break;
        default: return e_step_result::unsupported;
        }
        break;

    case 0x01u:
        switch (rt)
        {
        case 0x00u: branch(static_cast<int32_t>(left) < 0); break;
        case 0x01u: branch(static_cast<int32_t>(left) >= 0); break;
        case 0x10u:
            assign(31u, pc + 8u);
            branch(static_cast<int32_t>(left) < 0);
            break;
        case 0x11u:
            assign(31u, pc + 8u);
            branch(static_cast<int32_t>(left) >= 0);
            break;
        default: return e_step_result::unsupported;
        }
        break;

    case 0x02u:
    case 0x03u:
        if (op == 0x03u)
        {
            assign(31u, pc + 8u);
        }
        has_delay_slot = true;
        next_after_delay = (next & 0xf0000000u) |
                           ((word & 0x03ffffffu) << 2u);
        break;

    case 0x04u: branch(left == right); break;
    case 0x05u: branch(left != right); break;
    case 0x06u: branch(static_cast<int32_t>(left) <= 0); break;
    case 0x07u: branch(static_cast<int32_t>(left) > 0); break;

    case 0x08u:
        if (!signed_sum(left, static_cast<uint32_t>(sign16(immediate)), &value))
        {
            return m_guest_exceptions ? guest_trap(12u) :
                    e_step_result::overflow;
        }
        assign(rt, value);
        break;
    case 0x09u: assign(rt, address); break;
    case 0x0au:
        assign(rt, static_cast<int32_t>(left) < sign16(immediate) ? 1u : 0u);
        break;
    case 0x0bu:
        assign(rt, left < static_cast<uint32_t>(sign16(immediate)) ? 1u : 0u);
        break;
    case 0x0cu: assign(rt, left & immediate); break;
    case 0x0du: assign(rt, left | immediate); break;
    case 0x0eu: assign(rt, left ^ immediate); break;
    case 0x0fu: assign(rt, immediate << 16u); break;

    case 0x10u:
        if (rs == 0u)
        {
            switch (rd)
            {
            case 12u: assign(rt, m_state.m_cop0_status); break;
            case 13u: assign(rt, m_state.m_cop0_cause); break;
            case 14u: assign(rt, m_state.m_cop0_epc); break;
            case 15u: assign(rt, m_state.m_cop0_prid); break;
            default: return e_step_result::unsupported;
            }
        }
        else if (rs == 4u)
        {
            switch (rd)
            {
            case 12u: m_state.m_cop0_status = right; break;
            case 13u:
                // Only software interrupt pending bits are writable.
                m_state.m_cop0_cause =
                    (m_state.m_cop0_cause & ~0x0300u) |
                    (right & 0x0300u);
                break;
            case 14u: m_state.m_cop0_epc = right; break;
            default: return e_step_result::unsupported;
            }
        }
        else if (word == 0x42000010u)
        {
            m_state.m_cop0_status =
                (m_state.m_cop0_status & ~0x0fu) |
                ((m_state.m_cop0_status >> 2u) & 0x0fu);
        }
        else
        {
            return e_step_result::unsupported;
        }
        break;

    case 0x20u:
    case 0x24u:
        if (!read_word(address, 1u, &value))
        {
            return e_step_result::memory_fault;
        }
        assign(rt, op == 0x20u
            ? static_cast<uint32_t>(static_cast<int32_t>(
                static_cast<int8_t>(value)
            ))
            : value);
        break;

    case 0x21u:
    case 0x25u:
        if (!read_word(address, 2u, &value))
        {
            return e_step_result::memory_fault;
        }
        assign(rt, op == 0x21u
            ? static_cast<uint32_t>(static_cast<int32_t>(
                static_cast<int16_t>(value)
            ))
            : value);
        break;

    case 0x22u:
    case 0x26u:
    {
        if (!read_word(address & ~3u, 4u, &value))
        {
            return e_step_result::memory_fault;
        }
        const uint32_t shift =
            ((op == 0x22u ? 3u - (address & 3u) : address & 3u) * 8u);
        if (op == 0x22u)
        {
            assign(rt, (right & (0x00ffffffu >> ((address & 3u) * 8u))) |
                       (value << shift));
        }
        else
        {
            const uint32_t preserved = shift == 0u ? 0u :
                (right & (0xffffffffu << (32u - shift)));
            assign(rt, preserved | (value >> shift));
        }
        break;
    }

    case 0x23u:
        if (!read_word(address, 4u, &value))
        {
            return e_step_result::memory_fault;
        }
        assign(rt, value);
        break;

    case 0x28u: if (!write(address, 1u, right)) return e_step_result::memory_fault; break;
    case 0x29u: if (!write(address, 2u, right)) return e_step_result::memory_fault; break;
    case 0x2bu: if (!write(address, 4u, right)) return e_step_result::memory_fault; break;

    case 0x2au:
    case 0x2eu:
    {
        if (!read_word(address & ~3u, 4u, &value))
        {
            return e_step_result::memory_fault;
        }
        const uint32_t shift =
            ((op == 0x2au ? 3u - (address & 3u) : address & 3u) * 8u);
        const uint32_t stored = op == 0x2au
            ? ((value & ~(0xffffffffu >> shift)) | (right >> shift))
            : ((value & ~(0xffffffffu << shift)) | (right << shift));
        if (!write(address & ~3u, 4u, stored))
        {
            return e_step_result::memory_fault;
        }
        break;
    }

    default:
        return e_step_result::unsupported;
    }

    m_state.m_pc = next;
    m_state.m_next_pc = next_after_delay;
    m_state.m_in_delay_slot = has_delay_slot;
    m_state.m_branch_pc = has_delay_slot ? pc : 0u;
    m_state.m_gpr[0] = 0u;
    ++m_cycles;
    if (m_hardware != nullptr)
    {
        m_hardware->advance_cycles(1u);
    }
    return e_step_result::completed;
}

int c_cpu::aot_read(void*, uint32_t, uint32_t, uint32_t*)
{
    // The lifter handles aligned RAM directly. All other accesses must
    // resume in step() with the precise instruction PC and all preceding
    // cycles retired. A false callback result requests that fallback.
    return 0;
}

int c_cpu::aot_write(void*, uint32_t, uint32_t, uint32_t)
{
    // Do not perform speculative MMIO or faulting writes here.
    return 0;
}

s_run_result c_cpu::run(uint32_t instruction_budget)
{
    s_run_result result{};
    result.m_result = e_step_result::budget_exhausted;
    for (uint32_t index = 0u; index < instruction_budget; ++index)
    {
        // Translated IRX code runs from an ordinary instruction boundary
        // and returns the exact resume state. Fallback instructions are
        // executed by the interpreter step below in the same iteration.
        if (m_aot != nullptr && !m_guest_exceptions &&
            !m_state.m_in_delay_slot &&
            m_state.m_next_pc == m_state.m_pc + 4u)
        {
            const s_iop_aot_module* module = m_aot->find(m_state.m_pc);
            if (module != nullptr)
            {
                s_iop_aot_context context{};
                context.m_gpr = m_state.m_gpr;
                context.m_hi = &m_state.m_hi;
                context.m_lo = &m_state.m_lo;
                context.m_ram = m_ram.data();
                context.m_cpu = this;
                context.m_read = &c_cpu::aot_read;
                context.m_write = &c_cpu::aot_write;
                context.m_budget = instruction_budget - index;
                if (m_hardware != nullptr)
                {
                    // A RAM-only native batch may end at the next modeled
                    // device transition, but not run instructions beyond it.
                    // Unknown/immediate horizons retire at most one word.
                    const uint64_t horizon = m_hardware->idle_event_cycles_remaining();
                    if (horizon < context.m_budget)
                    {
                        context.m_budget = static_cast<uint32_t>(
                            std::max<uint64_t>(1u, horizon));
                    }
                    // The legacy clock-wrap path is not partitionable.
                    const auto& spu2 = m_hardware->spu2();
                    if (spu2.clock_active() && context.m_budget >
                        UINT64_MAX - spu2.clock_cycles())
                    {
                        context.m_budget = 1u;
                    }
                }
                // Legacy CPU/RAM verification cannot restore device
                // state or retirement clocks. Never replay on live hardware.
                static const bool verify_requested =
                    std::getenv("SDBZ_IOP_AOT_VERIFY") != nullptr;
                const bool verify = verify_requested && m_hardware == nullptr;
                s_cpu_state verify_state{};
                std::vector<uint8_t> verify_ram;
                if (verify)
                {
                    verify_state = m_state;
                    verify_ram = m_ram;
                }
                const uint32_t entry_pc = m_state.m_pc;
                const auto aot_started = std::chrono::steady_clock::now();
                const uint32_t reason =
                    module->m_entry(&context, m_state.m_pc);
                m_aot->add_time(std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - aot_started).count());
                if (verify)
                {
                    s_cpu_state aot_state = m_state;
                    aot_state.m_pc = context.m_pc;
                    aot_state.m_next_pc = context.m_next_pc;
                    std::vector<uint8_t> aot_ram = m_ram;
                    m_state = verify_state;
                    m_ram = verify_ram;
                    c_iop_aot* saved = m_aot;
                    m_aot = nullptr;
                    for (uint32_t k = 0u; k < context.m_executed; ++k)
                    {
                        if (step() != e_step_result::completed)
                        {
                            break;
                        }
                    }
                    m_aot = saved;
                    bool same = m_state.m_pc == aot_state.m_pc &&
                        m_state.m_next_pc == aot_state.m_next_pc &&
                        m_state.m_hi == aot_state.m_hi &&
                        m_state.m_lo == aot_state.m_lo &&
                        std::memcmp(m_state.m_gpr, aot_state.m_gpr,
                                    sizeof(m_state.m_gpr)) == 0;
                    size_t ram_diff = m_ram.size();
                    for (size_t b = 0; b < m_ram.size(); ++b)
                    {
                        if (m_ram[b] != aot_ram[b]) { ram_diff = b; break; }
                    }
                    static uint32_t reports = 0u;
                    if ((!same || ram_diff != m_ram.size()) && reports < 20u)
                    {
                        ++reports;
                        std::fprintf(stderr,
                            "IOP AOT VERIFY mismatch: module=%s entry=0x%08x "
                            "executed=%u reason=%u interp_pc=0x%08x/0x%08x "
                            "aot_pc=0x%08x/0x%08x ram_diff=0x%zx\n",
                            module->m_name, entry_pc, context.m_executed, reason,
                            m_state.m_pc, m_state.m_next_pc,
                            aot_state.m_pc, aot_state.m_next_pc, ram_diff);
                        for (int r = 0; r < 32; ++r)
                        {
                            if (m_state.m_gpr[r] != aot_state.m_gpr[r])
                            {
                                std::fprintf(stderr,
                                    "  r%d interp=0x%08x aot=0x%08x\n", r,
                                    m_state.m_gpr[r], aot_state.m_gpr[r]);
                            }
                        }
                        if (m_state.m_hi != aot_state.m_hi ||
                            m_state.m_lo != aot_state.m_lo)
                        {
                            std::fprintf(stderr, "  hi/lo interp=%08x/%08x aot=%08x/%08x\n",
                                m_state.m_hi, m_state.m_lo, aot_state.m_hi, aot_state.m_lo);
                        }
                    }
                    // Continue from the interpreter's (reference) result.
                    context.m_pc = m_state.m_pc;
                    context.m_next_pc = m_state.m_next_pc;
                    context.m_in_delay = m_state.m_in_delay_slot ? 1u : 0u;
                    context.m_branch_pc = m_state.m_branch_pc;
                }
                m_state.m_pc = context.m_pc;
                m_state.m_next_pc = context.m_next_pc;
                m_state.m_in_delay_slot = context.m_in_delay != 0u;
                m_state.m_branch_pc =
                    context.m_in_delay != 0u ? context.m_branch_pc : 0u;
                m_state.m_gpr[0] = 0u;
                m_aot->record(context.m_executed);
                static const bool exit_profile =
                    std::getenv("SDBZ_IOP_AOT_EXIT_PROFILE") != nullptr;
                if (exit_profile)
                {
                    m_aot->record_exit(*module, entry_pc, context, reason,
                                       instruction_budget - index);
                }
                if (context.m_executed != 0u)
                {
                    m_cycles += context.m_executed;
                    if (m_hardware != nullptr)
                    {
                        m_hardware->advance_cycles(context.m_executed);
                    }
                    result.m_instructions += context.m_executed;
                    index += context.m_executed;
                    if (index >= instruction_budget)
                    {
                        break;
                    }
                }
                // An exit without progress (unknown entry pc) interprets
                // one instruction instead of re-entering forever.
                if (reason != IOP_AOT_INTERPRET && context.m_executed != 0u)
                {
                    --index;
                    continue;
                }
            }
        }
        uint32_t opcode = 0u;
        const e_step_result next_result = step(&opcode);
        if (next_result != e_step_result::completed)
        {
            result.m_result = next_result;
            result.m_pc = m_state.m_pc;
            result.m_opcode = opcode;
            break;
        }
        ++result.m_instructions;
    }
    if (result.m_result == e_step_result::budget_exhausted)
    {
        result.m_pc = m_state.m_pc;
    }
    return result;
}
} // namespace iop
