#include "iop_kernel.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <utility>

namespace iop
{
namespace
{
constexpr uint32_t k_alarm_return_pc = 0x00013ff0u;
constexpr uint32_t k_hle_callback_return_pc = 0x00013fe0u;
// Reserved guest HLE trampoline immediately below the other return stubs.
constexpr uint32_t k_module_return_pc = 0x00013fd0u;
constexpr uint32_t k_thread_return_pc = 0x00013fc0u;
constexpr uint32_t k_interrupt_return_pc = 0x00013fb0u;
constexpr uint32_t k_interrupt_stack = 0x0001f000u;

bool valid_range(size_t size, uint64_t offset, uint64_t length)
{
    return offset <= size && length <= size - offset;
}

uint32_t read32(const std::vector<uint8_t>& bytes, size_t offset)
{
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1u]) << 8u) |
           (static_cast<uint32_t>(bytes[offset + 2u]) << 16u) |
           (static_cast<uint32_t>(bytes[offset + 3u]) << 24u);
}

uint16_t read16(const std::vector<uint8_t>& bytes, size_t offset)
{
    return static_cast<uint16_t>(
        bytes[offset] |
        (static_cast<uint32_t>(bytes[offset + 1u]) << 8u)
    );
}

uint64_t align16(uint64_t value)
{
    return (value + 15u) & ~uint64_t(15u);
}

e_kernel_stop stop_for_guest(e_step_result result)
{
    switch (result)
    {
    case e_step_result::memory_fault:
        return e_kernel_stop::memory_fault;
    case e_step_result::overflow:
        return e_kernel_stop::arithmetic_overflow;
    case e_step_result::breakpoint:
        return e_kernel_stop::breakpoint;
    default:
        return e_kernel_stop::unsupported_instruction;
    }
}
} // namespace

c_kernel::c_kernel()
{
    reset();
}

void c_kernel::reset()
{
    m_cpu.bind_hardware(nullptr);
    m_cpu.reset();
    m_hardware.reset();
    m_rom0_boot_active = false;
    m_allocations.clear();
    m_heap_allocations.clear();
    m_libraries.clear();
    m_modules.clear();
    m_ready_cache_dirty = true;
    m_threads.clear();
    m_ready_cache.clear();
    m_ready_cache_wake = std::numeric_limits<uint64_t>::max();
    m_alarms.clear();
    m_hle_events.clear();
    m_syscall = {};
    m_irq_lookup = {};
    m_irq_complete = {};
    m_interrupt_thread_id = 0u;
    m_critical_thread_id = 0u;
    m_interrupts_suspended = false;
    m_cycles = 0u;
    m_next_thread_wake = std::numeric_limits<uint64_t>::max();
    m_next_alarm_wake = std::numeric_limits<uint64_t>::max();
    m_next_module_id = 1u;
    m_next_thread_id = 1u;
    m_next_alarm_id = 1u;
    m_last_scheduled_id = 0u;
    m_current_thread_id = 0u;
    m_restart_current_thread = false;
}

bool c_kernel::start_rom0_boot(const std::vector<uint8_t>& image)
{
    // The ROM reset path is exclusive with the experimental module
    // bootstrap. A real BIOS is never represented as an IRX thread.
    if (m_rom0_boot_active || !m_modules.empty() ||
        !m_threads.empty() || !m_libraries.empty() ||
        !m_allocations.empty() ||
        !m_cpu.install_rom0(image))
    {
        return false;
    }
    m_cpu.bind_hardware(&m_hardware);
    if (!m_cpu.reset_rom0())
    {
        m_cpu.bind_hardware(nullptr);
        return false;
    }
    m_hardware.bind_dma_cpu(&m_cpu);
    m_cycles = 0u;
    m_current_thread_id = 0u;
    m_rom0_boot_active = true;
    return true;
}

bool c_kernel::attach_hle_hardware()
{
    if (m_rom0_boot_active)
    {
        return false;
    }
    // Reset detaches MMIO; the HLE bootstrap reattaches explicitly.
    m_cpu.bind_hardware(&m_hardware);
    m_hardware.bind_dma_cpu(&m_cpu);
    return true;
}

bool c_kernel::rom0_boot_active() const
{
    return m_rom0_boot_active;
}

c_hw& c_kernel::hardware()
{
    return m_hardware;
}

const c_hw& c_kernel::hardware() const
{
    return m_hardware;
}

c_cpu& c_kernel::cpu()
{
    return m_cpu;
}

const c_cpu& c_kernel::cpu() const
{
    return m_cpu;
}

uint64_t c_kernel::cycles() const
{
    return m_cycles;
}

uint32_t c_kernel::current_thread_id() const
{
    return m_current_thread_id;
}

bool c_kernel::in_interrupt_context() const
{
    return m_interrupt_thread_id != 0u &&
           m_current_thread_id == m_interrupt_thread_id;
}

bool c_kernel::interrupts_suspended() const
{
    return m_interrupts_suspended;
}

void c_kernel::set_interrupts_suspended(bool suspended)
{
    m_interrupts_suspended = suspended;
    if (m_current_thread_id != 0u)
    {
        const auto found = m_threads.find(m_current_thread_id);
        if (found != m_threads.end())
        {
            found->second.m_interrupts_suspended = suspended;
        }
    }
    m_critical_thread_id =
        suspended ? m_current_thread_id : 0u;
}

bool c_kernel::lookup_library(
    const std::string& name, uint16_t ordinal,
    uint32_t* entry
) const
{
    if (entry == nullptr)
    {
        return false;
    }
    const auto library = m_libraries.find(name);
    if (library == m_libraries.end())
    {
        return false;
    }
    const auto symbol = library->second.find(ordinal);
    if (symbol == library->second.end())
    {
        return false;
    }
    *entry = symbol->second;
    return true;
}

bool c_kernel::register_library(
    const std::string& name, uint16_t ordinal, uint32_t entry
)
{
    const uint32_t physical = entry & 0x1fffffffu;
    if (name.empty() || name.size() > 8u ||
        (entry & 3u) != 0u || physical >= c_cpu::k_ram_size)
    {
        return false;
    }

    auto& library = m_libraries[name];
    return library.emplace(ordinal, entry).second;
}

// LOADCORE RegisterLibraryEntries/ReleaseLibraryEntries operate on actual
// relocated IRX export tables, not an arbitrary guest-supplied name/address.
// Modules publish their entries during load_irx() after transactional
// validation; the original entrypoint's RegisterLibraryEntries is therefore
// initially idempotent. Release unregisters exactly that module's exports.
// Previously bound consumer JAL stubs are not repatched (linker limitation).
bool c_kernel::set_module_library_registration(
    uint32_t table, bool registered)
{
    if ((table & 3u) != 0u)
    {
        return false;
    }
    const uint32_t physical = table & 0x1fffffffu;
    const s_kernel_module* owner = nullptr;
    for (const auto& item : m_modules)
    {
        const s_irx_module& module = item.second.m_irx;
        const uint64_t base = module.m_base;
        const uint64_t end = base + module.m_size;
        if (physical >= base &&
            static_cast<uint64_t>(physical) + 24u <= end)
        {
            owner = &item.second;
            break;
        }
    }
    if (owner == nullptr)
    {
        return false;
    }

    uint32_t marker = 0u;
    if (!m_cpu.read32(table, &marker) || marker != 0x41c00000u)
    {
        return false;
    }
    std::string name;
    for (uint32_t i = 0u; i < 8u; ++i)
    {
        uint8_t ch = 0u;
        if (!m_cpu.read8(table + 12u + i, &ch))
        {
            return false;
        }
        if (ch == 0u)
        {
            break;
        }
        if (ch < 0x21u || ch > 0x7eu)
        {
            return false;
        }
        name.push_back(static_cast<char>(ch));
    }
    if (name.empty())
    {
        return false;
    }

    std::map<uint16_t, uint32_t> expected;
    for (const s_irx_export& item : owner->m_irx.m_exports)
    {
        if (item.m_library == name)
        {
            if (!expected.emplace(item.m_ordinal, item.m_entry).second)
            {
                return false;
            }
        }
    }
    if (expected.empty())
    {
        return false;
    }

    // Recheck all guest words against the real module export metadata,
    // including zero holes in sparse tables, before touching the registry.
    // The 20-byte header is followed by ordinal-indexed 32-bit pointers.
    const uint64_t end = static_cast<uint64_t>(owner->m_irx.m_base) +
        owner->m_irx.m_size;
    uint32_t entries_seen = 0u;
    bool terminated = false;
    for (uint32_t ordinal = 0u; ordinal < 1024u; ++ordinal)
    {
        const uint64_t offset = static_cast<uint64_t>(physical) +
            20u + static_cast<uint64_t>(ordinal) * 4u;
        if (offset + 4u > end)
        {
            return false;
        }
        uint32_t entry = 0u;
        if (!m_cpu.read32(table + 20u + ordinal * 4u, &entry))
        {
            return false;
        }
        const auto found = expected.find(static_cast<uint16_t>(ordinal));
        if (found == expected.end())
        {
            if (entry != 0u)
            {
                return false;
            }
            if (expected.upper_bound(static_cast<uint16_t>(ordinal)) ==
                expected.end())
            {
                terminated = true;
                break;
            }
            continue;
        }
        if (entry != found->second)
        {
            return false;
        }
        ++entries_seen;
    }
    if (!terminated || entries_seen != expected.size())
    {
        return false;
    }

    // An existing kernel/HLE provider cannot be overwritten or released
    // using a forged export table. Reject partial and conflicting state.
    const auto library = m_libraries.find(name);
    uint32_t existing_count = 0u;
    for (const auto& entry : expected)
    {
        if (library != m_libraries.end())
        {
            const auto found = library->second.find(entry.first);
            if (found != library->second.end())
            {
                if (found->second != entry.second)
                {
                    return false;
                }
                ++existing_count;
            }
        }
    }
    if (existing_count != 0u && existing_count != expected.size())
    {
        return false;
    }
    if (registered)
    {
        if (existing_count == 0u)
        {
            auto& entries = m_libraries[name];
            entries.insert(expected.begin(), expected.end());
        }
    }
    else
    {
        if (existing_count != expected.size() ||
            library == m_libraries.end())
        {
            return false;
        }
        for (const auto& entry : expected)
        {
            library->second.erase(entry.first);
        }
        if (library->second.empty())
        {
            m_libraries.erase(library);
        }
    }
    return true;
}

uint32_t c_kernel::allocate(uint32_t size)
{
    const uint64_t required = align16(size);
    if (required == 0u || required > c_cpu::k_ram_size - k_user_base)
    {
        return 0u;
    }

    uint64_t candidate = k_user_base;
    for (const auto& item : m_allocations)
    {
        const uint64_t start = item.first;
        const uint64_t end = start + item.second;
        if (candidate + required <= start)
        {
            break;
        }
        candidate = align16(std::max(candidate, end));
    }

    if (candidate + required > c_cpu::k_ram_size)
    {
        return 0u;
    }

    const uint32_t base = static_cast<uint32_t>(candidate);
    m_allocations.emplace(base, static_cast<uint32_t>(required));
    return base;
}

uint32_t c_kernel::allocate_thread_stack(uint32_t size)
{
    // ps2-web-recomp/SDBZ kernel::create_thread uses alloc(size, 1):
    // highest suitable gap, rounded to a 256-byte stack boundary.
    // Use 64-bit arithmetic so huge invalid guest stack requests cannot
    // wrap around the 2 MiB IOP physical RAM limit.
    const uint64_t required =
        (static_cast<uint64_t>(size) + 0xffu) & ~uint64_t(0xffu);
    if (required == 0u ||
        required > c_cpu::k_ram_size - k_user_base)
    {
        return 0u;
    }

    uint64_t upper = c_cpu::k_ram_size;
    uint64_t chosen = 0u;
    for (auto it = m_allocations.rbegin();
         it != m_allocations.rend(); ++it)
    {
        const uint64_t occupied_end =
            static_cast<uint64_t>(it->first) + it->second;
        if (upper >= required &&
            upper - required >= occupied_end &&
            upper - required >= k_user_base)
        {
            chosen = upper - required;
            break;
        }
        upper = it->first;
    }

    if (chosen == 0u && upper >= required &&
        upper - required >= k_user_base)
    {
        chosen = upper - required;
    }
    if (chosen != 0u)
    {
        m_allocations.emplace(static_cast<uint32_t>(chosen),
                              static_cast<uint32_t>(required));
    }
    return static_cast<uint32_t>(chosen);
}

uint32_t c_kernel::allocate_heap(uint32_t size)
{
    if (m_rom0_boot_active)
    {
        return 0u;
    }
    const uint32_t address = allocate(size);
    if (address != 0u)
    {
        m_heap_allocations.emplace(address, m_allocations.at(address));
    }
    return address;
}

bool c_kernel::free_heap(uint32_t address)
{
    if (m_rom0_boot_active ||
        m_heap_allocations.erase(address) == 0u)
    {
        return false;
    }
    return m_allocations.erase(address) == 1u;
}

bool c_kernel::owns_heap_span(uint32_t address, uint32_t size) const
{
    if (m_rom0_boot_active || size == 0u)
    {
        return false;
    }
    const auto upper = m_heap_allocations.upper_bound(address);
    if (upper == m_heap_allocations.begin())
    {
        return false;
    }
    const auto allocation = std::prev(upper);
    const uint64_t start = allocation->first;
    const uint64_t end = start + allocation->second;
    return static_cast<uint64_t>(address) >= start &&
           static_cast<uint64_t>(address) + size <= end;
}

uint32_t c_kernel::heap_max_free() const
{
    uint64_t candidate = k_user_base;
    uint64_t largest = 0u;
    for (const auto& item : m_allocations)
    {
        if (item.first > candidate)
        {
            largest = std::max(largest, item.first - candidate);
        }
        candidate = align16(std::max(
            candidate, static_cast<uint64_t>(item.first) + item.second
        ));
    }
    largest = std::max(
        largest, static_cast<uint64_t>(c_cpu::k_ram_size) - candidate
    );
    return static_cast<uint32_t>(largest);
}

uint32_t c_kernel::heap_total_free() const
{
    uint64_t candidate = k_user_base;
    uint64_t total = 0u;
    for (const auto& item : m_allocations)
    {
        if (item.first > candidate)
        {
            total += item.first - candidate;
        }
        candidate = align16(std::max(
            candidate, static_cast<uint64_t>(item.first) + item.second
        ));
    }
    total += static_cast<uint64_t>(c_cpu::k_ram_size) - candidate;
    return static_cast<uint32_t>(total);
}

bool c_kernel::query_heap_block(
    uint32_t address, uint32_t* base, uint32_t* size
) const
{
    if (base == nullptr || size == nullptr)
    {
        return false;
    }
    const auto upper = m_heap_allocations.upper_bound(address);
    if (upper == m_heap_allocations.begin())
    {
        return false;
    }
    const auto found = std::prev(upper);
    const uint64_t end =
        static_cast<uint64_t>(found->first) + found->second;
    if (static_cast<uint64_t>(address) >= end)
    {
        return false;
    }
    *base = found->first;
    *size = found->second;
    return true;
}

bool c_kernel::estimate_module_size(
    const std::vector<uint8_t>& bytes, uint32_t* size
) const
{
    if (size == nullptr || bytes.size() < 52u ||
        bytes[0] != 0x7fu || bytes[1] != 'E' ||
        bytes[2] != 'L' || bytes[3] != 'F' ||
        bytes[4] != 1u || bytes[5] != 1u)
    {
        return false;
    }

    const uint32_t phoff = read32(bytes, 28u);
    const uint32_t phnum = read16(bytes, 44u);
    const uint32_t phentsize = read16(bytes, 42u);
    if (phentsize < 32u || phnum == 0u ||
        !valid_range(bytes.size(), phoff,
                     static_cast<uint64_t>(phnum) * phentsize))
    {
        return false;
    }

    bool found_load = false;
    bool found_module = false;
    uint32_t memsize = 0u;
    uint32_t iopmod = 0u;
    for (uint32_t index = 0u; index < phnum; ++index)
    {
        const size_t at =
            static_cast<size_t>(phoff) + index * phentsize;
        switch (read32(bytes, at))
        {
        case 1u:
            if (found_load)
            {
                return false;
            }
            found_load = true;
            memsize = read32(bytes, at + 20u);
            break;
        case 0x70000080u:
            if (found_module)
            {
                return false;
            }
            found_module = true;
            iopmod = read32(bytes, at + 4u);
            break;
        default:
            break;
        }
    }

    if (!found_load || !found_module ||
        !valid_range(bytes.size(), iopmod, 26u))
    {
        return false;
    }

    const uint64_t total =
        static_cast<uint64_t>(read32(bytes, iopmod + 12u)) +
        read32(bytes, iopmod + 16u) +
        read32(bytes, iopmod + 20u);
    const uint64_t required = align16(std::max<uint64_t>(
        memsize, total
    ));
    if (required == 0u ||
        required > c_cpu::k_ram_size - k_user_base)
    {
        return false;
    }

    *size = static_cast<uint32_t>(required);
    return true;
}

s_module_load_result c_kernel::load_module(
    const std::vector<uint8_t>& bytes
)
{
    s_module_load_result result{};
    uint32_t required = 0u;
    if (!estimate_module_size(bytes, &required))
    {
        result.m_status = e_irx_load_result::invalid_module;
        return result;
    }

    const uint32_t base = allocate(required);
    if (base == 0u)
    {
        result.m_status = e_irx_load_result::insufficient_ram;
        return result;
    }

    const auto resolve = [this](
        const std::string& library, uint16_t ordinal, uint32_t* target
    )
    {
        const auto exports = m_libraries.find(library);
        if (exports == m_libraries.end() || target == nullptr)
        {
            return false;
        }
        const auto entry = exports->second.find(ordinal);
        if (entry == exports->second.end())
        {
            return false;
        }
        *target = entry->second;
        return true;
    };

    // Reject already registered exports before the IRX loader writes
    // any bytes. This prevents partially installed module state when
    // an authentic library collides with another module or HLE service.
    const auto accept_exports = [this](
        const std::vector<s_irx_export>& exports
    )
    {
        for (const s_irx_export& item : exports)
        {
            const auto library = m_libraries.find(item.m_library);
            if (library != m_libraries.end() &&
                library->second.count(item.m_ordinal) != 0u)
            {
                return false;
            }
        }
        return true;
    };

    s_irx_module image{};
    result.m_status = load_irx(
        m_cpu, bytes, base, &image, resolve, accept_exports,
        &result.m_link_diagnostic
    );
    if (result.m_status != e_irx_load_result::success)
    {
        m_allocations.erase(base);
        return result;
    }

    // All exports were checked against the existing registry and each
    // other on the staged image. Publish only after a successful load.
    for (const s_irx_export& item : image.m_exports)
    {
        m_libraries[item.m_library].emplace(
            item.m_ordinal, item.m_entry
        );
    }
    const uint32_t id = m_next_module_id++;
    const auto inserted =
        m_modules.emplace(id, s_kernel_module{id, std::move(image)});
    result.m_id = id;
    if (m_module_loaded)
    {
        m_module_loaded(inserted.first->second.m_irx);
    }
    return result;
}

const s_kernel_module* c_kernel::module(uint32_t id) const
{
    const auto found = m_modules.find(id);
    return found == m_modules.end() ? nullptr : &found->second;
}

const std::map<uint32_t, s_kernel_module>& c_kernel::modules() const
{
    return m_modules;
}

uint32_t c_kernel::create_thread(
    uint32_t entry, uint32_t gp, uint32_t priority,
    uint32_t stack_size, uint32_t a0, uint32_t a1,
    uint32_t attr, uint32_t option
)
{
    const uint32_t physical = entry & 0x1fffffffu;
    if (entry == 0u || (entry & 3u) != 0u ||
        physical >= c_cpu::k_ram_size ||
        stack_size < 64u)
    {
        return 0u;
    }

    const uint32_t stack_base = allocate_thread_stack(stack_size);
    if (stack_base == 0u)
    {
        return 0u;
    }
    const uint32_t allocated_size = m_allocations.at(stack_base);
    for (uint32_t offset = 0u;
         offset < allocated_size; offset += 4u)
    {
        m_cpu.write32(stack_base + offset, 0u);
    }

    s_kernel_thread thread{};
    thread.m_id = m_next_thread_id++;
    thread.m_priority = priority;
    thread.m_initial_priority = priority;
    thread.m_attr = attr;
    thread.m_option = option;
    thread.m_entry = entry;
    thread.m_gp = gp;
    thread.m_creator_thread_id = m_current_thread_id;
    thread.m_creator_pc = m_current_thread_id != 0u ?
        m_cpu.state().m_pc : 0u;
    thread.m_created_cycle = m_cycles;
    thread.m_stack_base = stack_base;
    thread.m_stack_size = allocated_size;
    thread.m_cpu.m_pc = entry;
    thread.m_cpu.m_next_pc = entry + 4u;
    thread.m_cpu.m_gpr[28] = gp;
    thread.m_cpu.m_gpr[29] =
        stack_base + allocated_size - 16u;
    thread.m_cpu.m_gpr[4] = a0;
    thread.m_cpu.m_gpr[5] = a1;
    const uint32_t id = thread.m_id;
    m_ready_cache_dirty = true;
    m_threads.emplace(id, std::move(thread));
    return id;
}

uint32_t c_kernel::create_module_thread(
    uint32_t module_id, uint32_t priority, uint32_t stack_size
)
{
    const s_kernel_module* loaded = module(module_id);
    if (loaded == nullptr ||
        !m_cpu.write32(k_module_return_pc, 0x0000000du))
    {
        return 0u;
    }

    const uint32_t id = create_thread(
        loaded->m_irx.m_entry,
        loaded->m_irx.m_gp,
        priority, stack_size
    );
    if (id != 0u)
    {
        s_kernel_thread& thread = m_threads.at(id);
        thread.m_module_entry = true;
        thread.m_module_id = module_id;
        thread.m_cpu.m_gpr[31] = k_module_return_pc;
    }
    return id;
}

const s_kernel_thread* c_kernel::thread(uint32_t id) const
{
    const auto found = m_threads.find(id);
    return found == m_threads.end() ? nullptr : &found->second;
}

const std::map<uint32_t, s_kernel_thread>& c_kernel::threads() const
{
    return m_threads;
}

bool c_kernel::has_guest_work() const
{
    if (m_rom0_boot_active)
    {
        return false;
    }
    for (const auto& item : m_threads)
    {
        const s_kernel_thread& thread = item.second;
        if (thread.m_status == e_thread_status::ready ||
            (thread.m_status == e_thread_status::blocked &&
             thread.m_wake_cycle != 0u))
        {
            return true;
        }
    }
    return !m_alarms.empty() || !m_hle_events.empty() ||
           m_hardware.spu2_dma_cycles_remaining() != 0u ||
           (m_irq_lookup && m_interrupt_thread_id == 0u &&
            m_hardware.next_pending_interrupt() >= 0);
}

bool c_kernel::can_schedule_hle_event(uint64_t ticks) const
{
    return !m_rom0_boot_active && ticks != 0u &&
           ticks <= std::numeric_limits<uint64_t>::max() - m_cycles &&
           m_hle_events.size() < 256u;
}

bool c_kernel::schedule_hle_event(
    uint64_t ticks, std::function<void()> callback
)
{
    if (!can_schedule_hle_event(ticks) || !callback)
    {
        return false;
    }
    m_hle_events.emplace(m_cycles + ticks, std::move(callback));
    return true;
}

s_guest_callback_origin c_kernel::make_guest_callback_origin(
    e_guest_callback_source source) const
{
    s_guest_callback_origin origin{};
    const s_cpu_state& state = m_cpu.state();
    origin.m_source = source;
    origin.m_registration_thread_id = m_current_thread_id;
    origin.m_registration_pc = state.m_pc;
    origin.m_registration_ra = state.m_gpr[31];
    origin.m_registration_gp = state.m_gpr[28];
    origin.m_registration_a0 = state.m_gpr[4];
    origin.m_registration_a1 = state.m_gpr[5];
    origin.m_registration_a2 = state.m_gpr[6];
    origin.m_registration_a3 = state.m_gpr[7];
    origin.m_registration_recent_count = state.m_recent_count;
    const uint32_t cap = s_cpu_state::k_recent_pc_capacity;
    const uint32_t first =
        (state.m_recent_next + cap - state.m_recent_count) % cap;
    for (uint32_t index = 0u; index < state.m_recent_count; ++index)
    {
        const uint32_t slot = (first + index) % cap;
        origin.m_registration_recent_pc[index] = state.m_recent_pc[slot];
        origin.m_registration_recent_opcode[index] =
            state.m_recent_opcode[slot];
    }
    return origin;
}

bool c_kernel::start_guest_callback(
    uint32_t callback, uint32_t gp, uint32_t argument,
    const s_guest_callback_origin& origin
)
{
    const uint32_t region = callback & 0xe0000000u;
    const uint32_t physical = callback & 0x1fffffffu;
    uint32_t opcode = 0u;
    if (m_rom0_boot_active || callback == 0u ||
        (callback & 3u) != 0u ||
        (region != 0u && region != 0x80000000u &&
         region != 0xa0000000u) ||
        physical >= c_cpu::k_ram_size ||
        !m_cpu.read32(callback, &opcode) ||
        !m_cpu.write32(k_hle_callback_return_pc, 0x0000000du))
    {
        return false;
    }
    const uint32_t id = create_thread(callback, gp, 1u,
                                      0x400u, argument);
    if (id == 0u)
    {
        return false;
    }
    s_kernel_thread& thread = m_threads.at(id);
    thread.m_hle_guest_callback = true;
    thread.m_callback_origin = origin;
    thread.m_cpu.m_gpr[31] = k_hle_callback_return_pc;
    return start_thread(id);
}

bool c_kernel::set_alarm(
    uint64_t ticks, uint32_t callback,
    uint32_t argument, uint32_t gp
)
{
    const uint32_t physical = callback & 0x1fffffffu;
    uint32_t opcode = 0u;
    if (m_rom0_boot_active || ticks == 0u ||
        ticks > std::numeric_limits<uint64_t>::max() - m_cycles ||
        callback == 0u || (callback & 3u) != 0u ||
        physical >= c_cpu::k_ram_size ||
        !m_cpu.read32(callback, &opcode))
    {
        return false;
    }
    for (const auto& item : m_alarms)
    {
        if (item.second.m_callback == callback &&
            item.second.m_argument == argument)
        {
            return false;
        }
    }
    // Reserved below the kernel HLE stubs and above their ctype table.
    // A real guest callback returns here through $ra; the break is
    // consumed only for a thread explicitly associated with an alarm.
    if (!m_cpu.write32(k_alarm_return_pc, 0x0000000du))
    {
        return false;
    }
    const uint32_t id = create_thread(
        callback, gp, 0u, 0x400u, argument);
    if (id == 0u)
    {
        return false;
    }
    s_kernel_thread& thread = m_threads.at(id);
    const uint32_t alarm_id = m_next_alarm_id++;
    thread.m_alarm_id = alarm_id;
    thread.m_cpu.m_gpr[31] = k_alarm_return_pc;
    m_alarms.emplace(alarm_id, s_kernel_alarm{
        alarm_id, id, callback, argument, gp, m_cycles + ticks, false
    });
    m_next_alarm_wake = std::min(m_next_alarm_wake, m_cycles + ticks);
    return true;
}

bool c_kernel::cancel_alarm(uint32_t callback, uint32_t argument)
{
    for (auto it = m_alarms.begin(); it != m_alarms.end(); ++it)
    {
        s_kernel_alarm& alarm = it->second;
        if (alarm.m_callback != callback ||
            alarm.m_argument != argument || alarm.m_cancelled)
        {
            continue;
        }
        auto thread = m_threads.find(alarm.m_thread_id);
        if (thread == m_threads.end())
        {
            return false;
        }
        if (thread->second.m_status == e_thread_status::running)
        {
            // A callback may cancel itself. Retire it on return.
            alarm.m_cancelled = true;
        }
        else
        {
            m_allocations.erase(thread->second.m_stack_base);
            m_ready_cache_dirty = true;
            m_threads.erase(thread);
            m_alarms.erase(it);
        }
        return true;
    }
    return false;
}

bool c_kernel::start_thread(uint32_t id)
{
    const auto found = m_threads.find(id);
    if (found == m_threads.end() ||
        found->second.m_status != e_thread_status::dormant ||
        !m_cpu.write32(k_thread_return_pc, 0x0000000du))
    {
        return false;
    }

    s_kernel_thread& thread = found->second;
    // The original IOP kernel's StartThread installs a guest exit
    // trampoline. Preserve the more specific callback and alarm
    // return sentinels; all ordinary threads must also have an RA.
    thread.m_cpu.m_gpr[31] =
        thread.m_hle_guest_callback ? k_hle_callback_return_pc :
        thread.m_alarm_id != 0u ? k_alarm_return_pc :
        thread.m_module_entry ? k_module_return_pc :
                                k_thread_return_pc;
    thread.m_module_entry_returned = false;
    thread.m_module_entry_result = 0u;
    thread.m_interrupts_suspended = false;
    m_ready_cache_dirty = true;
    thread.m_status = e_thread_status::ready;
    return true;
}

bool c_kernel::start_thread_with_argument(
    uint32_t id, uint32_t argument
)
{
    return start_thread_with_arguments(id, argument, 0u);
}

bool c_kernel::start_thread_with_arguments(
    uint32_t id, uint32_t a0, uint32_t a1
)
{
    // ps2-web-recomp/SDBZ resolves thbase StartThread(0) to current().
    // A self restart is legal while RUNNING. It must preserve the new
    // entry context across this syscall; an ordinary dormant target
    // still uses the existing start_thread() path.
    const auto found = m_threads.find(id);
    const bool restart_current =
        id != 0u && id == m_current_thread_id &&
        found != m_threads.end() &&
        found->second.m_status == e_thread_status::running;
    if (!restart_current && !start_thread(id))
    {
        return false;
    }
    if (restart_current)
    {
        if (!m_cpu.write32(k_thread_return_pc, 0x0000000du))
        {
            return false;
        }
        m_ready_cache_dirty = true;
        found->second.m_status = e_thread_status::ready;
        found->second.m_module_entry_returned = false;
        found->second.m_module_entry_result = 0u;
        found->second.m_interrupts_suspended = false;
    }
    s_kernel_thread& thread = m_threads.at(id);
    s_cpu_state initial{};
    initial.m_pc = thread.m_entry;
    initial.m_next_pc = thread.m_entry + 4u;
    initial.m_gpr[28] = thread.m_gp;
    initial.m_gpr[29] =
        thread.m_stack_base + thread.m_stack_size - 16u;
    initial.m_gpr[4] = a0;
    initial.m_gpr[5] = a1;
    // StartThreadArgs rebuilds the R3000 register context, including
    // $ra. Preserve the genuine exit trampoline for module starters,
    // generic threads and guest callback/alarm threads alike.
    initial.m_gpr[31] =
        thread.m_hle_guest_callback ? k_hle_callback_return_pc :
        thread.m_alarm_id != 0u ? k_alarm_return_pc :
        thread.m_module_entry ? k_module_return_pc :
                                k_thread_return_pc;
    thread.m_cpu = initial;
    m_ready_cache_dirty = true;
    thread.m_priority = thread.m_initial_priority;
    thread.m_wakeup_count = 0u;
    thread.m_wait_type = 0u;
    thread.m_delete_on_exit = false;
    if (restart_current)
    {
        // The live guest's RA is the original import caller, before
        // restoring the new thread entry context.
        thread.m_last_self_restart_ra = m_cpu.state().m_gpr[31];
        thread.m_last_self_restart_arg = a0;
        ++thread.m_self_restarts;
        m_restart_current_thread = true;
    }
    return true;
}

bool c_kernel::start_module_thread_with_arguments(
    uint32_t thread_id,
    const std::string& module_path,
    const std::vector<std::string>& arguments
)
{
    const auto found = m_threads.find(thread_id);
    if (found == m_threads.end() ||
        found->second.m_status != e_thread_status::dormant ||
        module_path.empty() || module_path.size() > 252u ||
        arguments.size() > 252u)
    {
        return false;
    }

    // The IOP modload ABI passes argv[0] as the module path, and
    // further NUL-separated LOADFILE arguments in argv[1..argc-1].
    // Stage pointers and all strings in the actual thread's stack
    // allocation rather than a temporary HLE buffer.
    const uint32_t argc =
        1u + static_cast<uint32_t>(arguments.size());
    const uint64_t pointer_bytes =
        (static_cast<uint64_t>(argc) + 1u) * 4u;
    uint64_t storage_bytes = pointer_bytes + module_path.size() + 1u;
    for (const std::string& argument : arguments)
    {
        storage_bytes += argument.size() + 1u;
    }
    // Leave a 128-byte MIPS entry frame below argv and a top redzone.
    const uint64_t aligned_bytes = align16(storage_bytes);
    const s_kernel_thread& thread = found->second;
    if (aligned_bytes + 144u > thread.m_stack_size)
    {
        return false;
    }
    const uint32_t end =
        thread.m_stack_base + thread.m_stack_size - 16u;
    const uint32_t argv =
        static_cast<uint32_t>(
            (static_cast<uint64_t>(end) - aligned_bytes) &
            ~uint64_t(15u));
    const uint32_t initial_sp = argv - 128u;
    if (initial_sp < thread.m_stack_base)
    {
        return false;
    }

    std::vector<uint8_t> frame(
        static_cast<size_t>(aligned_bytes), 0u);
    uint32_t cursor = static_cast<uint32_t>(pointer_bytes);
    const auto add_argument =
        [&](uint32_t index, const std::string& argument)
    {
        const uint32_t address = argv + cursor;
        for (uint32_t byte = 0u; byte < 4u; ++byte)
        {
            frame[index * 4u + byte] =
                static_cast<uint8_t>(address >> (byte * 8u));
        }
        for (unsigned char character : argument)
        {
            frame[cursor++] = character;
        }
        frame[cursor++] = 0u;
    };

    add_argument(0u, module_path);
    for (uint32_t index = 0u; index < arguments.size(); ++index)
    {
        add_argument(index + 1u, arguments[index]);
    }
    if (!m_cpu.dma_write(
            argv, frame.data(), static_cast<uint32_t>(frame.size())) ||
        !start_thread_with_arguments(thread_id, argc, argv))
    {
        return false;
    }
    m_threads.at(thread_id).m_cpu.m_gpr[29] = initial_sp;
    return true;
}

bool c_kernel::wake_thread(uint32_t id)
{
    const auto found = m_threads.find(id);
    if (found == m_threads.end() ||
        found->second.m_status != e_thread_status::blocked)
    {
        return false;
    }
    m_ready_cache_dirty = true;
    found->second.m_status = e_thread_status::ready;
    m_ready_cache_dirty = true;
    found->second.m_wake_cycle = 0u;
    found->second.m_wait_type = 0u;
    return true;
}

bool c_kernel::wake_thread_with_result(uint32_t id, uint32_t result)
{
    if (!wake_thread(id))
    {
        return false;
    }
    m_threads.at(id).m_cpu.m_gpr[2] = result;
    return true;
}

bool c_kernel::stop_thread(uint32_t id)
{
    const auto found = m_threads.find(id);
    if (found == m_threads.end() ||
        found->second.m_status == e_thread_status::faulted)
    {
        return false;
    }
    m_ready_cache_dirty = true;
    found->second.m_status = e_thread_status::dormant;
    found->second.m_interrupts_suspended = false;
    if (id == m_critical_thread_id)
    {
        m_critical_thread_id = 0u;
        m_interrupts_suspended = false;
    }
    m_ready_cache_dirty = true;
    found->second.m_wake_cycle = 0u;
    found->second.m_wait_type = 0u;
    found->second.m_wakeup_count = 0u;
    found->second.m_delete_on_exit = false;
    return true;
}

bool c_kernel::delete_thread(uint32_t id)
{
    const auto found = m_threads.find(id);
    if (found == m_threads.end() ||
        found->second.m_status != e_thread_status::dormant ||
        found->second.m_alarm_id != 0u ||
        m_allocations.find(found->second.m_stack_base) ==
            m_allocations.end())
    {
        return false;
    }
    m_allocations.erase(found->second.m_stack_base);
    m_ready_cache_dirty = true;
    m_threads.erase(found);
    return true;
}

bool c_kernel::exit_current_thread(bool delete_on_exit)
{
    const auto found = m_threads.find(m_current_thread_id);
    if (found == m_threads.end() ||
        found->second.m_status != e_thread_status::running)
    {
        return false;
    }
    m_ready_cache_dirty = true;
    found->second.m_status = e_thread_status::dormant;
    found->second.m_interrupts_suspended = false;
    if (m_current_thread_id == m_critical_thread_id)
    {
        m_critical_thread_id = 0u;
        m_interrupts_suspended = false;
    }
    m_ready_cache_dirty = true;
    found->second.m_wake_cycle = 0u;
    found->second.m_wait_type = 0u;
    found->second.m_delete_on_exit = delete_on_exit;
    return true;
}

bool c_kernel::change_thread_priority(
    uint32_t id, uint32_t priority
)
{
    if (id == 0u)
    {
        id = m_current_thread_id;
    }
    const auto found = m_threads.find(id);
    if (found == m_threads.end())
    {
        return false;
    }
    const uint32_t new_priority =
        priority == 0u ? found->second.m_initial_priority : priority;
    if (new_priority < 1u || new_priority > 126u)
    {
        return false;
    }
    m_ready_cache_dirty = true;
    found->second.m_priority = new_priority;
    return true;
}

bool c_kernel::sleep_thread()
{
    const auto found = m_threads.find(m_current_thread_id);
    if (found == m_threads.end() ||
        found->second.m_status != e_thread_status::running)
    {
        return false;
    }
    if (found->second.m_wakeup_count != 0u)
    {
        --found->second.m_wakeup_count;
        return true;
    }
    return block_current(1u);
}

bool c_kernel::wakeup_thread(uint32_t id)
{
    const auto found = m_threads.find(id);
    if (found == m_threads.end())
    {
        return false;
    }
    if (found->second.m_status == e_thread_status::blocked &&
        found->second.m_wait_type == 1u)
    {
        return wake_thread(id);
    }
    if (found->second.m_wakeup_count ==
        std::numeric_limits<uint32_t>::max())
    {
        return false;
    }
    ++found->second.m_wakeup_count;
    return true;
}

bool c_kernel::cancel_wakeup_thread(
    uint32_t id, uint32_t* previous
)
{
    const auto found = m_threads.find(id);
    if (found == m_threads.end() || previous == nullptr)
    {
        return false;
    }
    *previous = found->second.m_wakeup_count;
    found->second.m_wakeup_count = 0u;
    return true;
}

bool c_kernel::block_current(uint32_t wait_type)
{
    const auto found = m_threads.find(m_current_thread_id);
    if (found == m_threads.end() ||
        found->second.m_status != e_thread_status::running)
    {
        return false;
    }
    m_ready_cache_dirty = true;
    found->second.m_status = e_thread_status::blocked;
    m_ready_cache_dirty = true;
    found->second.m_wake_cycle = 0u;
    found->second.m_wait_type = wait_type;
    return true;
}

bool c_kernel::sleep_current(uint64_t delay_cycles)
{
    const auto found = m_threads.find(m_current_thread_id);
    if (found == m_threads.end() ||
        found->second.m_status != e_thread_status::running ||
        delay_cycles == 0u ||
        delay_cycles > std::numeric_limits<uint64_t>::max() -
                       m_cycles)
    {
        return false;
    }
    m_ready_cache_dirty = true;
    found->second.m_status = e_thread_status::blocked;
    m_ready_cache_dirty = true;
    found->second.m_wake_cycle = m_cycles + delay_cycles;
    m_next_thread_wake = std::min(m_next_thread_wake, found->second.m_wake_cycle);
    found->second.m_wait_type = 2u;
    return true;
}

void c_kernel::bind_syscall(t_kernel_syscall handler)
{
    m_syscall = std::move(handler);
}

void c_kernel::bind_interrupts(
    t_kernel_irq_lookup lookup, t_kernel_irq_complete complete
)
{
    m_irq_lookup = std::move(lookup);
    m_irq_complete = std::move(complete);
}

bool c_kernel::start_guest_interrupt(
    uint32_t line, const s_kernel_interrupt_target& target
)
{
    const uint32_t physical = target.m_handler & 0x1fffffffu;
    const uint32_t region = target.m_handler & 0xe0000000u;
    uint32_t opcode = 0u;
    if (m_interrupt_thread_id != 0u ||
        target.m_handler == 0u || (target.m_handler & 3u) != 0u ||
        (region != 0u && region != 0x80000000u &&
         region != 0xa0000000u) ||
        physical >= c_cpu::k_ram_size ||
        !m_cpu.read32(target.m_handler, &opcode) ||
        !m_cpu.write32(k_interrupt_return_pc, 0x0000000du))
    {
        return false;
    }
    const uint32_t id = create_thread(
        target.m_handler, 0u, 0u, 0x400u, target.m_argument);
    if (id == 0u)
    {
        return false;
    }
    s_kernel_thread& thread = m_threads.at(id);
    thread.m_interrupt_callback = true;
    thread.m_interrupt_line = line;
    if (!start_thread(id))
    {
        m_allocations.erase(thread.m_stack_base);
        m_ready_cache_dirty = true;
        m_threads.erase(id);
        return false;
    }
    // The original INTRMAN executes on its reserved interrupt stack;
    // this wrapper runs only the registered guest handler instructions.
    thread.m_cpu.m_gpr[29] = k_interrupt_stack;
    thread.m_cpu.m_gpr[31] = k_interrupt_return_pc;
    m_interrupt_thread_id = id;
    return true;
}

uint32_t c_kernel::next_ready(uint64_t& next_wake) const
{
    if (m_interrupts_suspended && m_critical_thread_id != 0u)
    {
        const auto held = m_threads.find(m_critical_thread_id);
        if (held != m_threads.end() &&
            held->second.m_status == e_thread_status::ready)
        {
            return m_critical_thread_id;
        }
    }
    if (m_interrupt_thread_id != 0u)
    {
        const auto handler = m_threads.find(m_interrupt_thread_id);
        if (handler != m_threads.end() &&
            handler->second.m_status == e_thread_status::ready)
        {
            return m_interrupt_thread_id;
        }
    }
    if (m_ready_cache_dirty || m_ready_cache_wake <= m_cycles)
    {
        rebuild_ready_cache();
    }
    next_wake = std::min(next_wake, m_ready_cache_wake);
    if (m_ready_cache.empty())
    {
        return 0u;
    }
    // With one highest-priority thread, the round-robin search
    // necessarily selects that thread, including after wraparound.
    if (m_ready_cache.size() == 1u)
    {
        return m_ready_cache.front();
    }
    const auto candidate = std::upper_bound(
        m_ready_cache.begin(), m_ready_cache.end(), m_last_scheduled_id);
    return candidate != m_ready_cache.end() ? *candidate : m_ready_cache.front();
}

void c_kernel::rebuild_ready_cache() const
{
    m_ready_cache.clear();
    m_ready_cache_wake = std::numeric_limits<uint64_t>::max();
    uint32_t priority = std::numeric_limits<uint32_t>::max();
    for (const auto& item : m_threads)
    {
        const s_kernel_thread& thread = item.second;
        if (thread.m_status == e_thread_status::blocked &&
            thread.m_wake_cycle > m_cycles)
        {
            m_ready_cache_wake = std::min(m_ready_cache_wake, thread.m_wake_cycle);
        }
        if (thread.m_status != e_thread_status::ready)
        {
            continue;
        }
        if (thread.m_priority < priority)
        {
            priority = thread.m_priority;
            m_ready_cache.clear();
        }
        if (thread.m_priority == priority)
        {
            m_ready_cache.push_back(thread.m_id);
        }
    }
    m_ready_cache_dirty = false;
}

void c_kernel::wake_due()
{
    // A runnable guest in CpuSuspendIntr retains the CPU. Defer
    // software events and context switches, but NOT hardware clocks
    // or DMA transfers, until the guest resumes interrupts.
    if (m_interrupts_suspended && m_critical_thread_id != 0u)
    {
        const auto held = m_threads.find(m_critical_thread_id);
        if (held != m_threads.end() &&
            (held->second.m_status == e_thread_status::ready ||
             held->second.m_status == e_thread_status::running))
        {
            return;
        }
    }
    // Events may schedule additional future events while executing.
    while (!m_hle_events.empty() &&
           m_hle_events.begin()->first <= m_cycles)
    {
        std::function<void()> callback =
            std::move(m_hle_events.begin()->second);
        m_hle_events.erase(m_hle_events.begin());
        callback();
    }
    // HLE callbacks above may add earlier alarms. Only skip a scan
    // when every live deadline is still in the future. Preserve the
    // original alarm-ID order, critical sections and exact idle scan.
    if (m_cycles >= m_next_alarm_wake)
    {
        m_next_alarm_wake = std::numeric_limits<uint64_t>::max();
        for (auto& item : m_alarms)
        {
            s_kernel_alarm& alarm = item.second;
            if (alarm.m_cancelled)
            {
                continue;
            }
            // Keep due but active alarms in the conservative hint: their
            // thread may become dormant without changing the deadline.
            m_next_alarm_wake = std::min(m_next_alarm_wake, alarm.m_deadline);
            if (alarm.m_deadline > m_cycles)
            {
                continue;
            }
            auto found = m_threads.find(alarm.m_thread_id);
            if (found == m_threads.end() ||
                found->second.m_status != e_thread_status::dormant)
            {
                continue;
            }
            s_kernel_thread& thread = found->second;
            s_cpu_state state{};
            state.m_pc = alarm.m_callback;
            state.m_next_pc = alarm.m_callback + 4u;
            state.m_gpr[4] = alarm.m_argument;
            state.m_gpr[28] = alarm.m_gp;
            state.m_gpr[29] =
                thread.m_stack_base + thread.m_stack_size - 16u;
            state.m_gpr[31] = k_alarm_return_pc;
            thread.m_cpu = state;
            start_thread(alarm.m_thread_id);
        }
    }
    // HLE callbacks, alarms and timed threads retain their original
    // ordering. Skip the timed-thread scan when no sleep is due.
    if (m_cycles >= m_next_thread_wake)
    {
        m_next_thread_wake = std::numeric_limits<uint64_t>::max();
        for (auto& item : m_threads)
        {
            s_kernel_thread& thread = item.second;
            if (thread.m_status == e_thread_status::blocked &&
                thread.m_wake_cycle != 0u)
            {
                if (thread.m_wake_cycle <= m_cycles)
                {
                    m_ready_cache_dirty = true;
                    thread.m_status = e_thread_status::ready;
                    m_ready_cache_dirty = true;
                    thread.m_wake_cycle = 0u;
                    thread.m_wait_type = 0u;
                }
                else
                {
                    m_next_thread_wake = std::min(
                        m_next_thread_wake, thread.m_wake_cycle);
                }
            }
        }
    }
}

s_kernel_run_result c_kernel::run(
    uint64_t cycle_budget, uint32_t quantum
)
{
    s_kernel_run_result result{};
    if (quantum == 0u)
    {
        quantum = 1u;
    }
    m_current_thread_id = 0u;
    m_restart_current_thread = false;

    // Unlike an IRX entrypoint, the actual IOP firmware begins at the
    // R3000 reset vector. Execute its own instruction stream directly.
    // Until BIOS kernel imports, interrupts and IOP MMIO are modeled,
    // stop at the first unsupported service without inventing a
    // successful firmware boot.
    if (m_rom0_boot_active)
    {
        uint64_t left = cycle_budget;
        while (left != 0u)
        {
            const uint32_t slice = static_cast<uint32_t>(
                std::min<uint64_t>(left, quantum)
            );
            const s_run_result stepped = m_cpu.run(slice);
            m_cycles += stepped.m_instructions;
            result.m_executed += stepped.m_instructions;
            // Translated IRX code may retire a few instructions past the slice;
            // never let the unsigned budget wrap.
            left -= std::min<uint64_t>(left, stepped.m_instructions);
            if (stepped.m_result == e_step_result::budget_exhausted)
            {
                continue;
            }
            result.m_stop =
                stepped.m_result == e_step_result::syscall
                    ? e_kernel_stop::unhandled_syscall
                    : stop_for_guest(stepped.m_result);
            result.m_guest_pc = stepped.m_pc;
            result.m_opcode = stepped.m_opcode;
            return result;
        }
        result.m_stop = e_kernel_stop::budget_exhausted;
        return result;
    }

    uint64_t remaining = cycle_budget;
    while (remaining != 0u)
    {
        // No software event can wake a thread while all three
        // conservative deadlines remain in the future. Keep the actual
        // wake ordering and interrupt-suspension checks in wake_due().
        if (m_cycles >= m_next_alarm_wake ||
            m_cycles >= m_next_thread_wake ||
            (!m_hle_events.empty() &&
             m_hle_events.begin()->first <= m_cycles))
        {
            wake_due();
        }
        // ps2-web-recomp/SDBZ saves interrupt state with each thread.
        // A suspended thread owns the CPU only while runnable. Once it
        // blocks or exits, a real DMA interrupt may wake it.
        if (m_interrupts_suspended && m_critical_thread_id != 0u)
        {
            const auto held = m_threads.find(m_critical_thread_id);
            if (held == m_threads.end() ||
                (held->second.m_status != e_thread_status::ready &&
                 held->second.m_status != e_thread_status::running))
            {
                std::fprintf(stderr,
                    "IOP critical owner yielded: thread=%u cycles=%llu\n",
                    m_critical_thread_id,
                    static_cast<unsigned long long>(m_cycles));
                m_interrupts_suspended = false;
                m_critical_thread_id = 0u;
            }
        }
        if (m_irq_lookup && !m_interrupts_suspended &&
            m_interrupt_thread_id == 0u)
        {
            // Resolve the real device source and acknowledge before
            // executing a registered guest INTRMAN callback.
            for (uint32_t attempt = 0u; attempt < 32u; ++attempt)
            {
                const int32_t pending =
                    m_hardware.next_pending_interrupt();
                if (pending < 0)
                {
                    break;
                }
                const uint32_t line = static_cast<uint32_t>(pending);
                s_kernel_interrupt_target target{};
                const bool registered = m_irq_lookup(line, &target);
                if (registered && !start_guest_interrupt(line, target))
                {
                    result.m_stop =
                        e_kernel_stop::unsupported_instruction;
                    result.m_guest_pc = target.m_handler;
                    return result;
                }
                if (!m_hardware.acknowledge_interrupt(line))
                {
                    result.m_stop =
                        e_kernel_stop::unsupported_instruction;
                    return result;
                }
                std::printf(
                    "IOP INTRMAN IRQ acknowledged: line=%u registered=%u "
                    "handler=0x%08x cycles=%llu\n",
                    line, registered ? 1u : 0u,
                    registered ? target.m_handler : 0u,
                    static_cast<unsigned long long>(m_cycles));
                if (registered)
                {
                    break;
                }
            }
        }
        uint64_t next_wake = std::numeric_limits<uint64_t>::max();
        const uint32_t id = next_ready(next_wake);
        if (id == 0u)
        {
            for (const auto& item : m_alarms)
            {
                if (!item.second.m_cancelled &&
                    item.second.m_deadline > m_cycles)
                {
                    next_wake = std::min(
                        next_wake, item.second.m_deadline
                    );
                }
            }
            if (!m_hle_events.empty() &&
                m_hle_events.begin()->first > m_cycles)
            {
                next_wake = std::min(
                    next_wake, m_hle_events.begin()->first
                );
            }
            // The original SPU2 DMA4/DMA7 backend schedules an
            // actual transfer-completion event after the copied
            // guest data has entered sound/IOP RAM. A blocked guest
            // thread must not freeze that device deadline.
            const uint32_t dma_left =
                m_hardware.spu2_dma_cycles_remaining();
            if (dma_left != 0u)
            {
                const uint64_t deadline =
                    dma_left <=
                        std::numeric_limits<uint64_t>::max() - m_cycles
                        ? m_cycles + dma_left
                        : std::numeric_limits<uint64_t>::max();
                next_wake = std::min(next_wake, deadline);
            }
            if (next_wake ==
                std::numeric_limits<uint64_t>::max())
            {
                uint64_t to_advance = remaining;
                while (to_advance != 0u)
                {
                    const uint32_t step = static_cast<uint32_t>(
                        std::min<uint64_t>(
                            to_advance,
                            std::numeric_limits<uint32_t>::max()));
                    m_hardware.advance_cycles(step);
                    to_advance -= step;
                }
                m_cycles += remaining;
                result.m_stop = e_kernel_stop::idle;
                break;
            }

            const uint64_t skipped = std::min(
                remaining, next_wake - m_cycles
            );
            // A real device continues counting IOP cycles while
            // the kernel has no runnable R3000 thread. Advance only
            // the supplied cycle budget, never wall-clock time or
            // unrelated EE dispatch counts.
            uint64_t to_advance = skipped;
            while (to_advance != 0u)
            {
                const uint32_t quantum_cycles =
                    static_cast<uint32_t>(std::min<uint64_t>(
                        to_advance,
                        std::numeric_limits<uint32_t>::max()));
                m_hardware.advance_cycles(quantum_cycles);
                to_advance -= quantum_cycles;
            }
            m_cycles += skipped;
            remaining -= skipped;
            continue;
        }

        s_kernel_thread& thread = m_threads.at(id);
        // Resume this thread's saved INTRMAN suspension state.
        m_interrupts_suspended = thread.m_interrupts_suspended;
        m_critical_thread_id = m_interrupts_suspended ? id : 0u;
        thread.m_status = e_thread_status::running;
        m_current_thread_id = id;
        m_last_scheduled_id = id;
        // Report the last real scheduled guest ID even when an execution
        // budget expires without a fault. Previously these successful
        // turns misleadingly reported thread=0 (idle/kernel).
        result.m_thread_id = id;
        m_cpu.state() = thread.m_cpu;

        const uint32_t slice = static_cast<uint32_t>(
            std::min<uint64_t>(remaining, quantum)
        );
        const s_run_result step_result = m_cpu.run(slice);
        ++thread.m_guest_slices;
        thread.m_guest_instructions += step_result.m_instructions;
        m_cycles += step_result.m_instructions;
        m_hardware.advance_cycles(step_result.m_instructions);
        result.m_executed += step_result.m_instructions;
        // Translated IRX code may retire a few instructions past the slice;
        // never let the unsigned budget wrap.
        remaining -= std::min<uint64_t>(remaining, step_result.m_instructions);

        // A registered INTRMAN handler has executed its real JR $ra.
        // Its return value controls whether an ordinary (<32) IRQ
        // becomes masked, exactly as in ps2-web-recomp/SDBZ.
        if (step_result.m_result == e_step_result::breakpoint &&
            step_result.m_pc == k_interrupt_return_pc &&
            thread.m_interrupt_callback &&
            m_interrupt_thread_id == id)
        {
            const uint32_t line = thread.m_interrupt_line;
            const uint32_t guest_result = m_cpu.state().m_gpr[2];
            m_allocations.erase(thread.m_stack_base);
            m_ready_cache_dirty = true;
            m_threads.erase(id);
            m_interrupt_thread_id = 0u;
            m_current_thread_id = 0u;
            if (m_irq_complete)
            {
                m_irq_complete(line, guest_result);
            }
            std::printf(
                "IOP INTRMAN guest handler returned: line=%u "
                "result=0x%08x cycles=%llu\n",
                line, guest_result,
                static_cast<unsigned long long>(m_cycles));
            continue;
        }

        // An ordinary guest thread has returned through its genuine
        // MIPS $ra trampoline. The completed guest thread is dormant;
        // this is not a synthetic successful syscall or module result.
        if (step_result.m_result == e_step_result::breakpoint &&
            step_result.m_pc == k_thread_return_pc &&
            !thread.m_module_entry)
        {
            thread.m_cpu = m_cpu.state();
            m_ready_cache_dirty = true;
            thread.m_status = e_thread_status::dormant;
            m_current_thread_id = 0u;
            continue;
        }

        // Module _start has returned its original V0 through the
        // installed $ra sentinel. Only the actual guest BREAK at that
        // reserved address can complete this startup thread; no time-
        // based or LOADFILE-triggered fake completion is permitted.
        if (step_result.m_result == e_step_result::breakpoint &&
            step_result.m_pc == k_module_return_pc &&
            thread.m_module_entry)
        {
            thread.m_cpu = m_cpu.state();
            thread.m_module_entry_result = m_cpu.state().m_gpr[2];
            thread.m_module_entry_returned = true;
            m_ready_cache_dirty = true;
            thread.m_status = e_thread_status::dormant;
            std::printf(
                "IOP guest module entry returned: module=%u thread=%u "
                "result=0x%08x cycles=%llu\n",
                thread.m_module_id, id, thread.m_module_entry_result,
                static_cast<unsigned long long>(m_cycles));
            m_current_thread_id = 0u;
            continue;
        }

        // A hosted CDVD completion invokes a real guest callback.
        // Once the original R3000 function returns through $ra, retire
        // only that one-shot thread; never reschedule it as an alarm.
        if (step_result.m_result == e_step_result::breakpoint &&
            step_result.m_pc == k_hle_callback_return_pc &&
            thread.m_hle_guest_callback)
        {
            m_allocations.erase(thread.m_stack_base);
            m_ready_cache_dirty = true;
            m_threads.erase(id);
            m_current_thread_id = 0u;
            continue;
        }

        if (step_result.m_result == e_step_result::breakpoint &&
            step_result.m_pc == k_alarm_return_pc &&
            thread.m_alarm_id != 0u)
        {
            const uint32_t alarm_id = thread.m_alarm_id;
            const uint32_t repeat_ticks = m_cpu.state().m_gpr[2];
            auto alarm = m_alarms.find(alarm_id);
            if (alarm == m_alarms.end() ||
                (repeat_ticks != 0u &&
                 repeat_ticks >
                     std::numeric_limits<uint64_t>::max() - m_cycles))
            {
                m_ready_cache_dirty = true;
                thread.m_status = e_thread_status::faulted;
                thread.m_cpu = m_cpu.state();
                result.m_stop = e_kernel_stop::arithmetic_overflow;
                result.m_thread_id = id;
                result.m_guest_pc = step_result.m_pc;
                result.m_opcode = step_result.m_opcode;
                m_current_thread_id = 0u;
                return result;
            }
            if (repeat_ticks == 0u || alarm->second.m_cancelled)
            {
                m_allocations.erase(thread.m_stack_base);
                m_ready_cache_dirty = true;
                m_threads.erase(id);
                m_alarms.erase(alarm);
            }
            else
            {
                alarm->second.m_deadline = m_cycles + repeat_ticks;
                m_next_alarm_wake = std::min(m_next_alarm_wake, alarm->second.m_deadline);
                m_ready_cache_dirty = true;
                thread.m_status = e_thread_status::dormant;
                thread.m_cpu = m_cpu.state();
            }
            m_current_thread_id = 0u;
            continue;
        }

        if (step_result.m_result == e_step_result::syscall)
        {
            // A handled syscall consumes one guest instruction, including
            // its kernel HLE boundary. It is never silently ignored.
            const uint32_t number =
                m_cpu.state().m_gpr[2];
            thread.m_last_syscall_number = number;
            thread.m_last_syscall_pc = step_result.m_pc;
            thread.m_last_syscall_ra = m_cpu.state().m_gpr[31];
            thread.m_last_syscall_a0 = m_cpu.state().m_gpr[4];
            // HLE may alter other thread states or invoke a scheduler
            // observation while this thread is temporarily RUNNING.
            m_ready_cache_dirty = true;
            const bool handled =
                m_syscall && m_syscall(*this, number);
            m_ready_cache_dirty = true;
            if (handled)
            {
                ++thread.m_handled_syscalls;
            }
            if (!handled)
            {
                m_ready_cache_dirty = true;
                thread.m_status = e_thread_status::faulted;
                result.m_stop =
                    e_kernel_stop::unhandled_syscall;
                result.m_thread_id = id;
                result.m_guest_pc = step_result.m_pc;
                result.m_opcode = step_result.m_opcode;
                thread.m_cpu = m_cpu.state();
                m_current_thread_id = 0u;
                return result;
            }
            if (m_restart_current_thread)
            {
                // start_thread_with_arguments installed a genuine
                // original entry context in the running thread. The
                // syscall continuation may not overwrite that state.
                m_cpu.state() = thread.m_cpu;
                m_restart_current_thread = false;
            }
            else
            {
                m_cpu.resume_syscall();
            }
            ++m_cycles;
            m_hardware.advance_cycles(1u);
            ++result.m_executed;
            --remaining;
        }
        else if (step_result.m_result !=
                 e_step_result::budget_exhausted)
        {
            m_ready_cache_dirty = true;
            thread.m_status = e_thread_status::faulted;
            result.m_stop = stop_for_guest(
                step_result.m_result
            );
            result.m_thread_id = id;
            result.m_guest_pc = step_result.m_pc;
            result.m_opcode = step_result.m_opcode;
            thread.m_cpu = m_cpu.state();
            m_current_thread_id = 0u;
            return result;
        }

        thread.m_cpu = m_cpu.state();
        if (thread.m_status == e_thread_status::dormant &&
            thread.m_delete_on_exit)
        {
            m_allocations.erase(thread.m_stack_base);
            m_ready_cache_dirty = true;
            m_threads.erase(id);
        }
        else if (thread.m_status == e_thread_status::running)
        {
            thread.m_status = e_thread_status::ready;
        }
        m_current_thread_id = 0u;
    }

    return result;
}
} // namespace iop
