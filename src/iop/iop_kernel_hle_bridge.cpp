#include "iop_kernel_hle_bridge.h"
#include "iop_folder_sector_source.h"
#include "iop_sysclib_strings.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <utility>
#include <vector>

namespace iop
{
namespace
{
struct s_export
{
    const char* m_library;
    uint16_t m_ordinal;
    uint32_t m_family;
};

// Explicit HLE coverage for the actual USA SIO2MAN import families.
// The same services are reusable by later PADMAN/MCMAN/LIBSD IRXs.
constexpr s_export k_exports[] = {
    {"loadcore", 4u, 1u}, {"loadcore", 5u, 1u},
    {"loadcore", 6u, 1u}, {"loadcore", 7u, 1u},
    {"intrman", 4u, 2u}, {"intrman", 5u, 2u},
    {"intrman", 6u, 2u}, {"intrman", 7u, 2u},
    {"intrman", 8u, 2u}, {"intrman", 9u, 2u},
    {"intrman", 17u, 2u}, {"intrman", 18u, 2u},
    {"intrman", 23u, 2u},
    {"stdio", 4u, 3u}, {"stdio", 6u, 3u},
    {"stdio", 7u, 3u},
    {"dmacman", 28u, 4u}, {"dmacman", 32u, 4u},
    {"dmacman", 33u, 4u}, {"dmacman", 34u, 4u},
    {"dmacman", 35u, 4u},
    {"thbase", 4u, 5u}, {"thbase", 5u, 5u},
    {"thbase", 6u, 5u}, {"thbase", 7u, 5u},
    {"thbase", 8u, 5u}, {"thbase", 9u, 5u},
    {"thbase", 10u, 5u}, {"thbase", 11u, 5u},
    {"thbase", 14u, 5u},
    {"thbase", 15u, 5u}, {"thbase", 20u, 5u},
    {"thbase", 22u, 5u}, {"thbase", 23u, 5u},
    {"thbase", 24u, 5u}, {"thbase", 25u, 5u},
    {"thbase", 26u, 5u}, {"thbase", 27u, 5u},
    {"thbase", 28u, 5u}, {"thbase", 33u, 5u},
    {"thbase", 34u, 5u}, {"thbase", 35u, 5u},
    {"thbase", 37u, 5u}, {"thbase", 39u, 5u},
    {"thbase", 40u, 5u}, {"thbase", 43u, 5u},
    {"thevent", 4u, 6u}, {"thevent", 5u, 6u},
    {"thevent", 6u, 6u}, {"thevent", 7u, 6u},
    {"thevent", 8u, 6u}, {"thevent", 9u, 6u},
    {"thevent", 10u, 6u}, {"thevent", 11u, 6u},
    {"thevent", 12u, 6u}, {"thevent", 13u, 6u},
    {"thevent", 14u, 6u},
    {"thsemap", 4u, 7u}, {"thsemap", 5u, 7u},
    {"thsemap", 6u, 7u}, {"thsemap", 7u, 7u},
    {"thsemap", 8u, 7u}, {"thsemap", 9u, 7u},
    {"thsemap", 11u, 7u}, {"thsemap", 12u, 7u},
    {"sysclib", 6u, 8u}, {"sysclib", 7u, 8u},
    {"sysclib", 8u, 8u}, {"sysclib", 9u, 8u},
    {"sysclib", 10u, 8u},
    {"sysclib", 11u, 8u}, {"sysclib", 12u, 8u},
    {"sysclib", 13u, 8u}, {"sysclib", 14u, 8u},
    {"sysclib", 15u, 8u}, {"sysclib", 16u, 8u},
    {"sysclib", 17u, 8u}, {"sysclib", 19u, 8u},
    {"sysclib", 20u, 8u}, {"sysclib", 21u, 8u},
    {"sysclib", 22u, 8u}, {"sysclib", 23u, 8u},
    {"sysclib", 24u, 8u}, {"sysclib", 25u, 8u},
    {"sysclib", 26u, 8u}, {"sysclib", 27u, 8u},
    {"sysclib", 28u, 8u}, {"sysclib", 29u, 8u},
    {"sysclib", 30u, 8u}, {"sysclib", 31u, 8u},
    {"sysclib", 32u, 8u}, {"sysclib", 33u, 8u},
    {"sysclib", 34u, 8u}, {"sysclib", 35u, 8u},
    {"sysclib", 36u, 8u}, {"sysclib", 38u, 8u},
    {"sysclib", 40u, 8u}, {"sysclib", 41u, 8u},
    {"sysclib", 42u, 8u},
    {"sysmem", 4u, 9u}, {"sysmem", 5u, 9u},
    {"sysmem", 6u, 9u}, {"sysmem", 7u, 9u},
    {"sysmem", 8u, 9u}, {"sysmem", 9u, 9u},
    {"sysmem", 10u, 9u}, {"sysmem", 14u, 9u},
    {"thmsgbx", 4u, 10u}, {"thmsgbx", 5u, 10u},
    {"thmsgbx", 6u, 10u}, {"thmsgbx", 7u, 10u},
    {"thmsgbx", 8u, 10u}, {"thmsgbx", 9u, 10u},
    {"heaplib", 4u, 11u}, {"heaplib", 5u, 11u},
    {"heaplib", 6u, 11u}, {"heaplib", 7u, 11u},
    {"thfpool", 4u, 12u}, {"thfpool", 5u, 12u},
    {"thfpool", 6u, 12u}, {"thfpool", 7u, 12u},
    {"thfpool", 8u, 12u}, {"thfpool", 9u, 12u},
    {"thvpool", 4u, 13u}, {"thvpool", 5u, 13u},
    {"thvpool", 6u, 13u}, {"thvpool", 7u, 13u},
    {"thvpool", 8u, 13u}, {"thvpool", 9u, 13u},
    {"thvpool", 11u, 13u}, {"thvpool", 12u, 13u},
    {"modload", 10u, 14u}, {"modload", 13u, 14u},
    {"modload", 16u, 14u}, {"modload", 17u, 14u},
    {"modload", 18u, 14u}, {"modload", 22u, 14u},
    {"ioman", 4u, 15u}, {"ioman", 5u, 15u},
    {"ioman", 6u, 15u}, {"ioman", 7u, 15u},
    {"ioman", 8u, 15u}, {"ioman", 16u, 15u},
    {"ioman", 20u, 15u}, {"ioman", 21u, 15u},
    {"secrman", 4u, 16u}, {"secrman", 5u, 16u},
    {"secrman", 6u, 16u},
    {"vblank", 4u, 17u}, {"vblank", 5u, 17u},
    {"vblank", 6u, 17u}, {"vblank", 7u, 17u},
    {"vblank", 8u, 17u}, {"vblank", 9u, 17u}
};

// SYSCLIB byte-memory functions operate on IOP RAM, including KSEG0/1
// aliases. Preflight every span before writing: unsupported MMIO/ROM/TLB
// addresses must not partially mutate guest RAM or fabricated hardware.
bool is_iop_ram_span(uint32_t address, uint32_t length)
{
    const uint32_t region = address & 0xe0000000u;
    if (region != 0u && region != 0x80000000u &&
        region != 0xa0000000u)
    {
        return false;
    }
    const uint32_t physical = address & 0x1fffffffu;
    return physical <= c_cpu::k_ram_size &&
        length <= c_cpu::k_ram_size - physical;
}

bool read_iop_ram(c_cpu& cpu, uint32_t address,
                  uint32_t length, std::vector<uint8_t>& data)
{
    if (!is_iop_ram_span(address, length))
    {
        return false;
    }
    data.resize(length);
    for (uint32_t i = 0u; i < length; ++i)
    {
        if (!cpu.read8(address + i, &data[i]))
        {
            return false;
        }
    }
    return true;
}

bool write_iop_ram(c_cpu& cpu, uint32_t address,
                   const std::vector<uint8_t>& data)
{
    if (!is_iop_ram_span(address, static_cast<uint32_t>(data.size())))
    {
        return false;
    }
    for (uint32_t i = 0u; i < data.size(); ++i)
    {
        if (!cpu.write8(address + i, data[i]))
        {
            return false;
        }
    }
    return true;
}

// PS2SDK sysclib/src/nonstd/ctype.c defines a 128-byte ASCII
// classification table, not the host C locale's ctype bit flags.
constexpr uint32_t k_ctype_guest_table = c_kernel_hle_bridge::k_stub_base - 0x100u;

uint8_t ctype_flags(uint32_t ch)
{
    if (ch == 0x20u)
    {
        return 0x18u;
    }
    if (ch >= 0x09u && ch <= 0x0du)
    {
        return 0x08u;
    }
    if (ch <= 0x1fu || ch == 0x7fu)
    {
        return 0x20u;
    }
    if (ch >= '0' && ch <= '9')
    {
        return 0x04u;
    }
    if (ch >= 'A' && ch <= 'F')
    {
        return 0x41u;
    }
    if (ch >= 'G' && ch <= 'Z')
    {
        return 0x01u;
    }
    if (ch >= 'a' && ch <= 'f')
    {
        return 0x42u;
    }
    if (ch >= 'g' && ch <= 'z')
    {
        return 0x02u;
    }
    return 0x10u; // printable ASCII punctuation
}

bool create_stub(c_cpu& cpu, uint32_t pc, uint32_t service)
{
    const uint32_t words[] = {
        0x3c020000u | (service >> 16u),
        0x34420000u | (service & 0xffffu),
        0x0000000cu,
        0x03e00008u,
        0u
    };
    for (uint32_t i = 0u; i < 5u; ++i)
    {
        if (!cpu.write32(pc + i * 4u, words[i]))
        {
            return false;
        }
    }
    return true;
}
} // namespace

c_kernel_hle_bridge::c_kernel_hle_bridge(c_kernel& kernel)
    : m_kernel(kernel)
{
}

void c_kernel_hle_bridge::attach_disc_source(
    c_folder_sector_source* source)
{
    m_ioman_files.clear();
    m_disc_source = source;
}

bool c_kernel_hle_bridge::install()
{
    if (m_installed || m_kernel.rom0_boot_active())
    {
        return false;
    }
    for (uint32_t i = 0u; i < sizeof(k_exports) / sizeof(k_exports[0]); ++i)
    {
        const s_export& item = k_exports[i];
        const uint32_t pc = k_stub_base + i * 0x20u;
        const uint32_t service = k_service_base +
            item.m_family * 0x100u + item.m_ordinal;
        if (!create_stub(m_kernel.cpu(), pc, service) ||
            !m_kernel.register_library(
                item.m_library, item.m_ordinal, pc))
        {
            return false;
        }
    }
    // Make get_ctype_table (#9) return an actual stable guest pointer.
    // Its reserved 128-byte region is just below the kernel HLE stubs,
    // outside the 0x20000+ relocated IRX module allocation range.
    for (uint32_t index = 0u; index < 128u; ++index)
    {
        if (!m_kernel.cpu().write8(
                k_ctype_guest_table + index, ctype_flags(index)))
        {
            return false;
        }
    }
    m_kernel.bind_interrupts(
        [this](uint32_t line, s_kernel_interrupt_target* target)
        {
            const auto found = m_interrupts.find(line);
            if (found == m_interrupts.end() ||
                found->second.m_handler == 0u || target == nullptr)
            {
                return false;
            }
            target->m_handler = found->second.m_handler;
            target->m_argument = found->second.m_argument;
            return true;
        },
        [this](uint32_t line, uint32_t guest_result)
        {
            if (guest_result == 0u && line < 32u)
            {
                (void)m_kernel.hardware().set_interrupt_mask(line, false);
            }
        }
    );
    m_installed = true;
    return true;
}

void c_kernel_hle_bridge::reset_for_reboot()
{
    m_interrupts.clear();
    m_semaphores.clear();
    m_events.clear();
    m_mailboxes.clear();
    m_heaps.clear();
    m_fixed_pools.clear();
    m_variable_pools.clear();
    m_vblank_start_waiters.clear();
    m_vblank_end_waiters.clear();
    m_vblank_handlers.clear();
    std::fill(std::begin(m_dma_direction),
              std::end(m_dma_direction), 0u);
    m_strtok_next = 0u;
    m_check_kelf_path_callback = 0u;
    m_ioman_files.clear();
    m_unknown_semaphore_wait_ids.clear();
    m_semaphore_trace_events = 0u;
    m_thread_failure_trace_events = 0u;
    m_next_object = 1u;
    m_cpu_interrupts_suspended = false;
    m_installed = false;
}

void c_kernel_hle_bridge::signal_vblank_edge(bool start)
{
    // Called only after the scanout scheduler has delivered an actual
    // field-start edge, or by an explicit VBlank-end event producer.
    // A wait never completes by elapsed host time or an IOP poll.
    std::vector<uint32_t>& waiters =
        start ? m_vblank_start_waiters : m_vblank_end_waiters;
    const uint32_t wait_type = start ? 16u : 17u;
    if (!m_installed)
    {
        waiters.clear();
        return;
    }
    for (const uint32_t id : waiters)
    {
        const s_kernel_thread* thread = m_kernel.thread(id);
        if (thread != nullptr &&
            thread->m_status == e_thread_status::blocked &&
            thread->m_wait_type == wait_type)
        {
            (void)m_kernel.wake_thread_with_result(id, 0u);
        }
    }
    waiters.clear();
    // A registered handler is not an event. Only the actual scanout
    // edge producer can dispatch these guest R3000 callbacks.
    // Snapshot the registration table so a handler can unregister
    // itself without invalidating the producer's iteration.
    const std::vector<s_vblank_handler> handlers = m_vblank_handlers;
    for (const s_vblank_handler& handler : handlers)
    {
        if ((handler.m_edge == 0u) == start)
        {
            (void)m_kernel.start_guest_callback(
                handler.m_handler, handler.m_gp, handler.m_argument,
                handler.m_origin);
        }
    }
}

bool c_kernel_hle_bridge::read_argument(
    uint32_t index, uint32_t* value
) const
{
    if (value == nullptr)
    {
        return false;
    }
    const s_cpu_state& cpu = m_kernel.cpu().state();
    if (index < 4u)
    {
        *value = cpu.m_gpr[4u + index];
        return true;
    }
    const uint64_t address =
        static_cast<uint64_t>(cpu.m_gpr[29]) + 16u +
        static_cast<uint64_t>(index - 4u) * 4u;
    return address <= std::numeric_limits<uint32_t>::max() &&
           m_kernel.cpu().read32(static_cast<uint32_t>(address), value);
}

bool c_kernel_hle_bridge::read_string(
    uint32_t pointer, uint32_t maximum, std::string* output
) const
{
    if (output == nullptr || maximum == 0u ||
        maximum > 65536u)
    {
        return false;
    }
    std::string text;
    for (uint32_t i = 0u; i < maximum; ++i)
    {
        uint8_t ch = 0u;
        if (pointer > std::numeric_limits<uint32_t>::max() - i ||
            !m_kernel.cpu().read8(pointer + i, &ch))
        {
            return false;
        }
        if (ch == 0u)
        {
            *output = std::move(text);
            return true;
        }
        text.push_back(static_cast<char>(ch));
    }
    return false;
}

bool c_kernel_hle_bridge::write_console(
    const std::string& content
)
{
    for (unsigned char ch : content)
    {
        if (!m_kernel.cpu().write8(c_hw::k_debug_console, ch))
        {
            return false;
        }
    }
    return true;
}

bool c_kernel_hle_bridge::register_loaded_guest_library(uint32_t table)
{
    // RegisterLibraryEntries, LOADCORE export #6. The real IRX linker
    // already validates and publishes exports during module load.
    // Re-registration after a real release is also supported.
    return m_kernel.set_module_library_registration(table, true);
}

bool c_kernel_hle_bridge::match_event(
    const s_event& event, uint32_t bits, uint32_t mode
) const
{
    return (mode & 1u) != 0u
        ? (event.m_bits & bits) != 0u
        : (event.m_bits & bits) == bits;
}

bool c_kernel_hle_bridge::take_event(
    s_event& event, uint32_t bits, uint32_t mode,
    uint32_t output
)
{
    if (output != 0u &&
        !m_kernel.cpu().write32(output, event.m_bits))
    {
        return false;
    }
    if ((mode & 0x10u) != 0u)
    {
        event.m_bits &= ~bits;
    }
    if ((mode & 0x20u) != 0u)
    {
        event.m_bits = 0u;
    }
    return true;
}

uint32_t c_kernel_hle_bridge::variable_pool_take(
    s_variable_pool& pool, uint32_t requested
)
{
    uint64_t size = (static_cast<uint64_t>(requested) + 7u) & ~uint64_t(7u);
    if (size == 0u)
    {
        size = 8u;
    }
    if (size > pool.m_size)
    {
        return 0u;
    }
    uint64_t candidate = pool.m_base;
    const uint64_t end = candidate + pool.m_size;
    for (const auto& block : pool.m_blocks)
    {
        if (candidate + size <= block.first)
        {
            break;
        }
        candidate = std::max(
            candidate, static_cast<uint64_t>(block.first) + block.second
        );
    }
    if (candidate + size > end ||
        candidate > std::numeric_limits<uint32_t>::max())
    {
        return 0u;
    }
    const uint32_t address = static_cast<uint32_t>(candidate);
    pool.m_blocks.emplace(address, static_cast<uint32_t>(size));
    return address;
}

bool c_kernel_hle_bridge::handle_fixed_pool(uint32_t ordinal)
{
    s_cpu_state& cpu = m_kernel.cpu().state();
    if (ordinal == 4u)
    {
        const uint32_t descriptor = cpu.m_gpr[4];
        uint32_t attr = 0u, option = 0u, block = 0u, count = 0u;
        if (!is_iop_ram_span(descriptor, 16u) ||
            !m_kernel.cpu().read32(descriptor, &attr) ||
            !m_kernel.cpu().read32(descriptor + 4u, &option) ||
            !m_kernel.cpu().read32(descriptor + 8u, &block) ||
            !m_kernel.cpu().read32(descriptor + 12u, &count))
        {
            return false;
        }
        const uint64_t block_size =
            (static_cast<uint64_t>(block) + 3u) & ~uint64_t(3u);
        const uint64_t allocation = block_size * count;
        if (block_size == 0u || count == 0u ||
            block_size > std::numeric_limits<uint32_t>::max() ||
            allocation > c_cpu::k_ram_size)
        {
            return false;
        }
        const uint32_t base =
            m_kernel.allocate_heap(static_cast<uint32_t>(allocation));
        if (base == 0u)
        {
            return false;
        }
        s_fixed_pool pool{};
        pool.m_attr = attr;
        pool.m_option = option;
        pool.m_base = base;
        pool.m_block_size = static_cast<uint32_t>(block_size);
        pool.m_block_count = count;
        for (uint32_t i = count; i-- > 0u;)
        {
            pool.m_free.push_back(
                base + i * static_cast<uint32_t>(block_size));
        }
        const uint32_t id = m_next_object++;
        m_fixed_pools.emplace(id, std::move(pool));
        cpu.m_gpr[2] = id;
        return true;
    }
    const uint32_t id = cpu.m_gpr[4];
    auto found = m_fixed_pools.find(id);
    if (found == m_fixed_pools.end())
    {
        // PS2 IOP kernel: unknown FPL ID is an API error, not an
        // unhandled host syscall. Preserve unsupported ordinals.
        if (ordinal >= 5u && ordinal <= 9u)
        {
            cpu.m_gpr[2] = static_cast<uint32_t>(-412);
            return true;
        }
        return false;
    }
    s_fixed_pool& pool = found->second;
    switch (ordinal)
    {
    case 5u:
        for (uint32_t waiter : pool.m_waiters)
        {
            const s_kernel_thread* thread = m_kernel.thread(waiter);
            if (thread == nullptr ||
                thread->m_status != e_thread_status::blocked)
            {
                return false;
            }
        }
        if (!m_kernel.owns_heap_span(pool.m_base, 1u))
        {
            return false;
        }
        for (uint32_t waiter : pool.m_waiters)
        {
            if (!m_kernel.wake_thread_with_result(
                    waiter, static_cast<uint32_t>(-425)))
            {
                return false;
            }
        }
        if (!m_kernel.free_heap(pool.m_base))
        {
            return false;
        }
        m_fixed_pools.erase(found);
        cpu.m_gpr[2] = 0u;
        return true;
    case 6u:
    case 7u:
    case 8u:
        if (pool.m_free.empty())
        {
            if (ordinal != 6u)
            {
                cpu.m_gpr[2] = 0u;
                return true;
            }
            if (!m_kernel.block_current(6u))
            {
                return false;
            }
            pool.m_waiters.push_back(m_kernel.current_thread_id());
            return true;
        }
        cpu.m_gpr[2] = pool.m_free.back();
        pool.m_in_use.insert(cpu.m_gpr[2]);
        pool.m_free.pop_back();
        return true;
    case 9u:
    {
        const uint32_t address = cpu.m_gpr[5];
        if (pool.m_in_use.count(address) == 0u)
        {
            return false;
        }
        if (!pool.m_waiters.empty())
        {
            const uint32_t waiter = pool.m_waiters.front();
            const s_kernel_thread* thread = m_kernel.thread(waiter);
            if (thread == nullptr ||
                thread->m_status != e_thread_status::blocked ||
                !m_kernel.wake_thread_with_result(waiter, address))
            {
                return false;
            }
            pool.m_waiters.pop_front();
        }
        else
        {
            pool.m_in_use.erase(address);
            pool.m_free.push_back(address);
        }
        cpu.m_gpr[2] = 0u;
        return true;
    }
    default:
        return false;
    }
}

bool c_kernel_hle_bridge::handle_variable_pool(uint32_t ordinal)
{
    s_cpu_state& cpu = m_kernel.cpu().state();
    if (ordinal == 4u)
    {
        const uint32_t descriptor = cpu.m_gpr[4];
        uint32_t attr = 0u, option = 0u, bytes = 0u;
        if (!is_iop_ram_span(descriptor, 12u) ||
            !m_kernel.cpu().read32(descriptor, &attr) ||
            !m_kernel.cpu().read32(descriptor + 4u, &option) ||
            !m_kernel.cpu().read32(descriptor + 8u, &bytes))
        {
            return false;
        }
        const uint64_t size =
            (static_cast<uint64_t>(bytes) + 7u) & ~uint64_t(7u);
        if (size == 0u || size > c_cpu::k_ram_size)
        {
            return false;
        }
        const uint32_t base =
            m_kernel.allocate_heap(static_cast<uint32_t>(size));
        if (base == 0u)
        {
            return false;
        }
        const uint32_t id = m_next_object++;
        s_variable_pool pool{};
        pool.m_attr = attr;
        pool.m_option = option;
        pool.m_base = base;
        pool.m_size = static_cast<uint32_t>(size);
        m_variable_pools.emplace(id, std::move(pool));
        cpu.m_gpr[2] = id;
        return true;
    }
    const uint32_t id = cpu.m_gpr[4];
    auto found = m_variable_pools.find(id);
    if (found == m_variable_pools.end())
    {
        if ((ordinal >= 5u && ordinal <= 9u) ||
            ordinal == 11u || ordinal == 12u)
        {
            cpu.m_gpr[2] = static_cast<uint32_t>(-411);
            return true;
        }
        return false;
    }
    s_variable_pool& pool = found->second;
    switch (ordinal)
    {
    case 5u:
        for (const s_variable_pool_waiter& waiter : pool.m_waiters)
        {
            const s_kernel_thread* thread =
                m_kernel.thread(waiter.m_thread);
            if (thread == nullptr ||
                thread->m_status != e_thread_status::blocked)
            {
                return false;
            }
        }
        if (!m_kernel.owns_heap_span(pool.m_base, 1u))
        {
            return false;
        }
        for (const s_variable_pool_waiter& waiter : pool.m_waiters)
        {
            if (!m_kernel.wake_thread_with_result(
                    waiter.m_thread, static_cast<uint32_t>(-425)))
            {
                return false;
            }
        }
        if (!m_kernel.free_heap(pool.m_base))
        {
            return false;
        }
        m_variable_pools.erase(found);
        cpu.m_gpr[2] = 0u;
        return true;
    case 6u:
    case 7u:
    case 8u:
    {
        const uint32_t address = variable_pool_take(
            pool, cpu.m_gpr[5]);
        if (address != 0u)
        {
            cpu.m_gpr[2] = address;
            return true;
        }
        if (ordinal != 6u)
        {
            cpu.m_gpr[2] = static_cast<uint32_t>(-400);
            return true;
        }
        if (!m_kernel.block_current(7u))
        {
            return false;
        }
        pool.m_waiters.push_back({
            m_kernel.current_thread_id(), cpu.m_gpr[5]
        });
        return true;
    }
    case 9u:
    {
        if (pool.m_blocks.erase(cpu.m_gpr[5]) == 0u)
        {
            return false;
        }
        while (!pool.m_waiters.empty())
        {
            const s_variable_pool_waiter waiter = pool.m_waiters.front();
            const s_kernel_thread* thread =
                m_kernel.thread(waiter.m_thread);
            if (thread == nullptr ||
                thread->m_status != e_thread_status::blocked)
            {
                return false;
            }
            const uint32_t assigned =
                variable_pool_take(pool, waiter.m_size);
            if (assigned == 0u)
            {
                break;
            }
            if (!m_kernel.wake_thread_with_result(
                    waiter.m_thread, assigned))
            {
                pool.m_blocks.erase(assigned);
                return false;
            }
            pool.m_waiters.pop_front();
        }
        cpu.m_gpr[2] = 0u;
        return true;
    }
    case 11u:
    case 12u:
    {
        const uint32_t output = cpu.m_gpr[5];
        if (!is_iop_ram_span(output, 20u))
        {
            return false;
        }
        uint32_t used = 0u;
        for (const auto& item : pool.m_blocks)
        {
            used += item.second;
        }
        const uint32_t words[5] = {
            pool.m_attr, pool.m_option, pool.m_size,
            pool.m_size - used,
            static_cast<uint32_t>(pool.m_waiters.size())
        };
        for (uint32_t i = 0u; i < 5u; ++i)
        {
            if (!m_kernel.cpu().write32(output + i * 4u, words[i]))
            {
                return false;
            }
        }
        cpu.m_gpr[2] = 0u;
        return true;
    }
    default:
        return false;
    }
}

bool c_kernel_hle_bridge::handle_heaplib(uint32_t ordinal)
{
    s_cpu_state& cpu = m_kernel.cpu().state();
    const uint32_t id = cpu.m_gpr[4];
    if (ordinal == 4u)
    {
        const uint32_t created = m_kernel.allocate_heap(16u);
        if (created == 0u)
        {
            return false;
        }
        m_heaps.emplace(created, std::set<uint32_t>{});
        cpu.m_gpr[2] = created;
        return true;
    }
    auto found = m_heaps.find(id);
    if (found == m_heaps.end())
    {
        return false;
    }
    switch (ordinal)
    {
    case 5u:
        for (uint32_t address : found->second)
        {
            if (!m_kernel.owns_heap_span(address, 1u))
            {
                return false;
            }
        }
        for (uint32_t address : found->second)
        {
            if (!m_kernel.free_heap(address))
            {
                return false;
            }
        }
        if (!m_kernel.free_heap(id))
        {
            return false;
        }
        m_heaps.erase(found);
        cpu.m_gpr[2] = 0u;
        return true;
    case 6u:
    {
        const uint32_t address = m_kernel.allocate_heap(cpu.m_gpr[5]);
        if (address == 0u)
        {
            return false;
        }
        found->second.insert(address);
        cpu.m_gpr[2] = address;
        return true;
    }
    case 7u:
        if (found->second.count(cpu.m_gpr[5]) == 0u ||
            !m_kernel.free_heap(cpu.m_gpr[5]))
        {
            return false;
        }
        found->second.erase(cpu.m_gpr[5]);
        cpu.m_gpr[2] = 0u;
        return true;
    default:
        return false;
    }
}

bool c_kernel_hle_bridge::handle_thmsgbx(uint32_t ordinal)
{
    s_cpu_state& cpu = m_kernel.cpu().state();
    if (ordinal == 4u)
    {
        const uint32_t descriptor = cpu.m_gpr[4];
        uint32_t attr = 0u;
        uint32_t option = 0u;
        if (!is_iop_ram_span(descriptor, 8u) ||
            !m_kernel.cpu().read32(descriptor, &attr) ||
            !m_kernel.cpu().read32(descriptor + 4u, &option))
        {
            return false;
        }
        const uint32_t created = m_next_object++;
        m_mailboxes.emplace(created, s_mailbox{attr, option, {}, {}});
        cpu.m_gpr[2] = created;
        return true;
    }
    const uint32_t id = ordinal == 8u || ordinal == 9u
        ? cpu.m_gpr[5] : cpu.m_gpr[4];
    auto found = m_mailboxes.find(id);
    if (found == m_mailboxes.end())
    {
        if (ordinal >= 5u && ordinal <= 9u)
        {
            cpu.m_gpr[2] = static_cast<uint32_t>(-410);
            return true;
        }
        return false;
    }
    s_mailbox& mailbox = found->second;
    switch (ordinal)
    {
    case 5u:
        for (const s_mailbox_waiter& waiter : mailbox.m_waiters)
        {
            const s_kernel_thread* thread =
                m_kernel.thread(waiter.m_thread);
            if (thread == nullptr ||
                thread->m_status != e_thread_status::blocked)
            {
                return false;
            }
        }
        for (const s_mailbox_waiter& waiter : mailbox.m_waiters)
        {
            if (!m_kernel.wake_thread_with_result(
                    waiter.m_thread, static_cast<uint32_t>(-425)))
            {
                return false;
            }
        }
        m_mailboxes.erase(found);
        cpu.m_gpr[2] = 0u;
        return true;
    case 6u:
    case 7u:
    {
        const uint32_t message = cpu.m_gpr[5];
        uint32_t header = 0u;
        if (!is_iop_ram_span(message, 4u) ||
            !m_kernel.cpu().read32(message, &header))
        {
            return false;
        }
        if (!mailbox.m_waiters.empty())
        {
            const s_mailbox_waiter& waiter = mailbox.m_waiters.front();
            const s_kernel_thread* thread =
                m_kernel.thread(waiter.m_thread);
            if (thread == nullptr ||
                thread->m_status != e_thread_status::blocked ||
                !m_kernel.cpu().write32(waiter.m_output, message) ||
                !m_kernel.wake_thread_with_result(
                    waiter.m_thread, 0u))
            {
                return false;
            }
            mailbox.m_waiters.pop_front();
        }
        else
        {
            mailbox.m_messages.push_back(message);
        }
        cpu.m_gpr[2] = 0u;
        return true;
    }
    case 8u:
    case 9u:
    {
        const uint32_t output = cpu.m_gpr[4];
        if (!is_iop_ram_span(output, 4u))
        {
            return false;
        }
        if (mailbox.m_messages.empty())
        {
            if (ordinal == 9u)
            {
                cpu.m_gpr[2] = static_cast<uint32_t>(-424);
                return true;
            }
            if (!m_kernel.block_current(5u))
            {
                return false;
            }
            mailbox.m_waiters.push_back({
                m_kernel.current_thread_id(), output
            });
        }
        else
        {
            if (!m_kernel.cpu().write32(
                    output, mailbox.m_messages.front()))
            {
                return false;
            }
            mailbox.m_messages.pop_front();
        }
        cpu.m_gpr[2] = 0u;
        return true;
    }
    default:
        return false;
    }
}

bool c_kernel_hle_bridge::handle_sysmem(uint32_t ordinal)
{
    s_cpu_state& cpu = m_kernel.cpu().state();
    switch (ordinal)
    {
    case 4u:
    {
        // AllocSysMemory(ALLOC_FIRST, size, nullptr). Fixed-address
        // modes require separate tested allocator semantics.
        if (cpu.m_gpr[4] != 0u || cpu.m_gpr[6] != 0u)
        {
            return false;
        }
        const uint32_t address = m_kernel.allocate_heap(cpu.m_gpr[5]);
        if (address == 0u)
        {
            return false;
        }
        cpu.m_gpr[2] = address;
        return true;
    }
    case 5u:
        if (!m_kernel.free_heap(cpu.m_gpr[4]))
        {
            return false;
        }
        cpu.m_gpr[2] = 0u;
        return true;
    case 6u:
        cpu.m_gpr[2] = c_cpu::k_ram_size;
        return true;
    case 7u:
        cpu.m_gpr[2] = m_kernel.heap_max_free();
        return true;
    case 8u:
        cpu.m_gpr[2] = m_kernel.heap_total_free();
        return true;
    case 9u:
    case 10u:
    {
        uint32_t base = 0u;
        uint32_t size = 0u;
        if (!m_kernel.query_heap_block(cpu.m_gpr[4], &base, &size))
        {
            cpu.m_gpr[2] = 0u;
            return true;
        }
        cpu.m_gpr[2] = ordinal == 9u ? base : size;
        return true;
    }
    case 14u:
        // Kprintf uses the same bounded guest formatter as stdio.
        if (!handle_stdio(4u))
        {
            return false;
        }
        cpu.m_gpr[2] = 0u;
        return true;
    default:
        return false;
    }
}

bool c_kernel_hle_bridge::handle_loadcore(uint32_t ordinal)
{
    const uint32_t table = m_kernel.cpu().state().m_gpr[4];
    // R3000 code and data use the same coherent RAM backing in this
    // HLE interpreter; there is no simulated stale I/D cache to flush.
    if (ordinal == 4u || ordinal == 5u)
    {
        m_kernel.cpu().state().m_gpr[2] = 0u;
        return true;
    }
    bool accepted = false;
    if (ordinal == 6u)
    {
        accepted = register_loaded_guest_library(table);
    }
    else if (ordinal == 7u)
    {
        // ReleaseLibraryEntries unregisters only the actual loaded
        // module's matching exports. Invalid tables do not mutate the
        // registry, and HLE kernel providers cannot be released.
        accepted = m_kernel.set_module_library_registration(table, false);
    }
    if (!accepted)
    {
        return false;
    }
    m_kernel.cpu().state().m_gpr[2] = 0u;
    return true;
}

bool c_kernel_hle_bridge::handle_intrman(uint32_t ordinal)
{
    s_cpu_state& cpu = m_kernel.cpu().state();
    const uint32_t line = cpu.m_gpr[4];
    if (ordinal == 23u)
    {
        // Real guest INTRMAN handler execution has its own context.
        cpu.m_gpr[2] = m_kernel.in_interrupt_context() ? 1u : 0u;
        return true;
    }
    if (ordinal == 8u || ordinal == 9u)
    {
        const bool disabled = ordinal == 8u;
        // The reference INTRMAN saves CPU suspension in the thread
        // context; it does not overwrite the hardware I_CTRL latch.
        m_cpu_interrupts_suspended = disabled;
        m_kernel.set_interrupts_suspended(disabled);
        cpu.m_gpr[2] = 0u;
        return true;
    }
    if (ordinal == 17u)
    {
        const bool suspended = m_kernel.interrupts_suspended();
        if (line != 0u &&
            !m_kernel.cpu().write32(line, suspended ? 0u : 1u))
        {
            return false;
        }
        m_cpu_interrupts_suspended = true;
        m_kernel.set_interrupts_suspended(true);
        cpu.m_gpr[2] = suspended ?
            static_cast<uint32_t>(-102) : 0u;
        return true;
    }
    if (ordinal == 18u)
    {
        m_cpu_interrupts_suspended = line == 0u;
        m_kernel.set_interrupts_suspended(m_cpu_interrupts_suspended);
        cpu.m_gpr[2] = 0u;
        return true;
    }
    // The IOP INTRMAN numbers DMA channels separately from the
    // 32-bit I_STAT/I_MASK register. LIBSD DisableIntr(0x24) targets
    // DMA channel 4 (DICR bit 20), not an invalid IOP IRQ number.
    if (!c_hw::supports_interrupt_line(line))
    {
        return false;
    }
    switch (ordinal)
    {
    case 4u:
        if (m_interrupts.find(line) != m_interrupts.end() ||
            (cpu.m_gpr[6] & 3u) != 0u ||
            (cpu.m_gpr[6] & 0x1fffffffu) >= c_cpu::k_ram_size)
        {
            return false;
        }
        m_interrupts.emplace(
            line, s_interrupt{cpu.m_gpr[5],
                              cpu.m_gpr[6], cpu.m_gpr[7]});
        break;
    case 5u:
        // Reference ReleaseIntrHandler is idempotent for valid lines.
        m_interrupts.erase(line);
        break;
    case 6u:
    case 7u:
    {
        // EnableIntr and DisableIntr operate on the kernel mask
        // even before a handler is registered. Do not require a
        // fictitious handler or shift by an out-of-range IRQ number.
        if (ordinal == 7u && cpu.m_gpr[5] != 0u &&
            !m_kernel.cpu().write32(cpu.m_gpr[5], line))
        {
            return false;
        }
        if (!m_kernel.hardware().set_interrupt_mask(
                line, ordinal == 6u))
        {
            return false;
        }
        break;
    }
    default:
        return false;
    }
    cpu.m_gpr[2] = 0u;
    return true;
}

bool c_kernel_hle_bridge::handle_stdio(uint32_t ordinal)
{
    s_cpu_state& cpu = m_kernel.cpu().state();
    if (ordinal == 6u)
    {
        const char ch = static_cast<char>(cpu.m_gpr[4] & 0xffu);
        if (!write_console(std::string(1u, ch)))
        {
            return false;
        }
        cpu.m_gpr[2] = static_cast<uint8_t>(ch);
        return true;
    }
    if (ordinal == 7u)
    {
        std::string value;
        if (!read_string(cpu.m_gpr[4], 4096u, &value) ||
            !write_console(value + "\n"))
        {
            return false;
        }
        cpu.m_gpr[2] = 0u;
        return true;
    }
    if (ordinal != 4u)
    {
        return false;
    }
    std::string output;
    if (!render_stdio_format(m_kernel.cpu(), &output))
    {
        // An unsupported guest format must not silently appear as a
        // successful print. Record enough bounded context to locate the
        // original module's call without exposing its entire image.
        std::string format;
        const bool valid_format =
            read_string(cpu.m_gpr[4], 4096u, &format);
        std::fprintf(stderr,
            "IOP HLE stdio printf rejected: thread=%u "
            "format_addr=0x%08x format=%.160s "
            "a1=0x%08x a2=0x%08x a3=0x%08x "
            "sp=0x%08x ra=0x%08x cycles=%llu\n",
            m_kernel.current_thread_id(), cpu.m_gpr[4],
            valid_format ? format.c_str() : "(unreadable)",
            cpu.m_gpr[5], cpu.m_gpr[6], cpu.m_gpr[7],
            cpu.m_gpr[29], cpu.m_gpr[31],
            static_cast<unsigned long long>(m_kernel.cycles()));
        return false;
    }
    if (!write_console(output))
    {
        return false;
    }
    cpu.m_gpr[2] = static_cast<uint32_t>(output.size());
    return true;
}

bool c_kernel_hle_bridge::handle_dmacman(uint32_t ordinal)
{
    s_cpu_state& cpu = m_kernel.cpu().state();
    const uint32_t channel = cpu.m_gpr[4];
    // The only modeled dmacman channels here are SIO2 DMA11/12.
    // No byte movement/interrupt is synthesized by MMIO registers.
    if (channel != 11u && channel != 12u)
    {
        return false;
    }
    const uint32_t base = channel == 11u
        ? c_hw::k_sio2_dma_rx_base
        : c_hw::k_sio2_dma_tx_base;
    switch (ordinal)
    {
    case 28u:
    {
        uint32_t direction = 0u;
        if (!read_argument(4u, &direction) ||
            !m_kernel.hardware().write(base, 4u, cpu.m_gpr[5]) ||
            !m_kernel.hardware().write(
                base + 4u, 4u,
                (cpu.m_gpr[7] << 16u) | (cpu.m_gpr[6] & 0xffffu)))
        {
            return false;
        }
        m_dma_direction[channel] = direction;
        break;
    }
    case 32u:
        if (!m_kernel.hardware().write(
                base + 8u, 4u,
                0x01000200u | (m_dma_direction[channel] & 1u)))
        {
            return false;
        }
        break;
    case 33u:
    case 34u:
    case 35u:
    {
        const uint32_t shift = (channel - 7u) * 4u;
        uint32_t pcr = 0u;
        if (!m_kernel.hardware().read(
                c_hw::k_dma_pcr2, 4u, &pcr))
        {
            return false;
        }
        const uint32_t mask = 0x0fu << shift;
        if (ordinal == 33u)
        {
            pcr = (pcr & ~mask) |
                ((cpu.m_gpr[5] & 7u) << shift) |
                (pcr & (8u << shift));
        }
        else if (ordinal == 34u)
        {
            pcr |= 8u << shift;
        }
        else
        {
            pcr &= ~(8u << shift);
        }
        if (!m_kernel.hardware().write(c_hw::k_dma_pcr2, 4u, pcr))
        {
            return false;
        }
        break;
    }
    default:
        return false;
    }
    cpu.m_gpr[2] = 1u;
    return true;
}

bool c_kernel_hle_bridge::handle_thbase(uint32_t ordinal)
{
    s_cpu_state& cpu = m_kernel.cpu().state();
    switch (ordinal)
    {
    case 4u:
    {
        const uint32_t descriptor = cpu.m_gpr[4];
        uint32_t attr = 0u;
        uint32_t option = 0u;
        uint32_t entry = 0u;
        uint32_t stack = 0u;
        uint32_t priority = 0u;
        if (!is_iop_ram_span(descriptor, 20u) ||
            !m_kernel.cpu().read32(descriptor, &attr) ||
            !m_kernel.cpu().read32(descriptor + 4u, &option) ||
            !m_kernel.cpu().read32(descriptor + 8u, &entry) ||
            !m_kernel.cpu().read32(descriptor + 12u, &stack) ||
            !m_kernel.cpu().read32(descriptor + 16u, &priority))
        {
            return false;
        }
        // A valid CreateThread request can fail because its stack no
        // longer fits physical IOP RAM. Return a genuine negative
        // kernel error rather than faulting the caller's guest syscall.
        // Never publish a synthetic thread ID or an unusable stack.
        const uint32_t created =
            m_kernel.create_thread(
                entry, cpu.m_gpr[28], priority, stack,
                0u, 0u, attr, option);
        if (created == 0u)
        {
            const uint32_t physical = entry & 0x1fffffffu;
            const bool parameters_valid =
                entry != 0u && (entry & 3u) == 0u &&
                physical < c_cpu::k_ram_size && stack >= 64u;
            cpu.m_gpr[2] = static_cast<uint32_t>(
                parameters_valid ? -400 : -201);
            if (m_thread_failure_trace_events < 12u)
            {
                std::fprintf(stderr,
                    "IOP HLE CreateThread rejected: thread=%u "
                    "entry=0x%08x stack=%u priority=%u "
                    "reason=%s max_free=%u total_free=%u "
                    "cycles=%llu\n",
                    m_kernel.current_thread_id(), entry, stack,
                    priority, parameters_valid
                        ? "stack_allocation_failed"
                        : "invalid_thread_parameters",
                    m_kernel.heap_max_free(), m_kernel.heap_total_free(),
                    static_cast<unsigned long long>(m_kernel.cycles()));
                ++m_thread_failure_trace_events;
            }
            return true;
        }
        cpu.m_gpr[2] = created;
        return true;
    }
    case 5u:
        if (!m_kernel.delete_thread(cpu.m_gpr[4]))
        {
            return false;
        }
        cpu.m_gpr[2] = 0u;
        return true;
    case 6u:
    case 7u:
    {
        const uint32_t requested_id = cpu.m_gpr[4];
        // Original SDBZ reference: kernel::thread(0) returns current().
        // This is an actual self-restart request, not an invalid ID.
        const uint32_t id = requested_id == 0u
            ? m_kernel.current_thread_id() : requested_id;
        const s_kernel_thread* thread = m_kernel.thread(id);
        const bool self_restart =
            id != 0u && id == m_kernel.current_thread_id() &&
            thread != nullptr &&
            thread->m_status == e_thread_status::running;
        if (thread == nullptr ||
            (thread->m_status != e_thread_status::dormant &&
             !self_restart))
        {
            // Unknown IDs fail with -407; other non-dormant threads
            // cannot be started unless they are the current caller.
            cpu.m_gpr[2] = static_cast<uint32_t>(
                thread == nullptr ? -407 : -414);
            if (m_thread_failure_trace_events < 12u)
            {
                std::fprintf(stderr,
                    "IOP HLE StartThread rejected: ordinal=%u "
                    "caller=%u target=%u reason=%s result=%d "
                    "cycles=%llu\n",
                    ordinal, m_kernel.current_thread_id(),
                    requested_id,
                    thread == nullptr
                        ? "unknown_thread" : "thread_not_dormant",
                    static_cast<int32_t>(cpu.m_gpr[2]),
                    static_cast<unsigned long long>(m_kernel.cycles()));
                ++m_thread_failure_trace_events;
            }
            return true;
        }
        const bool started = ordinal == 6u
            ? m_kernel.start_thread_with_argument(id, cpu.m_gpr[5])
            : m_kernel.start_thread_with_arguments(
                id, cpu.m_gpr[5], cpu.m_gpr[6]);
        if (!started)
        {
            // Do not grant a successful start without a scheduled
            // original R3000 thread.
            return false;
        }
        cpu.m_gpr[2] = 0u;
        return true;
    }
    case 8u:
    case 9u:
        if (!m_kernel.exit_current_thread(ordinal == 9u))
        {
            return false;
        }
        cpu.m_gpr[2] = 0u;
        return true;
    case 10u:
    case 11u:
    {
        const uint32_t id = cpu.m_gpr[4];
        const s_kernel_thread* target = m_kernel.thread(id);
        if (id == m_kernel.current_thread_id() ||
            target == nullptr ||
            target->m_status == e_thread_status::faulted)
        {
            return false;
        }
        // An already blocked waiter must not retain a dangling ID.
        for (auto& item : m_events)
        {
            auto& waiters = item.second.m_waiters;
            waiters.erase(std::remove_if(
                waiters.begin(), waiters.end(),
                [id](const s_event_waiter& waiter)
                {
                    return waiter.m_thread == id;
                }), waiters.end());
        }
        for (auto& item : m_semaphores)
        {
            auto& waiters = item.second.m_waiters;
            waiters.erase(std::remove(
                waiters.begin(), waiters.end(), id), waiters.end());
        }
        for (auto& item : m_mailboxes)
        {
            auto& waiters = item.second.m_waiters;
            waiters.erase(std::remove_if(
                waiters.begin(), waiters.end(),
                [id](const s_mailbox_waiter& waiter)
                {
                    return waiter.m_thread == id;
                }), waiters.end());
        }
        for (auto& item : m_fixed_pools)
        {
            auto& waiters = item.second.m_waiters;
            waiters.erase(std::remove(
                waiters.begin(), waiters.end(), id), waiters.end());
        }
        for (auto& item : m_variable_pools)
        {
            auto& waiters = item.second.m_waiters;
            waiters.erase(std::remove_if(
                waiters.begin(), waiters.end(),
                [id](const s_variable_pool_waiter& waiter)
                {
                    return waiter.m_thread == id;
                }), waiters.end());
        }
        if (!m_kernel.stop_thread(id))
        {
            return false;
        }
        cpu.m_gpr[2] = 0u;
        return true;
    }
    case 14u:
    case 15u:
        if (!m_kernel.change_thread_priority(
                cpu.m_gpr[4], cpu.m_gpr[5]))
        {
            return false;
        }
        cpu.m_gpr[2] = 0u;
        return true;
    case 20u:
        cpu.m_gpr[2] = m_kernel.current_thread_id();
        return true;
    case 22u:
    case 23u:
    {
        const uint32_t id = cpu.m_gpr[4] != 0u
            ? cpu.m_gpr[4] : m_kernel.current_thread_id();
        const s_kernel_thread* thread = m_kernel.thread(id);
        const uint32_t output = cpu.m_gpr[5];
        if (thread == nullptr ||
            thread->m_status == e_thread_status::faulted ||
            !is_iop_ram_span(output, 80u))
        {
            return false;
        }
        uint32_t status = 0u;
        switch (thread->m_status)
        {
        case e_thread_status::running: status = 1u; break;
        case e_thread_status::ready: status = 2u; break;
        case e_thread_status::blocked: status = 4u; break;
        case e_thread_status::dormant: status = 16u; break;
        default: return false;
        }
        const uint32_t words[20] = {
            thread->m_attr, thread->m_option, status,
            thread->m_entry, thread->m_stack_base,
            thread->m_stack_size, thread->m_gp,
            thread->m_initial_priority, thread->m_priority,
            thread->m_wait_type, 0u, thread->m_wakeup_count,
            0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u
        };
        for (uint32_t i = 0u; i < 20u; ++i)
        {
            if (!m_kernel.cpu().write32(output + i * 4u, words[i]))
            {
                return false;
            }
        }
        cpu.m_gpr[2] = 0u;
        return true;
    }
    case 24u:
        if (!m_kernel.sleep_thread())
        {
            return false;
        }
        cpu.m_gpr[2] = 0u;
        return true;
    case 25u:
    case 26u:
        if (!m_kernel.wakeup_thread(cpu.m_gpr[4]))
        {
            return false;
        }
        cpu.m_gpr[2] = 0u;
        return true;
    case 27u:
    case 28u:
    {
        uint32_t previous = 0u;
        if (!m_kernel.cancel_wakeup_thread(
                cpu.m_gpr[4], &previous))
        {
            return false;
        }
        cpu.m_gpr[2] = previous;
        return true;
    }
    case 33u:
    {
        // PS2SDK threadman: 0x1200 / 125 system-clock ticks per
        // microsecond. The scheduler clock is guest-instruction based;
        // this is a virtual delay, NOT calibrated IOP hardware time.
        const uint64_t ticks =
            static_cast<uint64_t>(cpu.m_gpr[4]) * 0x1200u / 125u;
        if (!m_kernel.sleep_current(std::max<uint64_t>(1u, ticks)))
        {
            return false;
        }
        cpu.m_gpr[2] = 0u;
        return true;
    }
    case 34u:
    {
        const uint32_t output = cpu.m_gpr[4];
        const uint64_t ticks = m_kernel.cycles();
        if (!is_iop_ram_span(output, 8u) ||
            !m_kernel.cpu().write32(output,
                                   static_cast<uint32_t>(ticks)) ||
            !m_kernel.cpu().write32(output + 4u,
                                   static_cast<uint32_t>(ticks >> 32u)))
        {
            return false;
        }
        cpu.m_gpr[2] = 0u;
        return true;
    }
    case 35u:
    {
        const uint32_t address = cpu.m_gpr[4];
        uint32_t low = 0u;
        uint32_t high = 0u;
        if (!is_iop_ram_span(address, 8u) ||
            !m_kernel.cpu().read32(address, &low) ||
            !m_kernel.cpu().read32(address + 4u, &high))
        {
            return false;
        }
        const uint64_t ticks =
            (static_cast<uint64_t>(high) << 32u) | low;
        if (!m_kernel.set_alarm(
                ticks, cpu.m_gpr[5], cpu.m_gpr[6], cpu.m_gpr[28]))
        {
            return false;
        }
        cpu.m_gpr[2] = 0u;
        return true;
    }
    case 37u:
        if (!m_kernel.cancel_alarm(cpu.m_gpr[4], cpu.m_gpr[5]))
        {
            return false;
        }
        cpu.m_gpr[2] = 0u;
        return true;
    case 39u:
    {
        const uint32_t output = cpu.m_gpr[5];
        const uint64_t ticks =
            static_cast<uint64_t>(cpu.m_gpr[4]) * 0x1200u / 125u;
        if (!is_iop_ram_span(output, 8u) ||
            !m_kernel.cpu().write32(
                output, static_cast<uint32_t>(ticks)) ||
            !m_kernel.cpu().write32(
                output + 4u, static_cast<uint32_t>(ticks >> 32u)))
        {
            return false;
        }
        // USec2SysClock is void; leave $v0 alone.
        return true;
    }
    case 40u:
    {
        const uint32_t input = cpu.m_gpr[4];
        const uint32_t seconds_output = cpu.m_gpr[5];
        const uint32_t microseconds_output = cpu.m_gpr[6];
        uint32_t low = 0u;
        uint32_t high = 0u;
        if (!is_iop_ram_span(input, 8u) ||
            !is_iop_ram_span(seconds_output, 4u) ||
            !is_iop_ram_span(microseconds_output, 4u) ||
            !m_kernel.cpu().read32(input, &low) ||
            !m_kernel.cpu().read32(input + 4u, &high))
        {
            return false;
        }
        const uint64_t ticks =
            (static_cast<uint64_t>(high) << 32u) | low;
        const uint64_t whole = ticks / 0x1200u;
        const uint64_t remainder = ticks % 0x1200u;
        const uint64_t microseconds =
            whole * 125u + remainder * 125u / 0x1200u;
        const uint64_t seconds = microseconds / 1000000u;
        if (seconds > std::numeric_limits<uint32_t>::max() ||
            !m_kernel.cpu().write32(
                seconds_output, static_cast<uint32_t>(seconds)) ||
            !m_kernel.cpu().write32(
                microseconds_output,
                static_cast<uint32_t>(microseconds % 1000000u)))
        {
            return false;
        }
        return true;
    }
    case 43u:
        cpu.m_gpr[2] = static_cast<uint32_t>(m_kernel.cycles());
        return true;
    default:
        return false;
    }
}

bool c_kernel_hle_bridge::handle_thevent(uint32_t ordinal)
{
    s_cpu_state& cpu = m_kernel.cpu().state();
    const uint32_t id = cpu.m_gpr[4];
    if (ordinal == 4u)
    {
        uint32_t attr = 0u;
        uint32_t option = 0u;
        uint32_t initial = 0u;
        if (!m_kernel.cpu().read32(id, &attr) ||
            !m_kernel.cpu().read32(id + 4u, &option) ||
            !m_kernel.cpu().read32(id + 8u, &initial))
        {
            return false;
        }
        const uint32_t created = m_next_object++;
        m_events.emplace(created, s_event{attr, option, initial, {}});
        cpu.m_gpr[2] = created;
        return true;
    }
    auto found = m_events.find(id);
    if (found == m_events.end())
    {
        if (ordinal >= 5u && ordinal <= 14u)
        {
            cpu.m_gpr[2] = static_cast<uint32_t>(-409);
            return true;
        }
        return false;
    }
    s_event& event = found->second;
    switch (ordinal)
    {
    case 5u:
    {
        // Verify all waiters before changing the object.
        for (const s_event_waiter& waiter : event.m_waiters)
        {
            const s_kernel_thread* thread =
                m_kernel.thread(waiter.m_thread);
            if (thread == nullptr ||
                thread->m_status != e_thread_status::blocked)
            {
                return false;
            }
        }
        for (const s_event_waiter& waiter : event.m_waiters)
        {
            if (!m_kernel.wake_thread_with_result(
                    waiter.m_thread, static_cast<uint32_t>(-425)))
            {
                return false;
            }
        }
        m_events.erase(found);
        cpu.m_gpr[2] = 0u;
        return true;
    }
    case 6u:
    case 7u:
        event.m_bits |= cpu.m_gpr[5];
        for (auto it = event.m_waiters.begin();
             it != event.m_waiters.end();)
        {
            if (!match_event(event, it->m_mask, it->m_mode))
            {
                ++it;
                continue;
            }
            if (!take_event(event, it->m_mask,
                            it->m_mode, it->m_output) ||
                !m_kernel.wake_thread(it->m_thread))
            {
                return false;
            }
            it = event.m_waiters.erase(it);
        }
        break;
    case 8u:
    case 9u:
        event.m_bits &= cpu.m_gpr[5];
        break;
    case 10u:
    {
        const uint32_t bits = cpu.m_gpr[5];
        const uint32_t mode = cpu.m_gpr[6];
        const uint32_t output = cpu.m_gpr[7];
        if ((mode & ~0x31u) != 0u || bits == 0u)
        {
            return false;
        }
        if (match_event(event, bits, mode))
        {
            if (!take_event(event, bits, mode, output))
            {
                return false;
            }
        }
        else
        {
            if (!m_kernel.block_current(4u))
            {
                return false;
            }
            event.m_waiters.push_back({
                m_kernel.current_thread_id(), bits, mode, output
            });
        }
        break;
    }
    case 11u:
    case 12u:
    {
        const uint32_t bits = cpu.m_gpr[5];
        const uint32_t mode = cpu.m_gpr[6];
        const uint32_t output = cpu.m_gpr[7];
        if ((mode & ~0x31u) != 0u || bits == 0u)
        {
            return false;
        }
        if (!match_event(event, bits, mode))
        {
            cpu.m_gpr[2] = static_cast<uint32_t>(-421);
            return true;
        }
        if (!take_event(event, bits, mode, output))
        {
            return false;
        }
        break;
    }
    case 13u:
    case 14u:
    {
        const uint32_t output = cpu.m_gpr[5];
        if (!is_iop_ram_span(output, 20u))
        {
            return false;
        }
        const uint32_t words[5] = {
            event.m_attr, event.m_option, 0u, event.m_bits,
            static_cast<uint32_t>(event.m_waiters.size())
        };
        for (uint32_t i = 0u; i < 5u; ++i)
        {
            if (!m_kernel.cpu().write32(output + i * 4u, words[i]))
            {
                return false;
            }
        }
        break;
    }
    default:
        return false;
    }
    cpu.m_gpr[2] = 0u;
    return true;
}

bool c_kernel_hle_bridge::handle_thsemap(uint32_t ordinal)
{
    s_cpu_state& cpu = m_kernel.cpu().state();
    const uint32_t id = cpu.m_gpr[4];
    // Record actual guest semaphore state transitions, not inferred
    // completions. Bound this to avoid flooding a long retail replay.
    const auto trace_transition =
        [&](const char* action, uint32_t sema_id,
            uint32_t old_count, uint32_t new_count, uint32_t target)
    {
        // SDBZ_TRACE_IOP_SEMA=<n> raises the bounded event count.
        static const uint32_t s_trace_limit = []
        {
            const char* text = std::getenv("SDBZ_TRACE_IOP_SEMA");
            return text != nullptr
                ? static_cast<uint32_t>(std::strtoul(text, nullptr, 0))
                : 128u;
        }();
        if (m_semaphore_trace_events < s_trace_limit)
        {
            ++m_semaphore_trace_events;
            std::fprintf(stderr,
                "IOP guest semaphore: action=%s id=%u thread=%u "
                "target=%u count=%u->%u pc=0x%08x ra=0x%08x "
                "cycles=%llu\n",
                action, sema_id, m_kernel.current_thread_id(),
                target, old_count, new_count, cpu.m_pc, cpu.m_gpr[31],
                static_cast<unsigned long long>(m_kernel.cycles()));
        }
    };
    if (ordinal == 4u)
    {
        uint32_t attr = 0u;
        uint32_t option = 0u;
        uint32_t count = 0u;
        uint32_t maximum = 0u;
        if (!m_kernel.cpu().read32(id, &attr) ||
            !m_kernel.cpu().read32(id + 4u, &option) ||
            !m_kernel.cpu().read32(id + 8u, &count) ||
            !m_kernel.cpu().read32(id + 12u, &maximum) ||
            maximum == 0u || count > maximum)
        {
            return false;
        }
        const uint32_t created = m_next_object++;
        m_semaphores.emplace(
            created, s_semaphore{attr, option, count, maximum, {}});
        trace_transition("create", created, 0u, count, 0u);
        cpu.m_gpr[2] = created;
        return true;
    }
    auto found = m_semaphores.find(id);
    if (found == m_semaphores.end())
    {
        if ((ordinal >= 5u && ordinal <= 9u) ||
            ordinal == 11u || ordinal == 12u)
        {
            // The real ARKD RPC worker reached thsemap #8 WaitSema
            // with an unknown guest object. Record the requested ID
            // once without manufacturing a semaphore or waking a
            // thread. Reference PS2 kernel returns KE_UNKNOWN_SEMID.
            if (ordinal == 8u &&
                m_unknown_semaphore_wait_ids.insert(id).second)
            {
                std::fprintf(stderr,
                    "IOP HLE WaitSema unknown id: id=%u "
                    "thread=%u pc=0x%08x ra=0x%08x "
                    "registered_semaphores=%zu\n",
                    id, m_kernel.current_thread_id(),
                    cpu.m_pc, cpu.m_gpr[31],
                    m_semaphores.size());
                for (const auto& item : m_semaphores)
                {
                    std::fprintf(stderr,
                        "IOP HLE WaitSema known id: id=%u count=%u "
                        "max=%u waiters=%zu\n",
                        item.first, item.second.m_count,
                        item.second.m_maximum,
                        item.second.m_waiters.size());
                }
            }
            cpu.m_gpr[2] = static_cast<uint32_t>(-408);
            return true;
        }
        return false;
    }
    s_semaphore& sema = found->second;
    switch (ordinal)
    {
    case 5u:
    {
        for (uint32_t waiter : sema.m_waiters)
        {
            const s_kernel_thread* thread = m_kernel.thread(waiter);
            if (thread == nullptr ||
                thread->m_status != e_thread_status::blocked)
            {
                return false;
            }
        }
        for (uint32_t waiter : sema.m_waiters)
        {
            if (!m_kernel.wake_thread_with_result(
                    waiter, static_cast<uint32_t>(-425)))
            {
                return false;
            }
        }
        m_semaphores.erase(found);
        cpu.m_gpr[2] = 0u;
        return true;
    }
    case 6u:
    case 7u:
        if (!sema.m_waiters.empty())
        {
            const uint32_t thread_id = sema.m_waiters.front();
            if (!m_kernel.wake_thread(thread_id))
            {
                return false;
            }
            sema.m_waiters.pop_front();
            trace_transition("signal_wake", id, sema.m_count,
                             sema.m_count, thread_id);
        }
        else if (sema.m_count < sema.m_maximum)
        {
            const uint32_t previous = sema.m_count;
            ++sema.m_count;
            trace_transition("signal_increment", id, previous,
                             sema.m_count, 0u);
        }
        else
        {
            return false;
        }
        break;
    case 8u:
        if (sema.m_count != 0u)
        {
            const uint32_t previous = sema.m_count;
            --sema.m_count;
            trace_transition("wait_acquire", id, previous,
                             sema.m_count, 0u);
        }
        else
        {
            if (!m_kernel.block_current(3u))
            {
                return false;
            }
            sema.m_waiters.push_back(m_kernel.current_thread_id());
            trace_transition("wait_block", id, 0u, 0u,
                             m_kernel.current_thread_id());
        }
        break;
    case 9u:
        if (sema.m_count == 0u)
        {
            cpu.m_gpr[2] = static_cast<uint32_t>(-419);
            return true;
        }
        --sema.m_count;
        break;
    case 11u:
    case 12u:
    {
        const uint32_t output = cpu.m_gpr[5];
        if (!is_iop_ram_span(output, 24u))
        {
            return false;
        }
        const uint32_t words[] = {
            sema.m_attr, sema.m_option, 0u,
            sema.m_maximum, sema.m_count,
            static_cast<uint32_t>(sema.m_waiters.size())
        };
        for (uint32_t i = 0u; i < 6u; ++i)
        {
            if (!m_kernel.cpu().write32(
                    output + i * 4u, words[i]))
            {
                return false;
            }
        }
        break;
    }
    default:
        return false;
    }
    cpu.m_gpr[2] = 0u;
    return true;
}

bool c_kernel_hle_bridge::handle_sysclib(uint32_t ordinal)
{
    s_cpu_state& cpu = m_kernel.cpu().state();
    if (ordinal == 6u || ordinal == 7u) // _toupper / _tolower
    {
        const uint32_t ch = cpu.m_gpr[4];
        uint32_t result = ch;
        if (ordinal == 6u && ch >= 'a' && ch <= 'z')
        {
            result = ch - ('a' - 'A');
        }
        else if (ordinal == 7u && ch >= 'A' && ch <= 'Z')
        {
            result = ch + ('a' - 'A');
        }
        cpu.m_gpr[2] = result;
        return true;
    }
    if (ordinal == 8u) // look_ctype_table
    {
        const uint32_t ch = cpu.m_gpr[4] & 0xffu;
        if (ch >= 128u)
        {
            return false;
        }
        cpu.m_gpr[2] = ctype_flags(ch);
        return true;
    }
    if (ordinal == 9u) // get_ctype_table
    {
        cpu.m_gpr[2] = k_ctype_guest_table;
        return true;
    }
    // PS2SDK SYSCLIB ordinals 10-17: memchr, memcmp, memcpy,
    // memmove, memset, bcmp, bcopy and bzero. All reads and writes
    // remain within real IOP RAM. No MMIO side effects are simulated.
    if (ordinal >= 10u && ordinal <= 17u)
    {
        c_cpu& memory = m_kernel.cpu();
        const uint32_t first = cpu.m_gpr[4];
        const uint32_t second = cpu.m_gpr[5];
        const uint32_t length =
            ordinal == 17u ? second : cpu.m_gpr[6];
        if (length == 0u)
        {
            if (ordinal == 10u || ordinal == 11u ||
                ordinal == 15u)
            {
                cpu.m_gpr[2] = 0u;
            }
            else if (ordinal == 12u || ordinal == 13u ||
                     ordinal == 14u)
            {
                cpu.m_gpr[2] = first;
            }
            return true;
        }

        if (ordinal == 10u) // memchr
        {
            if (!is_iop_ram_span(first, length))
            {
                return false;
            }
            cpu.m_gpr[2] = 0u;
            for (uint32_t i = 0u; i < length; ++i)
            {
                uint8_t byte = 0u;
                if (!memory.read8(first + i, &byte))
                {
                    return false;
                }
                if (byte == static_cast<uint8_t>(second))
                {
                    cpu.m_gpr[2] = first + i;
                    break;
                }
            }
            return true;
        }
        if (ordinal == 11u || ordinal == 15u) // memcmp / bcmp
        {
            if (!is_iop_ram_span(first, length) ||
                !is_iop_ram_span(second, length))
            {
                return false;
            }
            int32_t difference = 0;
            for (uint32_t i = 0u; i < length; ++i)
            {
                uint8_t lhs = 0u;
                uint8_t rhs = 0u;
                if (!memory.read8(first + i, &lhs) ||
                    !memory.read8(second + i, &rhs))
                {
                    return false;
                }
                if (lhs != rhs)
                {
                    difference = static_cast<int32_t>(lhs) -
                        static_cast<int32_t>(rhs);
                    break;
                }
            }
            cpu.m_gpr[2] = static_cast<uint32_t>(
                ordinal == 15u && difference != 0 ? 1 : difference);
            return true;
        }
        if (ordinal == 12u || ordinal == 13u ||
            ordinal == 16u) // memcpy / memmove / bcopy
        {
            const uint32_t destination = ordinal == 16u ? second : first;
            const uint32_t source = ordinal == 16u ? first : second;
            if (!is_iop_ram_span(source, length) ||
                !is_iop_ram_span(destination, length))
            {
                return false;
            }
            std::vector<uint8_t> data;
            // Snapshot before writing, preserving overlapping memmove/bcopy.
            if (!read_iop_ram(memory, source, length, data) ||
                !write_iop_ram(memory, destination, data))
            {
                return false;
            }
            if (ordinal != 16u)
            {
                cpu.m_gpr[2] = destination;
            }
            return true;
        }
        if (ordinal == 14u || ordinal == 17u) // memset / bzero
        {
            if (!is_iop_ram_span(first, length))
            {
                return false;
            }
            const uint8_t byte =
                ordinal == 17u ? 0u : static_cast<uint8_t>(second);
            const std::vector<uint8_t> data(length, byte);
            if (!write_iop_ram(memory, first, data))
            {
                return false;
            }
            if (ordinal == 14u)
            {
                cpu.m_gpr[2] = first;
            }
            return true;
        }
    }
    if (ordinal == 19u || ordinal == 42u)
    {
        return execute_sysclib_format(m_kernel.cpu(), ordinal);
    }
    if (ordinal >= 20u && ordinal <= 34u)
    {
        return execute_sysclib_string(m_kernel.cpu(), ordinal);
    }
    if (ordinal == 35u) // strtok (stateful, bounded IOP strings)
    {
        const uint32_t first = cpu.m_gpr[4];
        uint32_t at = first == 0u ? m_strtok_next : first;
        std::string delimiters;
        if (!read_string(cpu.m_gpr[5], 256u, &delimiters))
        {
            return false;
        }
        if (at == 0u)
        {
            cpu.m_gpr[2] = 0u;
            return true;
        }
        uint8_t ch = 0u;
        uint32_t count = 0u;
        for (; count < 65536u; ++count)
        {
            if (!is_iop_ram_span(at, 1u) ||
                !m_kernel.cpu().read8(at, &ch))
            {
                return false;
            }
            if (ch == 0u ||
                delimiters.find(static_cast<char>(ch)) ==
                    std::string::npos)
            {
                break;
            }
            if (at == std::numeric_limits<uint32_t>::max())
            {
                return false;
            }
            ++at;
        }
        if (count == 65536u)
        {
            return false;
        }
        if (ch == 0u)
        {
            m_strtok_next = 0u;
            cpu.m_gpr[2] = 0u;
            return true;
        }
        const uint32_t token = at;
        for (; count < 65536u; ++count)
        {
            if (!is_iop_ram_span(at, 1u) ||
                !m_kernel.cpu().read8(at, &ch))
            {
                return false;
            }
            if (ch == 0u ||
                delimiters.find(static_cast<char>(ch)) !=
                    std::string::npos)
            {
                break;
            }
            if (at == std::numeric_limits<uint32_t>::max())
            {
                return false;
            }
            ++at;
        }
        if (count == 65536u)
        {
            return false;
        }
        if (ch != 0u)
        {
            if (!m_kernel.cpu().write8(at, 0u))
            {
                return false;
            }
            m_strtok_next = at + 1u;
        }
        else
        {
            m_strtok_next = at;
        }
        cpu.m_gpr[2] = token;
        return true;
    }
    if (ordinal == 40u) // _wmemcopy (byte count in PS2SDK)
    {
        const uint32_t target = cpu.m_gpr[4];
        const uint32_t source = cpu.m_gpr[5];
        const uint32_t length = cpu.m_gpr[6];
        std::vector<uint8_t> bytes;
        if (!read_iop_ram(m_kernel.cpu(), source, length, bytes) ||
            !write_iop_ram(m_kernel.cpu(), target, bytes))
        {
            return false;
        }
        cpu.m_gpr[2] = target;
        return true;
    }
    if (ordinal == 41u) // _wmemset (complete 32-bit units)
    {
        const uint32_t target = cpu.m_gpr[4];
        const uint32_t length = cpu.m_gpr[6];
        if ((target & 3u) != 0u || (length & 3u) != 0u ||
            !is_iop_ram_span(target, length))
        {
            return false;
        }
        for (uint32_t i = 0u; i < length; i += 4u)
        {
            if (!m_kernel.cpu().write32(
                    target + i, cpu.m_gpr[5]))
            {
                return false;
            }
        }
        cpu.m_gpr[2] = target;
        return true;
    }
    if (ordinal == 36u || ordinal == 38u)
    {
        std::string input;
        if (!read_string(cpu.m_gpr[4], 256u, &input))
        {
            return false;
        }
        const uint32_t base = cpu.m_gpr[6];
        if (base != 0u && (base < 2u || base > 36u))
        {
            return false;
        }
        char* end = nullptr;
        const uint32_t converted = ordinal == 36u
            ? static_cast<uint32_t>(std::strtol(
                input.c_str(), &end, static_cast<int>(base)))
            : static_cast<uint32_t>(std::strtoul(
                input.c_str(), &end, static_cast<int>(base)));
        if (cpu.m_gpr[5] != 0u &&
            !m_kernel.cpu().write32(
                cpu.m_gpr[5],
                cpu.m_gpr[4] +
                    static_cast<uint32_t>(end - input.c_str())))
        {
            return false;
        }
        cpu.m_gpr[2] = converted;
        return true;
    }
    return false;
}


bool c_kernel_hle_bridge::handle_modload(uint32_t ordinal)
{
    s_cpu_state& cpu = m_kernel.cpu().state();
    constexpr uint32_t k_error_illegal_object =
        static_cast<uint32_t>(-201);
    constexpr uint32_t k_error_unknown_module =
        static_cast<uint32_t>(-202);

    if (ordinal == 13u) // SetCheckKelfPathCallback
    {
        // Registration is stateful, but this is NOT a KELF verifier.
        // Encrypted module loading remains unsupported and fail-closed.
        const uint32_t callback = cpu.m_gpr[4];
        uint32_t instruction = 0u;
        if (callback != 0u &&
            ((callback & 3u) != 0u ||
             !is_iop_ram_span(callback, 4u) ||
             !m_kernel.cpu().read32(callback, &instruction)))
        {
            cpu.m_gpr[2] = k_error_illegal_object;
            return true;
        }
        m_check_kelf_path_callback = callback;
        cpu.m_gpr[2] = 0u;
        return true;
    }

    if (ordinal == 10u) // LoadModuleBuffer, link but do not start
    {
        const uint32_t source = cpu.m_gpr[4];
        if (!is_iop_ram_span(source, 52u))
        {
            cpu.m_gpr[2] = k_error_illegal_object;
            return true;
        }
        const uint32_t physical = source & 0x1fffffffu;
        const uint32_t bytes_available = std::min<uint32_t>(
            c_cpu::k_ram_size - physical, 2u * 1024u * 1024u);
        std::vector<uint8_t> image;
        if (!read_iop_ram(m_kernel.cpu(), source, bytes_available, image))
        {
            return false;
        }
        const s_module_load_result loaded = m_kernel.load_module(image);
        cpu.m_gpr[2] = loaded.m_status == e_irx_load_result::success
            ? loaded.m_id : k_error_illegal_object;
        return true;
    }

    if (ordinal == 17u) // ReferModuleStatus
    {
        const s_kernel_module* module = m_kernel.module(cpu.m_gpr[4]);
        if (module == nullptr)
        {
            cpu.m_gpr[2] = k_error_unknown_module;
            return true;
        }
        const uint32_t destination = cpu.m_gpr[5];
        if (!is_iop_ram_span(destination, 0x50u))
        {
            cpu.m_gpr[2] = k_error_illegal_object;
            return true;
        }
        std::vector<uint8_t> status(0x50u, 0u);
        const std::string& name = module->m_irx.m_name;
        for (size_t i = 0u; i < name.size() && i < 55u; ++i)
        {
            status[i] = static_cast<uint8_t>(name[i]);
        }
        const auto set_word = [&status](size_t offset, uint32_t value)
        {
            for (uint32_t i = 0u; i < 4u; ++i)
            {
                status[offset + i] = static_cast<uint8_t>(
                    value >> (i * 8u));
            }
        };
        set_word(0x38u, module->m_id);
        set_word(0x40u, module->m_irx.m_entry);
        set_word(0x44u, module->m_irx.m_gp);
        set_word(0x48u, module->m_irx.m_base);
        set_word(0x4cu, module->m_irx.m_size);
        if (!write_iop_ram(m_kernel.cpu(), destination, status))
        {
            return false;
        }
        cpu.m_gpr[2] = 0u;
        return true;
    }

    if (ordinal == 22u || ordinal == 18u || ordinal == 16u)
    {
        // Snapshot the real linked modules. No synthetic IDs or start states.
        const bool by_name = ordinal != 16u;
        std::string requested;
        if (by_name)
        {
            if (!read_string(cpu.m_gpr[4], 64u, &requested))
            {
                cpu.m_gpr[2] = k_error_illegal_object;
                return true;
            }
            std::transform(
                requested.begin(), requested.end(), requested.begin(),
                [](unsigned char ch)
                {
                    return static_cast<char>(std::tolower(ch));
                });
        }
        std::vector<uint32_t> matching;
        for (const auto& pair : m_kernel.modules())
        {
            std::string name = pair.second.m_irx.m_name;
            if (by_name)
            {
                std::transform(
                    name.begin(), name.end(), name.begin(),
                    [](unsigned char ch)
                    {
                        return static_cast<char>(std::tolower(ch));
                    });
            }
            if (!by_name || name == requested)
            {
                matching.push_back(pair.first);
            }
        }
        if (ordinal == 22u)
        {
            cpu.m_gpr[2] = matching.empty()
                ? k_error_unknown_module : matching.front();
            return true;
        }
        const uint32_t output = cpu.m_gpr[by_name ? 5u : 4u];
        const uint32_t capacity = cpu.m_gpr[by_name ? 6u : 5u];
        const uint32_t count_output = cpu.m_gpr[by_name ? 7u : 6u];
        if (capacity > c_cpu::k_ram_size / 4u ||
            (capacity != 0u &&
             !is_iop_ram_span(output, capacity * 4u)) ||
            (count_output != 0u &&
             !is_iop_ram_span(count_output, 4u)))
        {
            cpu.m_gpr[2] = k_error_illegal_object;
            return true;
        }
        const uint32_t count = std::min<uint32_t>(
            capacity, static_cast<uint32_t>(matching.size()));
        for (uint32_t i = 0u; i < count; ++i)
        {
            if (!m_kernel.cpu().write32(output + i * 4u, matching[i]))
            {
                return false;
            }
        }
        if (count_output != 0u &&
            !m_kernel.cpu().write32(
                count_output, static_cast<uint32_t>(matching.size())))
        {
            return false;
        }
        cpu.m_gpr[2] = 0u;
        return true;
    }

    // LoadStartModule and StartModule require correct guest entrypoint
    // completion and caller-wakeup semantics; do not fabricate either.
    return false;
}


// The original IOMAN interface is broader than the read-only CD-ROM
// subset supported here. Never report successful driver registration,
// writes to disc/media, or memory-card operations without guest handlers.
bool c_kernel_hle_bridge::handle_ioman(uint32_t ordinal)
{
    s_cpu_state& cpu = m_kernel.cpu().state();
    constexpr uint32_t k_no_entry = static_cast<uint32_t>(-2);
    constexpr uint32_t k_bad_fd = static_cast<uint32_t>(-9);
    constexpr uint32_t k_permission = static_cast<uint32_t>(-13);
    constexpr uint32_t k_invalid = static_cast<uint32_t>(-22);
    constexpr uint32_t k_too_many = static_cast<uint32_t>(-24);
    const uint32_t argument = cpu.m_gpr[4];

    if (ordinal == 20u || ordinal == 21u)
    {
        // AddDrv/DelDrv must execute the guest driver's init/deinit
        // callbacks and install/remove its operations table. The IOP
        // scheduler has no synchronous guest-callback bridge for this.
        // Match the reference's explicit unsuccessful boundary.
        // In particular, do not pretend a memory card driver exists.
        cpu.m_gpr[2] = static_cast<uint32_t>(-1);
        return true;
    }

    if (ordinal == 4u || ordinal == 16u)
    {
        std::string path;
        if (!read_string(argument, 256u, &path))
        {
            cpu.m_gpr[2] = k_invalid;
            return true;
        }
        s_folder_file_info file{};
        if (m_disc_source == nullptr ||
            !m_disc_source->valid() ||
            !m_disc_source->find_file(path, &file))
        {
            cpu.m_gpr[2] = k_no_entry;
            return true;
        }

        if (ordinal == 4u) // open: CD-ROM, read-only
        {
            const uint32_t flags = cpu.m_gpr[5];
            if (flags != 0u && flags != 1u)
            {
                cpu.m_gpr[2] = k_permission;
                return true;
            }
            uint32_t fd = 3u;
            for (; fd < 64u; ++fd)
            {
                if (m_ioman_files.count(fd) == 0u)
                {
                    break;
                }
            }
            if (fd == 64u)
            {
                cpu.m_gpr[2] = k_too_many;
                return true;
            }
            m_ioman_files.emplace(
                fd, s_ioman_file{file.m_lba, file.m_size, 0u});
            cpu.m_gpr[2] = fd;
            return true;
        }

        // getstat: io_stat_t (40 bytes), file mode and size populated
        // from a real directory record; all other fields zero.
        if (!is_iop_ram_span(cpu.m_gpr[5], 40u))
        {
            cpu.m_gpr[2] = k_invalid;
            return true;
        }
        std::vector<uint8_t> status(40u, 0u);
        status[0] = 0x17u;
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            status[8u + i] = static_cast<uint8_t>(
                file.m_size >> (i * 8u));
        }
        if (!write_iop_ram(m_kernel.cpu(), cpu.m_gpr[5], status))
        {
            return false;
        }
        cpu.m_gpr[2] = 0u;
        return true;
    }

    if (ordinal == 5u) // close
    {
        const auto found = m_ioman_files.find(argument);
        if (found == m_ioman_files.end())
        {
            cpu.m_gpr[2] = k_bad_fd;
            return true;
        }
        m_ioman_files.erase(found);
        cpu.m_gpr[2] = 0u;
        return true;
    }

    if (ordinal == 7u) // write: console only, never disc/media
    {
        if (argument != 1u && argument != 2u)
        {
            cpu.m_gpr[2] = m_ioman_files.count(argument) != 0u
                ? k_permission : k_bad_fd;
            return true;
        }
        std::vector<uint8_t> data;
        const uint32_t length = cpu.m_gpr[6];
        if (!read_iop_ram(m_kernel.cpu(), cpu.m_gpr[5],
                          length, data))
        {
            cpu.m_gpr[2] = k_invalid;
            return true;
        }
        if (!write_console(std::string(data.begin(), data.end())))
        {
            return false;
        }
        cpu.m_gpr[2] = length;
        return true;
    }

    const auto found = m_ioman_files.find(argument);
    if (found == m_ioman_files.end())
    {
        cpu.m_gpr[2] = k_bad_fd;
        return true;
    }
    s_ioman_file& file = found->second;
    if (ordinal == 8u) // lseek: clamp to actual file size
    {
        const int64_t offset = static_cast<int32_t>(cpu.m_gpr[5]);
        const uint32_t whence = cpu.m_gpr[6];
        if (whence > 2u)
        {
            cpu.m_gpr[2] = k_invalid;
            return true;
        }
        const int64_t base = whence == 0u ? 0 :
            whence == 1u ? static_cast<int64_t>(file.m_position) :
                           static_cast<int64_t>(file.m_size);
        const int64_t target = std::max<int64_t>(0, std::min<int64_t>(
            static_cast<int64_t>(file.m_size), base + offset));
        file.m_position = static_cast<uint32_t>(target);
        cpu.m_gpr[2] = file.m_position;
        return true;
    }
    if (ordinal == 6u) // read: bounded guest RAM + real ISO sectors
    {
        const uint32_t destination = cpu.m_gpr[5];
        const uint32_t requested = cpu.m_gpr[6];
        const uint32_t count = std::min<uint32_t>(
            requested, file.m_size - file.m_position);
        if (!is_iop_ram_span(destination, count))
        {
            cpu.m_gpr[2] = k_invalid;
            return true;
        }
        if (count == 0u)
        {
            cpu.m_gpr[2] = 0u;
            return true;
        }
        if (m_disc_source == nullptr || !m_disc_source->valid())
        {
            cpu.m_gpr[2] = k_no_entry;
            return true;
        }
        constexpr uint32_t sector_size = c_iso_sector_source::k_sector_size;
        const uint32_t first = file.m_position / sector_size;
        const uint32_t start = file.m_position % sector_size;
        const uint32_t sector_count =
            static_cast<uint32_t>(
                (static_cast<uint64_t>(start) + count + sector_size - 1u)
                    / sector_size);
        std::vector<uint8_t> sectors;
        if (!m_disc_source->read_sectors(
                file.m_lba + first, sector_count, sectors) ||
            static_cast<uint64_t>(start) + count > sectors.size())
        {
            cpu.m_gpr[2] = k_no_entry;
            return true;
        }
        const std::vector<uint8_t> data(
            sectors.begin() + start,
            sectors.begin() + start + count);
        if (!write_iop_ram(m_kernel.cpu(), destination, data))
        {
            return false;
        }
        file.m_position += count;
        cpu.m_gpr[2] = count;
        return true;
    }
    return false;
}

bool c_kernel_hle_bridge::handle_secrman(uint32_t ordinal)
{
    if (ordinal != 4u && ordinal != 5u && ordinal != 6u)
    {
        return false;
    }
    auto& state = m_kernel.cpu().state();
    // The virtual file-backed PS2 card has no MagicGate processor.
    // Complete the guest driver's handshake only for its mounted
    // SIO2 port and slot after the card has received reset-auth.
    if (ordinal == 6u)
    {
        state.m_gpr[2] = m_kernel.hardware().complete_memory_card_auth_reset(
            state.m_gpr[4], state.m_gpr[5]) ? 1u : 0u;
    }
    else
    {
        state.m_gpr[2] = static_cast<uint32_t>(-1);
    }
    return true;
}

bool c_kernel_hle_bridge::handle_vblank(uint32_t ordinal)
{
    // PS2SDK vblank ABI: #8 registers an edge callback, #9 removes
    // a matching edge/function pair. Edge 0 is the start event;
    // edge 1 is the end event. No registration fabricates an edge.
    if (ordinal == 8u)
    {
        s_cpu_state& state = m_kernel.cpu().state();
        const uint32_t edge = state.m_gpr[4];
        const uint32_t callback = state.m_gpr[6];
        const uint32_t argument = state.m_gpr[7];
        const uint32_t region = callback & 0xe0000000u;
        const uint32_t physical = callback & 0x1fffffffu;
        uint32_t opcode = 0u;
        if (edge > 1u || callback == 0u ||
            (callback & 3u) != 0u ||
            (region != 0u && region != 0x80000000u &&
             region != 0xa0000000u) ||
            physical >= c_cpu::k_ram_size ||
            !m_kernel.cpu().read32(callback, &opcode) ||
            m_vblank_handlers.size() >= 64u)
        {
            state.m_gpr[2] = static_cast<uint32_t>(-1);
            return true;
        }
        const e_guest_callback_source source =
            edge == 0u ? e_guest_callback_source::vblank_start :
                         e_guest_callback_source::vblank_end;
        const s_guest_callback_origin origin =
            m_kernel.make_guest_callback_origin(source);
        m_vblank_handlers.push_back({
            edge, callback, argument, state.m_gpr[28], origin});
        // At most 64 live registrations can be retained. Report the
        // guest arguments before a later field schedules this pointer.
        std::fprintf(stderr,
            "IOP VBLANK handler registered: edge=%u priority=%u "
            "callback=0x%08x first_word=0x%08x arg=0x%08x "
            "thread=%u syscall_pc=0x%08x ra=0x%08x gp=0x%08x "
            "a0=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x\n",
            edge, state.m_gpr[5], callback, opcode, argument,
            origin.m_registration_thread_id, origin.m_registration_pc,
            origin.m_registration_ra, origin.m_registration_gp,
            origin.m_registration_a0, origin.m_registration_a1,
            origin.m_registration_a2, origin.m_registration_a3);
        state.m_gpr[2] = 0u;
        return true;
    }
    if (ordinal == 9u)
    {
        const uint32_t edge = m_kernel.cpu().state().m_gpr[4];
        const uint32_t callback = m_kernel.cpu().state().m_gpr[5];
        if (edge > 1u)
        {
            m_kernel.cpu().state().m_gpr[2] =
                static_cast<uint32_t>(-1);
            return true;
        }
        m_vblank_handlers.erase(std::remove_if(
            m_vblank_handlers.begin(), m_vblank_handlers.end(),
            [edge, callback](const s_vblank_handler& handler)
            {
                return handler.m_edge == edge &&
                       handler.m_handler == callback;
            }), m_vblank_handlers.end());
        m_kernel.cpu().state().m_gpr[2] = 0u;
        return true;
    }
    // Waits #4/#6 wait for start, #5/#7 wait for end.
    // The original caller remains blocked until the matching edge.
    if (ordinal < 4u || ordinal > 7u)
    {
        return false;
    }
    const uint32_t thread_id = m_kernel.current_thread_id();
    const bool start = ordinal == 4u || ordinal == 6u;
    if (thread_id == 0u ||
        !m_kernel.block_current(start ? 16u : 17u))
    {
        return false;
    }
    std::vector<uint32_t>& waiters =
        start ? m_vblank_start_waiters : m_vblank_end_waiters;
    waiters.push_back(thread_id);
    m_kernel.cpu().state().m_gpr[2] = 0u;
    return true;
}

bool c_kernel_hle_bridge::handle_syscall(
    c_kernel& kernel, uint32_t service
)
{
    if (&kernel != &m_kernel ||
        service < k_service_base ||
        service >= k_service_base + 0x1200u ||
        !m_installed)
    {
        return false;
    }
    const uint32_t family = (service - k_service_base) >> 8u;
    const uint32_t ordinal = service & 0xffu;
    switch (family)
    {
    case 1u: return handle_loadcore(ordinal);
    case 2u: return handle_intrman(ordinal);
    case 3u: return handle_stdio(ordinal);
    case 4u: return handle_dmacman(ordinal);
    case 5u: return handle_thbase(ordinal);
    case 6u: return handle_thevent(ordinal);
    case 7u: return handle_thsemap(ordinal);
    case 8u: return handle_sysclib(ordinal);
    case 9u: return handle_sysmem(ordinal);
    case 10u: return handle_thmsgbx(ordinal);
    case 11u: return handle_heaplib(ordinal);
    case 12u: return handle_fixed_pool(ordinal);
    case 13u: return handle_variable_pool(ordinal);
    case 14u: return handle_modload(ordinal);
    case 15u: return handle_ioman(ordinal);
    case 16u: return handle_secrman(ordinal);
    case 17u: return handle_vblank(ordinal);
    default: return false;
    }
}
} // namespace iop
