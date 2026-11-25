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
    return static_cast<uint32_t>(load_le(m_data + addr, 4u));
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
bool read_bus(s_ee_runtime* runtime, uint32_t physical, uint32_t width, s_ee_u128* out)
{
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

bool write_bus(s_ee_runtime* runtime, uint32_t physical, uint32_t width,
               const s_ee_u128* value)
{
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

// Main RAM is below the EE device aperture. The common case needs no
// VU0/MMIO checks, second alias translation or configured-range checks.
// Only a wholly backed physical span may take this path. Unusual maps,
// scratchpad, devices, and partial/out-of-bounds accesses keep the exact
// original checked bus/memory fallback.
uint8_t* fast_ram_span(ee_memory* memory, uint32_t physical, size_t width)
{
    return memory != nullptr && physical < k_mmio_first
        ? memory->physical_ram_span(physical, width) : nullptr;
}

// Aligned quadwords cannot straddle a direct-alias or device-aperture
// boundary. Deliberately reject unaligned input even when backed by RAM:
// the paired fallback must preserve independent address translation.
uint8_t* fast_quadword_span(s_ee_runtime* runtime, uint32_t address)
{
    if ((address & 15u) != 0u)
    {
        return nullptr;
    }
    ee_memory* memory = runtime_memory(runtime);
    if (memory == nullptr)
    {
        return nullptr;
    }
    return fast_ram_span(memory, memory->translate_direct_address(address), 16u);
}

// memcpy is alignment-safe and lowers to native-width loads/stores on
// little-endian hosts. Retain byte-wise LE semantics on other hosts.
template<typename T>
T read_ram(const uint8_t* bytes)
{
#if defined(_WIN32) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    T value{};
    std::memcpy(&value, bytes, sizeof(value));
    return value;
#else
    return static_cast<T>(load_le(bytes, sizeof(T)));
#endif
}

template<typename T>
void write_ram(uint8_t* bytes, T value)
{
#if defined(_WIN32) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    std::memcpy(bytes, &value, sizeof(value));
#else
    store_le(bytes, sizeof(T), static_cast<uint64_t>(value));
#endif
}

template<typename T, T (ee_memory::*Read)(uint32_t)>
T read_scalar(s_ee_runtime* runtime, uint32_t address)
{
    ee_memory* memory = runtime_memory(runtime);
    const uint32_t physical = memory != nullptr
        ? memory->translate_direct_address(address) : address;
    if (const uint8_t* bytes = fast_ram_span(memory, physical, sizeof(T)))
    {
        return read_ram<T>(bytes);
    }
    s_ee_u128 result{};
    if (read_bus(runtime, physical, static_cast<uint32_t>(sizeof(T)), &result))
    {
        return static_cast<T>(result.m_lo);
    }
    return memory != nullptr ? (memory->*Read)(address) : T{};
}

template<typename T, void (ee_memory::*Write)(uint32_t, T)>
void write_scalar(s_ee_runtime* runtime, uint32_t address, T value)
{
    ee_memory* memory = runtime_memory(runtime);
    const uint32_t physical = memory != nullptr
        ? memory->translate_direct_address(address) : address;
    if (uint8_t* bytes = fast_ram_span(memory, physical, sizeof(T)))
    {
        write_ram(bytes, value);
        return;
    }
    const s_ee_u128 data{value, 0};
    if (!write_bus(runtime, physical, static_cast<uint32_t>(sizeof(T)), &data) && memory != nullptr)
    {
        (memory->*Write)(address, value);
    }
}
} // namespace

uint8_t ee_mem_read8(s_ee_runtime* runtime, uint32_t address)
{
    return read_scalar<uint8_t, &ee_memory::read8>(runtime, address);
}

uint16_t ee_mem_read16(s_ee_runtime* runtime, uint32_t address)
{
    return read_scalar<uint16_t, &ee_memory::read16>(runtime, address);
}

uint32_t ee_mem_read32(s_ee_runtime* runtime, uint32_t address)
{
    return read_scalar<uint32_t, &ee_memory::read32>(runtime, address);
}

uint64_t ee_mem_read64(s_ee_runtime* runtime, uint32_t address)
{
    return read_scalar<uint64_t, &ee_memory::read64>(runtime, address);
}

void ee_mem_read128(s_ee_runtime* runtime, uint32_t address, s_ee_u128* out)
{
    if (out == nullptr)
    {
        return;
    }

    ee_memory* memory = runtime_memory(runtime);
    const uint32_t physical = memory != nullptr
        ? memory->translate_direct_address(address) : address;
    if (const uint8_t* bytes = fast_ram_span(memory, physical, 16u))
    {
#if defined(_WIN32) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
        static_assert(sizeof(s_ee_u128) == 16u, "EE quadword ABI");
        std::memcpy(out, bytes, 16u);
#else
        out->m_lo = read_ram<uint64_t>(bytes);
        out->m_hi = read_ram<uint64_t>(bytes + 8u);
#endif
        return;
    }
    *out = {};
    if (!read_bus(runtime, physical, 16u, out) && memory != nullptr)
    {
        memory->read128(address, out);
    }
}

void ee_mem_write8(s_ee_runtime* runtime, uint32_t address, uint8_t value)
{
    write_scalar<uint8_t, &ee_memory::write8>(runtime, address, value);
}

void ee_mem_write16(s_ee_runtime* runtime, uint32_t address, uint16_t value)
{
    write_scalar<uint16_t, &ee_memory::write16>(runtime, address, value);
}

void ee_mem_write32(s_ee_runtime* runtime, uint32_t address, uint32_t value)
{
    write_scalar<uint32_t, &ee_memory::write32>(runtime, address, value);
}

void ee_mem_write64(s_ee_runtime* runtime, uint32_t address, uint64_t value)
{
    write_scalar<uint64_t, &ee_memory::write64>(runtime, address, value);
}

void ee_mem_write128(s_ee_runtime* runtime, uint32_t address, const s_ee_u128* value)
{
    if (value == nullptr)
    {
        return;
    }

    ee_memory* memory = runtime_memory(runtime);
    const uint32_t physical = memory != nullptr
        ? memory->translate_direct_address(address) : address;
    if (uint8_t* bytes = fast_ram_span(memory, physical, 16u))
    {
#if defined(_WIN32) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
        static_assert(sizeof(s_ee_u128) == 16u, "EE quadword ABI");
        std::memcpy(bytes, value, 16u);
#else
        write_ram(bytes, value->m_lo);
        write_ram(bytes + 8u, value->m_hi);
#endif
        return;
    }
    if (!write_bus(runtime, physical, 16u, value) && memory != nullptr)
    {
        memory->write128(address, value);
    }
}


void ee_mem_read128_pair(s_ee_runtime* runtime, uint32_t address, s_ee_u128* out)
{
    if (out == nullptr)
    {
        return;
    }

    if (const uint8_t* bytes = fast_quadword_span(runtime, address))
    {
#if defined(_WIN32) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
        static_assert(sizeof(s_ee_u128) == 16u, "EE quadword ABI");
        std::memcpy(out, bytes, 16u);
#else
        out->m_lo = read_ram<uint64_t>(bytes);
        out->m_hi = read_ram<uint64_t>(bytes + 8u);
#endif
        return;
    }

    // Finish both reads before publishing the result, just as the IR did.
    const uint64_t lo = ee_mem_read64(runtime, address);
    const uint64_t hi = ee_mem_read64(runtime, address + 8u);
    *out = {lo, hi};
}

static bool try_write128_ram(s_ee_runtime* runtime, uint32_t address,
                             uint64_t lo, uint64_t hi)
{
    if (uint8_t* bytes = fast_quadword_span(runtime, address))
    {
#if defined(_WIN32) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
        static_assert(sizeof(s_ee_u128) == 16u, "EE quadword ABI");
        const s_ee_u128 value{lo, hi};
        std::memcpy(bytes, &value, 16u);
#else
        write_ram(bytes, lo);
        write_ram(bytes + 8u, hi);
#endif
        return true;
    }
    return false;
}

void ee_mem_write128_pair(s_ee_runtime* runtime, uint32_t address,
                          uint64_t lo, uint64_t hi)
{
    if (!try_write128_ram(runtime, address, lo, hi))
    {
        ee_mem_write64(runtime, address, lo);
        ee_mem_write64(runtime, address + 8u, hi);
    }
}

uint32_t ee_mem_try_write128_ram(s_ee_runtime* runtime, uint32_t address,
                                uint64_t lo, uint64_t hi)
{
    // Even a currently exhausted trace budget can retain ring history.
    // Keep all traced stores on the original per-lane path.
    return runtime != nullptr && !runtime->m_guest_store_trace_enabled &&
        try_write128_ram(runtime, address, lo, hi) ? 1u : 0u;
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
