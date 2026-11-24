#include "iop_cdvdman_bridge.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <utility>
#include <vector>

namespace iop
{
namespace
{
bool create_stub(c_cpu& cpu, uint32_t pc, uint32_t service)
{
    // lui v0, upper; ori v0, lower; syscall; jr ra; nop.
    const uint32_t words[5] =
    {
        0x3c020000u | (service >> 16u),
        0x34420000u | (service & 0xffffu),
        0x0000000cu,
        0x03e00008u,
        0x00000000u
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

void put32(uint8_t* bytes, uint32_t value)
{
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        bytes[i] = static_cast<uint8_t>(value >> (8u * i));
    }
}

uint8_t decimal_to_bcd(uint32_t value)
{
    return static_cast<uint8_t>(((value / 10u) << 4u) | (value % 10u));
}
} // namespace

c_cdvdman_bridge::c_cdvdman_bridge(
    c_kernel& kernel, c_folder_sector_source& source
) : m_kernel(kernel), m_source(source)
{
}

bool c_cdvdman_bridge::install()
{
    if (!m_source.valid() || m_source.sector_count() == 0u)
    {
        return false;
    }

    // Keep the original #10 address stable. Other CDVDMAN services have
    // distinct executable stubs in the reserved IOP kernel area.
    constexpr std::array<uint16_t, 14u> ordinals = {
        4u, 5u, 6u, 8u, 10u, 11u, 12u, 13u,
        15u, 24u, 28u, 37u, 38u, 75u
    };
    for (uint16_t ordinal : ordinals)
    {
        const uint32_t pc = ordinal == 10u
            ? k_search_file_stub
            : k_extra_stub_base + static_cast<uint32_t>(ordinal) * 0x20u;
        if (!create_stub(m_kernel.cpu(), pc, k_service_base + ordinal) ||
            !m_kernel.register_library("cdvdman", ordinal, pc))
        {
            return false;
        }
    }
    m_read_pending = false;
    m_drive_command = 0u;
    m_read_destination = 0u;
    m_read_data.clear();
    m_sync_waiters.clear();
    m_callback = 0u;
    m_callback_gp = 0u;
    m_callback_origin = {};
    m_callback_registration_reports = 0u;
    m_requested_media_mode = 0u;
    m_initialized = false;
    m_last_error = 0u;
    return true;
}

bool c_cdvdman_bridge::ram_pointer(
    uint32_t guest, uint32_t size, uint32_t* physical
)
{
    if (physical == nullptr)
    {
        return false;
    }
    const uint32_t region = guest & 0xe0000000u;
    if (region != 0u && region != 0x80000000u &&
        region != 0xa0000000u)
    {
        return false;
    }
    const uint32_t address = guest & 0x1fffffffu;
    if (address == 0u || address >= c_cpu::k_ram_size ||
        size > c_cpu::k_ram_size - address)
    {
        return false;
    }
    *physical = address;
    return true;
}

bool c_cdvdman_bridge::read_path(
    uint32_t pointer, std::string* path
) const
{
    uint32_t physical = 0u;
    if (path == nullptr || !ram_pointer(pointer, 1u, &physical))
    {
        return false;
    }
    std::string name;
    for (uint32_t i = 0u; i < 256u; ++i)
    {
        if (physical + i >= c_cpu::k_ram_size)
        {
            return false;
        }
        uint32_t word = 0u;
        const uint32_t address = physical + i;
        if (!m_kernel.cpu().read32(address & ~3u, &word))
        {
            return false;
        }
        const uint8_t byte = static_cast<uint8_t>(
            word >> ((address & 3u) * 8u)
        );
        if (byte == 0u)
        {
            if (name.empty())
            {
                return false;
            }
            *path = std::move(name);
            return true;
        }
        if (byte < 0x20u || byte > 0x7eu)
        {
            return false;
        }
        name.push_back(static_cast<char>(byte));
    }
    // A nonterminated path is not a lookup miss: refuse the syscall.
    return false;
}

bool c_cdvdman_bridge::handle_read_clock(uint32_t destination)
{
    uint32_t physical = 0u;
    if (!ram_pointer(destination, 8u, &physical))
    {
        return false;
    }

    // sceCdReadClock returns the eight-byte CDVD RTC layout in BCD.
    // The reference reports Japan Standard Time (UTC+9); deriving the
    // calendar after applying the offset also handles date rollover.
    constexpr std::time_t k_jst_offset = 9 * 60 * 60;
    const std::time_t now = std::time(nullptr);
    if (now == static_cast<std::time_t>(-1))
    {
        return false;
    }
    const std::time_t in_japan = now + k_jst_offset;
    std::tm calendar{};
#if defined(_WIN32)
    if (::gmtime_s(&calendar, &in_japan) != 0)
#else
    if (::gmtime_r(&in_japan, &calendar) == nullptr)
#endif
    {
        return false;
    }
    const uint32_t year = static_cast<uint32_t>(
        (calendar.tm_year + 1900) % 100);
    const std::array<uint8_t, 8u> rtc = {
        0u,
        decimal_to_bcd(static_cast<uint32_t>(calendar.tm_sec)),
        decimal_to_bcd(static_cast<uint32_t>(calendar.tm_min)),
        decimal_to_bcd(static_cast<uint32_t>(calendar.tm_hour)),
        0u,
        decimal_to_bcd(static_cast<uint32_t>(calendar.tm_mday)),
        decimal_to_bcd(static_cast<uint32_t>(calendar.tm_mon + 1)),
        decimal_to_bcd(year)
    };
    for (uint32_t i = 0u; i < rtc.size(); ++i)
    {
        if (!m_kernel.cpu().write8(destination + i, rtc[i]))
        {
            return false;
        }
    }
    return true;
}

void c_cdvdman_bridge::complete_read()
{
    if (!m_read_pending)
    {
        return;
    }
    // Copy only sectors already retrieved from the mounted disc.
    // The guest destination was validated before queueing the command.
    if (!m_kernel.cpu().dma_write(
            m_read_destination, m_read_data.data(),
            static_cast<uint32_t>(m_read_data.size())))
    {
        m_last_error = 1u;
    }
    m_read_data.clear();
    m_read_destination = 0u;
    m_read_pending = false;
    finish_command(1u);
}

bool c_cdvdman_bridge::start_drive_command(uint32_t function)
{
    // Standby, stop and pause are N-commands on real hardware: the call
    // returns 1 once the command is accepted (0 while the drive is busy),
    // completion raises the CDVD interrupt, and CDVDMAN then calls the
    // user callback with the SCECdFunc code. ARKD_DVD.IRX locks its drive
    // mutex before sceCdStandby and releases it only from that callback.
    if (!m_source.valid() || m_source.sector_count() == 0u || busy())
    {
        return false;
    }
    if (!m_kernel.schedule_hle_event(32u,
            [this]() { complete_drive_command(); }))
    {
        return false;
    }
    m_drive_command = function;
    m_last_error = 0u;
    return true;
}

void c_cdvdman_bridge::complete_drive_command()
{
    const uint32_t function = m_drive_command;
    if (function == 0u)
    {
        return;
    }
    m_drive_command = 0u;
    finish_command(function);
}

void c_cdvdman_bridge::finish_command(uint32_t function)
{
    const size_t sync_waiters = m_sync_waiters.size();

    for (const uint32_t id : m_sync_waiters)
    {
        const s_kernel_thread* thread = m_kernel.thread(id);
        if (thread != nullptr &&
            thread->m_status == e_thread_status::blocked &&
            thread->m_wait_type == 18u)
        {
            (void)m_kernel.wake_thread_with_result(id, 0u);
        }
    }
    m_sync_waiters.clear();

    // The guest callback runs only after the accepted command completed,
    // with its SCECdFunc reason (1 read, 5 standby, 6 stop, 7 pause).
    // It executes as an R3000 guest function and returns via a bounded
    // one-shot kernel trampoline. Never claim a callback was delivered
    // if its guest entry cannot be scheduled.
    const bool started = m_callback == 0u ||
        m_kernel.start_guest_callback(
            m_callback, m_callback_gp, function, m_callback_origin);
    if (!started)
    {
        m_last_error = 1u;
    }
    static const bool trace_cdvd = std::getenv("SDBZ_TRACE_CDVD") != nullptr;
    if (trace_cdvd)
    {
        std::fprintf(stderr,
            "IOP CDVD command complete: function=%u callback=0x%08x "
            "started=%d error=%u woken_sync=%zu cycles=%llu\n",
            function, m_callback, started ? 1 : 0, m_last_error,
            sync_waiters,
            static_cast<unsigned long long>(m_kernel.cycles()));
    }
}

bool c_cdvdman_bridge::handle_syscall(
    c_kernel& kernel, uint32_t service
)
{
    if (&kernel != &m_kernel)
    {
        return false;
    }

    s_cpu_state& state = m_kernel.cpu().state();
    switch (service)
    {
    case k_service_base + 4u: // sceCdInit
        // Only the already-mounted verified source is initialised.
        if (!m_source.valid() || m_source.sector_count() == 0u)
        {
            state.m_gpr[2] = 0u;
            return true;
        }
        m_initialized = true;
        m_last_error = 0u;
        state.m_gpr[2] = 1u;
        return true;

    case k_service_base + 5u: // sceCdStandby
        state.m_gpr[2] = start_drive_command(5u) ? 1u : 0u;
        return true;

    case k_service_base + 6u: // sceCdRead
    {
        const uint32_t lba = state.m_gpr[4];
        const uint32_t sectors = state.m_gpr[5];
        const uint32_t destination = state.m_gpr[6];
        const uint32_t mode = state.m_gpr[7];
        uint32_t physical = 0u;
        static const bool trace_cdvd = std::getenv("SDBZ_TRACE_CDVD") != nullptr;
        // Only 2048-byte sectors written to regular IOP RAM. Reject
        // unsupported sizes, alias regions and invalid disc extents
        // without touching the guest destination.
        const char* rejected =
            busy() ? "drive busy" :
            sectors == 0u ? "zero sectors" :
            sectors > c_cpu::k_ram_size / 2048u ? "too many sectors" :
            static_cast<uint64_t>(lba) + sectors > m_source.sector_count()
                ? "past end of disc" :
            !ram_pointer(destination, sectors * 2048u, &physical)
                ? "destination outside IOP RAM" :
            (mode != 0u && !ram_pointer(mode, 4u, &physical))
                ? "mode outside IOP RAM" : nullptr;
        if (trace_cdvd)
        {
            std::fprintf(stderr,
                "IOP CDVD sceCdRead lba=%u sectors=%u dst=0x%08x mode=0x%08x thread=%u %s\n",
                lba, sectors, destination, mode, m_kernel.current_thread_id(),
                rejected != nullptr ? rejected : "accepted");
        }
        if (rejected != nullptr)
        {
            state.m_gpr[2] = 0u;
            return true;
        }
        std::vector<uint8_t> data;
        if (!m_source.read_sectors(lba, sectors, data) ||
            data.size() != static_cast<size_t>(sectors) * 2048u)
        {
            m_last_error = 1u;
            state.m_gpr[2] = 0u;
            return true;
        }
        // The primary reference models 400us + 250us per sector.
        // Here IOP cycles count guest instructions, not calibrated
        // hardware time; use a bounded deterministic compatibility
        // delay instead of inventing a CDVD IRQ or host-time callback.
        const uint64_t delay = 32u + static_cast<uint64_t>(sectors) * 8u;
        if (!m_kernel.schedule_hle_event(delay,
                [this]() { complete_read(); }))
        {
            state.m_gpr[2] = 0u;
            return true;
        }
        m_read_data = std::move(data);
        m_read_destination = destination;
        m_read_pending = true;
        m_last_error = 0u;
        state.m_gpr[2] = 1u;
        return true;
    }

    case k_service_base + 8u: // sceCdGetError
        state.m_gpr[2] = m_last_error;
        return true;

    case k_service_base + 11u: // sceCdSync
        if (state.m_gpr[4] == 1u || state.m_gpr[4] == 17u)
        {
            state.m_gpr[2] = busy() ? 1u : 0u;
            return true;
        }
        if (busy())
        {
            const uint32_t id = m_kernel.current_thread_id();
            if (id == 0u || !m_kernel.block_current(18u))
            {
                return false;
            }
            m_sync_waiters.push_back(id);
        }
        state.m_gpr[2] = 0u;
        return true;

    case k_service_base + 12u: // sceCdGetDiskType
        // The mounted SDBZ virtual DVD has one deterministic disc type.
        state.m_gpr[2] = m_source.valid() &&
            m_source.sector_count() != 0u ? 0x14u : 0u;
        return true;

    case k_service_base + 13u: // sceCdDiskReady
        // 2 = SCECdComplete, 6 = SCECdNotReady while an N-command runs.
        // This checks the mounted sector backend, not physical hardware.
        state.m_gpr[2] = !m_source.valid() ||
            m_source.sector_count() == 0u ? 0u : busy() ? 6u : 2u;
        return true;

    case k_service_base + 15u: // sceCdStop
        state.m_gpr[2] = start_drive_command(6u) ? 1u : 0u;
        return true;

    case k_service_base + 24u: // sceCdReadClock
        if (!handle_read_clock(state.m_gpr[4]))
        {
            // Invalid guest output is an unsupported syscall boundary,
            // never a fabricated valid clock.
            return false;
        }
        state.m_gpr[2] = 1u;
        return true;

    case k_service_base + 28u: // sceCdStatus
        state.m_gpr[2] = busy() ? 0x06u :
            (m_source.valid() && m_source.sector_count() != 0u
                ? 0x0au : 0u);
        return true;

    case k_service_base + 37u: // sceCdCallback
    {
        // ABI: a0 is the new guest callback pointer, v0 returns old.
        // Save the registering thread's global pointer so the guest
        // callback can access its own module data when read completes.
        const uint32_t callback = state.m_gpr[4];
        uint32_t opcode = 0u;
        if (callback != 0u)
        {
            const uint32_t region = callback & 0xe0000000u;
            const uint32_t physical = callback & 0x1fffffffu;
            if ((callback & 3u) != 0u ||
                (region != 0u && region != 0x80000000u &&
                 region != 0xa0000000u) ||
                physical >= c_cpu::k_ram_size ||
                !m_kernel.cpu().read32(callback, &opcode))
            {
                return false;
            }
        }
        const uint32_t previous = m_callback;
        m_callback = callback;
        m_callback_gp = callback != 0u ? state.m_gpr[28] : 0u;
        m_callback_origin = callback != 0u ?
            m_kernel.make_guest_callback_origin(
                e_guest_callback_source::cdvdman_read) :
            s_guest_callback_origin{};
        if (m_callback_registration_reports < 32u)
        {
            ++m_callback_registration_reports;
            std::fprintf(stderr,
                "IOP CDVDMAN callback registered: "
                "callback=0x%08x first_word=0x%08x previous=0x%08x "
                "thread=%u syscall_pc=0x%08x ra=0x%08x gp=0x%08x\n",
                callback, opcode, previous,
                m_callback_origin.m_registration_thread_id,
                m_callback_origin.m_registration_pc,
                m_callback_origin.m_registration_ra,
                m_callback_origin.m_registration_gp);
        }
        state.m_gpr[2] = previous;
        return true;
    }

    case k_service_base + 38u: // sceCdPause
        state.m_gpr[2] = start_drive_command(7u) ? 1u : 0u;
        return true;

    case k_service_base + 75u: // sceCdMmode
        // The only mounted medium is a DVD. 0 selects auto and 2
        // selects DVD; CD-only mode 1 cannot change this source.
        if (state.m_gpr[4] != 0u && state.m_gpr[4] != 2u)
        {
            state.m_gpr[2] = 0u;
            return true;
        }
        m_requested_media_mode = state.m_gpr[4];
        state.m_gpr[2] = m_source.valid() ? 1u : 0u;
        return true;

    case k_search_file_syscall:
        break;
    default:
        return false;
    }

    // sceCdSearchFile(sceCdlFILE* output, const char* name)
    // sceCdlFILE = u32 lsn, u32 size, char name[16], u8 date[8].
    uint32_t destination = 0u;
    std::string name;
    if ((state.m_gpr[4] & 3u) != 0u ||
        !ram_pointer(state.m_gpr[4], 32u, &destination) ||
        !read_path(state.m_gpr[5], &name))
    {
        return false;
    }

    // cdvdman accepts ISO paths such as "\\SYSTEM.CNF;1", not only
    // LOADFILE's fully qualified "cdrom0:\\..." paths.
    const bool qualified = name.size() >= 7u &&
        (name.substr(0u, 7u) == "cdrom0:" ||
         name.substr(0u, 7u) == "CDROM0:");
    const std::string guest_path = qualified ? name : "cdrom0:" + name;
    s_folder_file_info file{};
    const bool found = m_source.find_file(guest_path, &file);
    static const bool trace_search = std::getenv("SDBZ_TRACE_CDVD") != nullptr;
    if (trace_search)
    {
        std::fprintf(stderr,
            "IOP CDVD sceCdSearchFile %s found=%d lba=%u size=%u thread=%u\n",
            guest_path.c_str(), found ? 1 : 0, found ? file.m_lba : 0u,
            found ? file.m_size : 0u, m_kernel.current_thread_id());
    }
    if (!found)
    {
        // A missing/unsupported filename is a genuine search miss.
        // Leave the caller's output structure untouched.
        kernel.cpu().state().m_gpr[2] = 0u;
        return true;
    }

    // Probe the complete destination before writing any guest bytes.
    // The bridge writes only ordinary physical IOP RAM, never MMIO.
    uint32_t ignored = 0u;
    for (uint32_t i = 0u; i < 8u; ++i)
    {
        if (!kernel.cpu().read32(destination + i * 4u, &ignored))
        {
            return false;
        }
    }
    uint8_t bytes[32]{};
    put32(bytes, file.m_lba);
    put32(bytes + 4u, file.m_size);
    const std::string file_name = file.m_name + ";1";
    const size_t length = (std::min)(file_name.size(), size_t(15u));
    std::memcpy(bytes + 8u, file_name.data(), length);

    // Match the deterministic ISO9660 directory records emitted by
    // c_folder_sector_source: 2026-01-01 00:00:00, no directory flag.
    // date[] = flags, seconds, minutes, hours, day, month, year LE16.
    bytes[28] = 1u;
    bytes[29] = 1u;
    bytes[30] = 0xeau;
    bytes[31] = 0x07u;
    for (uint32_t i = 0u; i < 8u; ++i)
    {
        const uint32_t offset = i * 4u;
        const uint32_t word =
            static_cast<uint32_t>(bytes[offset]) |
            (static_cast<uint32_t>(bytes[offset + 1u]) << 8u) |
            (static_cast<uint32_t>(bytes[offset + 2u]) << 16u) |
            (static_cast<uint32_t>(bytes[offset + 3u]) << 24u);
        if (!kernel.cpu().write32(destination + offset, word))
        {
            return false;
        }
    }
    kernel.cpu().state().m_gpr[2] = 1u;
    return true;
}
} // namespace iop
