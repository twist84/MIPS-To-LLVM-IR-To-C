#include "ee_memory.h"
#include "ee_runtime.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <new>

namespace ee {

namespace
{
uint64_t load_le(const uint8_t* bytes, size_t count)
{
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i)
    {
        value |= static_cast<uint64_t>(bytes[i]) << (i * 8u);
    }
    return value;
}

void store_le(uint8_t* bytes, size_t count, uint64_t value)
{
    for (size_t i = 0; i < count; ++i)
    {
        bytes[i] = static_cast<uint8_t>(value >> (i * 8u));
    }
}
} // namespace

ee_memory::ee_memory(size_t size)
    : m_size(size)
{
    m_data = static_cast<uint8_t*>(std::malloc(size));
    if (m_data) {
        std::memset(m_data, 0, size);
    }
}

ee_memory::~ee_memory()
{
    std::free(m_data);
    m_data = nullptr;
}

void ee_memory::set_kseg_ram_aliases(bool enabled)
{
    m_kseg_ram_aliases = enabled;
}

c_ee_tlb& ee_memory::tlb()
{
    return m_tlb;
}

const c_ee_tlb& ee_memory::tlb() const
{
    return m_tlb;
}

uint32_t ee_memory::translate_direct_address(uint32_t addr) const
{
    if (!m_kseg_ram_aliases)
    {
        return addr;
    }

    // ps2-web-recomp/runtime/src/memory.cpp::physical()
    if (addr >= 0x80000000u && addr < 0xc0000000u)
    {
        return addr & 0x1fffffffu;
    }

    const uint32_t top = addr >> 28;
    if (top == 2u || top == 3u)
    {
        return addr & 0x0fffffffu;
    }

    return addr;
}

uint32_t ee_memory::normalize_ram_address(uint32_t addr) const
{
    const uint32_t physical = translate_direct_address(addr);
    return static_cast<size_t>(physical) < m_size ? physical : addr;
}


const uint8_t* ee_memory::map_address(uint32_t addr, size_t len) const
{
    // Scratchpad has its own storage. It is NOT part of RAM and is not
    // affected by the optional KSEG0/KSEG1 physical-RAM mirror mode.
    if (addr >= k_scratchpad_base &&
        static_cast<uint64_t>(addr - k_scratchpad_base) < k_scratchpad_size)
    {
        const size_t offset = static_cast<size_t>(addr - k_scratchpad_base);
        return len <= k_scratchpad_size - offset ? m_scratchpad + offset : nullptr;
    }

    const size_t physical = normalize_ram_address(addr);
    if (m_data == nullptr || physical > m_size || len > m_size - physical)
    {
        return nullptr;
    }
    return m_data + physical;
}

uint8_t* ee_memory::map_address(uint32_t addr, size_t len)
{
    return const_cast<uint8_t*>(
        static_cast<const ee_memory*>(this)->map_address(addr, len)
    );
}

bool ee_memory::contains(uint32_t addr, size_t len) const
{
    return map_address(addr, len) != nullptr;
}

uint8_t ee_memory::read8(uint32_t addr)
{
    const uint8_t* bytes = map_address(addr, 1);
    return bytes != nullptr ? bytes[0] : 0;
}

uint16_t ee_memory::read16(uint32_t addr)
{
    const uint8_t* bytes = map_address(addr, 2);
    return bytes != nullptr ? static_cast<uint16_t>(load_le(bytes, 2)) : 0;
}

void ee_memory::read128(uint32_t addr, s_ee_u128* out)
{
    if (out == nullptr)
    {
        return;
    }

    const uint8_t* bytes = map_address(addr, 16);
    if (bytes == nullptr)
    {
        *out = {};
        return;
    }

    out->m_lo = load_le(bytes, 8);
    out->m_hi = load_le(bytes + 8, 8);
}

void ee_memory::write8(uint32_t addr, uint8_t value)
{
    if (uint8_t* bytes = map_address(addr, 1))
    {
        bytes[0] = value;
    }
}

void ee_memory::write16(uint32_t addr, uint16_t value)
{
    if (uint8_t* bytes = map_address(addr, 2))
    {
        store_le(bytes, 2, value);
    }
}

void ee_memory::write128(uint32_t addr, const s_ee_u128* value)
{
    if (value == nullptr)
    {
        return;
    }

    // Reject a cross-boundary quadword in its entirety.
    if (uint8_t* bytes = map_address(addr, 16))
    {
        store_le(bytes, 8, value->m_lo);
        store_le(bytes + 8, 8, value->m_hi);
    }
}

void ee_memory::set_ram_range(uint32_t base, uint32_t size)
{
    m_ram_base = base;
    m_ram_size = size;
    m_ram_configured = true;
}


bool ee_memory::in_ram_range(uint32_t addr, size_t len) const
{
    if (!m_ram_configured || m_data == nullptr)
    {
        return false;
    }

    // Verify both the configured range and the actual host buffer before
    // allowing the unchecked direct_* path. Do not accept wraparound.
    const size_t start = static_cast<size_t>(addr);
    const size_t range_start = static_cast<size_t>(m_ram_base);
    const size_t range_end = range_start + static_cast<size_t>(m_ram_size);
    return start >= range_start && start <= range_end &&
        len <= range_end - start &&
        start <= m_size && len <= m_size - start;
}

uint32_t ee_memory::direct_read32(uint32_t addr)
{
    return (uint32_t)(m_data[addr] | (m_data[addr + 1] << 8) |
                      (m_data[addr + 2] << 16) | (m_data[addr + 3] << 24));
}

void ee_memory::direct_write32(uint32_t addr, uint32_t v)
{
    m_data[addr]   = (uint8_t)(v & 0xff);
    m_data[addr+1] = (uint8_t)((v >> 8) & 0xff);
    m_data[addr+2] = (uint8_t)((v >> 16) & 0xff);
    m_data[addr+3] = (uint8_t)((v >> 24) & 0xff);
}

uint64_t ee_memory::direct_read64(uint32_t addr)
{
    uint64_t lo = direct_read32(addr);
    uint64_t hi = direct_read32(addr + 4);
    return lo | (hi << 32);
}

void ee_memory::direct_write64(uint32_t addr, uint64_t v)
{
    direct_write32(addr,   (uint32_t)(v & 0xffffffffu));
    direct_write32(addr+4, (uint32_t)((v >> 32) & 0xffffffffu));
}


uint32_t ee_memory::read32(uint32_t addr)
{
    const uint32_t physical = normalize_ram_address(addr);
    if (in_ram_range(physical, 4))
    {
        return direct_read32(physical);
    }

    const uint8_t* bytes = map_address(addr, 4);
    return bytes != nullptr ? static_cast<uint32_t>(load_le(bytes, 4)) : 0;
}

void ee_memory::write32(uint32_t addr, uint32_t value)
{
    const uint32_t physical = normalize_ram_address(addr);
    if (in_ram_range(physical, 4))
    {
        direct_write32(physical, value);
        return;
    }

    if (uint8_t* bytes = map_address(addr, 4))
    {
        store_le(bytes, 4, value);
    }
}

uint64_t ee_memory::read64(uint32_t addr)
{
    const uint32_t physical = normalize_ram_address(addr);
    if (in_ram_range(physical, 8))
    {
        return direct_read64(physical);
    }

    const uint8_t* bytes = map_address(addr, 8);
    return bytes != nullptr ? load_le(bytes, 8) : 0;
}

void ee_memory::write64(uint32_t addr, uint64_t value)
{
    const uint32_t physical = normalize_ram_address(addr);
    if (in_ram_range(physical, 8))
    {
        direct_write64(physical, value);
        return;
    }

    if (uint8_t* bytes = map_address(addr, 8))
    {
        store_le(bytes, 8, value);
    }
}

namespace
{
// EE bus apertures taken from ps2-web-recomp/runtime/src/memory.cpp.
// This is a dispatcher, not an implementation of the IOP/DMAC/GS/etc.
constexpr uint32_t k_mmio_first = 0x10000000u;
constexpr uint32_t k_mmio_end = 0x13000000u;
constexpr uint32_t k_vu0_code_base = 0x11000000u;
constexpr uint32_t k_vu0_data_base = 0x11004000u;
constexpr uint32_t k_vu0_window_size = 0x1000u;

uint32_t bus_address(s_ee_runtime* runtime, uint32_t address)
{
    const ee_memory* memory = static_cast<const ee_memory*>(
        runtime != nullptr ? runtime->m_user : nullptr
    );
    return memory != nullptr ? memory->translate_direct_address(address) : address;
}

uint8_t* map_vu0_memory(s_ee_runtime* runtime, uint32_t physical, size_t width)
{
    if (runtime == nullptr || runtime->m_vu0_state == nullptr)
    {
        return nullptr;
    }

    uint8_t* bytes = nullptr;
    uint32_t offset = 0;
    if (physical >= k_vu0_code_base &&
        physical < k_vu0_code_base + k_vu0_window_size)
    {
        bytes = runtime->m_vu0_state->m_micro_memory;
        offset = physical - k_vu0_code_base;
    }
    else if (physical >= k_vu0_data_base &&
             physical < k_vu0_data_base + k_vu0_window_size)
    {
        bytes = runtime->m_vu0_state->m_data_memory;
        offset = physical - k_vu0_data_base;
    }
    else
    {
        return nullptr;
    }

    // No partial read/write at an aperture boundary. This does not
    // introduce a silent wraparound to the VU micro/data arrays.
    return width <= k_vu0_window_size - offset ? bytes + offset : nullptr;
}

bool in_mmio_aperture(uint32_t physical, size_t width)
{
    return physical >= k_mmio_first && physical < k_mmio_end &&
        width <= static_cast<size_t>(k_mmio_end - physical);
}

// A transfer beginning inside a VU0 aperture must not spill into adjacent
// hardware space and then be misrouted to an unrelated MMIO handler.
bool begins_in_vu0_aperture(uint32_t physical)
{
    return (physical >= k_vu0_code_base &&
            physical < k_vu0_code_base + k_vu0_window_size) ||
           (physical >= k_vu0_data_base &&
            physical < k_vu0_data_base + k_vu0_window_size);
}

ee_memory* runtime_memory(s_ee_runtime* runtime)
{
    return static_cast<ee_memory*>(runtime != nullptr ? runtime->m_user : nullptr);
}

// True means the address belongs to an EE hardware bus window, even if no
// device implements it. Unknown MMIO reads return zero without touching RAM.
bool read_bus(s_ee_runtime* runtime, uint32_t guest, uint32_t width, s_ee_u128* out)
{
    const uint32_t physical = bus_address(runtime, guest);
    if (const uint8_t* bytes = map_vu0_memory(runtime, physical, width))
    {
        out->m_lo = load_le(bytes, width > 8 ? 8 : width);
        if (width == 16)
        {
            out->m_hi = load_le(bytes + 8, 8);
        }
        return true;
    }

    if (begins_in_vu0_aperture(physical))
    {
        return true; // unmapped or invalid-width VU0 access
    }

    if (!in_mmio_aperture(physical, width))
    {
        return false;
    }

    if (runtime != nullptr && runtime->m_mmio_read != nullptr)
    {
        s_ee_u128 value{};
        if (runtime->m_mmio_read(runtime->m_mmio_user, physical, width, &value))
        {
            *out = value;
        }
    }
    return true;
}

bool write_bus(s_ee_runtime* runtime, uint32_t guest, uint32_t width,
               const s_ee_u128* value)
{
    const uint32_t physical = bus_address(runtime, guest);
    if (uint8_t* bytes = map_vu0_memory(runtime, physical, width))
    {
        store_le(bytes, width > 8 ? 8 : width, value->m_lo);
        if (width == 16)
        {
            store_le(bytes + 8, 8, value->m_hi);
        }
        return true;
    }

    if (begins_in_vu0_aperture(physical))
    {
        return true; // unmapped or invalid-width VU0 access
    }

    if (!in_mmio_aperture(physical, width))
    {
        return false;
    }

    if (runtime != nullptr && runtime->m_mmio_write != nullptr)
    {
        (void)runtime->m_mmio_write(runtime->m_mmio_user, physical, width, value);
    }
    return true;
}
} // namespace

uint8_t ee_mem_read8(s_ee_runtime* runtime, uint32_t address)
{
    s_ee_u128 result{};
    if (read_bus(runtime, address, 1, &result))
    {
        return static_cast<uint8_t>(result.m_lo);
    }
    ee_memory* memory = runtime_memory(runtime);
    return memory != nullptr ? memory->read8(address) : 0;
}

uint16_t ee_mem_read16(s_ee_runtime* runtime, uint32_t address)
{
    s_ee_u128 result{};
    if (read_bus(runtime, address, 2, &result))
    {
        return static_cast<uint16_t>(result.m_lo);
    }
    ee_memory* memory = runtime_memory(runtime);
    return memory != nullptr ? memory->read16(address) : 0;
}

uint32_t ee_mem_read32(s_ee_runtime* runtime, uint32_t address)
{
    s_ee_u128 result{};
    if (read_bus(runtime, address, 4, &result))
    {
        return static_cast<uint32_t>(result.m_lo);
    }
    ee_memory* memory = runtime_memory(runtime);
    return memory != nullptr ? memory->read32(address) : 0;
}

uint64_t ee_mem_read64(s_ee_runtime* runtime, uint32_t address)
{
    s_ee_u128 result{};
    if (read_bus(runtime, address, 8, &result))
    {
        return result.m_lo;
    }
    ee_memory* memory = runtime_memory(runtime);
    return memory != nullptr ? memory->read64(address) : 0;
}

void ee_mem_read128(s_ee_runtime* runtime, uint32_t address, s_ee_u128* out)
{
    if (out == nullptr)
    {
        return;
    }

    *out = {};
    if (read_bus(runtime, address, 16, out))
    {
        return;
    }
    if (ee_memory* memory = runtime_memory(runtime))
    {
        memory->read128(address, out);
    }
}

void ee_mem_write8(s_ee_runtime* runtime, uint32_t address, uint8_t value)
{
    const s_ee_u128 data{value, 0};
    if (!write_bus(runtime, address, 1, &data))
    {
        if (ee_memory* memory = runtime_memory(runtime))
        {
            memory->write8(address, value);
        }
    }
}

void ee_mem_write16(s_ee_runtime* runtime, uint32_t address, uint16_t value)
{
    const s_ee_u128 data{value, 0};
    if (!write_bus(runtime, address, 2, &data))
    {
        if (ee_memory* memory = runtime_memory(runtime))
        {
            memory->write16(address, value);
        }
    }
}

void ee_mem_write32(s_ee_runtime* runtime, uint32_t address, uint32_t value)
{
    const s_ee_u128 data{value, 0};
    if (!write_bus(runtime, address, 4, &data))
    {
        if (ee_memory* memory = runtime_memory(runtime))
        {
            memory->write32(address, value);
        }
    }
}

void ee_mem_write64(s_ee_runtime* runtime, uint32_t address, uint64_t value)
{
    const s_ee_u128 data{value, 0};
    if (!write_bus(runtime, address, 8, &data))
    {
        if (ee_memory* memory = runtime_memory(runtime))
        {
            memory->write64(address, value);
        }
    }
}

void ee_mem_write128(s_ee_runtime* runtime, uint32_t address, const s_ee_u128* value)
{
    if (value == nullptr)
    {
        return;
    }

    if (!write_bus(runtime, address, 16, value))
    {
        if (ee_memory* memory = runtime_memory(runtime))
        {
            memory->write128(address, value);
        }
    }
}


void ee_trace_guest_store(
    s_ee_runtime* runtime, uint32_t pc, uint32_t opcode,
    uint32_t effective_address, uint32_t affected_address,
    uint32_t width, uint64_t source_value, uint32_t source_lane,
    uint64_t base_value, uint64_t sp, uint64_t fp, uint64_t ra
)
{
    if (runtime == nullptr || !runtime->m_guest_store_trace_enabled ||
        width == 0u ||
        (runtime->m_guest_store_trace_remaining == 0u &&
         (runtime->m_guest_store_trace_recent == nullptr ||
          runtime->m_guest_store_trace_capacity == 0u)))
    {
        return;
    }

    // Compare half-open byte ranges in 64 bits to avoid uint32 wraparound.
    // For SWL/SWR/SDL/SDR, 'affected_address' is the containing aligned
    // word/doubleword, not the (possibly unaligned) effective address.
    const uint64_t write_start = affected_address;
    const uint64_t write_end = write_start + width;
    const uint64_t watch_start = runtime->m_guest_store_trace_word;
    const uint64_t watch_end = watch_start + 4u;
    if (write_start >= watch_end || watch_start >= write_end)
    {
        return;
    }

    // Keep the last N matching stores even when the first-N console trace
    // has exhausted its budget. This is host state, never guest memory.
    if (runtime->m_guest_store_trace_recent != nullptr &&
        runtime->m_guest_store_trace_capacity != 0u)
    {
        const uint32_t slot = runtime->m_guest_store_trace_next;
        runtime->m_guest_store_trace_recent[slot] =
        {
            pc, opcode, effective_address, affected_address,
            width, source_lane, source_value, base_value,
            static_cast<uint32_t>(sp), static_cast<uint32_t>(fp),
            static_cast<uint32_t>(ra)
        };
        runtime->m_guest_store_trace_next =
            slot + 1u == runtime->m_guest_store_trace_capacity ? 0u : slot + 1u;
        if (runtime->m_guest_store_trace_count <
            runtime->m_guest_store_trace_capacity)
        {
            ++runtime->m_guest_store_trace_count;
        }
    }
    if (runtime->m_guest_store_trace_remaining == 0u)
    {
        return;
    }
    --runtime->m_guest_store_trace_remaining;

    // The raw primary opcode distinguishes store classes without relying on
    // host disassembly, and the original word remains available for auditing.
    const uint32_t primary = opcode >> 26u;
    const char* name =
        primary == 0x28u ? "SB" : primary == 0x29u ? "SH" :
        primary == 0x2bu ? "SW" : primary == 0x3fu ? "SD" :
        primary == 0x1fu ? "SQ" : primary == 0x2au ? "SWL" :
        primary == 0x2eu ? "SWR" : primary == 0x2cu ? "SDL" :
        primary == 0x2du ? "SDR" : primary == 0x39u ? "SWC1" :
        primary == 0x3eu ? "SQC2" : "STORE";
    const char* source_class =
        primary == 0x39u ? "fpr" : primary == 0x3eu ? "vu" : "gpr";

    // For partial stores source_value is the original register operand;
    // the affected-memory word is formed by the existing R5900 RMW helper.
    // Do not label the source operand as the final merged write value.
    std::printf(
        "EE guest-store: pc=0x%08x opcode=0x%08x ins=%s "
        "effective=0x%08x affected=0x%08x width=%u "
        "src=%s%u lane=%u source_value=0x%016llx "
        "base=gpr%u base_value=0x%016llx "
        "sp=0x%08x fp=0x%08x ra=0x%08x\n",
        pc, opcode, name, effective_address, affected_address, width,
        source_class, (opcode >> 16u) & 31u, source_lane,
        static_cast<unsigned long long>(source_value),
        (opcode >> 21u) & 31u,
        static_cast<unsigned long long>(base_value),
        static_cast<uint32_t>(sp), static_cast<uint32_t>(fp),
        static_cast<uint32_t>(ra)
    );
    std::fflush(stdout);
}


void ee_trace_guest_v0_write(
    s_ee_runtime* runtime, uint32_t pc, uint32_t opcode,
    uint64_t old_value, uint64_t new_value
)
{
    if (runtime == nullptr || runtime->m_guest_v0_trace_remaining == 0u ||
        pc < runtime->m_guest_v0_trace_first_pc ||
        pc > runtime->m_guest_v0_trace_last_pc ||
        old_value == new_value)
    {
        return;
    }

    --runtime->m_guest_v0_trace_remaining;
    std::printf(
        "EE guest-v0-write: pc=0x%08x opcode=0x%08x "
        "old=0x%016llx new=0x%016llx "
        "rs=%u rt=%u rd=%u\n",
        pc, opcode,
        static_cast<unsigned long long>(old_value),
        static_cast<unsigned long long>(new_value),
        (opcode >> 21u) & 31u, (opcode >> 16u) & 31u,
        (opcode >> 11u) & 31u
    );
    std::fflush(stdout);
}

} // namespace ee
