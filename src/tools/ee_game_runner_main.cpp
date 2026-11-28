Warning: truncated output (original token count: 128470)
Total output lines: 11119

#include "ee_scanout_clock.h"
#include "ee_runtime.h"
#include "ee_dispatch_index.h"
#include "ee_native_target_cache.h"
#include "ee_runtime_profile.h"
#include "ee_pc_profile.h"
#include "ee_host_audio.h"
#include "ee_vu1_aot.h"
#include "ee_exceptions.h"
#include "ee_memory.h"
#include "ee_ram_interpreter.h"
#include "ee_hw_bus.h"
#include "ee_host_timers.h"
#include "ee_iop_clock.h"
#include "ee_iop_service_attribution.h"
#include "ee_execution_horizon.h"
#include "ee_hot_expand_protocol.h"
#include "ee_kernel_thread_status.h"
#include "ee_irq_syscall_services.h"
#include "ee_hot_loop_diagnostics.h"
#include "ee_host_loop_yield_budget.h"
#include "ee_idle_loop_skip.h"
#include "ee_pad_script.h"
#include "ee_reached_isa_inventory.h"
#include "ee_vsync_flags.h"
#include "ee_sdbz_dma_wait_probe.h"
#include "ee_sdbz_slot_callback_probe.h"
#include "ee_sdbz_pad_return_probe.h"
#include "ee_sdbz_spawn_probe.h"
#include "ee_gif.h"
#include "ee_gs.h"
#include "ee_gs_display.h"
#include "ee_gs_presentation_probe.h"
#ifdef EE_ENABLE_WEBGPU
#include "ee_webgpu_bridge.h"
#include "ps2_webgpu/gs/gpu/gpu.hpp"
#endif
#ifdef EE_ENABLE_RAYLIB_WINDOW
#include "ee_raylib_window.h"
#include "ee_window_pump_gate.h"
#include "ee_frame_pacer.h"
#endif
#include "elf_loader.h"
#include "iop_kernel.h"
#include "iop_pad_poll_replay.h"
#include "iop_aot.h"
#include "iop_memory_card.h"
#include "iop_guest_fault_probe.h"
#include "iop_cdvd.h"
#include "iop_cdvd_rtc.h"
#include "iop_folder_sector_source.h"
#include "iop_cdvdman_bridge.h"
#include "iop_sifman_bridge.h"
#include "iop_sif1_bridge.h"
#include "iop_sif1_dma_transport.h"
#include "iop_sif0_dma_transport.h"
#include "iop_sifcmd_bridge.h"
#include "iop_kernel_hle_bridge.h"
#include "iop_disc_loader.h"
#include "iop_sif_rpc_bridge.h"
#include "iop_sif_mmio_link.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace
{

// Host-only CRT adapters: never change emitted guest LLVM IR or cache keys.
FILE* open_host_file(const char* path, const char* mode)
{
    FILE* result = nullptr;
#if defined(_WIN32)
    if (path != nullptr && mode != nullptr)
    {
        (void)::fopen_s(&result, path, mode);
    }
#else
    if (path != nullptr && mode != nullptr)
    {
        result = std::fopen(path, mode);
    }
#endif
    return result;
}

const char* read_host_environment(
    const char* name, std::string* storage
)
{
    const char* result = nullptr;
    if (name != nullptr && storage != nullptr)
    {
#if defined(_WIN32)
        char* allocated = nullptr;
        size_t length = 0u;
        if (::_dupenv_s(&allocated, &length, name) == 0 &&
            allocated != nullptr)
        {
            *storage = allocated;
            result = storage->c_str();
        }
        std::free(allocated);
#else
        const char* value = std::getenv(name);
        if (value != nullptr)
        {
            *storage = value;
            result = storage->c_str();
        }
#endif
    }
    return result;
}

constexpr uint32_t k_ee_ram_size = 32u * 1024u * 1024u;
constexpr uint32_t k_default_stack = 0x01fff000u;
constexpr uint32_t k_default_dispatch_budget = 10000u;
constexpr uint32_t k_report_history_limit = 64u;
constexpr uint64_t k_report_snapshot_interval = 128u;
constexpr uint64_t k_report_progress_interval = 256u;
constexpr uint32_t k_kernel_ram_snapshot_size = 0x80000u;
constexpr uint32_t k_instruction_trace_limit = 65536u;
// ps2-web-recomp runtime/src/kernel.cpp uses this guest-visible HLE
// syscall table. Kernel addresses 0x80001800..0x80001BFF alias it.
constexpr uint32_t k_kernel_syscall_table_base = 0x00001800u;
constexpr uint32_t k_kernel_syscall_table_entries = 0x100u;

// HLE IOP reboot may select an IOPRP ROMDIR image from the extracted
// disc. Only actual mounted files are eligible; unknown ROM0 programs
// or inaccessible images are not reported as successfully booted.
bool validate_hle_iop_reboot(
    const std::string& command_line,
    const iop::c_folder_sector_source& sectors,
    std::string* reason
)
{
    if (reason == nullptr)
    {
        return false;
    }
    if (command_line.empty())
    {
        return true; // SifIopReset("", 0): default HLE kernel bootstrap.
    }

    std::istringstream arguments(command_line);
    std::string loader;
    arguments >> loader;
    std::transform(
        loader.begin(), loader.end(), loader.begin(),
        [](unsigned char ch)
        {
            return static_cast<char>(std::tolower(ch));
        }
    );
    if (loader != "rom0:udnl")
    {
        *reason = "unsupported IOP reset loader (expected rom0:UDNL)";
        return false;
    }

    std::string path;
    while (arguments >> path)
    {
        if (path == "-v")
        {
            continue;
        }
        const auto device_end = path.find(':');
        if (device_end == std::string::npos ||
            path.substr(0u, device_end) != "cdrom0")
        {
            *reason = "unsupported IOP reboot image device: " + path;
            return false;
        }
        iop::s_folder_file_info image{};
        if (!sectors.find_file(path, &image) || image.m_size == 0u)
        {
            *reason = "missing or invalid IOP reboot image: " + path;
            return false;
        }
    }
    return true;
}


// Inspect guest IOP contexts only at bounded EE WaitSema checkpoints.
// A loaded IRX's module/thread "started" bit does not prove the
// scheduler ever entered its code or reached sceSifRegisterRpc.
void dump_iop_rpc_wait_threads(
    const iop::c_kernel& kernel, uint32_t semaphore_id, uint32_t turn
)
{
    std::fprintf(stderr,
        "IOP RPC wait snapshot: sema=%u turn=%u cycles=%llu "
        "threads=%zu modules=%zu\n",
        semaphore_id, turn,
        static_cast<unsigned long long>(kernel.cycles()),
        kernel.threads().size(), kernel.modules().size());

    for (const auto& item : kernel.threads())
    {
        const iop::s_kernel_thread& thread = item.second;
        const uint32_t pc = thread.m_cpu.m_pc;
        const uint32_t region = pc & 0xe0000000u;
        const uint32_t physical = pc & 0x1fffffffu;
        const bool ram_pc =
            (region == 0u || region == 0x80000000u ||
             region == 0xa0000000u) &&
            (pc & 3u) == 0u &&
            physical < iop::c_cpu::k_ram_mirror_size;
        const uint32_t backing =
            physical & (iop::c_cpu::k_ram_size - 1u);
        uint32_t module_id = 0u;
        if (ram_pc)
        {
            for (const auto& module : kernel.modules())
            {
                const iop::s_irx_module& irx = module.second.m_irx;
                if (backing >= irx.m_base &&
                    backing - irx.m_base < irx.m_size)
                {
                    module_id = module.first;
                    break;
                }
            }
        }
        uint32_t opcode = 0u;
        const bool fetched = ram_pc && kernel.cpu().read32(pc, &opcode);
        std::fprintf(stderr,
            "IOP RPC wait thread: id=%u status=%u priority=%u "
            "wait=%u wake=%llu entry=0x%08x "
            "pc=0x%08x next=0x%08x backing=0x%08x "
            "opcode=0x%08x fetch=%u image_module=%u "
            "ra=0x%08x gp=0x%08x sp=0x%08x\n",
            item.first, static_cast<uint32_t>(thread.m_status),
            thread.m_priority, thread.m_wait_type,
            static_cast<unsigned long long>(thread.m_wake_cycle),
            thread.m_entry, pc, thread.m_cpu.m_next_pc, backing,
            opcode, fetched ? 1u : 0u, module_id,
            thread.m_cpu.m_gpr[31], thread.m_cpu.m_gpr[28],
            thread.m_cpu.m_gpr[29]);
    }
} 

// Side-effect-free, bounded checkpoint for an outstanding original IOP
// RPC. It distinguishes an actively spinning guest PC from a blocked
// semaphore worker without guessing a result or waking any thread.
void dump_pending_iop_guest_rpc(
    const iop::c_kernel& kernel, uint64_t ee_dispatches,
    size_t pending_calls, const char* phase
)
{
    uint32_t ready = 0u;
    uint32_t blocked = 0u;
    uint32_t faulted = 0u;
    uint32_t highest_ready_priority =
        std::numeric_limits<uint32_t>::max();
    for (const auto& entry : kernel.threads())
    {
        switch (entry.second.m_status)
        {
        case iop::e_thread_status::ready:
        case iop::e_thread_status::running:
            ++ready;
            highest_ready_priority = std::min(
                highest_ready_priority, entry.second.m_priority);
            break;
        case iop::e_thread_status::blocked:
            ++blocked;
            break;
        case iop::e_thread_status::faulted:
            ++faulted;
            break;
        default:
            break;
        }
    }

    std::fprintf(stderr,
        "IOP guest RPC pending snapshot: phase=%s ee_dispatch=%llu "
        "iop_cycles=%llu pending_calls=%zu guest_work=%u "
        "ready=%u blocked=%u faulted=%u threads=%zu "
        "sio2_ctrl_starts=%u sio2_packets=%u sio2_completed=%u\n",
        phase, static_cast<unsigned long long>(ee_dispatches),
        static_cast<unsigned long long>(kernel.cycles()),
        pending_calls, kernel.has_guest_work() ? 1u : 0u,
        ready, blocked, faulted, kernel.threads().size(),
        kernel.hardware().sio2_ctrl_starts(),
        kernel.hardware().sio2_packets(),
        kernel.hardware().sio2_completed_transfers());

    uint32_t sampled_opcode_windows = 0u;
    for (const auto& entry : kernel.threads())
    {
        const iop::s_kernel_thread& thread = entry.second;
        if (thread.m_status == iop::e_thread_status::dormant)
        {
            continue;
        }
        const uint32_t pc = thread.m_cpu.m_pc;
        uint32_t opcode = 0u;
        const bool fetched = kernel.cpu().read32(pc, &opcode);
        std::fprintf(stderr,
            "IOP guest RPC pending context: thread=%u status=%u "
            "priority=%u wait=%u wake=%llu suspended=%u "
            "entry=0x%08x pc=0x%08x next=0x%08x "
            "opcode=0x%08x fetch=%u ra=0x%08x gp=0x%08x "
            "sp=0x%08x v0=0x%08x a0=0x%08x a1=0x%08x "
            "guest_insns=%llu slices=%llu syscalls=%llu "
            "self_restarts=%llu restart_ra=0x%08x "
            "restart_arg=0x%08x last_syscall=0x%08x "
            "last_syscall_pc=0x%08x last_syscall_ra=0x%08x "
            "last_syscall_a0=0x%08x\n",
            entry.first, static_cast<uint32_t>(thread.m_status),
            thread.m_priority, thread.m_wait_type,
            static_cast<unsigned long long>(thread.m_wake_cycle),
            thread.m_interrupts_suspended ? 1u : 0u,
            thread.m_entry, pc, thread.m_cpu.m_next_pc,
            opcode, fetched ? 1u : 0u, thread.m_cpu.m_gpr[31],
            thread.m_cpu.m_gpr[28], thread.m_cpu.m_gpr[29],
            thread.m_cpu.m_gpr[2], thread.m_cpu.m_gpr[4],
            thread.m_cpu.m_gpr[5],
            static_cast<unsigned long long>(thread.m_guest_instructions),
            static_cast<unsigned long long>(thread.m_guest_slices),
            static_cast<unsigned long long>(thread.m_handled_syscalls),
            static_cast<unsigned long long>(thread.m_self_restarts),
            thread.m_last_self_restart_ra, thread.m_last_self_restart_arg,
            thread.m_last_syscall_number, thread.m_last_syscall_pc,
            thread.m_last_syscall_ra, thread.m_last_syscall_a0);

        // At most two highest-priority runnable guest PCs.
        // Previously iterating IDs captured lower-priority ARKD
        // workers instead of the higher-priority PADMAN threads
        // responsible for consuming the budget. The full context
        // lines above still include every non-dormant thread.
        // Do not synthesize any instruction, byte, return or wakeup.
        if (phase[0] == 'f' &&
            (thread.m_status == iop::e_thread_status::ready ||
             thread.m_status == iop::e_thread_status::running) &&
            thread.m_priority == highest_ready_priority &&
            sampled_opcode_windows < 2u &&
            pc >= 0x100u &&
            pc < iop::c_cpu::k_ram_size - 0x180u)
        {
            ++sampled_opcode_windows;
            for (uint32_t group = 0u; group < 8u; ++group)
            {
                const uint32_t first = group * 4u;
                std::fprintf(stderr,
                    "IOP guest RPC pending registers: thread=%u "
                    "r%02u=0x%08x r%02u=0x%08x "
                    "r%02u=0x%08x r%02u=0x%08x\n",
                    entry.first,
                    first, thread.m_cpu.m_gpr[first],
                    first + 1u, thread.m_cpu.m_gpr[first + 1u],
                    first + 2u, thread.m_cpu.m_gpr[first + 2u],
                    first + 3u, thread.m_cpu.m_gpr[first + 3u]);
            }
            const uint32_t fp = thread.m_cpu.m_gpr[30];
            if ((fp & 3u) == 0u &&
                fp != 0u &&
                fp < iop::c_cpu::k_ram_size - 0x20u)
            {
                for (uint32_t offset = 0x10u;
                     offset <= 0x1cu; offset += 4u)
                {
                    uint32_t value = 0u;
                    if (!kernel.cpu().read32(fp + offset, &value))
                    {
                        break;
                    }
                    std::fprintf(stderr,
                        "IOP guest RPC pending stack: thread=%u "
                        "fp=0x%08x offset=0x%02x word=0x%08x\n",
                        entry.first, fp, offset, value);
                }
            }
            // The 09:11 retail opcode window ended before the ARKD
            // inner-loop exit branch. Preserve +/-256 bytes around
            // each sampled PC to include both the body and its bounds.
            const uint32_t start = (pc - 0x100u) & ~3u;
            for (uint32_t index = 0u; index < 129u; ++index)
            {
                const uint32_t address = start + index * 4u;
                uint32_t word = 0u;
                if (!kernel.cpu().read32(address, &word))
                {
                    break;
                }
                std::fprintf(stderr,
                    "IOP guest RPC pending opcode: thread=%u "
                    "address=0x%08x word=0x%08x\n",
                    entry.first, address, word);
            }
        }
    }
}

using t_game_run = uint32_t (*)(

    ee::s_ee_cpu_state*,
    ee::s_ee_runtime*,
    uint32_t,
    uint32_t*,
    uint32_t*,
    uint32_t*
);

using t_game_resolve_entry = ee::t_ee_translated_entry (*)(uint32_t);
using t_game_resolve_owner = uint32_t (*)(uint32_t, ee::t_ee_translated_entry);
using t_game_alu_words = const uint32_t* (*)(uint32_t, ee::t_ee_translated_entry, uint32_t*);

struct c_native_module
{
#if defined(_WIN32)
    HMODULE m_handle = nullptr;
#else
    void* m_handle = nullptr;
#endif

    ~c_native_module()
    {
#if defined(_WIN32)
        if (m_handle != nullptr)
        {
            FreeLibrary(m_handle);
        }
#else
        if (m_handle != nullptr)
        {
            dlclose(m_handle);
        }
#endif
    }

    bool open(const char* path)
    {
#if defined(_WIN32)
        m_handle = LoadLibraryA(path);
#else
        m_handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
        return m_handle != nullptr;
    }

    void swap(c_native_module& other)
    {
        std::swap(m_handle, other.m_handle);
    }

    t_game_run entry() const
    {
#if defined(_WIN32)
        return reinterpret_cast<t_game_run>(
            GetProcAddress(m_handle, "ee_game_run")
        );
#else
        return reinterpret_cast<t_game_run>(dlsym(m_handle, "ee_game_run"));
#endif
    }

    t_game_resolve_entry resolver() const
    {
#if defined(_WIN32)
        return reinterpret_cast<t_game_resolve_entry>(
            GetProcAddress(m_handle, "ee_game_resolve_entry"));
#else
        return reinterpret_cast<t_game_resolve_entry>(
            dlsym(m_handle, "ee_game_resolve_entry"));
#endif
    }

    t_game_resolve_owner owner_resolver() const
    {
#if defined(_WIN32)
        return reinterpret_cast<t_game_resolve_owner>(
            GetProcAddress(m_handle, "ee_game_resolve_owner"));
#else
        return reinterpret_cast<t_game_resolve_owner>(
            dlsym(m_handle, "ee_game_resolve_owner"));
#endif
    }

    t_game_alu_words alu_certificate() const
    {
#if defined(_WIN32)
        return reinterpret_cast<t_game_alu_words>(
            GetProcAddress(m_handle, "ee_game_alu_words"));
#else
        return reinterpret_cast<t_game_alu_words>(
            dlsym(m_handle, "ee_game_alu_words"));
#endif
    }

    void print_error() const
    {
#if defined(_WIN32)
        std::fprintf(stderr, "native module load failed (Win32 error %lu)\n",
                     static_cast<unsigned long>(GetLastError()));
#else
        const char* message = dlerror();
        std::fprintf(stderr, "native module load failed: %s\n",
                     message != nullptr ? message : "unknown dynamic loader error");
#endif
    }
};

// No guest machine state is serialized here.  On a zero-dispatch cache miss
// the native runner stays alive, and the compiler publishes a new DLL/SO.  This
// preserves EE/IOP memory, pending callbacks, DMA, GPU, timers, and pad/card
// state in their owning objects.  Unique request IDs and atomic file renames
// prevent consumption of stale or incomplete responses.
// A response of "overlay=<path>" names a small library holding ONLY the newly
// translated functions. The runner keeps the current module loaded and adds
// the overlay behind it, instead of swapping in a full relinked library.
bool request_hot_expand(
    const std::filesystem::path& directory,
    uint32_t sequence,
    uint32_t pc,
    std::filesystem::path* library,
    bool* overlay,
    std::string* failure)
{
    if (library == nullptr || overlay == nullptr || failure == nullptr)
    {
        return false;
    }
    *overlay = false;
    const std::string key = std::to_string(sequence);
    const std::filesystem::path request = directory / ("request-" + key + ".txt");
    const std::filesystem::path pending = directory / ("request-" + key + ".tmp");
    const std::filesystem::path response = directory / ("response-" + key + ".txt");
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
    {
        *failure = "cannot create hot-expand directory: " + error.message();
        return false;
    }
    {
        std::ofstream output(pending, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            *failure = "cannot write hot-expand request";
            return false;
        }
        char address[16]{};
        std::snprintf(address, sizeof(address), "0x%08x", pc);
        output << sequence << '\n' << address << '\n';
        output.close();
        if (!output)
        {
            *failure = "incomplete hot-expand request";
            return false;
        }
    }
    std::filesystem::rename(pending, request, error);
    if (error)
    {
        *failure = "cannot publish hot-expand request: " + error.message();
        return false;
    }
    std::printf("EE hot-expand: paused at missing guest pc=0x%08x request=%u\n",
                pc, sequence);
    std::fflush(stdout);

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::minutes(30);
    while (std::chrono::steady_clock::now() < deadline)
    {
        std::ifstream input(response, std::ios::binary);
        if (input)
        {
            std::string result;
            if (!hot_expand::parse_response(input, key, &result, failure))
            {
                return false;
            }
            if (result == "STOP")
            {
                library->clear();
                return true;
            }
            if (result.compare(0, 6, "ERROR:") == 0)
            {
                *failure = result;
                return false;
            }
            static const std::string overlay_prefix = "overlay=";
            if (result.compare(0, overlay_prefix.size(), overlay_prefix) == 0)
            {
                *overlay = true;
                result.erase(0, overlay_prefix.size());
            }
            *library = std::filesystem::path(result);
            if (!library->is_absolute() ||
                !std::filesystem::is_regular_file(*library))
            {
                *failure = "hot-expand replacement library missing or not absolute";
                return false;
            }
            return true;
        }
        // The compiler answers in well under a second once only new
        // functions are linked; a coarse poll would dominate that latency.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    *failure = "timed out waiting for hot-expand compiler";
    return false;
}

bool parse_u32(const char* value, uint32_t* out)
{
    if (value == nullptr || *value == '\0' || *value == '-')
    {
        return false;
    }

    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0' ||
        parsed > (std::numeric_limits<uint32_t>::max)())
    {
        return false;
    }

    *out = static_cast<uint32_t>(parsed);
    return true;
}

struct s_runner_vu0_control
{
    ee::c_ee_vif1* m_vif1;
    ee::c_ee_dmac* m_dmac;
    ee::s_ee_runtime* m_runtime;
    ee::s_ee_vu0_ialu_engine m_engine{};
    ee::s_ee_vu0_scheduler m_scheduler{};
    uint64_t m_last_attempt = 0u;
    uint64_t m_next_attempt = 0u;
    uint64_t m_cmsar1_last_attempt = 0u;
    uint64_t m_callback_generation = 0u;
};

bool runner_service_cmsar1(s_runner_vu0_control& context, ee::s_ee_vu0_state& vu0)
{
    if (!context.m_vif1->cmsar1_running())
    {
        return true;
    }
    const uint64_t now = context.m_runtime->m_guest_instruction_attempts;
    if (now < context.m_cmsar1_last_attempt)
    {
        return false;
    }
    // Use reached EE instructions as the existing approximate compatibility
    // budget, not wall time or native dispatch count. Bound each host turn.
    const uint32_t pairs = static_cast<uint32_t>(
        std::min<uint64_t>(now - context.m_cmsar1_last_attempt, 4096u));
    if (pairs == 0u)
    {
        return true;
    }
    const auto result = context.m_vif1->service_cmsar1(pairs);
    context.m_cmsar1_last_attempt += pairs;
    if (result == ee::e_ee_vu1_slice::rejected)
    {
        return false;
    }
    if (result == ee::e_ee_vu1_slice::completed)
    {
        vu0.m_vpu_status &= ~0x100u;
    }
    else
    {
        vu0.m_vpu_status |= 0x100u;
    }
    vu0.m_vi[29] = vu0.m_vpu_status;
    return true;
}

uint32_t runner_vu0_execute(
    void* user, ee::s_ee_cpu_state* cpu, ee::s_ee_vu0_state* vu0,
    uint64_t cycles, uint32_t flags, uint32_t pc)
{
    auto* context = static_cast<s_runner_vu0_control*>(user);
    return ee::ee_vu0_execute_ialu_callback(
        &context->m_engine, cpu, vu0, cycles, flags, pc);
}

uint32_t runner_vu0_vu1_catch_up(
    void* user, ee::s_ee_cpu_state*, ee::s_ee_vu0_state*, uint32_t)
{
    auto* context = static_cast<s_runner_vu0_control*>(user);
    const auto result = context->m_dmac->service_deferred_vif1(true);
    return static_cast<uint32_t>(
        result == ee::c_ee_dmac::e_deferred_vif1::rejected ||
        result == ee::c_ee_dmac::e_deferred_vif1::pending
            ? ee::e_ee_vu0_sync_result::UNAVAILABLE
            : ee::e_ee_vu0_sync_result::COMPLETE);
}

void runner_vu0_schedule_event(
    void* user, ee::s_ee_cpu_state*, ee::s_ee_vu0_state*,
    uint32_t delta, uint32_t)
{
    auto* context = static_cast<s_runner_vu0_control*>(user);
    context->m_next_attempt = context->m_runtime->m_guest_instruction_attempts + delta;
}

uint32_t runner_vu0_sync(
    void* user, ee::s_ee_cpu_state* cpu, ee::s_ee_vu0_state* vu0,
    uint32_t mode, uint32_t pc)
{
    using result = ee::e_ee_vu0_sync_result;
    if (cpu == nullptr || vu0 == nullptr ||
        mode > static_cast<uint32_t>(ee::e_ee_vu0_sync_mode::WAIT_TO_E_OR_M))
    {
        return static_cast<uint32_t>(result::INVALID_ARGUMENT);
    }
    auto* context = static_cast<s_runner_vu0_control*>(user);
    if (context != nullptr && !runner_service_cmsar1(*context, *vu0))
    {
        return static_cast<uint32_t>(result::FAILED);
    }
    // Preserve the hot halted macro path used by retail code.
    if ((vu0->m_vpu_status & 1u) == 0u && vu0->m_running == 0u)
    {
        return static_cast<uint32_t>(result::COMPLETE);
    }
    if (context == nullptr)
    {
        return static_cast<uint32_t>(result::UNAVAILABLE);
    }
    // Instruction attempts are the runner's existing reference-compatible
    // budget, not calibrated R5900 issue cycles. Do not install a hardware
    // clock or report these as such.
    const uint64_t now = context->m_runtime->m_guest_instruction_attempts;
    if (now < context->m_last_attempt ||
        now - context->m_last_attempt > UINT64_MAX - vu0->m_ee_cycle)
    {
        return static_cast<uint32_t>(result::FAILED);
    }
    vu0->m_ee_cycle += now - context->m_last_attempt;
    context->m_last_attempt = now;
    const uint32_t synced = ee::ee_vu0_scheduler_sync_callback(
        &context->m_scheduler, cpu, vu0, mode, pc);
    if (vu0->m_running != 0u && context->m_next_attempt <= now)
    {
        context->m_next_attempt = now + 1u;
    }
    return synced;
}

// Install only when profiling: the normal callback remains exactly
// runner_vu0_sync. Classify by entry state, not by the eventual result.
uint32_t runner_vu0_profiled_sync(
    void* user, ee::s_ee_cpu_state* cpu, ee::s_ee_vu0_state* vu0,
    uint32_t mode, uint32_t pc)
{
    using id = ee::e_runtime_profile_id;
    auto category = id::vu0_sync_invalid;
    if (cpu != nullptr && vu0 != nullptr &&
        mode <= static_cast<uint32_t>(ee::e_ee_vu0_sync_mode::WAIT_TO_E_OR_M))
    {
        const auto* context = static_cast<s_runner_vu0_control*>(user);
        if (context != nullptr && context->m_vif1->cmsar1_running())
        {
            category = id::vu0_sync_cmsar1;
        }
        else if ((vu0->m_vpu_status & 1u) == 0u && vu0->m_running == 0u)
        {
            category = id::vu0_sync_halted;
        }
        else
        {
            category = id::vu0_sync_micro;
        }
    }
    ee::c_runtime_profile_scope profile_scope(category);
    return runner_vu0_sync(user, cpu, vu0, mode, pc);
}

uint32_t runner_vu0_start(
    void* user, ee::s_ee_cpu_state* cpu, ee::s_ee_vu0_state* vu0,
    uint32_t address, uint32_t pc)
{
    auto* context = static_cast<s_runner_vu0_control*>(user);
    if (context == nullptr || vu0 == nullptr || cpu == nullptr ||
        context->m_runtime->m_ee_cycle_scheduler.source() !=
            ee::e_ee_cycle_source::REFERENCE_COMPATIBILITY)
    {
        return static_cast<uint32_t>(ee::e_ee_vu0_sync_result::UNAVAILABLE);
    }
    const uint64_t now = context->m_runtime->m_guest_instruction_attempts;
    const uint64_t prior_ee_cycle = vu0->m_ee_cycle;
    vu0->m_ee_cycle = std::max(now, vu0->m_ee_cycle);
    const uint32_t started = ee::ee_vu0_start_ialu_callback(
        &context->m_engine, cpu, vu0, address, pc);
    if (started == static_cast<uint32_t>(ee::e_ee_vu0_sync_result::COMPLETE))
    {
        context->m_last_attempt = now;
        context->m_next_attempt = now + 1u;
    }
    else
    {
        vu0->m_ee_cycle = prior_ee_cycle;
    }
    return started;
}

// CMSAR1 uses the resident VU1 engine, but is not a VIF command.
uint32_t runner_vu0_control(
    void* user, ee::s_ee_cpu_state* cpu, ee::s_ee_vu0_state* vu0,
    uint32_t register_index, uint32_t value, uint32_t
)
{
    if (cpu == nullptr || vu0 == nullptr)
    {
        return static_cast<uint32_t>(ee::e_ee_vu0_sync_result::INVALID_ARGUMENT);
    }
    if (register_index == 31u)
    {
        auto* context = static_cast<s_runner_vu0_control*>(user);
        if (context == nullptr)
        {
            return static_cast<uint32_t>(ee::e_ee_vu0_sync_result::UNAVAILABLE);
        }
        const auto pending = context->m_dmac->service_deferred_vif1(true);
        if (pending == ee::c_ee_dmac::e_deferred_vif1::rejected ||
            pending == ee::c_ee_dmac::e_deferred_vif1::pending ||
            !context->m_vif1->start_cmsar1(value))
        {
            return static_cast<uint32_t>(ee::e_ee_vu0_sync_result::UNAVAILABLE);
        }
        // CMSAR1 accepts the launch, not completion. Subsequent EE
        // execution advances the resident pipeline until a real E-bit.
        context->m_cmsar1_last_attempt = context->m_runtime->m_guest_instruction_attempts;
        vu0->m_vpu_status = (vu0->m_vpu_status & ~0xff00u) | 0x100u;
        vu0->m_vi[29] = vu0->m_vpu_status;
        return static_cast<uint32_t>(ee::e_ee_vu0_sync_result::COMPLETE);
    }
    if (register_index != 28u)
    {
        return static_cast<uint32_t>(ee::e_ee_vu0_sync_result::UNAVAILABLE);
    }
    auto* context = static_cast<s_runner_vu0_control*>(user);
    if (context == nullptr)
    {
        return static_cast<uint32_t>(ee::e_ee_vu0_sync_result::UNAVAILABLE);
    }
    // A worker owns a transactional VU1 run until publication. We cannot
    // interrupt it mid-pair or silently publish its uncommitted effects.
    if ((value & 0x300u) != 0u &&
        !context->m_vif1->control_vu_break_reset(value))
    {
        return static_cast<uint32_t>(ee::e_ee_vu0_sync_result::UNAVAILABLE);
    }
    if ((value & 3u) != 0u)
    {
        context->m_next_attempt = 0u;
        context->m_last_attempt = context->m_runtime->m_guest_instruction_attempts;
    }
    // The architectural FBRST/status updates are owned by ee_vu0_ctc2.
    return static_cast<uint32_t>(ee::e_ee_vu0_sync_result::COMPLETE);
}


bool write_gs_ppm(
    const char* path, const std::vector<uint32_t>& pixels,
    uint32_t width, uint32_t height
)
{
    if (path == nullptr || width == 0u || height == 0u ||
        static_cast<uint64_t>(width) * height != pixels.size())
    {
        return false;
    }
    FILE* output = open_host_file(path, "wb");
    if (output == nullptr)
    {
        return false;
    }
    bool success = std::fprintf(
        output, "P6\n%u %u\n255\n", width, height) > 0;
    for (uint32_t color : pixels)
    {
        const uint8_t rgb[3] =
        {
            static_cast<uint8_t>(color),
            static_cast<uint8_t>(color >> 8u),
            static_cast<uint8_t>(color >> 16u)
        };
        if (!success || std::fwrite(rgb, 1u, 3u, output) != 3u)
        {
            success = false;
            break;
        }
    }
    if (std::fclose(output) != 0)
    {
        success = false;
    }
    if (!success)
    {
        std::remove(path);
    }
    return success;
}

#ifdef EE_ENABLE_WEBGPU
// Diagnostic-only, bounded field-start captures. An offscreen WebGPU
// replay uses the same guest scanout clock as a windowed one.
struct s_gs_field_probe
{
    FILE* m_file = nullptr;
    std::string m_prefix;
    uint64_t m_last_field = 0u;
    uint32_t m_captures = 0u;
    uint32_t m_start_dispatch = 0u;
    bool m_first_ppm[3]{};
    bool m_first_nonblack_ppm[3]{};
    uint64_t m_last_present_attempts = 0u;
    uint64_t m_last_present_succeeded = 0u;
};

bool capture_gs_field(
    s_gs_field_probe& probe, ee::c_ee_webgpu_bridge& gpu,
    const ee::c_ee_gs_state& gs,
    const ee::c_ee_gs_presentation_probe& presentation,
    const ee::s_ee_gs_display_registers& display, uint32_t crt_mode,
    uint64_t fields_started, uint64_t dispatches, uint64_t ee_cycle,
    uint32_t ee_pc)
{
    constexpr uint32_t k_max_captures = 64u;
    if (probe.m_file == nullptr || fields_started <= probe.m_last_field)
    {
        return true;
    }
    const uint64_t skipped = fields_started - probe.m_last_field - 1u;
    probe.m_last_field = fields_started;
    if (dispatches < probe.m_start_dispatch ||
        probe.m_captures >= k_max_captures)
    {
        return true;
    }
    ++probe.m_captures;
    const uint64_t present_attempts = presentation.present_attempts();
    const uint64_t present_succeeded = presentation.present_succeeded();
    const bool attempted_now =
        present_attempts > probe.m_last_present_attempts;
    const bool succeeded_now =
        present_succeeded > probe.m_last_present_succeeded;
    probe.m_last_present_attempts = present_attempts;
    probe.m_last_present_succeeded = present_succeeded;

    const uint32_t circuit = (display.m_pmode & 2u) != 0u ? 1u : 0u;
    const uint64_t selected_fb = display.m_dispfb[circuit];
    const uint64_t frames[2] =
    {
        gs.register_value(0x4cu), gs.register_value(0x4du)
    };
    const bool frame_written[2] =
    {
        gs.was_written(0x4cu), gs.was_written(0x4du)
    };
    std::fprintf(probe.m_file,
        "{\"event\":\"field_started\",\"field\":%llu,"
        "\"skipped_fields\":%llu,\"dispatches\":%llu,"
        "\"ee_cycle\":%llu,\"ee_pc\":\"0x%08x\","
        "\"guest_crt_mode\":%u,"
        "\"pmode\":\"0x%016llx\",\"smode2\":\"0x%016llx\","
        "\"dispfb1\":\"0x%016llx\",\"dispfb2\":\"0x%016llx\","
        "\"display1\":\"0x%016llx\",\"display2\":\"0x%016llx\","
        "\"selected_circuit\":%u,"
        "\"frame1\":\"0x%016llx\",\"frame2\":\"0x%016llx\","
        "\"frame1_written\":%s,\"frame2_written\":%s,"
        "\"sprite_pairs\":%llu,\"textured_sprite_pairs\":%llu,"
        "\"gpu_draws\":%llu,"
        "\"window_present_attempted\":%s,"
        "\"window_present_succeeded\":%s,"
        "\"window_present_attempts_total\":%llu,"
        "\"window_present_succeeded_total\":%llu,\"targets\":[",
        static_cast<unsigned long long>(fields_started),
        static_cast<unsigned long long>(skipped),
        static_cast<unsigned long long>(dispatches),
        static_cast<unsigned long long>(ee_cycle), ee_pc, crt_mode,
        static_cast<unsigned long long>(display.m_pmode),
        static_cast<unsigned long long>(display.m_smode2),
        static_cast<unsigned long long>(display.m_dispfb[0]),
        static_cast<unsigned long long>(display.m_dispfb[1]),
        static_cast<unsigned long long>(display.m_display[0]),
        static_cast<unsigned long long>(display.m_display[1]),
        circuit + 1u,
        static_cast<unsigned long long>(frames[0]),
        static_cast<unsigned long long>(frames[1]),
        frame_written[0] ? "true" : "false",
        frame_written[1] ? "true" : "false",
        static_cast<unsigned long long>(gs.state().m_total_sprite_pair_kicks),
        static_cast<unsigned long long>(gs.state().m_total_sprite_textured_kicks),
        static_cast<unsigned long long>(gpu.rendered_primitives()),
        attempted_now ? "true" : "false",
        succeeded_now ? "true" : "false",
        static_cast<unsigned long long>(present_attempts),
        static_cast<unsigned long long>(present_succeeded));

    static constexpr const char* k_names[3] =
    {
        "display", "frame1", "frame2"
    };
    for (uint32_t i = 0u; i < 3u; ++i)
    {
        const bool available = i == 0u || frame_written[i - 1u];
        const uint64_t raw = i == 0u ? selected_fb : frames[i - 1u];
        const ee::s_ee_gs_frame_target frame =
            ee::decode_gs_frame_target(raw);
        const uint32_t base = i == 0u
            ? static_cast<uint32_t>(raw & 0x1ffu) * 32u : frame.m_base;
        const uint32_t fbw = i == 0u
            ? static_cast<uint32_t>((raw >> 9u) & 0x3fu) : frame.m_fbw;
        const uint32_t psm = i == 0u
            ? static_cast<uint32_t>((raw >> 15u) & 0x1fu) : frame.m_psm;
        std::vector<uint32_t> pixels;
        uint32_t width = 0u;
        uint32_t height = 0u;
        const bool readback = available && (i == 0u
            ? gpu.readback(display, crt_mode, pixels, width, height)
            : gpu.readback_frame(raw, display, crt_mode,
                pixels, width, height));
        uint64_t nonblack = 0u;
        uint64_t digest = 0u;
        const bool valid = readback && ee::summarize_gs_field_pixels(
            pixels, width, height, &nonblack, &digest);
        bool ppm_written = false;
        if (valid && (!probe.m_first_ppm[i] ||
            (nonblack != 0u && !probe.m_first_nonblack_ppm[i])))
        {
            const std::string filename = probe.m_prefix +
                ".field" + std::to_string(fields_started) +
                "." + k_names[i] + ".ppm";
            ppm_written = write_gs_ppm(
                filename.c_str(), pixels, width, height);
            if (ppm_written)
            {
                probe.m_first_ppm[i] = true;
                if (nonblack != 0u)
                {
                    probe.m_first_nonblack_ppm[i] = true;
                }
            }
        }
        std::fprintf(probe.m_file,
            "%s{\"name\":\"%s\",\"available\":%s,"
            "\"base_blocks\":%u,\"fbw\":%u,\"psm\":%u,"
            "\"readback_succeeded\":%s,\"width\":%u,\"height\":%u,"
            "\"nonblack_rgb\":%llu,\"fnv1a64_rgba\":\"0x%016llx\","
            "\"ppm_written\":%s}",
            i != 0u ? "," : "", k_names[i],
            available ? "true" : "false", base, fbw, psm,
            valid ? "true" : "false", width, height,
            static_cast<unsigned long long>(nonblack),
            static_cast<unsigned long long>(digest),
            ppm_written ? "true" : "false");
    }
    std::fputs("]}\n", probe.m_file);
    return std::fflush(probe.m_file) == 0 &&
        std::ferror(probe.m_file) == 0;
}
#endif

void usage(const char* program)
{
    std::printf(
        "usage: %s <game.elf> <linked-native-library> [options]\n"
        "  --max-dispatches=N    Native function dispatch budget (default 10000)\n"
        "  --entry=0xADDR       Override ELF entry PC\n"
        "  --stack=0xADDR       Initial EE stack pointer (default 0x01fff000)\n"
        "  --loop-budget=N       Stop at a guest CFG loop after N header entries (0=off)\n"
        "  --loop-yield-resumes=N  Resume at most N host yields; requires loop-budget\n"
        "  --ee-clock=off|reference-compat  HLE compatibility clock (default: reference-compat)\n"
        "  --sif-poll-limit=N    Diagnose N repeated IOP SMFLAG zero reads (0=off)\n"
        "  --iop-irx=FILE       Repeat in dependency order; last IRX starts by default\n"
        "  --iop-rom0=FILE      Boot caller-supplied 4 MiB PS2 BIOS ROM at BFC00000\n"
        "                        Also accepts SDBZ_IOP_ROM0 environment variable\n"
        "  --disc-image=FILE    Attach a raw 2048-byte-sector ISO to ROM0 CDVD\n"
        "  --disc-folder=DIR    Mount extracted files for HLE IOP LOADFILE (no ISO)\n"
        "  --iop-start-all      Start all loaded IOP modules (default: last only)\n"
        "  --iop-quantum=N      Max R3000 instructions per SIF poll (default 256)\n"
        "  --gs-snapshot=FILE   Read-only displayed GS VRAM PPM (not a frame)\n"
        "  --gs-upload-snapshot=FILE  Preview completed GIF upload VRAM PPM (not a frame)\n"
        "  --gs-webgpu-snapshot=FILE  Render actual native WebGPU GS raster to PPM (opt-in)\n"
        "  --speed-percent=N   Window pacing target (0=unlimited, default 100)\n"
        "  --gs-webgpu-window   Show guest scanout fields in a raylib/WebGPU window (opt-in)\n"
        "  --gs-field-probe=FILE  Bounded GPU FRAME/DISPFB JSONL at real guest fields\n"
        "                        Also accepts SDBZ_GS_FIELD_PROBE (WebGPU build)\n"
        "  --trace              Print guest PC, reason and dispatch count per step\n"
        "  --hot-expand-dir=DIR Pause on missing guest code and hot-load compiled DLL\n"
        "  --trace-entry=0xPC   Print arguments and pointed-to words at a guest entry\n"
        "  --memcard-discard-writes Keep card writes in memory for this replay\n"
        "  --watch-word=0xADDR  Report guest word changes between native dispatches\n"
        "  --trace-store=0xADDR Trace individual guest stores overlapping this word (64 max)\n"
        "  --trace-v0=0xSTART:0xEND Trace low-64 V0 changes by guest PC (64 max)\n"
        "  --trace-instructions=FILE  Save bounded native guest PC/word stream (not cycles)\n"
        "  --trace-instructions-window=START:END  Restrict PC stream to this range\n"
        "  --hle-startup        Handle selected startup and semaphore kernel calls\n"
        "  --kseg-ram           Enable direct EE RAM aliases (KSEG0/1 and 0x2/0x3)\n"
        "  --kernel-ram=FILE    Load 512 KiB of captured EE kernel RAM (not BIOS ROM)\n"
        "  --help               Show this message\n"
        "\n"
        "Runs already-linked translated functions, not a PS2 BIOS/IOP/GS.\n"
        "The limit counts function dispatches, NOT guest instructions.\n",
        program
    );
}

const char* exit_name(uint32_t reason)
{
    switch (static_cast<ee::e_ee_exit_reason>(reason))
    {
    case ee::e_ee_exit_reason::EE_EXIT_RETURN: return "RETURN";
    case ee::e_ee_exit_reason::EE_EXIT_EXCEPTION: return "EXCEPTION";
    case ee::e_ee_exit_reason::EE_EXIT_EXTERNAL_CALL: return "EXTERNAL_CALL";
    case ee::e_ee_exit_reason::EE_EXIT_DISPATCH: return "DISPATCH";
    case ee::e_ee_exit_reason::EE_EXIT_UNSUPPORTED: return "UNSUPPORTED";
    case ee::e_ee_exit_reason::EE_EXIT_LOOP_BUDGET: return "LOOP_BUDGET";
    case ee::e_ee_exit_reason::EE_EXIT_HOST_YIELD: return "HOST_YIELD";
    case ee::e_ee_exit_reason::EE_EXIT_LEVEL2_EXCEPTION: return "LEVEL2_EXCEPTION";
    default: return "UNKNOWN";
    }
}

struct s_kernel_interrupt_handler
{
    uint32_t m_id = 0;
    uint32_t m_cause = 0;
    uint32_t m_address = 0;
    uint32_t m_arg = 0;
    uint32_t m_gp = 0;
};

struct s_cdvd_context
{
    std::unique_ptr<iop::c_sector_source> m_source;
    std::unique_ptr<iop::c_cdvd_dma_transport> m_transport;
};

// ps2-web-recomp/runtime/src/kernel.cpp::add_handler:
// all INTC and DMAC registrations share one monotonically increasing ID.
// next=0 inserts at the front, next=-1 appends, and another handler ID
// inserts before that handler (or appends if the ID is not present).
uint32_t kernel_add_interrupt_handler(
    std::vector<s_kernel_interrupt_handler>& handlers,
    uint32_t& next_handler_id,
    uint32_t cause,
    uint32_t address,
    uint32_t next,
    uint32_t argument,
    uint32_t gp)
{
    const s_kernel_interrupt_handler handler{
        next_handler_id++, cause, address, argument, gp
    };

    auto insertion = handlers.end();
    if (next == 0u)
    {
        insertion = handlers.begin();
    }
    else if (next != 0xffffffffu)
    {
        insertion = std::find_if(
            handlers.begin(), handlers.end(),
            [next](const s_kernel_interrupt_handler& existing)
            {
                return existing.m_id == next;
            }
        );
    }
    handlers.insert(insertion, handler);
    return handler.m_id;
}

// Reference RemoveIntcHandler/RemoveDmacHandler returns the number of
// handlers still installed for the requested cause, not a success boolean.
uint32_t kernel_remove_interrupt_handler(
    std::vector<s_kernel_interrupt_handler>& handlers,
    uint32_t cause,
    uint32_t id)
{
    handlers.erase(
        std::remove_if(
            handlers.begin(), handlers.end(),
            [id](const s_kernel_interrupt_handler& handler)
            {
                return handler.m_id == id;
            }
        ),
        handlers.end()
    );
    return static_cast<uint32_t>(std::count_if(
        handlers.begin(), handlers.end(),
        [cause](const s_kernel_interrupt_handler& handler)
        {
            return handler.m_cause == cause;
        }
    ));
}

struct s_syscall_sample
{
    uint64_t m_dispatch = 0;
    uint32_t m_service = 0;
    uint32_t m_handler = 0;
    uint32_t m_resume = 0;
    uint32_t m_a0 = 0;
    uint32_t m_a1 = 0;
    uint32_t m_a2 = 0;
    uint32_t m_a3 = 0;
    uint32_t m_v0 = 0;
    bool m_returned = false;
};

struct s_dispatch_sample
{
    uint64_t m_index = 0;
    uint32_t m_from = 0;
    uint32_t m_to = 0;
    uint32_t m_reason = 0;
    uint32_t m_status = 0;
    // Read-only register snapshots at native dispatch boundaries.
    // These are not per-instruction values inside a translated function.
    uint32_t m_s0_before = 0;
    uint32_t m_s0_after = 0;
    uint32_t m_s1_before = 0;
    uint32_t m_s1_after = 0;
    uint32_t m_s2_before = 0;
    uint32_t m_s2_after = 0;
};

const char* ee_cycle_source_name(ee::e_ee_cycle_source source)
{
    switch (source)
    {
    case ee::e_ee_cycle_source::VALIDATED_R5900:
        return "validated_r5900";
    case ee::e_ee_cycle_source::REFERENCE_COMPATIBILITY:
        return "reference_compatibility";
    default:
        return "unavailable";
    }
}

// This is a read-only summary of committed GIF PATH1/PATH2/PATH3 work. It never
// treats the current primitive's vertex count as an entire-boot total.
void write_gs_sprite_sample_json(
    FILE* output, const ee::s_ee_gs_sprite_sample& sample)
{
    std::fprintf(output,
        "{\"index\": %llu, \"context\": %u, "
        "\"prim\": \"0x%016llx\", \"effective_prim\": \"0x%016llx\", "
        "\"rgbaq\": \"0x%016llx\", "
        "\"xyz0\": \"0x%016llx\", \"xyz1\": \"0x%016llx\", "
        "\"xyoffset\": \"0x%016llx\", \"scissor\": \"0x%016llx\", "
        "\"frame\": \"0x%016llx\", \"zbuf\": \"0x%016llx\", "
        "\"test\": \"0x%016llx\"}",
        static_cast<unsigned long long>(sample.m_index),
        sample.m_context,
        static_cast<unsigned long long>(sample.m_prim),
        static_cast<unsigned long long>(sample.m_effective_prim),
        static_cast<unsigned long long>(sample.m_rgbaq),
        static_cast<unsigned long long>(sample.m_xyz0),
        static_cast<unsigned long long>(sample.m_xyz1),
        static_cast<unsigned long long>(sample.m_xyoffset),
        static_cast<unsigned long long>(sample.m_scissor),
        static_cast<unsigned long long>(sample.m_frame),
        static_cast<unsigned long long>(sample.m_zbuf),
        static_cast<unsigned long long>(sample.m_test));
}

void write_gs_diagnostics_json(FILE* output, const ee::s_ee_gs_state& gs)
{
    std::fprintf(output,
        "{\n    \"schema\": \"ee-m2-gif-gs-v1\","
        "\n    \"scope\": \"committed_gif_paths_1_2_and_3\","
        "\n    \"gpu_vram_authoritative\": %s,"
        "\n    \"accepted_batches\": %llu,"
        "\n    \"accepted_tags\": %llu,"
        "\n    \"prim_writes_total\": %llu,"
        "\n    \"vertex_writes_total\": %llu,"
        "\n    \"vertex_kicks_total\": %llu,"
        "\n    \"adc_vertices_total\": %llu,"
        "\n    \"current_primitive_vertex_writes\": %llu,"
        "\n    \"packed_events_total\": %llu,"
        "\n    \"reglist_events_total\": %llu,"
        "\n    \"ad_events_total\": %llu,"
        "\n    \"image_events_total\": %llu,"
        "\n    \"image_qwc_total\": %llu,"
        "\n    \"image_qwc_current_transfer\": %u,"
        "\n    \"pixels_current_transfer\": %u,"
        "\n    \"tag_formats\": [",
        gs.m_gpu_vram_authoritative ? "true" : "false",
        static_cast<unsigned long long>(gs.m_committed_gif_batches),
        static_cast<unsigned long long>(gs.m_tag_count),
        static_cast<unsigned long long>(gs.m_total_prim_writes),
        static_cast<unsigned long long>(gs.m_total_vertex_writes),
        static_cast<unsigned long long>(gs.m_total_vertex_kicks),
        static_cast<unsigned long long>(gs.m_total_adc_vertices),
        static_cast<unsigned long long>(gs.m_vertex_count),
        static_cast<unsigned long long>(gs.m_total_packed_events),
        static_cast<unsigned long long>(gs.m_total_reglist_events),
        static_cast<unsigned long long>(gs.m_total_ad_events),
        static_cast<unsigned long long>(gs.m_total_image_events),
        static_cast<unsigned long long>(gs.m_total_image_qwc),
        gs.m_image_qwc, gs.m_transfer_pixels_written);
    for (size_t i = 0u; i < gs.m_tag_format_counts.size(); ++i)
    {
        std::fprintf(output, "%s%llu", i ? ", " : "",
            static_cast<unsigned long long>(gs.m_tag_format_counts[i]));
    }
    std::fputs("],\n    \"primitive_types\": [", output);
    for (size_t i = 0u; i < gs.m_prim_type_counts.size(); ++i)
    {
        std::fprintf(output, "%s%llu", i ? ", " : "",
            static_cast<unsigned long long>(gs.m_prim_type_counts[i]));
    }
    std::fputs("],\n    \"packed_registers\": [", output);
    for (size_t i = 0u; i < gs.m_packed_register_counts.size(); ++i)
    {
        std::fprintf(output, "%s%llu", i ? ", " : "",
            static_cast<unsigned long long>(gs.m_packed_register_counts[i]));
    }
    std::fputs("],\n    \"ad_registers\": [", output);
    for (size_t i = 0u; i < gs.m_ad_register_counts.size(); ++i)
    {
        std::fprintf(output, "%s%llu", i ? ", " : "",
            static_cast<unsigned long long>(gs.m_ad_register_counts[i]));
    }
    std::fprintf(output,
        "],\n    \"sprite_pair_kicks_total\": %llu,"
        "\n    \"sprite_textured_kicks_total\": %llu,"
        "\n    \"sprite_nonblack_rgb_kicks_total\": %llu,"
        "\n    \"sprite_samples_first\": [",
        static_cast<unsigned long long>(gs.m_total_sprite_pair_kicks),
        static_cast<unsigned long long>(gs.m_total_sprite_textured_kicks),
        static_cast<unsigned long long>(gs.m_total_sprite_nonblack_rgb_kicks));
    const uint64_t cap = ee::s_ee_gs_state::k_sprite_sample_capacity;
    const size_t count = static_cast<size_t>(
        gs.m_total_sprite_pair_kicks < cap ? gs.m_total_sprite_pair_kicks : cap
    );
    for (size_t i = 0u; i < count; ++i)
    {
        std::fputs(i == 0u ? "\n      " : ",\n      ", output);
        write_gs_sprite_sample_json(output, gs.m_first_sprite_samples[i]);
    }
    std::fputs(count ? "\n    ],\n    \"sprite_samples_last\": [" :
        "],\n    \"sprite_samples_last\": [", output);
    const uint64_t first = gs.m_total_sprite_pair_kicks - count;
    for (size_t i = 0u; i < count; ++i)
    {
        const uint64_t index = first + i;
        std::fputs(i == 0u ? "\n      " : ",\n      ", output);
        write_gs_sprite_sample_json(output,
            gs.m_recent_sprite_samples[index % cap]);
    }
    std::fputs(count ? "\n    ],\n    \"limitations\": [" :
        "],\n    \"limitations\": [", output);
    std::fputs(
        "\"does_not_include_unexecuted_vu1_path1_xgkick\", "
        "\"does_not_imply_rasterization_or_present\", "
        "\"samples_are_bounded_committed_path1_path2_path3_sprite_pairs\", "
        "\"coordinates_and_registers_are_uninterpreted_raw_gs_values\", "
        "\"current_primitive_and_transfer_counters_reset\"]\n  }", output);
}

// Parser instances are distinct: VU1 XGKICK -> PATH1, VIF1 DIRECT ->
// PATH2, and GIF DMA channel 2 -> PATH3. A valid VIF1 DMA without a
// resulting GIF packet does not increment either PATH1 or PATH2.
void write_gif_path_counters_json(
    FILE* output, const char* name,
    const ee::s_ee_gif_path_counters& counters, bool comma)
{
    std::fprintf(output,
        "%s\n      \"%s\": {"
        "\"accepted_batches\": %llu, \"rejected_batches\": %llu, "
        "\"accepted_input_qwc\": %llu, "
        "\"committed_tags\": %llu, "
        "\"committed_prim_events\": %llu, "
        "\"committed_packed_events\": %llu, "
        "\"committed_reglist_events\": %llu, "
        "\"committed_image_events\": %llu, "
        "\"committed_image_qwc\": %llu, "
        "\"image_payload_digest_enabled\": %s, "
        "\"committed_image_nonzero_bytes\": ",
        comma ? "," : "", name,
        static_cast<unsigned long long>(counters.m_accepted_batches),
        static_cast<unsigned long long>(counters.m_rejected_batches),
        static_cast<unsigned long long>(counters.m_input_qwc),
        static_cast<unsigned long long>(counters.m_tags),
        static_cast<unsigned long long>(counters.m_primitive_events),
        static_cast<unsigned long long>(counters.m_packed_events),
        static_cast<unsigned long long>(counters.m_reglist_events),
        static_cast<unsigned long long>(counters.m_image_events),
        static_cast<unsigned long long>(counters.m_image_qwc),
        counters.m_image_payload_digest_enabled ? "true" : "false");
    if (counters.m_image_payload_digest_enabled)
    {
        std::fprintf(output, "%llu, \"committed_image_payload_fnv1a64\": %llu}",
            static_cast<unsigned long long>(counters.m_image_nonzero_bytes),
            static_cast<unsigned long long>(counters.m_image_payload_fnv1a64));
    }
    else
    {
        std::fputs("null, \"committed_image_payload_fnv1a64\": null}", output);
    }
}

void write_gif_paths_json(
    FILE* output, const ee::c_ee_gif_path3& path1,
    const ee::c_ee_gif_path3& path2, const ee::c_ee_gif_path3& path3)
{
    std::fputs(
        "{\n    \"schema\": \"ee-gif-path-provenance-v1\","
        "\n    \"scope\": \"parser_accepted_committed_gs_sink_events\","
        "\n    \"paths\": {", output);
    write_gif_path_counters_json(output, "vu1_xgkick_path1",
        path1.counters(), false);
    write_gif_path_counters_json(output, "vif1_direct_path2",
        path2.counters(), true);
    write_gif_path_counters_json(output, "gif_dma_path3",
        path3.counters(), true);
    std::fputs(
        "\n    },\n    \"limitations\": ["
        "\"accepted_batches_are_parser_consume_calls_not_complete_packets\", "
        "\"input_qwc_is_consumed_gif_bytes_not_whole_vif_dma\", "
        "\"zero_counts_do_not_imply_guest_never_attempted_path\"]\n  }",
        output);
}

void write_vif1_producer_json(FILE* output, const ee::c_ee_vif1& vif1)
{
    const ee::c_ee_vif1::s_diagnostic& detail = vif1.diagnostic();
    const auto& commands = detail.m_committed_opcodes;
    const unsigned long long vu_starts =
        static_cast<unsigned long long>(
            commands[0x14u] + commands[0x15u] + commands[0x17u]);
    // Counts only fully committed VIF1 commands. Rejected DMA attempts
    // may include repeated retries of the very same guest packet.
    std::fprintf(output,
        "{\n    \"schema\": \"ee-vif1-graphics-producer-v1\","
        "\n    \"scope\": \"committed_vif_commands_and_attempted_rejections\","
        "\n    \"committed_commands\": %u,"
        " \"pending_words\": %u,"
        "\n    \"mpg_uploads\": %u,"
        " \"unpack_commands\": %u,"
        " \"direct_qwc\": %u,"
        "\n    \"mscal\": %llu,"
        " \"mscalf\": %llu,"
        " \"mscnt\": %llu,"
        " \"vu_program_starts\": %llu,"
        "\n    \"flushe\": %llu,"
        " \"flush\": %llu,"
        " \"flusha\": %llu,"
        "\n    \"direct\": %llu,"
        " \"directhl\": %llu,"
        "\n    \"vu_start_attempts\": %llu,"
        " \"vu_start_rejections\": %llu,"
        " \"last_vu_start_command\": %u,"
        " \"last_rejected_vu_start_command\": %u,"
        " \"last_rejected_vu_pc\": %u,"
        " \"last_rejected_vu_lower\": %u,"
        " \"last_rejected_vu_upper\": %u,"
        " \"last_rejected_vu_steps\": %u,"
        " \"last_kick_reject\": %u,"
        " \"last_kick_address\": %u,"
        " \"last_kick_tag_address\": %u,"
        " \"last_kick_packet_qwc\": %u,"
        " \"last_kick_staged_qwc\": %u,"
        " \"last_kick_tag\": \"0x%016llx\","
        " \"last_rejected_staged_mpg\": %u,"
        " \"last_rejected_staged_unpack\": %u,"
        "\n    \"rejected_dma_attempts\": %llu,"
        " \"first_rejection\": %u,"
        " \"first_rejected_command\": %u,"
        " \"last_rejection\": %u,"
        " \"last_rejected_command\": %u,"
        " \"last_vu_pc\": %u,"
        " \"last_vu_lower\": %u,"
        " \"last_vu_upper\": %u"
        "\n  }",
        detail.m_commands, detail.m_pending_words,
        detail.m_mpg_uploads, detail.m_unpack_commands, detail.m_direct_qwc,
        static_cast<unsigned long long>(commands[0x14u]),
        static_cast<unsigned long long>(commands[0x15u]),
        static_cast<unsigned long long>(commands[0x17u]), vu_starts,
        static_cast<unsigned long long>(commands[0x10u]),
        static_cast<unsigned long long>(commands[0x11u]),
        static_cast<unsigned long long>(commands[0x13u]),
        static_cast<unsigned long long>(commands[0x50u]),
        static_cast<unsigned long long>(commands[0x51u]),
        static_cast<unsigned long long>(detail.m_vu_start_attempts),
        static_cast<unsigned long long>(detail.m_vu_start_rejections),
        detail.m_last_vu_start_code, detail.m_last_rejected_vu_start_code,
        detail.m_last_rejected_vu_pc, detail.m_last_rejected_vu_lower,
        detail.m_last_rejected_vu_upper, detail.m_last_rejected_vu_steps,
        static_cast<uint32_t>(detail.m_last_rejected_vu_issue.m_kick_reject),
        detail.m_last_rejected_vu_issue.m_kick_address,
        detail.m_last_rejected_vu_issue.m_kick_tag_address,
        detail.m_last_rejected_vu_issue.m_kick_packet_qwc,
        detail.m_last_rejected_vu_issue.m_kick_staged_qwc,
        static_cast<unsigned long long>(detail.m_last_rejected_vu_issue.m_kick_tag),
        detail.m_last_rejected_staged_mpg, detail.m_last_rejected_staged_unpack,
        static_cast<unsigned long long>(detail.m_rejected_batches),
        static_cast<uint32_t>(detail.m_first_rejection),
        detail.m_first_rejected_code,
        static_cast<uint32_t>(detail.m_reason), detail.m_code,
        detail.m_vu_pc, detail.m_vu_lower, detail.m_vu_upper);
}

bool write_report(
    const char* path,
    const ee::elf_loader& image,
    const ee::s_ee_cpu_state& cpu,
    uint32_t entry,
    uint64_t dispatched,
    uint64_t instruction_attempts,
    const ee::s_ee_runtime& runtime,
    ee::c_ee_hw_bus& hardware,
    const ee::c_ee_vsync_flags& vsync_flags,
    uint32_t reason,
    uint32_t status,
    const ee::c_bounded_diagnostic_history<s_dispatch_sample, k_report_history_limit>& history,
    const std::vector<s_syscall_sample>& syscall_history,
    const ee::s_ee_gs_state& gs,
    const ee::c_ee_gif_path3& gif_path1,
    const ee::c_ee_gif_path3& gif_path2,
    const ee::c_ee_gif_path3& gif_path3,
    const ee::c_ee_vif1& vif1,
    const ee::c_ee_gs_presentation_probe& graphics_probe,
    const iop::c_iop_guest_fault_probe& iop_fault_probe,
    const iop::c_kernel* guest_iop_kernel,
    const ee::c_ee_sdbz_dma_wait_probe& dma_wait_probe,
    const ee::c_ee_sdbz_pad_return_probe& pad_return_probe,
    const ee::c_ee_sdbz_spawn_probe& spawn_probe,
    const ee::c_ee_dispatch_hotspots& hotspots,
    bool in_progress
)
{
    ee::c_runtime_profile_scope report_profile_scope(ee::e_runtime_profile_id::report_snapshot_body);
    FILE* output = open_host_file(path, "wb");
    if (output == nullptr)
    {
        std::fprintf(stderr, "failed to create runner report: %s\n", path);
        return false;
    }
    const uint32_t cause = cpu.m_cop0[13];
    const uint32_t epc = cpu.m_cop0[14];
    uint32_t instruction = 0;
    const bool has_instruction = image.read32_at(epc, &instruction);
    std::fprintf(output,
        "{\n  \"schema\": \"ee-game-run-report-v1\","
        "\n  \"snapshot_kind\": \"%s\","
        "\n  \"entry_pc\": %u, \"final_pc\": %u,"
        "\n  \"dispatches\": %llu, \"exit_reason\": %u,"
        "\n  \"exit_name\": \"%s\", \"dispatch_status\": %u,"
        "\n  \"cop0_cause\": %u, \"cop0_epc\": %u,"
        "\n  \"cop0_status\": %u, \"bad_vaddr\": %u,"
        "\n  \"epc_instruction\": ",
        in_progress ? "in_progress" : "stopped",
        entry, cpu.m_pc, static_cast<unsigned long long>(dispatched),
        reason, exit_name(reason), status, cause, epc,
        cpu.m_cop0[12], cpu.m_cop0[8]);
    if (has_instruction)
    {
        std::fprintf(output, "%u", instruction);
    }
    else
    {
        std::fputs("null", output);
    }
    std::fprintf(output, ",\n  \"guest_instruction_attempts\": %llu",
                 static_cast<unsigned long long>(instruction_attempts));
    // A zero clock is not proof of calibrated R5900 execution.
    std::fprintf(output,
        ",\n  \"ee_cycle_source\": \"%s\""
        ",\n  \"ee_cycles\": %llu"
        ",\n  \"reference_instruction_slots\": %llu"
        ",\n  \"hle_timing_boundaries\": %llu"
        ",\n  \"timing_observer_failed\": %s"
        ",\n  \"scheduler_yields\": %llu"
        ",\n  \"next_gs_field_cycle\": %llu"
        ",\n  \"gs_fields_started\": %llu"
        ",\n  \"gs_csr\": %llu"
        ",\n  \"intc_stat\": %u, \"intc_mask\": %u"
        ",\n  \"vsync_flag_address\": %u, \"vsync_csr_address\": %u",
        ee_cycle_source_name(runtime.m_ee_cycle_scheduler.source()),
        static_cast<unsigned long long>(runtime.m_ee_cycle_scheduler.current_cycle()),
        static_cast<unsigned long long>(runtime.m_reference_instruction_slots),
        static_cast<unsigned long long>(runtime.m_hle_timing_boundaries),
        runtime.m_timing_observer_failed != 0u ? "true" : "false",
        static_cast<unsigned long long>(runtime.m_ee_cycle_scheduler.scheduler_yields()),
        static_cast<unsigned long long>(hardware.scanout().next_field_cycle()),
        static_cast<unsigned long long>(hardware.scanout().fields_started()),
        static_cast<unsigned long long>(hardware.scanout().csr()),
        hardware.intc().status(), hardware.intc().mask(),
        vsync_flags.flag_address(), vsync_flags.csr_address());
    std::fputs(",\n  \"gpr_lo\": [", output);
    for (uint32_t i = 0; i < 32; ++i)
    {
        std::fprintf(output, "%s%llu", i ? ", " : "",
            static_cast<unsigned long long>(cpu.m_gpr[i].m_lo));
    }
    std::fputs("],\n  \"history\": [", output);
    for (size_t i = 0; i < history.size(); ++i)
    {
        const auto& item = history[i];
        std::fprintf(output,
            "%s\n    {\"dispatch\": %llu, \"from_pc\": %u,"
            " \"to_pc\": %u, \"reason\": %u, \"status\": %u,"
            " \"s0_before\": %u, \"s0_after\": %u,"
            " \"s1_before\": %u, \"s1_after\": %u,"
            " \"s2_before\": %u, \"s2_after\": %u}",
            i ? "," : "", static_cast<unsigned long long>(item.m_index),
            item.m_from, item.m_to, item.m_reason, item.m_status,
            item.m_s0_before, item.m_s0_after,
            item.m_s1_before, item.m_s1_after,
            item.m_s2_before, item.m_s2_after);
    }
    std::fputs(history.empty() ? "]," : "\n  ],", output);
    std::fputs("\n  \"syscall_history\": [", output);
    for (size_t i = 0; i < syscall_history.size(); ++i)
    {
        const auto& item = syscall_history[i];
        std::fprintf(output,
            "%s\n    {\"dispatch\": %llu, \"service\": %u,"
            " \"handler\": %u, \"resume_pc\": %u,"
            " \"a0\": %u, \"a1\": %u, \"a2\": %u, \"a3\": %u,"
            " \"v0\": %u, \"returned\": %s}",
            i ? "," : "", static_cast<unsigned long long>(item.m_dispatch),
            item.m_service, item.m_handler, item.m_resume,
            item.m_a0, item.m_a1, item.m_a2, item.m_a3,
            item.m_v0, item.m_returned ? "true" : "false");
    }
    std::fputs(syscall_history.empty() ? "]," : "\n  ],", output);
    std::fputs("\n  \"m2_runtime\": ", output);
    hotspots.write_json(output);
    std::fputs(",\n  \"m2_gif_gs\": ", output);
    write_gs_diagnostics_json(output, gs);
    std::fputs(",\n  \"m2_gif_paths\": ", output);
    write_gif_paths_json(output, gif_path1, gif_path2, gif_path3);
    std::fputs(",\n  \"m2_vif1_producer\": ", output);
    write_vif1_producer_json(output, vif1);
    std::fputs(",\n  \"m2_gs_presentation\": ", output);
    graphics_probe.write_json(output, hardware.scanout().fields_started());
    std::fputs(",\n  \"m2_iop_guest_fault\": ", output);
    iop_fault_probe.write_json(output);
    // Observe actual IOP SIO2 transactions, not module-load success.
    // A virtual neutral pad does not imply the game has polled a button.
    std::fputs(",\n  \"m2_iop_sio2\": ", output);
    if (guest_iop_kernel == nullptr)
    {
        std::fputs("null", output);
    }
    else
    {
        const iop::c_hw& sio2 = guest_iop_kernel->hardware();
        const iop::c_hw::s_sio2_diagnostics detail =
            sio2.sio2_diagnostics();
        std::fprintf(output,
            "{\"schema\":\"ee-iop-sio2-input-observation-v1\","
            "\"ctrl_starts\":%u,\"packets\":%u,"
            "\"completed_transfers\":%u,"
            "\"transfer_requested\":%s,"
            "\"pending_tx_bytes\":%zu,"
            "\"queued_rx_bytes\":%zu,"
            "\"empty_rx_reads\":%u,"
            "\"pad0_polls\":%u,\"pad0_cross_pressed_polls\":%u,"
            "\"pad0_cross_first_poll\":%u,"
            "\"pad0_cross_poll_count\":%u,"
            "\"pad0_cross_bytes_pio_read\":%u,"
            "\"pad0_cross_bytes_dma_written\":%u,"
            "\"pad0_cross_bytes_fifo_discarded\":%u,"
            "\"packet_index\":%u,\"ctrl_word\":%u,\"stat_word\":%u,"
            "\"intr_word\":%u,"
            "\"last_rejection\":\"%s\",\"last_rejection_code\":%u,"
            "\"rejected_starts\":%u,"
            "\"send3\":[",
            sio2.sio2_ctrl_starts(),
            sio2.sio2_packets(),
            sio2.sio2_completed_transfers(),
            sio2.sio2_transfer_requested() ? "true" : "false",
            detail.m_tx_bytes, detail.m_rx_bytes,
            detail.m_empty_rx_reads,
            detail.m_pad0_polls, detail.m_pad0_cross_pressed_polls,
            detail.m_pad0_cross_first_poll, detail.m_pad0_cross_poll_count,
            detail.m_pad0_cross_bytes_pio_read,
            detail.m_pad0_cross_bytes_dma_written,
            detail.m_pad0_cross_bytes_fifo_discarded,
            detail.m_packet_index, detail.m_ctrl, detail.m_stat,
            detail.m_intr, sio2.sio2_rejection_name(),
            static_cast<uint32_t>(detail.m_last_rejection),
            detail.m_rejected_starts);
        for (uint32_t index = 0u; index < 4u; ++index)
        {
            std::fprintf(output, "%s%u",
                index != 0u ? "," : "", detail.m_send3[index]);
        }
        std::fputs("],\"dma11\":[", output);
        for (uint32_t index = 0u; index < 3u; ++index)
        {
            std::fprintf(output, "%s%u",
                index != 0u ? "," : "", detail.m_dma11[index]);
        }
        std::fputs("],\"dma12\":[", output);
        for (uint32_t index = 0u; index < 3u; ++index)
        {
            std::fprintf(output, "%s%u",
                index != 0u ? "," : "", detail.m_dma12[index]);
        }
        std::fprintf(output,
            "],\"last_start_guest_pc\":%u,"
            "\"last_start_guest_ra\":%u,"
            "\"packets_at_last_start\":%u,"
            "\"tx_bytes_at_last_start\":%zu,"
            "\"rx_bytes_at_last_start\":%zu,"
            "\"first_packets\":[",
            detail.m_last_start_guest_pc,
            detail.m_last_start_guest_ra,
            detail.m_packets_at_last_start,
            detail.m_tx_bytes_at_last_start,
            detail.m_rx_bytes_at_last_start);
        for (uint32_t index = 0u;
             index < detail.m_first_packet_count; ++index)
        {
            const iop::c_hw::s_sio2_packet_observation& packet =
                detail.m_first_packets[index];
            std::fprintf(output,
                "%s{\"port\":%u,\"send3\":%u,"
                "\"input_size\":%u,\"output_size\":%u,"
                "\"first_tx_byte\":%u,\"command\":%u,"
                "\"pad_present\":%s}",
                index != 0u ? "," : "",
                packet.m_port, packet.m_send3,
                packet.m_input_size, packet.m_output_size,
                packet.m_first_tx_byte, packet.m_command,
                packet.m_pad_present ? "true" : "false");
        }
        std::fputs(
            "],\"limitations\":["
            "\"hardware_counters_not_pad_button_state\","
            "\"module_load_does_not_prove_controller_poll\","
            "\"rejected_ctrl_start_does_not_raise_irq17\"]}",
            output);
    }
    std::fputs(",\n  \"m2_sdbz_dma_wait\": ", output);
    dma_wait_probe.write_json(output);
    std::fputs(",\n  \"m2_sdbz_ee_pad_return\": ", output);
    pad_return_probe.write_json(output);
    std::fputs(",\n  \"sdbz_spawn_boundaries\": ", output);
    spawn_probe.write_json(output);
    // Finish the report-body scope before serializing the profile so
    // completed child counters and covered time remain consistent.
    // Profile serialization and final stream close remain uninstrumented.
    report_profile_scope.stop();
    std::fputs(",\n  \"vu1_aot\": ", output);
    vif1.write_aot_json(output);
    std::fputs(",\n  \"vu1_worker\": ", output);
    vif1.write_worker_json(output);
    std::fputs(",\n  \"runtime_profile\": ", output);
    ee::s_gpu_timestamp_profile gpu_timestamps{};
#ifdef EE_ENABLE_WEBGPU
    // Snapshot the last published counters without touching the device
    // or joining an active GS worker at periodic report boundaries.
    gpu_timestamps = ps2::gs::gpu::timestamp_profile();
#endif
    ee::write_runtime_profile_json(output, dispatched, hardware.scanout().fields_started(),
        graphics_probe.present_succeeded(), gpu_timestamps);
    if (ee::g_runtime_profile.m_enabled)
    {
        // Retain phase deltas in the archived replay log, even though
        // the periodic JSON report itself is overwritten.
        ee::write_runtime_profile_interval(stderr, dispatched,
            runtime.m_ee_cycle_scheduler.current_cycle(),
            hardware.scanout().fields_started(), graphics_probe.present_succeeded(),
            !in_progress, ee::t_runtime_profile_clock::now(), gpu_timestamps);
    }
    std::fputs("\n}\n", output);
    const bool no_write_error = std::ferror(output) == 0;
    const bool closed = std::fclose(output) == 0;
    const bool ok = no_write_error && closed;
    if (!ok)
    {
        std::fprintf(stderr, "failed to write complete runner report: %s\n", path);
    }
    return ok;
}

bool load_kernel_ram_snapshot(const char* path, ee::ee_memory* memory)
{
    FILE* input = open_host_file(path, "rb");
    if (input == nullptr)
    {
        std::fprintf(stderr, "could not open EE kernel RAM snapshot: %s\n", path);
        return false;
    }
    std::vector<uint8_t> data(k_kernel_ram_snapshot_size);
    const size_t count = std::fread(data.data(), 1, data.size(), input);
    const int trailing = std::fgetc(input);
    const bool read_ok = count == data.size() && trailing == EOF &&
                         std::ferror(input) == 0;
    const bool closed = std::fclose(input) == 0;
    if (!read_ok || !closed)
    {
        std::fprintf(stderr,
            "EE kernel RAM snapshot must be exactly %u bytes: %s\n",
            k_kernel_ram_snapshot_size, path);
        return false;
    }
    // A .p2s is a ZIP archive. Simply slicing its first 512 KiB creates
    // a correctly sized but invalid kernel snapshot; reject it explicitly.
    if (data[0] == 0x50u && data[1] == 0x4bu &&
        ((data[2] == 0x03u && data[3] == 0x04u) ||
         (data[2] == 0x05u && data[3] == 0x06u) ||
         (data[2] == 0x07u && data[3] == 0x08u)))
    {
        std::fprintf(stderr,
            "EE kernel RAM snapshot is a ZIP/savestate archive, not raw EE RAM: %s\n",
            path);
        return false;
    }
    if (!memory->contains(0u, data.size()))
    {
        std::fprintf(stderr, "EE memory cannot hold kernel RAM snapshot\n");
        return false;
    }
    for (uint32_t i = 0u; i < k_kernel_ram_snapshot_size; ++i)
    {
        memory->write8(i, data[i]);
    }
    std::printf("  EE kernel RAM snapshot: %u bytes -> physical 0x00000000\n",
                k_kernel_ram_snapshot_size);
    return true;
}

bool load_segments(
    const ee::elf_loader& image,
    ee::ee_memory* memory
)
{
    uint32_t count = 0;
    for (const auto& segment : image.segments())
    {
        if (segment.memsz < segment.filesz ||
            segment.data.size() != segment.filesz ||
            !memory->contains(segment.vaddr, segment.memsz))
        {
            std::fprintf(
                stderr,
                "invalid or out-of-RAM PT_LOAD: vaddr=0x%08x file=%u mem=%u\n",
                segment.vaddr, segment.filesz, segment.memsz
            );
            return false;
        }

        // RAM is zero-initialized, so the difference between file and
        // memory sizes represents an already-zeroed BSS tail.
        for (size_t i = 0; i < segment.data.size(); ++i)
        {
            memory->write8(segment.vaddr + static_cast<uint32_t>(i),
                           segment.data[i]);
        }

        ++count;
        std::printf(
            "  PT_LOAD 0x%08x file=%u memory=%u %c%c%c\n",
            segment.vaddr, segment.filesz, segment.memsz,
            segment.readable ? 'r' : '-',
            segment.writable ? 'w' : '-',
            segment.executable ? 'x' : '-'
        );
    }

    if (count == 0)
    {
        std::fprintf(stderr, "ELF has no loadable segments\n");
        return false;
    }

    return true;
}

} // namespace

int main(int argc, char** argv)
{
    const char* elf_path = nullptr;
    const char* library_path = nullptr;
    bool trace = false;
    bool hle_startup = false;
    bool discard_memory_card_writes = false;
    bool compatibility_clock = true;
    bool kseg_ram = false;
    const char* kernel_ram_path = nullptr;
    const char* report_path = nullptr;
    const char* hot_expand_dir = nullptr;
    const char* gs_snapshot_path = nullptr;
    const char* gs_upload_snapshot_path = nullptr;
    const char* gs_webgpu_snapshot_path = nullptr;
    const char* gs_field_probe_path = nullptr;
    std::string gs_field_probe_environment;
    bool gs_webgpu_window = false;
    uint32_t speed_percent = 100u;
    bool has_speed_percent = false;
    const char* instruction_trace_path = nullptr;
    uint32_t instruction_trace_first_pc = 0u;
    uint32_t instruction_trace_last_pc = 0xffffffffu;
    bool instruction_trace_window_seen = false;
    std::vector<std::string> iop_irx_paths;
    const char* iop_rom0_path = nullptr;
    // Retain ownership when Windows allocates the environment value.
    std::string iop_rom0_environment;
    const char* disc_image_path = nullptr;
    const char* disc_folder_path = nullptr;
    bool iop_start_all = false;
    uint32_t budget = k_default_dispatch_budget;
    uint32_t loop_budget = 0u;
    uint32_t loop_yield_resumes = 0u;
    uint32_t sif_poll_limit = 0u;
    uint32_t iop_quantum = 256u;
    bool iop_quantum_explicit = false;
    uint32_t trace_entry = 0u;
    uint32_t watch_word = 0u;
    uint32_t trace_store = 0u;
    uint32_t trace_v0_first_pc = 0u;
    uint32_t trace_v0_last_pc = 0u;
    bool has_trace_entry = false;
    bool has_watch_word = false;
    bool has_trace_store = false;
    bool has_trace_v0 = false;
    uint32_t stack = k_default_stack;
    uint32_t entry_override = 0;
    bool override_entry = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string argument = argv[i];
        if (argument == "--help" || argument == "-h")
        {
            usage(argv[0]);
            return 0;
        }
        if (argument.compare(0, 21, "--trace-instructions=") == 0)
        {
            if (argument.size() == 21u || instruction_trace_path != nullptr)
            {
                std::fprintf(stderr, "invalid or duplicate instruction trace path\n");
                return 2;
            }
            instruction_trace_path = argv[i] + 21;
            continue;
        }
        if (argument.compare(0, 28, "--trace-instructions-window=") == 0)
        {
            const std::string range = argument.substr(28);
            const size_t separator = range.find(':');
            if (separator == std::string::npos || separator == 0u ||
                separator + 1u >= range.size() ||
                range.find(':', separator + 1u) != std::string::npos ||
                !parse_u32(range.substr(0, separator).c_str(),
                           &instruction_trace_first_pc) ||
                !parse_u32(range.substr(separator + 1u).c_str(),
                           &instruction_trace_last_pc) ||
                (instruction_trace_first_pc & 3u) != 0u ||
                (instruction_trace_last_pc & 3u) != 0u ||
                instruction_trace_first_pc > instruction_trace_last_pc)
            {
                std::fprintf(stderr, "invalid instruction PC trace window\n");
                return 2;
            }
            instruction_trace_window_seen = true;
            continue;
        }
        if (argument == "--trace")        {
            trace = true;
            continue;
        }
        if (argument.compare(0, 14, "--gs-snapshot=") == 0)
        {
            if (argument.size() == 14u || gs_snapshot_path != nullptr)
            {
                std::fprintf(stderr, "invalid or duplicate GS snapshot path\n");
                return 2;
            }
            gs_snapshot_path = argv[i] + 14;
            continue;
        }
        if (argument.compare(0, 21, "--gs-upload-snapshot=") == 0)
        {
            if (argument.size() == 21u || gs_upload_snapshot_path != nullptr)
            {
                std::fprintf(stderr,
                    "invalid or duplicate GS upload snapshot path\n");
                return 2;
            }
            gs_upload_snapshot_path = argv[i] + 21;
            continue;
        }
        {
            constexpr const char* k_prefix = "--gs-webgpu-snapshot=";
            constexpr size_t k_prefix_len = sizeof("--gs-webgpu-snapshot=") - 1u;
            if (argument.compare(0u, k_prefix_len, k_prefix) == 0)
            {
                if (argument.size() == k_prefix_len ||
                    gs_webgpu_snapshot_path != nullptr)
                {
                    std::fprintf(stderr, "invalid or duplicate WebGPU snapshot path\n");
                    return 2;
                }
#ifndef EE_ENABLE_WEBGPU
                std::fprintf(stderr,
                    "--gs-webgpu-snapshot requires -DEE_ENABLE_WEBGPU=ON "
                    "and native wgpu-native\n");
                return 2;
#endif
                gs_webgpu_snapshot_path = argv[i] + k_prefix_len;
                continue;
            }
        }
        if (argument.compare(0, 17, "--gs-field-probe=") == 0)
        {
            if (argument.size() == 17u || gs_field_probe_path != nullptr)
            {
                std::fprintf(stderr, "invalid or duplicate GS field probe path\n");
                return 2;
            }
            gs_field_probe_path = argv[i] + 17;
            continue;
        }
        if (argument.compare(0, 16, "--speed-percent=") == 0)
        {
            if (has_speed_percent || !parse_u32(argv[i] + 16, &speed_percent) ||
                speed_percent > 1000u)
            {
                std::fprintf(stderr, "invalid --speed-percent (expected 0..1000)\n");
                return 2;
            }
            has_speed_percent = true;
            continue;
        }
        if (argument == "--gs-webgpu-window")
        {
#ifndef EE_ENABLE_RAYLIB_WINDOW
            std::fprintf(stderr,
                "--gs-webgpu-window requires EE_ENABLE_RAYLIB_WINDOW=ON\n");
            return 2;
#else
            if (gs_webgpu_window)
            {
                std::fprintf(stderr, "duplicate --gs-webgpu-window\n");
                return 2;
            }
            gs_webgpu_window = true;
#endif
            continue;
        }
        if (argument.compare(0, 9, "--report=") == 0)
        {
            if (argument.size() == 9)
            {
                std::fprintf(stderr, "empty report path\n");
                return 2;
            }
            report_path = argv[i] + 9;
            continue;
        }
        if (argument.compare(0, 13, "--kernel-ram=") == 0)
        {
            if (argument.size() == 13)
            {
                std::fprintf(stderr, "empty EE kernel RAM snapshot path\n");
                return 2;
            }
            kernel_ram_path = argv[i] + 13;
            continue;
        }
        if (argument == "--kseg-ram")
        {
            kseg_ram = true;
            continue;
        }
        if (argument == "--memcard-discard-writes")
        {
            discard_memory_card_writes = true;
            continue;
        }
        if (argument.compare(0, 17, "--hot-expand-dir=") == 0)
        {
            if (argument.size() == 17u || hot_expand_dir != nullptr)
            {
                std::fprintf(stderr, "invalid or duplicate hot-expand directory\n");
                return 2;
            }
            hot_expand_dir = argv[i] + 17;
            continue;
        }
        if (argument == "--hle-startup")
        {
            hle_startup = true;
            continue;
        }
        if (argument == "--ee-clock=off")
        {
            compatibility_clock = false;
            continue;
        }
        if (argument == "--ee-clock=reference-compat")
        {
            compatibility_clock = true;
            continue;
        }
        if (argument.compare(0, 10, "--iop-irx=") == 0)
        {
            if (argument.size() == 10u ||
                iop_irx_paths.size() >= 32u)
            {
                std::fprintf(stderr, "empty IOP IRX path or more than 32 modules\n");
                return 2;
            }
            iop_irx_paths.push_back(argument.substr(10));
            continue;
        }
        if (argument.compare(0, 11, "--iop-rom0=") == 0)
        {
            if (argument.size() == 11u || iop_rom0_path != nullptr)
            {
                std::fprintf(stderr, "invalid or duplicate IOP ROM0 path\n");
                return 2;
            }
            iop_rom0_path = argv[i] + 11;
            continue;
        }
        if (argument.compare(0, 13, "--disc-image=") == 0)
        {
            if (argument.size() == 13u || disc_image_path != nullptr)
            {
                std::fprintf(stderr, "invalid or duplicate disc image path\n");
                return 2;
            }
            disc_image_path = argv[i] + 13;
            continue;
        }
        if (argument.compare(0, 14, "--disc-folder=") == 0)
        {
            if (argument.size() == 14u || disc_folder_path != nullptr)
            {
                std::fprintf(stderr, "invalid or duplicate disc folder path\n");
                return 2;
            }
            disc_folder_path = argv[i] + 14;
            continue;
        }
        if (argument == "--iop-start-all")
        {
            iop_start_all = true;
            continue;
        }

        if (argument.compare(0, 11, "--trace-v0=") == 0)
        {
            const std::string range = argument.substr(11);
            const size_t separator = range.find(':');
            if (separator == std::string::npos || separator == 0u ||
                separator + 1u >= range.size() ||
                range.find(':', separator + 1u) != std::string::npos ||
                !parse_u32(range.substr(0, separator).c_str(), &trace_v0_first_pc) ||
                !parse_u32(range.substr(separator + 1u).c_str(), &trace_v0_last_pc) ||
                (trace_v0_first_pc & 3u) != 0u ||
                (trace_v0_last_pc & 3u) != 0u ||
                trace_v0_first_pc > trace_v0_last_pc ||
                trace_v0_last_pc - trace_v0_first_pc > 0x2000u)
            {
                std::fprintf(stderr, "invalid --trace-v0 guest PC window: %s\n", argument.c_str());
                return 2;
            }
            has_trace_v0 = true;
            continue;
        }

        const auto parse_option = [&](const char* prefix, uint32_t* destination) -> int
        {
            const size_t prefix_length = std::strlen(prefix);
            if (argument.compare(0, prefix_length, prefix) != 0)
            {
                return 0;
            }
            return parse_u32(argument.c_str() + prefix_length, destination) ? 1 : -1;
        };

        int parsed = parse_option("--max-dispatches=", &budget);
        if (parsed == 0)
        {
            parsed = parse_option("--loop-budget=", &loop_budget);
        }
        if (parsed == 0)
        {
            parsed = parse_option("--loop-yield-resumes=", &loop_yield_resumes);
        }
        if (parsed == 0)
        {
            parsed = parse_option("--sif-poll-limit=", &sif_poll_limit);
        }
        if (parsed == 0)
        {
            parsed = parse_option("--iop-quantum=", &iop_quantum);
            if (parsed == 1)
            {
                iop_quantum_explicit = true;
            }
        }
        if (parsed == 0)
        {
            parsed = parse_option("--trace-entry=", &trace_entry);
            if (parsed == 1)
            {
                has_trace_entry = true;
            }
        }
        if (parsed == 0)
        {
            parsed = parse_option("--watch-word=", &watch_word);
            if (parsed == 1)
            {
                has_watch_word = true;
            }
        }
        if (parsed == 0)
        {
            parsed = parse_option("--trace-store=", &trace_store);
            if (parsed == 1)
            {
                has_trace_store = true;
            }
        }
        if (parsed == 0)
        {
            parsed = parse_option("--stack=", &stack);
        }
        if (parsed == 0)
        {
            parsed = parse_option("--entry=", &entry_override);
            if (parsed == 1)
            {
                override_entry = true;
            }
        }
        if (parsed == -1)
        {
            std::fprintf(stderr, "invalid numeric option: %s\n", argument.c_str());
            return 2;
        }
        if (parsed == 1)
        {
            continue;
        }
        if (!argument.empty() && argument[0] == '-')
        {
            std::fprintf(stderr, "unknown option: %s\n", argument.c_str());
            return 2;
        }
        if (elf_path == nullptr)
        {
            elf_path = argv[i];
        }
        else if (library_path == nullptr)
        {
            library_path = argv[i];
        }
        else
        {
            std::fprintf(stderr, "unexpected positional argument: %s\n", argument.c_str());
            return 2;
        }
    }

    if (loop_yield_resumes != 0u && loop_budget == 0u)
    {
        std::fprintf(stderr,
            "--loop-yield-resumes requires --loop-budget > 0; "
            "no guest-cycle clock is provided by this option\n");
        return 2;
    }

    if (iop_rom0_path == nullptr && disc_folder_path == nullptr)
    {
        // The explicit folder HLE mode takes precedence over the legacy
        // SDBZ_IOP_ROM0 environment. No BIOS file is required for the folder.
        // Host-only boot configuration: run_game.py can pass this
        // through unchanged without invalidating the existing EE
        // translation provenance / --no-recompile cache.
        iop_rom0_path = read_host_environment(
            "SDBZ_IOP_ROM0", &iop_rom0_environment);
    }
    if (iop_rom0_path != nullptr && iop_rom0_path[0] == '\0')
    {
        std::fprintf(stderr, "empty SDBZ_IOP_ROM0 path\n");
        return 2;
    }
    if (iop_rom0_path != nullptr && !iop_quantum_explicit)
    {
        // The standalone official OpenPS2BIOS test grants 10 million
        // guest instructions for ROM0 readiness. 256 instructions per
        // SMFLAG poll gave the game only 65,536 before its 256-read guard.
        // This bounded quantum gives firmware time to actually execute;
        // it does not invent SIF register values or claim cycle accuracy.
        iop_quantum = 65536u;
        std::string quantum_environment;
        const char* configured = read_host_environment(
            "SDBZ_IOP_ROM0_QUANTUM", &quantum_environment);
        if (configured != nullptr &&
            !parse_u32(configured, &iop_quantum))
        {
            std::fprintf(stderr,
                         "invalid SDBZ_IOP_ROM0_QUANTUM value: %s\n",
                         configured);
            return 2;
        }
    }
    if (elf_path == nullptr || library_path == nullptr || budget == 0 ||
        iop_quantum == 0u || iop_quantum > 65536u ||
        (iop_start_all && iop_irx_paths.empty()) ||
        (iop_rom0_path != nullptr && !iop_irx_paths.empty()) ||
        (disc_image_path != nullptr && iop_rom0_path == nullptr) ||
        (disc_folder_path != nullptr &&
         (disc_image_path != nullptr || iop_rom0_path != nullptr)) ||
        stack >= k_ee_ram_size ||
        (has_trace_entry && (trace_entry & 3u) != 0u) ||
        (has_watch_word && (watch_word & 3u) != 0u) ||
        (has_trace_store && (trace_store & 3u) != 0u))
    {
        usage(argv[0]);
        return 2;
    }

    if (disc_folder_path != nullptr)
    {
        std::error_code folder_error;
        const std::filesystem::path folder(disc_folder_path);
        if (!std::filesystem::is_directory(folder, folder_error) ||
            folder_error ||
            std::filesystem::is_symlink(
                std::filesystem::symlink_status(folder, folder_error)) ||
            folder_error)
        {
            std::fprintf(stderr,
                "disc folder must be an existing, non-symlink directory: %s\n",
                disc_folder_path);
            return 2;
        }
    }

    // A full cached replay can execute millions of dispatches while
    // reporting every one of them verbatim. An opt-in host-only cadence
    // reduces trace I/O without affecting guest dispatch cadence, IRQ
    // sampling, RPC scheduling, report snapshots or saved histories.
    if (gs_webgpu_window && !has_speed_percent)
    {
        std::string speed_environment;
        const char* speed = read_host_environment("SDBZ_SPEED_PERCENT", &speed_environment);
        if (speed != nullptr && (!parse_u32(speed, &speed_percent) || speed_percent > 1000u))
        {
            std::fprintf(stderr, "invalid SDBZ_SPEED_PERCENT (expected 0..1000)\n");
            return 2;
        }
    }
    // Preserve device-free uncapped benchmarks and headless runs by default.
    bool host_audio_enabled = gs_webgpu_window && speed_percent != 0u;
    std::string host_audio_environment;
    const char* host_audio_setting = read_host_environment(
        "SDBZ_HOST_AUDIO", &host_audio_environment);
    if (host_audio_setting != nullptr)
    {
        if (std::strcmp(host_audio_setting, "0") == 0)
        {
            host_audio_enabled = false;
        }
        else if (std::strcmp(host_audio_setting, "1") == 0)
        {
            host_audio_enabled = true;
        }
        else
        {
            std::fprintf(stderr, "invalid SDBZ_HOST_AUDIO (expected 0 or 1)\n");
            return 2;
        }
    }
    // Both CDVDMAN and SIF RPC use the same optional frozen UTC
    // source. Reject invalid input before running any guest code.
    if (const char* fixed_rtc = std::getenv("SDBZ_RTC_FIXED_UTC"))
    {
        std::time_t utc{};
        if (!iop::parse_fixed_cdvd_rtc(fixed_rtc, utc))
        {
            std::fprintf(stderr,
                "Invalid SDBZ_RTC_FIXED_UTC (UTC Unix seconds in the PS2 RTC era)\n");
            return 2;
        }
        std::printf("IOP CDVD RTC: fixed UTC=%llu (diagnostic)\n",
            static_cast<unsigned long long>(utc));
    }
    uint32_t dispatch_trace_every = 1u;
    std::string dispatch_trace_environment;
    const char* dispatch_trace_setting = read_host_environment(
        "SDBZ_DISPATCH_TRACE_EVERY", &dispatch_trace_environment);
    if (dispatch_trace_setting != nullptr &&
        (!parse_u32(dispatch_trace_setting, &dispatch_trace_every) ||
         dispatch_trace_every == 0u))
    {
        std::fprintf(stderr,
            "invalid SDBZ_DISPATCH_TRACE_EVERY "
            "(expected a positive integer)\n");
        return 2;
    }
    uint32_t state_trace_first = 0u;
    uint32_t state_trace_last = 0u;
    uint32_t state_trace_every = 1u;
    uint64_t state_trace_next = 0u;
    uint64_t state_trace_observations = 0u;
    uint64_t state_trace_path = 14695981039346656037ull;
    uint64_t state_trace_clocks = 14695981039346656037ull;
    std::string state_trace_every_environment;
    const char* state_trace_stride = read_host_environment(
        "SDBZ_STATE_TRACE_EVERY", &state_trace_every_environment);
    if (state_trace_stride != nullptr &&
        (!parse_u32(state_trace_stride, &state_trace_every) ||
         state_trace_every == 0u))
    {
        std::fprintf(stderr, "Invalid SDBZ_STATE_TRACE_EVERY (positive integer)\n");
        return 2;
    }
    std::string state_trace_first_environment;
    std::string state_trace_last_environment;
    const char* state_trace_begin = read_host_environment(
        "SDBZ_STATE_TRACE_FIRST", &state_trace_first_environment);
    const char* state_trace_end = read_host_environment(
        "SDBZ_STATE_TRACE_LAST", &state_trace_last_environment);
    if ((state_trace_begin == nullptr) != (state_trace_end == nullptr) ||
        (state_trace_begin != nullptr &&
         (!parse_u32(state_trace_begin, &state_trace_first) ||
          !parse_u32(state_trace_end, &state_trace_last) ||
          state_trace_first == 0u || state_trace_last < state_trace_first ||
          (state_trace_last - state_trace_first) / state_trace_every >= 100000u)))
    {
        std::fprintf(stderr,
            "Invalid SDBZ_STATE_TRACE_FIRST/LAST (positive inclusive range, "
            "at most 100000 sampled dispatch intervals)\n");
        return 2;
    }
    std::string state_trace_detail_environment;
    const char* state_trace_detail_value = read_host_environment(
        "SDBZ_STATE_TRACE_DETAIL", &state_trace_detail_environment);
    const bool state_trace_detail = state_trace_detail_value != nullptr &&
        std::strcmp(state_trace_detail_value, "1") == 0;
    if (state_trace_detail &&
        (state_trace_first == 0u || state_trace_every != 1u ||
         state_trace_last - state_trace_first >= 256u))
    {
        std::fprintf(stderr,
            "SDBZ_STATE_TRACE_DETAIL requires a dense range of at most 256 dispatches\n");
        return 2;
    }
    state_trace_next = state_trace_first;
    uint32_t report_progress_interval = static_cast<uint32_t>(k_report_progress_interval);
    std::string progress_environment;
    const char* progress_setting = read_host_environment(
        "SDBZ_REPORT_PROGRESS_EVERY", &progress_environment);
    if (progress_setting != nullptr &&
        (!parse_u32(progress_setting, &report_progress_interval) ||
         report_progress_interval == 0u))
    {
        std::fprintf(stderr,
            "invalid SDBZ_REPORT_PROGRESS_EVERY (expected a positive integer)\n");
        return 2;
    }
    if (report_path != nullptr && report_progress_interval != k_report_progress_interval)
    {
        std::printf("EE progress reporting stride: %u (snapshots and history unchanged)\n",
            report_progress_interval);
    }
    bool trace_buffered = false;
    std::string trace_buffer_environment;
    const char* trace_buffer_setting = read_host_environment(
        "SDBZ_TRACE_BUFFERED", &trace_buffer_environment);
    if (trace_buffer_setting != nullptr)
    {
        if (std::strcmp(trace_buffer_setting, "1") == 0)
        {
            trace_buffered = true;
        }
        else if (std::strcmp(trace_buffer_setting, "0") != 0)
        {
            std::fprintf(stderr,
                "invalid SDBZ_TRACE_BUFFERED (expected 0 or 1)\n");
            return 2;
        }
    }
    // Reproducible, explicitly requested controller response to a real
    // guest SIO2 pad poll. Never press Cross by default.
    uint32_t pad0_cross_first_poll = 0u;
    uint32_t pad0_cross_poll_count = 0u;
    std::string pad0_cross_first_environment;
    std::string pad0_cross_count_environment;
    const char* pad0_first = read_host_environment(
        "SDBZ_PAD0_CROSS_FIRST_POLL", &pad0_cross_first_environment);
    const char* pad0_count = read_host_environment(
        "SDBZ_PAD0_CROSS_POLL_COUNT", &pad0_cross_count_environment);
    if ((pad0_first == nullptr) != (pad0_count == nullptr) ||
        (pad0_first != nullptr &&
         (!parse_u32(pad0_first, &pad0_cross_first_poll) ||
          !parse_u32(pad0_count, &pad0_cross_poll_count) ||
          pad0_cross_first_poll == 0u ||
          pad0_cross_poll_count == 0u ||
          pad0_cross_poll_count > 120u)))
    {
        std::fprintf(stderr,
            "SDBZ_PAD0_CROSS_FIRST_POLL and "
            "SDBZ_PAD0_CROSS_POLL_COUNT must both be set; "
            "expected positive first poll and 1..120 held polls\n");
        return 2;
    }
    std::vector<iop::c_hw::s_pad0_script_step> pad0_script;
    {
        std::string script_environment;
        const char* script = read_host_environment(
            "SDBZ_PAD0_SCRIPT", &script_environment);
        if (script != nullptr && script[0] != '\0')
        {
            std::vector<ee::s_pad_script_step> parsed;
            std::string script_error;
            if (!ee::parse_pad_script(script, &parsed, &script_error))
            {
                std::fprintf(stderr, "invalid SDBZ_PAD0_SCRIPT: %s\n",
                             script_error.c_str());
                return 2;
            }
            for (const ee::s_pad_script_step& step : parsed)
            {
                iop::c_hw::s_pad0_script_step hw_step{};
                hw_step.m_first_poll = step.first_poll;
                hw_step.m_poll_count = step.poll_count;
                hw_step.m_buttons = step.buttons;
                pad0_script.push_back(hw_step);
            }
        }
    }
    iop::c_pad_poll_replay pad_poll;
    std::string pad_record_environment;
    std::string pad_replay_environment;
    const char* pad_record = read_host_environment(
        "SDBZ_PAD0_RECORD", &pad_record_environment);
    const char* pad_replay = read_host_environment(
        "SDBZ_PAD0_REPLAY", &pad_replay_environment);
    const bool record_pad = pad_record != nullptr &&
        std::strcmp(pad_record, "1") == 0;
    if (record_pad || pad_replay != nullptr)
    {
        if ((record_pad && pad_replay != nullptr) ||
            pad0_cross_poll_count != 0u || !pad0_script.empty() ||
            !hle_startup || iop_rom0_path != nullptr ||
            (iop_irx_paths.empty() && disc_folder_path == nullptr))
        {
            std::fprintf(stderr,
                "Pad recording/replay requires HLE IOP and cannot mix with "
                "another input probe or recording/replay mode\n");
            return 2;
        }
        if (record_pad)
        {
            pad_poll.record(stderr);
        }
        else
        {
            std::ifstream input(pad_replay);
            if (!input || !pad_poll.load(input))
            {
                std::fprintf(stderr, "Invalid or incomplete PAD0 input recording\n");
                return 2;
            }
        }
    }
    if (trace)
    {
        // Keep the historical immediate trace by default. The optional
        // buffered mode coalesces CRT writes without dropping any events.
        // The buffer must outlive stdout, including process-exit flushing.
        if (trace_buffered)
        {
            static char trace_buffer[64u * 1024u];
            if (std::setvbuf(stdout, trace_buffer, _IOFBF,
                    sizeof(trace_buffer)) != 0)
            {
                std::fprintf(stderr, "failed to configure buffered EE trace\n");
                return 2;
            }
            std::printf("EE trace output: buffered (64 KiB; flushed at progress checkpoints)\n");
        }
        else
        {
            // Preserve recent lines if the Python host kills a timeout.
            std::setvbuf(stdout, nullptr, _IONBF, 0);
        }
   …78470 tokens truncated…en)
                        {
                            if (iop_sifcmd == nullptr ||
                                !iop_sifcmd->initialize_hle_bootstrap())
                            {
                                std::fprintf(
                                    stderr,
                                    "IOP HLE reboot could not initialize "
                                    "its SIFCMD services\n"
                                );
                                break;
                            }
                            hle_iop_reboot_bootstrap_pending = false;
                            std::printf(
                                "[iop] HLE reboot bootstrap complete: "
                                "subaddr=0x%08x smflag=0x00060000 "
                                "(modeled services, not ROM0 or IRX execution)\n",
                                iop::c_sifcmd_bridge::k_command_buffer
                            );
                        }
                        uint32_t value = 0u;
                        if (!hardware.sif().get_reg(id, &value))
                        {
                            std::fprintf(
                                stderr,
                                "SifGetReg HLE refused unsupported "
                                "register=0x%08x\n", id
                            );
                            break;
                        }

                        // Only the guest IOP can publish new SMFLAG bits.
                        // Give its already-loaded R3000 module a bounded
                        // execution turn when the EE asks for a missing
                        // IOP-owned flag. No module means no IOP execution.
                        if (id == 4u && value == 0u && iop_kernel != nullptr)
                        {
                            const iop::s_kernel_run_result iop_result =
                                iop_kernel->run(iop_quantum, 128u);
                            if (iop_result.m_stop != iop::e_kernel_stop::budget_exhausted &&
                                iop_result.m_stop != iop::e_kernel_stop::idle)
                            {
                                std::fprintf(
                                    stderr,
                                    "IOP guest stopped: reason=%u thread=%u "
                                    "pc=0x%08x opcode=0x%08x\n",
                                    static_cast<uint32_t>(iop_result.m_stop),
                                    iop_result.m_thread_id,
                                    iop_result.m_guest_pc,
                                    iop_result.m_opcode
                                );
                                if (iop_result.m_stop ==
                                    iop::e_kernel_stop::memory_fault)
                                {
                                    const iop::s_memory_fault& fault =
                                        iop_kernel->cpu().last_memory_fault();
                                    const iop::s_cpu_state& iop_state =
                                        iop_kernel->cpu().state();
                                    std::fprintf(stderr,
                                        "IOP guest memory fault: "
                                        "address=0x%08x width=%u write=%u "
                                        "gpr_at=0x%08x valid=%u\n",
                                        fault.m_address, fault.m_width,
                                        fault.m_write ? 1u : 0u,
                                        iop_state.m_gpr[1],
                                        fault.m_valid ? 1u : 0u);
                                }
                                break;
                            }
                            if (!hardware.sif().get_reg(id, &value))
                            {
                                break;
                            }
                            if (trace && value != 0u)
                            {
                                std::printf(
                                    "IOP guest published SMFLAG: "
                                    "value=0x%08x instructions=%llu\n",
                                    value,
                                    static_cast<unsigned long long>(
                                        iop_result.m_executed
                                    )
                                );
                            }
                        }

                        // MIPS32 scalar values return sign-extended in
                        // the EE's 64-bit GPR low half.
                        cpu.m_gpr[2].m_lo = static_cast<uint64_t>(
                            static_cast<int64_t>(static_cast<int32_t>(value))
                        );
                        cpu.m_gpr[2].m_hi = 0u;
                        if (trace)
                        {
                            std::printf(
                                "HLE SifGetReg: register=0x%08x "
                                "result=0x%08x\n", id, value
                            );
                        }

                        if (sif_poll_limit != 0u && id == 4u && value == 0u)
                        {
                            // One SIF get-reg call can be separated from
                            // the next by several native dispatches. Require
                            // the same guest syscall PC and a tight replay
                            // cadence before treating the read as a stall.
                            if (repeated_sif_smflag_reads != 0u &&
                                last_sif_smflag_epc == epc &&
                                dispatched >= last_sif_smflag_dispatch &&
                                dispatched - last_sif_smflag_dispatch <= 64u)
                            {
                                ++repeated_sif_smflag_reads;
                            }
                            else
                            {
                                repeated_sif_smflag_reads = 1u;
                            }

                            last_sif_smflag_epc = epc;
                            last_sif_smflag_dispatch = dispatched;
                            if (repeated_sif_smflag_reads >= sif_poll_limit)
                            {
                                std::fprintf(
                                    stderr,
                                    "SIF SMFLAG polling stalled: "
                                    "guest_epc=0x%08x register=4 value=0 "
                                    "reads=%u (%s)\n",
                                    epc, repeated_sif_smflag_reads,
                                    iop_kernel == nullptr
                                        ? "IOP producer not running"
                                        : "loaded IOP did not publish SMFLAG"
                                );
                                if (iop_rom0_path != nullptr &&
                                    iop_kernel != nullptr)
                                {
                                    uint32_t subaddr = 0u;
                                    uint32_t msflag = 0u;
                                    uint32_t smflag = 0u;
                                    uint32_t dma9_chcr = 0u;
                                    uint32_t dma10_chcr = 0u;
                                    uint32_t dma_icr = 0u;
                                    uint32_t i_stat = 0u;
                                    uint32_t i_mask = 0u;
                                    uint32_t dma_icr2 = 0u;
                                    uint32_t op_prev = 0u;
                                    uint32_t op_pc = 0u;
                                    uint32_t op_next = 0u;
                                    (void)hardware.sif().get_reg(2u, &subaddr);
                                    (void)hardware.sif().get_reg(3u, &msflag);
                                    (void)hardware.sif().get_reg(4u, &smflag);
                                    (void)iop_kernel->hardware().read(
                                        iop::c_hw::k_sif0_dma_base + 8u,
                                        4u, &dma9_chcr
                                    );
                                    (void)iop_kernel->hardware().read(
                                        iop::c_hw::k_sif1_dma_base + 8u,
                                        4u, &dma10_chcr
                                    );
                                    (void)iop_kernel->hardware().read(
                                        iop::c_hw::k_dma_icr, 4u, &dma_icr);
                                    (void)iop_kernel->hardware().read(
                                        iop::c_hw::k_i_stat, 4u, &i_stat);
                                    (void)iop_kernel->hardware().read(
                                        iop::c_hw::k_i_mask, 4u, &i_mask);
                                    (void)iop_kernel->hardware().read(
                                        iop::c_hw::k_dma_icr2, 4u, &dma_icr2);
                                    const iop::s_cpu_state& iop_state =
                                        iop_kernel->cpu().state();
                                    (void)iop_kernel->cpu().read32(
                                        iop_state.m_pc - 4u, &op_prev);
                                    (void)iop_kernel->cpu().read32(
                                        iop_state.m_pc, &op_pc);
                                    (void)iop_kernel->cpu().read32(
                                        iop_state.m_pc + 4u, &op_next);
                                    std::fprintf(
                                        stderr,
                                        "IOP ROM0 stall state: pc=0x%08x "
                                        "instructions=%llu quantum=%u "
                                        "subaddr=0x%08x msflag=0x%08x "
                                        "smflag=0x%08x dma9_chcr=0x%08x "
                                        "dma10_chcr=0x%08x "
                                        "sif1_delivered=%llu sif0_delivered=%llu\n",
                                        iop_kernel->cpu().state().m_pc,
                                        static_cast<unsigned long long>(
                                            iop_kernel->cycles()),
                                        iop_quantum, subaddr, msflag, smflag,
                                        dma9_chcr, dma10_chcr,
                                        static_cast<unsigned long long>(
                                            iop_sif1_transport->delivered_bytes()),
                                        static_cast<unsigned long long>(
                                            iop_sif0_transport->delivered_bytes())
                                    );
                                    std::fprintf(
                                        stderr,
                                        "IOP ROM0 context: opcodes=%08x/%08x/%08x "
                                        "v0=%08x a0=%08x a1=%08x gp=%08x "
                                        "sp=%08x ra=%08x status=%08x cause=%08x "
                                        "epc=%08x i_stat=%08x i_mask=%08x "
                                        "dma_icr=%08x dma_icr2=%08x\n",
                                        op_prev, op_pc, op_next,
                                        iop_state.m_gpr[2], iop_state.m_gpr[4],
                                        iop_state.m_gpr[5], iop_state.m_gpr[28],
                                        iop_state.m_gpr[29], iop_state.m_gpr[31],
                                        iop_state.m_cop0_status,
                                        iop_state.m_cop0_cause,
                                        iop_state.m_cop0_epc,
                                        i_stat, i_mask, dma_icr, dma_icr2
                                    );
                                    const std::string& iop_console =
                                        iop_kernel->hardware().debug_console();
                                    if (!iop_console.empty())
                                    {
                                        std::fprintf(
                                            stderr,
                                            "IOP ROM0 console: %s%s\n",
                                            iop_console.c_str(),
                                            iop_kernel->hardware()
                                                    .debug_console_truncated()
                                                ? " [truncated]" : ""
                                        );
                                    }
                                }
                                // Fail closed at the observed guest syscall,
                                // not at an arbitrary later dispatch budget.
                                // No IOP-owned status bits are synthesized.
                                cpu.m_cop0[12] &= ~2u;
                                cpu.m_pc = epc;
                                reason = static_cast<uint32_t>(
                                    ee::e_ee_exit_reason::EE_EXIT_UNSUPPORTED
                                );
                                cpu.m_exit_reason = reason;
                                status = 0u;
                                break;
                            }
                        }
                        else
                        {
                            repeated_sif_smflag_reads = 0u;
                        }
                    }
                }

                else if (service == 0x7fu)
                {
                    // EE GetMemorySize (0x7F) reports installed EE RDRAM
                    // capacity in bytes, not the remaining heap capacity.
                    // Reference: ps2-web-recomp:SDBZ/runtime/src/kernel.cpp.
                    // This read-only service cannot create guest progress.
                    cpu.m_gpr[2].m_lo = k_ee_ram_size;
                    cpu.m_gpr[2].m_hi = 0u;
                    if (trace)
                    {
                        std::printf("HLE GetMemorySize: bytes=0x%08x\\n",
                                    k_ee_ram_size);
                    }
                }

                else if (service == 0x7cu)
                {
                    // DECI2 (syscall 0x7C) is the EE debug transport.
                    // Match PCSX2's guest-visible debug-kernel behavior:
                    // OPEN records addr[1]/addr[2], CLOSE clears them,
                    // REQSEND clears the registered channel's word 3,
                    // and POLL/EXRECV/EXSEND acknowledge without a physical
                    // DECI2 backend. No IOP transfer or callback is claimed.
                    const uint32_t operation =
                        static_cast<uint32_t>(cpu.m_gpr[4].m_lo);
                    const uint32_t argument_address =
                        static_cast<uint32_t>(cpu.m_gpr[5].m_lo);

                    if (operation == 1u)
                    {
                        if (argument_address == 0u ||
                            (argument_address & 3u) != 0u ||
                            !memory->contains(argument_address, 16u))
                        {
                            std::fprintf(
                                stderr,
                                "Deci2Call OPEN HLE refused unbacked or "
                                "unaligned descriptor 0x%08x\n",
                                argument_address
                            );
                            break;
                        }

                        deci2_descriptor_address =
                            memory->read32(argument_address + 4u);
                        deci2_handler_pc =
                            memory->read32(argument_address + 8u);
                        if (trace)
                        {
                            std::printf(
                                "HLE Deci2Call OPEN: descriptor=0x%08x "
                                "channel=0x%08x handler=0x%08x result=1\n",
                                argument_address, deci2_descriptor_address,
                                deci2_handler_pc
                            );
                        }
                    }
                    else if (operation == 2u)
                    {
                        deci2_descriptor_address = 0u;
                        deci2_handler_pc = 0u;
                        if (trace)
                        {
                            std::printf("HLE Deci2Call CLOSE: result=1\n");
                        }
                    }
                    else if (operation == 3u)
                    {
                        // PCSX2 Deci2Call(REQSEND) consumes the descriptor
                        // registered at OPEN, not A1 (used for logging only).
                        // It may emit a bounded host debug message, but the
                        // observable EE effects are channel[3]=0 and V0=1.
                        // Do not pretend to transmit anything to the IOP.
                        if (deci2_descriptor_address != 0u)
                        {
                            if ((deci2_descriptor_address & 3u) != 0u ||
                                !memory->contains(deci2_descriptor_address, 32u))
                            {
                                std::fprintf(
                                    stderr,
                                    "Deci2Call REQSEND HLE refused unbacked "
                                    "or unaligned channel 0x%08x\n",
                                    deci2_descriptor_address
                                );
                                break;
                            }

                            const uint32_t packet_size =
                                memory->read32(deci2_descriptor_address + 4u);
                            const uint32_t packet_pointer =
                                memory->read32(deci2_descriptor_address + 16u);
                            const uint32_t pending_before =
                                memory->read32(deci2_descriptor_address + 12u);
                            // The debug-message payload is host-only output,
                            // not an EE architectural effect. Do not read an
                            // unchecked guest pointer or fabricate transport.
                            memory->write32(
                                deci2_descriptor_address + 12u, 0u
                            );
                            if (trace)
                            {
                                std::printf(
                                    "HLE Deci2Call REQSEND: channel=0x%08x "
                                    "packet_ptr=0x%08x packet_size=%u "
                                    "pending_before=0x%08x pending_after=0 "
                                    "result=1\n",
                                    deci2_descriptor_address, packet_pointer,
                                    packet_size, pending_before
                                );
                            }
                        }
                        else if (trace)
                        {
                            // PCSX2 also returns 1 for REQSEND without
                            // a registered channel, without any memory write.
                            std::printf(
                                "HLE Deci2Call REQSEND: no active channel "
                                "result=1\n"
                            );
                        }
                    }
                    else if (operation >= 4u && operation <= 6u)
                    {
                        // PCSX2 HLE returns 1 for POLL, EXRECV and EXSEND
                        // without an EE-visible channel or buffer mutation.
                        // This is not a general DECI2 transport model.
                        if (trace)
                        {
                            const char* name =
                                operation == 4u ? "POLL" :
                                operation == 5u ? "EXRECV" : "EXSEND";
                            std::printf(
                                "HLE Deci2Call %s: result=1 "
                                "(debug-only transport)\n",
                                name
                            );
                        }
                    }
                    else if (operation == 0x10u)
                    {
                        // PCSX2 __Deci2Call(0x10) is KPUTS: A1 points to
                        // a guest 32-bit word containing the string address.
                        // This is debug output only, not SIF/IOP traffic.
                        // Bound and validate the entire read before returning
                        // success; malformed guest pointers fail closed.
                        if (argument_address != 0u)
                        {
                            if ((argument_address & 3u) != 0u ||
                                !memory->contains(argument_address, 4u))
                            {
                                std::fprintf(
                                    stderr,
                                    "Deci2Call KPUTS HLE refused unbacked "
                                    "or unaligned string pointer 0x%08x\n",
                                    argument_address);
                                break;
                            }
                            const uint32_t string_address =
                                memory->read32(argument_address);
                            constexpr size_t k_kputs_max_bytes = 4096u;
                            std::string guest_text;
                            bool terminated = false;
                            for (size_t index = 0u;
                                 index < k_kputs_max_bytes; ++index)
                            {
                                const uint64_t guest_address =
                                    static_cast<uint64_t>(string_address) +
                                    index;
                                if (guest_address > UINT32_MAX ||
                                    !memory->contains(
                                        static_cast<uint32_t>(guest_address),
                                        1u))
                                {
                                    break;
                                }
                                const uint8_t value = memory->read8(
                                    static_cast<uint32_t>(guest_address));
                                if (value == 0u)
                                {
                                    terminated = true;
                                    break;
                                }
                                guest_text.push_back(static_cast<char>(value));
                            }
                            if (!terminated)
                            {
                                std::fprintf(
                                    stderr,
                                    "Deci2Call KPUTS HLE refused unterminated "
                                    "or unbacked guest text 0x%08x\n",
                                    string_address);
                                break;
                            }

                            // A real guest KPUTS message is application
                            // output, not a diagnostic that requires --trace.
                            // Keep host logs line-safe and byte-exact without
                            // claiming to decode guest Shift-JIS.
                            std::string display;
                            for (unsigned char value : guest_text)
                            {
                                if (value >= 0x20u && value <= 0x7eu &&
                                    value != static_cast<unsigned char>('\\') &&
                                    value != static_cast<unsigned char>('"'))
                                {
                                    display.push_back(
                                        static_cast<char>(value));
                                }
                                else
                                {
                                    char encoded[5]{};
                                    std::snprintf(encoded, sizeof(encoded),
                                        "\\x%02x",
                                        static_cast<unsigned int>(value));
                                    display += encoded;
                                }
                            }
                            // Make messages visible in the normal batch
                            // console and the Python launcher's live stream.
                            // Flush so a long-running guest need not exit
                            // before a buffered line becomes visible.
                            std::printf("EE KPUTS: %s\n", display.c_str());
                            std::fflush(stdout);
                            if (trace)
                            {
                                std::printf(
                                    "HLE Deci2Call KPUTS: text=\"%s\" "
                                    "bytes=%zu result=1\n",
                                    display.c_str(), guest_text.size());
                            }
                        }
                        else if (trace)
                        {
                            std::printf(
                                "HLE Deci2Call KPUTS: null argument result=1\n");
                        }
                    }
                    else
                    {
                        std::fprintf(
                            stderr,
                            "Deci2Call HLE refused unsupported operation=%u "
                            "descriptor=0x%08x handler=0x%08x\n",
                            operation, deci2_descriptor_address,
                            deci2_handler_pc
                        );
                        break;
                    }

                    cpu.m_gpr[2].m_lo = 1u;
                    cpu.m_gpr[2].m_hi = 0u;
                }

                else if (service == 0x74u)
                {
                    // SetSyscall(number, handler) installs an EE kernel
                    // callback in the native dispatcher AND in guest RAM.
                    // The game searches the kernel syscall table directly.
                    // Neither registration nor this HLE path invokes it.
                    const uint32_t syscall_number =
                        static_cast<uint32_t>(cpu.m_gpr[4].m_lo);
                    const uint32_t handler_pc =
                        static_cast<uint32_t>(cpu.m_gpr[5].m_lo);
                    // ps2-web-recomp's SetSyscall(0x74) accepts only
                    // indices 0..255. Out-of-range requests are reported
                    // but complete with V0=0, without modifying RAM or
                    // the installed handler table. SDBZ requests 0x12C
                    // after setting up its relocated kernel handlers.
                    if (syscall_number >= k_kernel_syscall_table_entries)
                    {
                        std::fprintf(stderr,
                            "HLE SetSyscall: unsupported number=0x%08x "
                            "handler=0x%08x (ignored)\n",
                            syscall_number, handler_pc
                        );
                    }
                    else
                    {
                        const uint32_t table_address =
                            k_kernel_syscall_table_base +
                            syscall_number * sizeof(uint32_t);
                        // The kernel accepts handlers in direct-mapped EE
                        // RAM, not only executable ELF segments. Zero
                        // clears the registration.
                        const bool handler_backed =
                            handler_pc == 0u ||
                            image.is_executable_address(handler_pc) ||
                            memory->contains(handler_pc, sizeof(uint32_t));
                        if ((handler_pc & 3u) != 0u ||
                            !handler_backed ||
                            !memory->contains(table_address, sizeof(uint32_t)))
                        {
                            std::fprintf(stderr,
                                "SetSyscall HLE refused invalid handler "
                                "number=0x%08x pc=0x%08x\n",
                                syscall_number, handler_pc
                            );
                            break;
                        }
                        // Keep guest-visible lookup and native dispatch in
                        // sync. Invalid in-range registrations still fail
                        // closed without changing either location.
                        memory->write32(table_address, handler_pc);
                        installed_syscall_handlers[syscall_number] = handler_pc;
                        if (trace)
                        {
                            std::printf(
                                "HLE SetSyscall: number=0x%02x "
                                "handler=0x%08x\n",
                                syscall_number, handler_pc
                            );
                        }
                    }
                    // The reference kernel returns zero from SetSyscall,
                    // including for unsupported out-of-range indices.
                    cpu.m_gpr[2].m_lo = 0u;
                    cpu.m_gpr[2].m_hi = 0u;
                }

                else
                {
                    std::fprintf(stderr, "Unhandled EE HLE syscall 0x%02x\n", service);
                    break;
                }

                // Syscall HLE returns to the interrupted guest callback.
                // Do not clear EXL for an interrupt-safe SIF syscall:
                // the registered DMA5 callback still owns its kernel
                // context until it returns via the callback sentinel.
                if (!irq_callback_active)
                {
                    cpu.m_cop0[12] &= ~2u;
                }
                cpu.m_pc = epc + 4u;
                cpu.m_exit_reason = static_cast<uint32_t>(
                    ee::e_ee_exit_reason::EE_EXIT_DISPATCH
                );
                if (reschedule_requested && !irq_callback_active &&
                    !callback_active && !schedule_threads())
                {
                    reason = static_cast<uint32_t>(
                        ee::e_ee_exit_reason::EE_EXIT_UNSUPPORTED);
                    status = 0u;
                    break;
                }
                continue;
            }
            if (irq_callback_active)
            {
                // Architectural EPC still belongs to the interrupted EE
                // thread. m_pad records this nested syscall's actual PC.
                // Preserve the exception and fail closed; report the
                // callback PC so the next unmodeled HLE service is visible.
                std::fprintf(stderr,
                    "HLE IRQ callback syscall unsupported: signed=%d "
                    "actual_pc=0x%08x architectural_epc=0x%08x\n",
                    signed_service, cpu.m_pad, cpu.m_cop0[14]);
            }
        }

        if (status != 1u || count == 0u)
        {
            break;
        }
    }

    // Keep service counters available at a deliberate short replay cap,
    // even when strict controller completion validation rejects the run.
    // These are observations, not an assertion of successful playback.
    if (iop_kernel != nullptr)
    {
        const uint64_t batches = iop_coalesce ? iop_clock_runs : iop_clock_turns;
        std::printf("IOP scheduler totals: dispatches=%llu logical_turns=%llu "
            "service_batches=%llu kernel_runs=%llu quiet_advances=%llu debt=%llu\n",
            static_cast<unsigned long long>(dispatched),
            static_cast<unsigned long long>(iop_clock_turns),
            static_cast<unsigned long long>(batches),
            static_cast<unsigned long long>(batches - iop_quiet_advances),
            static_cast<unsigned long long>(iop_quiet_advances),
            static_cast<unsigned long long>(iop_clock.due()));
    }
    if (attribute_iop)
    {
        std::fprintf(stderr, "IOP service profile: checkpoints=%llu no_debt=%llu "
            "batches=%llu sample_group=%u\n",
            static_cast<unsigned long long>(iop_attribution.checkpoints()),
            static_cast<unsigned long long>(iop_attribution.no_debt()),
            static_cast<unsigned long long>(iop_attribution.batches()),
            ee::c_iop_service_attribution::k_sample_group);
        const char* names[] = {"no_guest", "guest", "quiet"};
        for (uint32_t kind = 0u; kind < static_cast<uint32_t>(
                ee::e_iop_service_kind::count); ++kind)
        {
            const auto& b = iop_attribution.bucket(static_cast<ee::e_iop_service_kind>(kind));
            std::fprintf(stderr, "IOP service profile: kind=%s batches=%llu "
                "requested_cycles=%llu guest_instructions=%llu samples=%llu "
                "sampled_planner_ns=%llu sampled_kernel_ns=%llu rpc_calls=%llu "
                "rpc_samples=%llu sampled_rpc_ns=%llu\n", names[kind],
                static_cast<unsigned long long>(b.m_batches),
                static_cast<unsigned long long>(b.m_requested_cycles),
                static_cast<unsigned long long>(b.m_guest_instructions),
                static_cast<unsigned long long>(b.m_samples),
                static_cast<unsigned long long>(b.m_sampled_planner_ns),
                static_cast<unsigned long long>(b.m_sampled_kernel_ns),
                static_cast<unsigned long long>(b.m_rpc_calls),
                static_cast<unsigned long long>(b.m_rpc_samples),
                static_cast<unsigned long long>(b.m_sampled_rpc_ns));
        }
    }
    if (attribute_iop_kernel)
    {
        const char* kinds[] = {"no_guest", "guest"};
        const char* phases[] = {"wake_irq", "ready", "idle_deadline",
            "idle_advance", "guest_context", "guest_cpu"};
        static_assert(sizeof(phases) / sizeof(phases[0]) ==
            iop::s_kernel_phase_sample::k_count);
        for (uint32_t kind = 0u; kind < 2u; ++kind)
        {
            for (uint32_t phase = 0u; phase < iop::s_kernel_phase_sample::k_count; ++phase)
            {
                std::fprintf(stderr, "IOP kernel profile: kind=%s phase=%s "
                    "sampled_batches=%llu calls=%llu ns=%llu\n",
                    kinds[kind], phases[phase],
                    static_cast<unsigned long long>(iop_phase_samples[kind]),
                    static_cast<unsigned long long>(iop_phase_totals[kind].m_calls[phase]),
                    static_cast<unsigned long long>(iop_phase_totals[kind].m_ns[phase]));
            }
        }
    }
    if (iop_fast_quiet)
    {
        std::printf("IOP clock quiet: enabled=1 attempts=%llu advanced=%llu "
            "fallback=%llu cycles=%llu\n",
            static_cast<unsigned long long>(iop_quiet_attempts),
            static_cast<unsigned long long>(iop_quiet_advances),
            static_cast<unsigned long long>(iop_quiet_attempts - iop_quiet_advances),
            static_cast<unsigned long long>(iop_quiet_cycles));
    }
    if (iop_coalesce)
    {
        std::printf("IOP clock coalesce: enabled=1 logical_turns=%llu physical_runs=%llu "
            "saved_turns=%llu combined_batches=%llu debt=%llu\n",
            static_cast<unsigned long long>(iop_clock_turns),
            static_cast<unsigned long long>(iop_clock_runs),
            static_cast<unsigned long long>(iop_clock_turns - iop_clock_runs),
            static_cast<unsigned long long>(iop_coalesced_batches),
            static_cast<unsigned long long>(iop_clock.due()));
    }

    if (pad_poll.enabled())
    {
        // A recording must reach its requested dispatch budget. Do not
        // put a complete footer on a cancelled, faulted or partial run.
        if (remaining != 0 || !pad_poll.finished())
        {
            if (!record_pad)
            {
                pad_poll.summary(stderr);
            }
            std::fprintf(stderr, "PAD0 input stream incomplete at session stop\n");
            return 1;
        }
        pad_poll.summary(stderr);
    }

    // The last authentic IOP thread state is the most useful artifact
    // when EE reaches its dispatch budget with a guest RPC still active.
    if (report_path != nullptr &&
        hle_startup && iop_kernel != nullptr &&
        iop_rpc != nullptr && iop_rpc->pending_guest_calls() != 0u)
    {
        dump_pending_iop_guest_rpc(
            *iop_kernel, dispatched,
            iop_rpc->pending_guest_calls(), "final");
    }

    if (report_path != nullptr)
    {
        const double elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - wall_start
        ).count();
        std::fprintf(
            stderr,
            "EE timing: dispatched=%llu elapsed_ms=%.1f native_ms=%.1f "
            "report_ms=%.1f slowest_native_pc=0x%08x "
            "slowest_native_ms=%.3f iop_ms=%.1f compile_wait_ms=%.1f fields=%llu\n",
            static_cast<unsigned long long>(dispatched), elapsed_ms,
            static_cast<double>(native_time_ns) / 1e6, report_time_ms,
            slowest_native_pc, static_cast<double>(slowest_native_ns) / 1e6,
            static_cast<double>(iop_time_ns) / 1e6, compile_wait_ms,
            static_cast<unsigned long long>(hardware.scanout().fields_started())
        );
    }
    std::string trace_iop_storage;
    if (read_host_environment("SDBZ_TRACE_IOP_THREADS", &trace_iop_storage) != nullptr && iop_kernel != nullptr)
    {
        for (const auto& item : iop_kernel->threads())
        {
            const iop::s_kernel_thread& thread = item.second;
            std::fprintf(stderr,
                "IOP thread at exit: id=%u status=%u wait=%u prio=%u module=%u "
                "entry=0x%08x pc=0x%08x ra=0x%08x a0=0x%08x "
                "last_syscall=%u at=0x%08x ra=0x%08x a0=0x%08x insns=%llu\n",
                thread.m_id, static_cast<uint32_t>(thread.m_status),
                thread.m_wait_type, thread.m_priority, thread.m_module_id,
                thread.m_entry, thread.m_cpu.m_pc, thread.m_cpu.m_gpr[31],
                thread.m_cpu.m_gpr[4], thread.m_last_syscall_number,
                thread.m_last_syscall_pc, thread.m_last_syscall_ra,
                thread.m_last_syscall_a0,
                static_cast<unsigned long long>(thread.m_guest_instructions));
        }
    }
    std::string trace_ipu_storage;
    if (read_host_environment("SDBZ_TRACE_IPU", &trace_ipu_storage) != nullptr)
    {
        const auto& observed = hardware.ipu_observation();
        std::fprintf(stderr, "EE IPU MMIO: reads=%llu writes=%llu\n",
                     static_cast<unsigned long long>(observed.reads),
                     static_cast<unsigned long long>(observed.writes));
        for (size_t i = 0; i < observed.first.size() && i < 200u; ++i)
        {
            const auto& access = observed.first[i];
            std::fprintf(stderr, "EE IPU MMIO %s addr=0x%08x width=%u value=0x%016llx\n",
                         access.write ? "W" : "R", access.address, access.width,
                         static_cast<unsigned long long>(access.value));
        }
    }
    if (iop_aot != nullptr)
    {
        std::fprintf(stderr,
            "IOP AOT: modules=%zu active=%zu entries=%llu instructions=%llu "
            "translated_ms=%.1f refresh_ms=%.1f\n",
            iop_aot->module_count(), iop_aot->active_count(),
            static_cast<unsigned long long>(iop_aot->entries()),
            static_cast<unsigned long long>(iop_aot->instructions()),
            iop_aot->milliseconds(), iop_aot->refresh_milliseconds());
    }
    // SDBZ_DUMP_RAM=FILE writes the 32 MiB EE RAM image at exit (diagnostic).
    std::string dump_path_storage;
    if (const char* dump_path = read_host_environment("SDBZ_DUMP_RAM", &dump_path_storage))
    {
        if (FILE* dump = open_host_file(dump_path, "wb"))
        {
            std::vector<uint8_t> image(32u * 1024u * 1024u);
            for (uint32_t address = 0u; address < image.size(); address += 4u)
            {
                const uint32_t word = memory->read32(address);
                std::memcpy(image.data() + address, &word, 4u);
            }
            std::fwrite(image.data(), 1u, image.size(), dump);
            std::fclose(dump);
            std::fprintf(stderr, "EE RAM dumped: %s\n", dump_path);
        }
    }
    {
        const ee::c_ee_ipu& ipu = hardware.ipu();
        uint64_t ipu_commands = 0u;
        for (uint32_t command = 0u; command < 16u; ++command)
        {
            ipu_commands += ipu.command_count(command);
        }
        if (ipu_commands != 0u)
        {
            const auto reg = [&](uint32_t address) -> uint32_t
            {
                ee::s_ee_u128 value{};
                return ee::c_ee_hw_bus::mmio_read(&hardware, address, 4u, &value)
                    ? static_cast<uint32_t>(value.m_lo) : 0xdeadbeefu;
            };
            std::fprintf(stderr,
                "EE IPU DMA at exit: ch3 chcr=0x%08x madr=0x%08x qwc=0x%x "
                "ch4 chcr=0x%08x madr=0x%08x qwc=0x%x tadr=0x%08x "
                "d_stat=0x%08x d_pcr=0x%08x d_ctrl=0x%08x i_stat=0x%08x "
                "i_mask=0x%08x ipu_ctrl=0x%08x ipu_bp=0x%08x ipu_cmd=0x%08x\n",
                reg(0x1000b000u), reg(0x1000b010u), reg(0x1000b020u),
                reg(0x1000b400u), reg(0x1000b410u), reg(0x1000b420u),
                reg(0x1000b430u), reg(0x1000e010u), reg(0x1000e020u),
                reg(0x1000e000u), reg(0x1000f000u), reg(0x1000f010u),
                reg(0x10002010u), reg(0x10002020u), reg(0x10002000u));
            std::fprintf(stderr,
                "EE IPU: commands bclr=%llu idec=%llu bdec=%llu vdec=%llu "
                "fdec=%llu setiq=%llu setvq=%llu csc=%llu setth=%llu "
                "stalls=%llu out_qwords=%llu bad_codes=%llu busy=%d\n",
                static_cast<unsigned long long>(ipu.command_count(0u)),
                static_cast<unsigned long long>(ipu.command_count(1u)),
                static_cast<unsigned long long>(ipu.command_count(2u)),
                static_cast<unsigned long long>(ipu.command_count(3u)),
                static_cast<unsigned long long>(ipu.command_count(4u)),
                static_cast<unsigned long long>(ipu.command_count(5u)),
                static_cast<unsigned long long>(ipu.command_count(6u)),
                static_cast<unsigned long long>(ipu.command_count(7u)),
                static_cast<unsigned long long>(ipu.command_count(9u)),
                static_cast<unsigned long long>(ipu.stalls()),
                static_cast<unsigned long long>(ipu.output_qwords()),
                static_cast<unsigned long long>(ipu.bad_codes()),
                ipu.busy() ? 1 : 0);
        }
    }
    if (!pad0_script.empty() && iop_kernel != nullptr)
    {
        std::fprintf(stderr,
            "IOP SIO2 pad0 script: polls=%u scripted_polls=%u steps=%zu\n",
            iop_kernel->hardware().sio2_pad0_poll_count(),
            iop_kernel->hardware().sio2_pad0_script_pressed_polls(),
            pad0_script.size());
    }
    if (idle_skip)
    {
        size_t eligible_loops = 0u;
        for (const auto& item : idle_loop_shapes)
        {
            eligible_loops += item.second.eligible ? 1u : 0u;
        }
        std::fprintf(stderr,
            "EE idle skip: slices=%llu charged_instructions=%llu "
            "mispredictions=%llu analyzed_loops=%zu eligible_loops=%zu\n",
            static_cast<unsigned long long>(idle_skip_slices),
            static_cast<unsigned long long>(idle_skip_instructions),
            static_cast<unsigned long long>(idle_skip_mispredictions),
            idle_loop_shapes.size(), eligible_loops);
        for (const auto& item : idle_loop_shapes)
        {
            std::fprintf(stderr, "EE idle loop: pc=0x%08x %s\n",
                         item.first, item.second.reason);
            const auto& shape = item.second;
            if (shape.carried_read_register != 0u)
            {
                std::fprintf(stderr,
                    "EE idle loop dependency: header=0x%08x read_pc=0x%08x gpr=%u\n",
                    item.first, shape.carried_read_pc, shape.carried_read_register);
            }
            if (!shape.eligible)
            {
                for (size_t i = 0u; i < shape.words.size(); ++i)
                {
                    std::fprintf(stderr,
                        "EE idle loop word: pc=0x%08x word=0x%08x\n",
                        shape.header_pc + static_cast<uint32_t>(4u * i),
                        shape.words[i]);
                }
            }
        }
    }
    if (profile_horizon)
    {
        constexpr const char* names[horizon_sources] = {
            "barrier", "timer", "scanout", "cop0", "iop", "none"
        };
        // Reduce the fixed-size mask histograms only once, rather
        // than updating every overlapping counter on every dispatch.
        uint64_t horizon_dmac_started_channels[ee::c_ee_dmac::k_channel_count]{};
        uint64_t horizon_entry_reasons[5]{};
        uint64_t horizon_vu_reasons[3]{};
        for (uint32_t mask = 1u; mask < 8u; ++mask)
        {
            for (uint32_t index = 0u; index < 3u; ++index)
            {
                if ((mask & (1u << index)) != 0u)
                {
                    horizon_vu_reasons[index] += horizon_vu_masks[mask];
                }
            }
        }
        uint32_t largest_dmac_mask = 0u;
        for (uint32_t mask = 1u; mask < horizon_dmac_mask_count; ++mask)
        {
            if (horizon_dmac_masks[mask] > horizon_dmac_masks[largest_dmac_mask] ||
                largest_dmac_mask == 0u)
            {
                if (horizon_dmac_masks[mask] != 0u)
                {
                    largest_dmac_mask = mask;
                }
            }
            for (uint32_t channel = 0u; channel < ee::c_ee_dmac::k_channel_count;
                 ++channel)
            {
                if ((mask & (1u << channel)) != 0u)
                {
                    horizon_dmac_started_channels[channel] += horizon_dmac_masks[mask];
                }
            }
        }
        for (uint32_t mask = 1u; mask < 32u; ++mask)
        {
            for (uint32_t reason_index = 0u; reason_index < 5u; ++reason_index)
            {
                if ((mask & (1u << reason_index)) != 0u)
                {
                    horizon_entry_reasons[reason_index] += horizon_entry_masks[mask];
                }
            }
        }
        std::fprintf(stderr,
            "EE horizon: shadow_only=1 normal_before=%llu "
            "normal_before_without_iop=%llu normal_before_iop_lookahead=%llu "
            "iop_idle_candidates=%llu iop_rpc_barriers=%llu "
            "lookahead_mmio_free=%llu lookahead_mmio_touched=%llu "
            "gs_worker_barriers=%llu dmac_started_barriers=%llu clock_barriers=%llu "
            "vu_callback_boundaries=%llu cop0_changed_boundaries=%llu "
            "mmio_free_direct_touched=%llu mmio_free_direct_quiet=%llu unknown=%llu "
            "dmac_sif0_only=%llu dmac_passive_sif0=%llu "
            "dmac_str0=%llu dmac_str1=%llu dmac_str2=%llu dmac_str3=%llu dmac_str4=%llu "
            "dmac_str5=%llu dmac_str6=%llu dmac_str7=%llu dmac_str8=%llu dmac_str9=%llu "
            "entry_callback=%llu entry_idle=%llu entry_irq=%llu entry_vu=%llu entry_other=%llu "
            "dmac_top_mask=0x%03x dmac_top_mask_count=%llu "
            "entry_vif1_pending=%llu entry_cmsar1=%llu entry_vu0_running=%llu "
            "alu_bound_candidates=%llu alu_bound_matched=%llu alu_bound_mismatch=%llu "
            "alu_owner_known=%llu alu_owner_matched=%llu "
            "alu_certified=%llu alu_certified_matched=%llu "
            "alu_identity_hits=%llu alu_identity_misses=%llu\n",
            static_cast<unsigned long long>(horizon_normal_before),
            static_cast<unsigned long long>(horizon_without_iop_before),
            static_cast<unsigned long long>(horizon_iop_lookahead_before),
            static_cast<unsigned long long>(horizon_iop_idle_candidates),
            static_cast<unsigned long long>(horizon_iop_rpc_barriers),
            static_cast<unsigned long long>(horizon_lookahead_mmio_free),
            static_cast<unsigned long long>(horizon_lookahead_mmio_touched),
            static_cast<unsigned long long>(horizon_gs_worker_barriers),
            static_cast<unsigned long long>(horizon_dmac_started_barriers),
            static_cast<unsigned long long>(horizon_clock_barriers),
            static_cast<unsigned long long>(horizon_vu_callback_boundaries),
            static_cast<unsigned long long>(horizon_cop0_changed_boundaries),
            static_cast<unsigned long long>(horizon_mmio_free_direct_touched),
            static_cast<unsigned long long>(horizon_mmio_free_direct_quiet),
            static_cast<unsigned long long>(horizon_unknown),
            static_cast<unsigned long long>(horizon_dmac_sif0_only),
            static_cast<unsigned long long>(horizon_dmac_passive_sif0),
            static_cast<unsigned long long>(horizon_dmac_started_channels[0]),
            static_cast<unsigned long long>(horizon_dmac_started_channels[1]),
            static_cast<unsigned long long>(horizon_dmac_started_channels[2]),
            static_cast<unsigned long long>(horizon_dmac_started_channels[3]),
            static_cast<unsigned long long>(horizon_dmac_started_channels[4]),
            static_cast<unsigned long long>(horizon_dmac_started_channels[5]),
            static_cast<unsigned long long>(horizon_dmac_started_channels[6]),
            static_cast<unsigned long long>(horizon_dmac_started_channels[7]),
            static_cast<unsigned long long>(horizon_dmac_started_channels[8]),
            static_cast<unsigned long long>(horizon_dmac_started_channels[9]),
            static_cast<unsigned long long>(horizon_entry_reasons[0]),
            static_cast<unsigned long long>(horizon_entry_reasons[1]),
            static_cast<unsigned long long>(horizon_entry_reasons[2]),
            static_cast<unsigned long long>(horizon_entry_reasons[3]),
            static_cast<unsigned long long>(horizon_entry_reasons[4]),
            largest_dmac_mask,
            static_cast<unsigned long long>(largest_dmac_mask != 0u ?
                horizon_dmac_masks[largest_dmac_mask] : 0u),
            static_cast<unsigned long long>(horizon_vu_reasons[0]),
            static_cast<unsigned long long>(horizon_vu_reasons[1]),
            static_cast<unsigned long long>(horizon_vu_reasons[2]),
            static_cast<unsigned long long>(horizon_alu_candidates),
            static_cast<unsigned long long>(horizon_alu_matched),
            static_cast<unsigned long long>(horizon_alu_mismatch),
            static_cast<unsigned long long>(horizon_alu_owner_known),
            static_cast<unsigned long long>(horizon_alu_owner_matched),
            static_cast<unsigned long long>(horizon_alu_certified),
            static_cast<unsigned long long>(horizon_alu_certified_matched),
            static_cast<unsigned long long>(alu_identities.hits()),
            static_cast<unsigned long long>(alu_identities.misses()));
        for (size_t i = 0u; i < horizon_sources; ++i)
        {
            std::fprintf(stderr,
                "EE horizon: source=%s samples=%llu before=%llu reached=%llu\n",
                names[i], static_cast<unsigned long long>(horizon_samples[i]),
                static_cast<unsigned long long>(horizon_before[i]),
                static_cast<unsigned long long>(horizon_reached[i]));
        }
    }
    if (profile_pcs && !pc_profile.entries().empty())
    {
        const auto& entries = pc_profile.entries();
        std::vector<std::pair<uint32_t, ee::s_pc_profile>> ranked(
            entries.begin(), entries.end());
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto& a, const auto& b)
                  {
                      if (a.second.m_wall_ns != b.second.m_wall_ns)
                      {
                          return a.second.m_wall_ns > b.second.m_wall_ns;
                      }
                      return a.first < b.first;
                  });
        uint64_t total_instructions = 0u;
        uint64_t total_idle_instructions = 0u;
        uint64_t total_wall_ns = 0u;
        uint64_t total_nested_ns = 0u;
        for (const auto& item : ranked)
        {
            total_instructions += item.second.m_instructions;
            total_idle_instructions += item.second.m_idle_instructions;
            total_wall_ns += item.second.m_wall_ns;
            total_nested_ns += item.second.m_nested_ns;
        }
        std::fprintf(stderr,
            "EE profile: %zu entry PCs, %llu guest instructions "
            "(ranked by inclusive native boundary time; compilation excluded) "
            "idle_charged=%llu nested_profile=%u nested_ns=%llu native_ns=%llu\n",
            ranked.size(), static_cast<unsigned long long>(total_instructions),
            static_cast<unsigned long long>(total_idle_instructions),
            ee::g_runtime_profile.m_enabled ? 1u : 0u,
            static_cast<unsigned long long>(total_nested_ns),
            static_cast<unsigned long long>(total_wall_ns));
        for (size_t i = 0; i < ranked.size() && i < 25u; ++i)
        {
            const ee::s_pc_profile& item = ranked[i].second;
            const uint64_t non_skipped = item.m_instructions - item.m_idle_instructions;
            std::fprintf(stderr,
                "EE profile: pc=0x%08x calls=%llu dispatches=%llu "
                "instructions=%llu (%.1f%%) wall_ms=%.3f ns_per_instruction=%.2f "
                "idle_charged=%llu non_skipped=%llu ns_per_non_skipped=%.2f "
                "nested_ns=%llu native_ns=%llu\n",
                ranked[i].first,
                static_cast<unsigned long long>(item.m_calls),
                static_cast<unsigned long long>(item.m_dispatches),
                static_cast<unsigned long long>(item.m_instructions),
                total_instructions != 0u
                    ? 100.0 * static_cast<double>(item.m_instructions) /
                          static_cast<double>(total_instructions)
                    : 0.0,
                static_cast<double>(item.m_wall_ns) / 1.0e6,
                item.m_instructions != 0u
                    ? static_cast<double>(item.m_wall_ns) /
                          static_cast<double>(item.m_instructions)
                    : 0.0,
                static_cast<unsigned long long>(item.m_idle_instructions),
                static_cast<unsigned long long>(non_skipped),
                non_skipped != 0u
                    ? static_cast<double>(item.m_wall_ns) /
                          static_cast<double>(non_skipped)
                    : 0.0,
                static_cast<unsigned long long>(item.m_nested_ns),
                static_cast<unsigned long long>(item.m_wall_ns));
        }
    }

    if (direct_dispatch)
    {
        std::fprintf(stderr,
            "EE direct dispatch: executed=%llu cache_hits=%llu cache_misses=%llu\n",
            static_cast<unsigned long long>(direct_dispatches),
            static_cast<unsigned long long>(native_targets.hits()),
            static_cast<unsigned long long>(native_targets.misses()));
    }

    if (callback_active)
    {
        std::fprintf(stderr,
            "registered syscall handler did not return: pc=0x%08x resume=0x%08x\n",
            cpu.m_pc, callback_resume_pc);
    }

#ifdef EE_ENABLE_WEBGPU
    if (async_gs)
    {
        const bool drained = webgpu_bridge.flush_committed();
        webgpu_bridge.write_worker_statistics(stderr);
        if (!drained)
        {
            std::fprintf(stderr, "GS async worker failed before final snapshot\n");
            return 1;
        }
    }
#endif

    // Only accepted GIF PATH2/PATH3 batches contribute to these counters.
    // This does not imply scanout or rasterized presentation.
    if (gs_state.state().m_tag_count != 0u)
    {
        std::printf(
            "GS PATH2/PATH3 accepted: tags=%llu current_primitive_vertices=%llu "
            "current_transfer_image_qwc=%u transfer_pixels=%u\n",
            static_cast<unsigned long long>(gs_state.state().m_tag_count),
            static_cast<unsigned long long>(gs_state.state().m_vertex_count),
            gs_state.state().m_image_qwc,
            gs_state.state().m_transfer_pixels_written
        );
    }

    if (gs_state.state().m_tag_count != 0u)
    {
        const auto& gs = gs_state.state();
        std::printf(
            "GS PATH2/PATH3 cumulative: batches=%llu prim_writes=%llu "
            "vertex_writes=%llu kicks=%llu adc=%llu "
            "packed=%llu reglist=%llu ad=%llu image_events=%llu "
            "image_qwc=%llu tags_by_flg=[%llu,%llu,%llu,%llu]\n",
            static_cast<unsigned long long>(gs.m_committed_gif_batches),
            static_cast<unsigned long long>(gs.m_total_prim_writes),
            static_cast<unsigned long long>(gs.m_total_vertex_writes),
            static_cast<unsigned long long>(gs.m_total_vertex_kicks),
            static_cast<unsigned long long>(gs.m_total_adc_vertices),
            static_cast<unsigned long long>(gs.m_total_packed_events),
            static_cast<unsigned long long>(gs.m_total_reglist_events),
            static_cast<unsigned long long>(gs.m_total_ad_events),
            static_cast<unsigned long long>(gs.m_total_image_events),
            static_cast<unsigned long long>(gs.m_total_image_qwc),
            static_cast<unsigned long long>(gs.m_tag_format_counts[0]),
            static_cast<unsigned long long>(gs.m_tag_format_counts[1]),
            static_cast<unsigned long long>(gs.m_tag_format_counts[2]),
            static_cast<unsigned long long>(gs.m_tag_format_counts[3]));
        std::printf(
            "GS PATH2/PATH3 sprite provenance: pairs=%llu textured=%llu "
            "nonblack_rgb=%llu samples=first_and_last_%zu\n",
            static_cast<unsigned long long>(gs.m_total_sprite_pair_kicks),
            static_cast<unsigned long long>(gs.m_total_sprite_textured_kicks),
            static_cast<unsigned long long>(gs.m_total_sprite_nonblack_rgb_kicks),
            gs.m_total_sprite_pair_kicks < gs.k_sprite_sample_capacity ?
            static_cast<size_t>(gs.m_total_sprite_pair_kicks) :
            gs.k_sprite_sample_capacity);
    }

    if (memory_card.mounted())
    {
        std::printf("[mc] slot 1 completed SIO2 exchanges: %llu\n",
            static_cast<unsigned long long>(memory_card.completed_exchanges()));
    }
    if (!discard_memory_card_writes && !memory_card.save())
    {
        std::fprintf(stderr, "[mc] failed to save card image: %s\n",
                     memory_card_path);
        return 1;
    }
    if (gs_snapshot_path != nullptr || gs_upload_snapshot_path != nullptr)
    {
        const auto& display = hardware.gs_display_registers();
        std::printf(
            "GS privileged display: PMODE=0x%016llx SMODE2=0x%016llx "
            "DISPFB1=0x%016llx DISPLAY1=0x%016llx "
            "DISPFB2=0x%016llx DISPLAY2=0x%016llx CRT_mode=%u "
            "(register values only; no present event)\n",
            static_cast<unsigned long long>(display.m_pmode),
            static_cast<unsigned long long>(display.m_smode2),
            static_cast<unsigned long long>(display.m_dispfb[0]),
            static_cast<unsigned long long>(display.m_display[0]),
            static_cast<unsigned long long>(display.m_dispfb[1]),
            static_cast<unsigned long long>(display.m_display[1]),
            hardware.gs_crt_mode()
        );
        const uint32_t circuit = (display.m_pmode & 2u) != 0u ? 1u : 0u;
        const uint64_t fb = display.m_dispfb[circuit];
        const uint64_t disp = display.m_display[circuit];
        const uint32_t fbp = static_cast<uint32_t>(fb & 0x1ffu);
        const uint32_t fbw = static_cast<uint32_t>((fb >> 9u) & 0x3fu);
        const uint32_t psm = static_cast<uint32_t>((fb >> 15u) & 0x1fu);
        const uint32_t magh = static_cast<uint32_t>((disp >> 23u) & 15u) + 1u;
        const uint32_t dw = static_cast<uint32_t>((disp >> 32u) & 0xfffu) + 1u;
        std::printf(
            "GS display source: circuit=%u FBP=%u FBW=%u PSM=%u "
            "DW=%u MAGH=%u (FBW=0 uses reference one-page stride; "
            "no rasterization or VBlank inferred)\n",
            circuit + 1u, fbp, fbw, psm, dw, magh);
    }

    if (gs_snapshot_path != nullptr)
    {
        graphics_probe.request_capture(ee::e_ee_gs_capture_kind::cpu_display);
        std::vector<uint32_t> pixels;
        uint32_t width = 0u;
        uint32_t height = 0u;
        std::remove(gs_snapshot_path);
        if (ee::c_ee_gs_display::readback(
                gs_state, hardware.gs_display_registers(),
                hardware.gs_crt_mode(), pixels, width, height))
        {
            graphics_probe.record_capture(
                ee::e_ee_gs_capture_kind::cpu_display,
                pixels, width, height, &hardware.gs_display_registers(),
                hardware.gs_crt_mode());
            if (write_gs_ppm(gs_snapshot_path, pixels, width, height))
            {
                graphics_probe.record_file_written(
                    ee::e_ee_gs_capture_kind::cpu_display, true);
                std::printf(
                    "GS diagnostic display VRAM: %ux%u path=%s "
                    "(not a rendered frame; no VBlank generated)\n",
                    width, height, gs_snapshot_path
                );
            }
            else
            {
                std::fprintf(stderr,
                    "GS display snapshot write failed: %s\n",
                    gs_snapshot_path);
            }
        }
        else
        {
            std::printf(
                "GS display snapshot unavailable: no supported "
                "guest-configured display circuit\n");
        }
    }

#ifdef EE_ENABLE_WEBGPU
    if (gs_webgpu_snapshot_path != nullptr)
    {
        graphics_probe.request_capture(ee::e_ee_gs_capture_kind::gpu_display);
        std::vector<uint32_t> pixels;
        uint32_t width = 0u;
        uint32_t height = 0u;
        std::remove(gs_webgpu_snapshot_path);
        const bool readback_succeeded = webgpu_bridge.readback(
            hardware.gs_display_registers(), hardware.gs_crt_mode(),
            pixels, width, height);
        if (readback_succeeded)
        {
            graphics_probe.record_capture(
                ee::e_ee_gs_capture_kind::gpu_display,
                pixels, width, height, &hardware.gs_display_registers(),
                hardware.gs_crt_mode());
        }
        if (readback_succeeded &&
            write_gs_ppm(gs_webgpu_snapshot_path, pixels, width, height))
        {
            graphics_probe.record_file_written(
                ee::e_ee_gs_capture_kind::gpu_display, true);
            std::printf(
                "GS native WebGPU raster readback: %ux%u path=%s "
                "draws=%llu (GPU-processed GS VRAM; not a present event)\n",
                width, height, gs_webgpu_snapshot_path,
                static_cast<unsigned long long>(
                    webgpu_bridge.rendered_primitives()));
        }
        else
        {
            std::fprintf(stderr,
                "GS WebGPU snapshot unavailable: failed render/readback "
                "or unsupported guest display circuit\n");
        }
    }
#endif

    if (gs_upload_snapshot_path != nullptr)
    {
        graphics_probe.request_capture(ee::e_ee_gs_capture_kind::upload);
        std::vector<uint32_t> pixels;
        uint32_t width = 0u;
        uint32_t height = 0u;
        std::remove(gs_upload_snapshot_path);
        if (ee::c_ee_gs_display::readback_upload(
                gs_state, pixels, width, height))
        {
            graphics_probe.record_capture(
                ee::e_ee_gs_capture_kind::upload, pixels, width, height);
            if (write_gs_ppm(gs_upload_snapshot_path, pixels, width, height))
            {
                graphics_probe.record_file_written(
                    ee::e_ee_gs_capture_kind::upload, true);
                std::printf(
                    "GS diagnostic completed upload VRAM: %ux%u path=%s "
                    "(texture transfer, NOT a rendered frame)\n",
                    width, height, gs_upload_snapshot_path);
            }
            else
            {
                std::fprintf(stderr,
                    "GS upload snapshot write failed: %s\n",
                    gs_upload_snapshot_path);
            }
        }
        else
        {
            std::printf(
                "GS upload snapshot unavailable: no completed supported "
                "host-to-local transfer\n");
        }
    }

    std::printf(
        "EE timing: source=%s current_cycles=%llu scheduler_yields=%llu next_gs_field=%llu fields=%llu\n",
        ee_cycle_source_name(runtime.m_ee_cycle_scheduler.source()),
        static_cast<unsigned long long>(runtime.m_ee_cycle_scheduler.current_cycle()),
        static_cast<unsigned long long>(runtime.m_ee_cycle_scheduler.scheduler_yields()),
        static_cast<unsigned long long>(hardware.scanout().next_field_cycle()),
        static_cast<unsigned long long>(hardware.scanout().fields_started()));
    std::printf(
        "EE reference-interpreter instruction slots: %llu "
        "(not R5900 cycles)\n",
        static_cast<unsigned long long>(
            runtime.m_reference_instruction_slots)
    );
    std::printf(
        "EE diagnostic guest-instruction attempts: %llu (NOT EE cycles)\n",
        static_cast<unsigned long long>(
            runtime.m_guest_instruction_attempts)
    );
    std::printf(
        "EE observed HLE timing boundaries: %llu "
        "(reference compatibility: 300 budget units each; not calibrated)\n",
        static_cast<unsigned long long>(runtime.m_hle_timing_boundaries)
    );
    std::printf("EE timing observer: %s (stock runner does not install one)\n",
                runtime.m_timing_observer_failed ? "rejected" : "not validated");
    std::printf(
        "EE stopped: dispatched=%llu pc=0x%08x reason=%s(%u) status=%u\n",
        static_cast<unsigned long long>(dispatched), cpu.m_pc,
        exit_name(reason), reason, status
    );
    if (reason == static_cast<uint32_t>(
            ee::e_ee_exit_reason::EE_EXIT_EXCEPTION))
    {
        // COP0 Cause[6:2] describes the exception; EPC identifies the
        // original guest instruction, not the architectural vector PC.
        const uint32_t cause = cpu.m_cop0[13];
        const uint32_t epc = cpu.m_cop0[14];
        const uint32_t exception_code = (cause >> 2) & 0x1fu;
        uint32_t instruction = 0;
        const bool has_instruction = image.read32_at(epc, &instruction);
        std::printf(
            "EE exception: code=%u EPC=0x%08x Cause=0x%08x "
            "Status=0x%08x BadVAddr=0x%08x\n",
            exception_code, epc, cause,
            cpu.m_cop0[12], cpu.m_cop0[8]
        );
        if (has_instruction)
        {
            std::printf("  instruction=0x%08x", instruction);
            if (exception_code == 8u && (instruction & 0x3fu) == 0x0cu)
            {
                std::printf(" syscall_code=0x%05x", (instruction >> 6) & 0xfffffu);
            }
            std::printf("\n");
        }
        if ((exception_code == 4u || exception_code == 5u) &&
            has_instruction)
        {
            // Address errors need a guest register/instruction context,
            // not a fabricated memory read or an exception workaround.
            // The immediate is sign-extended for regular base+offset
            // loads/stores. This remains diagnostic for other opcodes.
            const uint32_t base_register = (instruction >> 21u) & 31u;
            const int32_t offset = static_cast<int16_t>(instruction & 0xffffu);
            const uint64_t base_value = cpu.m_gpr[base_register].m_lo;
            const uint32_t effective_address = static_cast<uint32_t>(
                base_value + static_cast<int64_t>(offset));
            std::printf(
                "EE address fault: base=r%u value=0x%016llx offset=%d "
                "effective=0x%08x badvaddr=0x%08x "
                "sp=0x%08x ra=0x%08x gp=0x%08x\n",
                base_register,
                static_cast<unsigned long long>(base_value),
                offset, effective_address, cpu.m_cop0[8],
                static_cast<uint32_t>(cpu.m_gpr[29].m_lo),
                static_cast<uint32_t>(cpu.m_gpr[31].m_lo),
                static_cast<uint32_t>(cpu.m_gpr[28].m_lo)
            );
            std::printf("EE address fault native dispatch history "
                        "(last 24 boundaries; before/after are not "
                        "per-instruction values):\n");
            const size_t first = history.size() > 24u
                ? history.size() - 24u : 0u;
            for (size_t index = first; index < history.size(); ++index)
            {
                const s_dispatch_sample& sample = history[index];
                std::printf(
                    "  dispatch=%llu from=0x%08x to=0x%08x"
                    " s0=%08x/%08x s1=%08x/%08x s2=%08x/%08x\n",
                    static_cast<unsigned long long>(sample.m_index),
                    sample.m_from, sample.m_to,
                    sample.m_s0_before, sample.m_s0_after,
                    sample.m_s1_before, sample.m_s1_after,
                    sample.m_s2_before, sample.m_s2_after
                );
            }
            std::printf("EE address fault saved registers:");
            for (uint32_t index = 16u; index <= 23u; ++index)
            {
                std::printf(" s%u=%08x", index - 16u,
                    static_cast<uint32_t>(cpu.m_gpr[index].m_lo));
            }
            std::printf("\n");
            std::printf("EE address fault nearby instructions "
                        "(guest image, raw opcodes):\n");
            for (int32_t delta = -24; delta <= 12; ++delta)
            {
                const int64_t candidate =
                    static_cast<int64_t>(epc) +
                    static_cast<int64_t>(delta) * 4;
                if (candidate < 0 || candidate > UINT32_MAX)
                {
                    continue;
                }
                const uint32_t candidate_pc =
                    static_cast<uint32_t>(candidate);
                uint32_t candidate_word = 0u;
                if (image.read32_at(candidate_pc, &candidate_word))
                {
                    std::printf("  %c 0x%08x: 0x%08x\n",
                        delta == 0 ? '>' : ' ', candidate_pc,
                        candidate_word);
                }
            }
        }
        if (exception_code == 8u)
        {
            std::printf(
                "  v0=0x%016llx v1=0x%016llx "
                "a0=0x%016llx a1=0x%016llx "
                "a2=0x%016llx a3=0x%016llx\n",
                static_cast<unsigned long long>(cpu.m_gpr[2].m_lo),
                static_cast<unsigned long long>(cpu.m_gpr[3].m_lo),
                static_cast<unsigned long long>(cpu.m_gpr[4].m_lo),
                static_cast<unsigned long long>(cpu.m_gpr[5].m_lo),
                static_cast<unsigned long long>(cpu.m_gpr[6].m_lo),
                static_cast<unsigned long long>(cpu.m_gpr[7].m_lo)
            );
        }
    }
    if (runtime.m_guest_store_trace_count != 0u)
    {
        // Dump the actual last stores, oldest first, for both a crashing
        // guest and a successful capped replay. A source operand is not
        // represented as an observed post-write value.
        const uint32_t stored = runtime.m_guest_store_trace_count;
        const uint32_t capacity = runtime.m_guest_store_trace_capacity;
        const uint32_t oldest = stored == capacity ?
            runtime.m_guest_store_trace_next : 0u;
        std::printf(
            "EE guest-store recent: addr=0x%08x count=%u"
            " (before-store operands; oldest first)\n",
            runtime.m_guest_store_trace_word, stored
        );
        for (uint32_t index = 0u; index < stored; ++index)
        {
            const ee::s_ee_guest_store_trace_entry& entry =
                runtime.m_guest_store_trace_recent[(oldest + index) % capacity];
            std::printf(
                "  guest-store pc=0x%08x opcode=0x%08x"
                " effective=0x%08x affected=0x%08x width=%u"
                " lane=%u source=0x%016llx base=0x%016llx"
                " sp=%08x fp=%08x ra=%08x\n",
                entry.m_pc, entry.m_opcode, entry.m_effective_address,
                entry.m_affected_address, entry.m_width, entry.m_source_lane,
                static_cast<unsigned long long>(entry.m_source_value),
                static_cast<unsigned long long>(entry.m_base_value),
                entry.m_sp, entry.m_fp, entry.m_ra
            );
        }
    }
#ifdef EE_ENABLE_WEBGPU
    if (webgpu_bridge.active())
    {
        // All committed GS batches are already drained. Retire any
        // ready timestamp without waiting for a new GPU submission.
        ps2::gs::gpu::collect_timestamp_profile();
    }
    graphics_probe.set_gpu_progress(
        webgpu_bridge.active(), webgpu_bridge.rendered_primitives());
#endif
#ifdef EE_ENABLE_RAYLIB_WINDOW
    if (gs_webgpu_window && speed_percent != 0u)
    {
        std::printf("EE frame pacer: calls=%llu high_res_calls=%llu fallback_calls=%llu "
            "timer_failures=%llu requested_ns=%llu waited_ns=%llu "
            "overshoot_ns=%llu max_overshoot_ns=%llu\n",
            static_cast<unsigned long long>(frame_pacer.calls()),
            static_cast<unsigned long long>(frame_pacer.high_res_calls()),
            static_cast<unsigned long long>(frame_pacer.fallback_calls()),
            static_cast<unsigned long long>(frame_pacer.timer_failures()),
            static_cast<unsigned long long>(frame_pacer.requested_ns()),
            static_cast<unsigned long long>(frame_pacer.waited_ns()),
            static_cast<unsigned long long>(frame_pacer.overshoot_ns()),
            static_cast<unsigned long long>(frame_pacer.max_overshoot_ns()));
    }
#endif
    host_audio.shutdown();
    host_audio.report();
    if (iop_kernel != nullptr)
    {
        const auto& spu2 = iop_kernel->hardware().spu2();
        spu2.report_audio_profile();
        std::printf(
            "IOP SPU2 clock: active=%u iop_cycles=%llu spu_cycles=%llu "
            "elapsed_samples=%llu sample_phase=%u cycles_per_sample=%u\n",
            spu2.clock_active() ? 1u : 0u,
            static_cast<unsigned long long>(iop_kernel->cycles()),
            static_cast<unsigned long long>(spu2.clock_cycles()),
            static_cast<unsigned long long>(spu2.elapsed_samples()),
            spu2.sample_phase(), iop::c_spu2::k_cycles_per_sample);
        std::printf(
            "IOP SPU2 voices: starts=%llu decoded_blocks=%llu end_events=%llu "
            "active0=0x%06x active1=0x%06x\n",
            static_cast<unsigned long long>(spu2.voice_starts()),
            static_cast<unsigned long long>(spu2.decoded_blocks()),
            static_cast<unsigned long long>(spu2.voice_end_events()),
            spu2.active_voice_mask(0u), spu2.active_voice_mask(1u));
        std::printf(
            "IOP SPU2 playback IRQ: events=%llu latches=0x%x next_fetch_cycles=%u\n",
            static_cast<unsigned long long>(spu2.playback_irq_events()),
            spu2.irq_latches(), spu2.voice_irq_cycles_remaining());
        std::printf(
            "IOP SPU2 AutoDMA: starts=%llu completed=%llu bytes=%llu "
            "input_written=%llu input_consumed=%llu buffered0=%u buffered1=%u\n",
            static_cast<unsigned long long>(iop_kernel->hardware().spu2_auto_dma_started()),
            static_cast<unsigned long long>(iop_kernel->hardware().spu2_auto_dma_completed()),
            static_cast<unsigned long long>(iop_kernel->hardware().spu2_auto_dma_bytes()),
            static_cast<unsigned long long>(spu2.input_frames_written()),
            static_cast<unsigned long long>(spu2.input_frames_consumed()),
            spu2.input_frames(0u), spu2.input_frames(1u));
        std::printf(
            "IOP SPU2 PCM: frames=%llu nonzero=%llu peak=%u rate=%u channels=2 bits=16\n",
            static_cast<unsigned long long>(spu2.mixed_frames()),
            static_cast<unsigned long long>(spu2.nonzero_frames()),
            spu2.pcm_peak(), iop::c_spu2::k_sample_rate);
        std::printf(
            "IOP SPU2 mixer rings: batches=%llu irq_events=%llu clipped_samples=%llu\n",
            static_cast<unsigned long long>(spu2.mixer_ring_batches()),
            static_cast<unsigned long long>(spu2.mixer_irq_events()),
            static_cast<unsigned long long>(spu2.clipped_samples()));
        spu2.report_signal_path();
        std::printf("IOP SPU2 effect IRQs: events=%llu\n",
            static_cast<unsigned long long>(spu2.effect_irq_events()));
        std::printf("IOP SPU2 effect input: nonzero_core0=%llu nonzero_core1=%llu\n",
            static_cast<unsigned long long>(spu2.effect_input_frames(0u)),
            static_cast<unsigned long long>(spu2.effect_input_frames(1u)));
        for (uint32_t core = 0u; core < 2u; ++core)
        {
            std::printf("IOP SPU2 effect area: core=%u enabled=%u enable_writes=%llu "
                "start=0x%06x end=0x%06x ticks=%llu return_frames=%llu "
                "fast_ticks=%llu zero_fir_skips=%llu\n",
                core, spu2.effect_enabled(core) ? 1u : 0u,
                static_cast<unsigned long long>(spu2.effect_enable_writes(core)),
                spu2.effect_start(core), spu2.effect_end(core),
                static_cast<unsigned long long>(spu2.effect_ticks(core)),
                static_cast<unsigned long long>(spu2.effect_return_frames(core)),
                static_cast<unsigned long long>(spu2.effect_fast_ticks(core)),
                static_cast<unsigned long long>(spu2.effect_zero_fir_skips(core)));
        }
    }
    if (report_path != nullptr &&
        !write_report(report_path, image, cpu, entry, dispatched,
                      runtime.m_guest_instruction_attempts,
                      runtime, hardware, vsync_flags,
                      reason, status, history, syscall_history, gs_state.state(),
                      gif_path1, gif_path2, gif_path3, vif1,
                      graphics_probe, iop_fault_probe, iop_kernel.get(),
                      dma_wait_probe, pad_return_probe, spawn_probe, hotspots, false))
    {
        return 1;
    }
    if (instruction_trace_path != nullptr)
    {
        FILE* output = open_host_file(instruction_trace_path, "wb");
        if (output == nullptr)
        {
            std::fprintf(stderr, "failed to write instruction trace: %s\n",
                         instruction_trace_path);
            return 1;
        }
        std::fprintf(output, "attempt,pc,opcode_word,kind,reference_slot,hle_service\n");
        for (uint32_t index = 0u; index < runtime.m_instruction_trace_count; ++index)
        {
            const auto& item = instruction_trace[index];
            std::fprintf(output, "%llu,0x%08x,0x%08x,%u,%llu,%u\n",
                         static_cast<unsigned long long>(item.m_attempt),
                         item.m_pc, item.m_opcode_word, item.m_kind,
                         static_cast<unsigned long long>(item.m_reference_slot),
                         item.m_hle_service);
        }
        std::fprintf(output, "# truncated=%s range=0x%08x:0x%08x\n",
                     runtime.m_instruction_trace_overflow ? "true" : "false",
                     instruction_trace_first_pc, instruction_trace_last_pc);
        if (std::fclose(output) != 0)
        {
            std::fprintf(stderr, "failed to close instruction trace: %s\n",
                         instruction_trace_path);
            return 1;
        }
        std::printf("EE instruction trace: %u entries truncated=%u path=%s "
                    "(NO cycle timestamps)\n",
                    runtime.m_instruction_trace_count,
                    runtime.m_instruction_trace_overflow,
                    instruction_trace_path);

        // M2.0: classify reached words from the actual bounded recorded
        // stream. Kind=1 branch annulment and kind=3 HLE boundaries must
        // never be counted as an executed native instruction.
        ee::c_ee_reached_isa_inventory reached_inventory;
        for (uint32_t index = 0u;
             index < runtime.m_instruction_trace_count; ++index)
        {
            reached_inventory.record(instruction_trace[index]);
        }
        const std::string isa_path =
            std::string(instruction_trace_path) + ".isa.json";
        std::ofstream isa_output(isa_path, std::ios::binary | std::ios::trunc);
        if (!isa_output)
        {
            std::fprintf(stderr,
                         "failed to create reached ISA inventory: %s\n",
                         isa_path.c_str());
            return 1;
        }
        reached_inventory.write_json(
            isa_output, instruction_trace_first_pc,
            instruction_trace_last_pc, runtime.m_instruction_trace_overflow != 0u);
        isa_output.close();
        if (!isa_output)
        {
            std::fprintf(stderr,
                         "failed to write reached ISA inventory: %s\n",
                         isa_path.c_str());
            return 1;
        }
        std::printf("EE reached ISA sample: events=%llu native_reached=%llu "
                    "unique_pc_opcode=%llu path=%s (bounded, not full coverage)\n",
                    static_cast<unsigned long long>(reached_inventory.events()),
                    static_cast<unsigned long long>(reached_inventory.native_reached()),
                    static_cast<unsigned long long>(
                        reached_inventory.totals().m_total),
                    isa_path.c_str());
    }
    if (ee::ee_is_level2_exception_exit(reason))
    {
        const uint32_t cause = cpu.m_cop0[ee::COP0_Cause];
        std::fprintf(stderr,
            "EE Level 2 exception: EXC2=%u BD2=%u ErrorEPC=0x%08x "
            "Cause=0x%08x Status=0x%08x vector=0x%08x "
            "(runner delivery unsupported)\n",
            (cause >> 16u) & 7u, (cause >> 30u) & 1u,
            cpu.m_cop0[ee::COP0_ErrorEPC], cause,
            cpu.m_cop0[ee::COP0_Status], cpu.m_pc);
        return 3;
    }
    if (status == 1u && remaining == 0)
    {
        std::fprintf(stderr, "dispatch budget exhausted; resume or raise the limit\n");
        return 4;
    }
    if (status == 2u)
    {
        std::fprintf(stderr, "invalid native dispatch metadata/entry table\n");
        return 3;
    }
    if (dispatched == 0 &&
        reason != static_cast<uint32_t>(
            ee::e_ee_exit_reason::EE_EXIT_EXCEPTION))
    {
        std::fprintf(stderr, "ELF entry has no translated host block; emit the entry function\n");
        return 3;
    }
    if (reason == static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_HOST_YIELD))
    {
        std::fprintf(stderr,
            "guest host loop yield cap reached: pc=0x%08x "
            "resumes_without_progress=%u cap=%u total_resumes=%u "
            "progress_resets=%llu fields=%llu rpc=%llu gif_writes=%llu "
            "(clock=%s)\n",
            cpu.m_pc, loop_yield_budget.without_progress(),
            loop_yield_resumes, completed_loop_yields,
            static_cast<unsigned long long>(
                loop_yield_budget.progress_resets()),
            static_cast<unsigned long long>(
                hardware.scanout().fields_started()),
            static_cast<unsigned long long>(
                last_iop_rpc_sequence),
            static_cast<unsigned long long>(
                hardware.dmac().gif_trace_count()),
            ee_cycle_source_name(runtime.m_ee_cycle_scheduler.source()));
        // A repeated guest loop may be waiting for DMA, an IOP response,
        // an MMIO bit, or an ordinary RAM flag. More time or extra VBlank
        // fields must not be assumed to satisfy it. Record bounded,
        // read-only diagnostics from the original guest ELF and live CPU
        // state so the actual poll can be identified without a full
        // instruction trace or resetting the translated-function cache.
        std::fprintf(stderr,
            "EE host loop state: pc=0x%08x cause=0x%08x "
            "status=0x%08x epc=0x%08x intc_stat=0x%08x "
            "intc_mask=0x%08x fields=%llu ee_cycles=%llu\n",
            cpu.m_pc, cpu.m_cop0[13], cpu.m_cop0[12],
            cpu.m_cop0[14], hardware.intc().status(),
            hardware.intc().mask(),
            static_cast<unsigned long long>(hardware.scanout().fields_started()),
            static_cast<unsigned long long>(
                runtime.m_ee_cycle_scheduler.current_cycle()));
        // Capture the genuine DMA2 channel state at a host-yield stop.
        // This only reads the DMAC's host-owned registers and validated
        // guest RAM; it neither retries nor completes a DMA transfer.
        const auto gif_register = [&](uint32_t offset) -> uint32_t
        {
            ee::s_ee_u128 value{};
            if (!ee::c_ee_dmac::mmio_read(
                    &hardware.dmac(),
                    ee::c_ee_dmac::k_gif_base + offset, 4u, &value))
            {
                return 0u;
            }
            return static_cast<uint32_t>(value.m_lo);
        };
        const uint32_t gif_chcr = gif_register(0u);
        const uint32_t gif_madr = gif_register(0x10u);
        const uint32_t gif_qwc = gif_register(0x20u);
        const uint32_t gif_tadr = gif_register(0x30u);
        std::fprintf(stderr,
            "EE host GIF DMA: CHCR=0x%08x MADR=0x%08x "
            "QWC=0x%04x TADR=0x%08x D_STAT=0x%08x\n",
            gif_chcr, gif_madr, gif_qwc, gif_tadr,
            hardware.dmac().status());
        const uint32_t gif_tag_mapped =
            (gif_tadr & 0x80000000u) != 0u
                ? ee::ee_memory::k_scratchpad_base +
                    (gif_tadr & static_cast<uint32_t>(
                        ee::ee_memory::k_scratchpad_size - 1u))
                : gif_tadr;
        if ((gif_chcr & ee::c_ee_dmac::k_chcr_str) != 0u &&
            ((gif_chcr >> 2u) & 3u) == 1u &&
            (gif_tadr & 15u) == 0u &&
            ((gif_tadr & 0x80000000u) != 0u ||
             gif_tadr < 0x02000000u) &&
            memory->contains(gif_tag_mapped, 16u))
        {
            const uint64_t tag = memory->read64(gif_tag_mapped);
            std::fprintf(stderr,
                "EE host GIF tag: TADR=0x%08x mapped=0x%08x "
                "raw=0x%016llx ID=%u QWC=%u ADDR=0x%08x "
                "IRQ=%u TTE=%u TIE=%u\n",
                gif_tadr, gif_tag_mapped,
                static_cast<unsigned long long>(tag),
                static_cast<uint32_t>((tag >> 28u) & 7u),
                static_cast<uint32_t>(tag & 0xffffu),
                static_cast<uint32_t>(tag >> 32u),
                (tag & 0x80000000ull) != 0u ? 1u : 0u,
                (gif_chcr & 0x40u) != 0u ? 1u : 0u,
                (gif_chcr & 0x80u) != 0u ? 1u : 0u);
        }
        else if ((gif_chcr & ee::c_ee_dmac::k_chcr_str) != 0u &&
                 ((gif_chcr >> 2u) & 3u) == 1u)
        {
            std::fprintf(stderr,
                "EE host GIF tag: unavailable (invalid or unbacked TADR)\n");
        }
        // These are diagnostic observations of already committed guest
        // MMIO writes. They must not retry a DMA or mutate guest memory.
        const auto gif_reject_name =
            [](ee::c_ee_dmac::e_gif_dma_reject reason) -> const char*
        {
            using e_reject = ee::c_ee_dmac::e_gif_dma_reject;
            switch (reason)
            {
            case e_reject::none: return "none";
            case e_reject::missing_memory_or_sink: return "missing_memory_or_sink";
            case e_reject::invalid_start_state: return "invalid_start_state";
            case e_reject::invalid_tag_address: return "invalid_tag_address";
            case e_reject::unsupported_tag: return "unsupported_tag";
            case e_reject::invalid_payload_span: return "invalid_payload_span";
            case e_reject::chain_qwc_limit: return "chain_qwc_limit";
            case e_reject::chain_tag_limit: return "chain_tag_limit";
            case e_reject::rejected_by_sink: return "rejected_by_sink";
            }
            return "unknown";
        };
        const auto& gif_dma = hardware.dmac();
        const uint64_t gif_write_count = gif_dma.gif_trace_count();
        std::fprintf(stderr,
            "EE host GIF history: writes=%llu last_reject=%s\n",
            static_cast<unsigned long long>(gif_write_count),
            gif_reject_name(gif_dma.gif_last_reject()));
        const uint32_t gif_recent = static_cast<uint32_t>(
            gif_write_count < ee::c_ee_dmac::k_gif_trace_capacity
                ? gif_write_count : ee::c_ee_dmac::k_gif_trace_capacity);
        for (uint32_t index = gif_recent; index > 0u; --index)
        {
            ee::c_ee_dmac::s_gif_dma_write event{};
            if (!gif_dma.gif_trace_newest(index - 1u, &event))
            {
                break;
            }
            std::fprintf(stderr,
                "EE host GIF write: seq=%llu offset=0x%02x value=0x%08x "
                "previous=0x%08x CHCR=0x%08x MADR=0x%08x "
                "QWC=0x%04x TADR=0x%08x reject=%s\n",
                static_cast<unsigned long long>(event.m_sequence),
                event.m_offset, event.m_value, event.m_previous,
                event.m_chcr, event.m_madr, event.m_qwc, event.m_tadr,
                gif_reject_name(event.m_reject));
        }
        // A nearby RAM word can help distinguish a tag *value* from
        // the actual tag *address*. This is explicitly not a tag decode.
        if (gif_madr >= 16u && (gif_madr & 15u) == 0u &&
            memory->contains(gif_madr - 16u, 16u))
        {
            std::fprintf(stderr,
                "EE host GIF nearby RAM: address=0x%08x raw_lo=0x%016llx "
                "(candidate bytes only, not a validated DMA tag)\n",
                gif_madr - 16u,
                static_cast<unsigned long long>(
                    memory->read64(gif_madr - 16u)));
        }
        // If RA still references a JAL callsite, inspect the original
        // ELF callee bytes without executing or modifying guest code.
        // This is context for an investigation, NOT proof that the
        // identified callee wrote the current DMA registers.
        const uint32_t return_address =
            static_cast<uint32_t>(cpu.m_gpr[31].m_lo);
        if (return_address >= 8u)
        {
            const uint32_t callsite = return_address - 8u;
            uint32_t call_word = 0u;
            if (image.read32_at(callsite, &call_word) &&
                (call_word >> 26u) == 3u)
            {
                const uint32_t callee =
                    ((callsite + 4u) & 0xf0000000u) |
                    ((call_word & 0x03ffffffu) << 2u);
                std::fprintf(stderr,
                    "EE host last JAL: RA=0x%08x callsite=0x%08x "
                    "target=0x%08x (context only)\n",
                    return_address, callsite, callee);
                for (uint32_t index = 0u; index < 48u; ++index)
                {
                    const uint64_t address =
                        static_cast<uint64_t>(callee) + index * 4u;
                    uint32_t instruction = 0u;
                    if (address > 0xffffffffull ||
                        !image.read32_at(
                            static_cast<uint32_t>(address),
                            &instruction))
                    {
                        break;
                    }
                    std::fprintf(stderr,
                        "EE host last JAL ELF: 0x%08x opcode=0x%08x\n",
                        static_cast<uint32_t>(address), instruction);
                }
            }
        }
        for (uint32_t timer_index = 0u; timer_index < 4u; ++timer_index)
        {
            std::fprintf(stderr,
                "EE host timer: index=%u COUNT=0x%04x MODE=0x%04x\n",
                timer_index, host_timers.current_count(timer_index),
                host_timers.current_mode(timer_index));
        }
        for (uint32_t index = 0u; index < 32u; index += 4u)
        {
            std::fprintf(stderr,
                "EE host loop GPR: r%02u=0x%016llx r%02u=0x%016llx "
                "r%02u=0x%016llx r%02u=0x%016llx\n",
                index, static_cast<unsigned long long>(cpu.m_gpr[index].m_lo),
                index + 1u,
                static_cast<unsigned long long>(cpu.m_gpr[index + 1u].m_lo),
                index + 2u,
                static_cast<unsigned long long>(cpu.m_gpr[index + 2u].m_lo),
                index + 3u,
                static_cast<unsigned long long>(cpu.m_gpr[index + 3u].m_lo));
        }
        // Only examine a small static window around the exact yielded
        // loop header. This is not a guest execution trace: dynamic RAM
        // writes and MMIO reads are never performed to collect it.
        for (int32_t relative = -64; relative <= 64; relative += 4)
        {
            const int64_t address =
                static_cast<int64_t>(cpu.m_pc) + relative;
            if (address < 0 || address > 0xffffffffll)
            {
                continue;
            }
            uint32_t instruction = 0u;
            if (image.read32_at(static_cast<uint32_t>(address),
                                &instruction))
            {
                std::fprintf(stderr,
                    "EE host loop ELF: %c 0x%08x opcode=0x%08x\n",
                    relative == 0 ? '>' : ' ',
                    static_cast<uint32_t>(address), instruction);
            }
        }
        return 3;
    }
    if (reason == static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_LOOP_BUDGET))
    {
        if (vsync_flags.flag_address() != 0u)
        {
            const uint32_t flag_address = vsync_flags.flag_address();
            const uint32_t csr_address = vsync_flags.csr_address();
            const uint32_t flag_value = memory->contains(flag_address, 4u)
                ? memory->read32(flag_address) : 0u;
            const uint64_t csr_value =
                csr_address != 0u && memory->contains(csr_address, 8u)
                    ? memory->read64(csr_address) : 0u;
            std::fprintf(stderr,
                "EE VBlank wait: INTC_STAT=0x%08x INTC_MASK=0x%08x "
                "flag=0x%08x value=0x%08x csr_dst=0x%08x "
                "csr_dst_value=0x%016llx GS_CSR=0x%016llx "
                "CRT_configured=%u fields=%llu "
                "cycle_source=%s\n",
                hardware.intc().status(), hardware.intc().mask(),
                flag_address, flag_value, csr_address,
                static_cast<unsigned long long>(csr_value),
                static_cast<unsigned long long>(hardware.scanout().csr()),
                hardware.scanout().enabled() ? 1u : 0u,
                static_cast<unsigned long long>(
                    hardware.scanout().fields_started()),
                ee_cycle_source_name(runtime.m_ee_cycle_scheduler.source()));
        }
        std::fprintf(
            stderr,
            "guest loop budget exhausted: next_pc=0x%08x budget=%u; "
            "inspect guest state and translated loop instead of "
            "increasing the wall-clock timeout\n",
            cpu.m_pc, runtime.m_loop_safepoint_limit
        );
        return 3;
    }
    if (reason == static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_UNSUPPORTED))
    {
        // Fail closed, but preserve the actual guest opcode as evidence.
        // COP0 EPC is not the failing instruction for a native unsupported
        // exit; cpu.m_pc is the address emitted by the translated code.
        // Compare the original ELF with live guest RAM to distinguish
        // an unimplemented static opcode from self-modifying guest code.
        const uint32_t failed_pc = cpu.m_pc;
        uint32_t elf_word = 0u;
        const bool has_elf_word = image.read32_at(failed_pc, &elf_word);
        const bool has_ram_word = memory->contains(failed_pc, 4u);
        const uint32_t ram_word = has_ram_word
            ? memory->read32(failed_pc) : 0u;
        std::fprintf(stderr,
            "EE unsupported guest PC: pc=0x%08x "
            "elf=%s0x%08x ram=%s0x%08x "
            "COP0_status=0x%08x COP0_cause=0x%08x COP0_epc=0x%08x\n",
            failed_pc, has_elf_word ? "" : "unmapped:",
            elf_word, has_ram_word ? "" : "unmapped:", ram_word,
            cpu.m_cop0[12], cpu.m_cop0[13], cpu.m_cop0[14]);
        for (int32_t relative = -32; relative <= 64; relative += 4)
        {
            const int64_t address =
                static_cast<int64_t>(failed_pc) + relative;
            if (address < 0 || address > 0xfffffffcll)
            {
                continue;
            }
            const uint32_t guest_address = static_cast<uint32_t>(address);
            uint32_t original = 0u;
            const bool original_valid =
                image.read32_at(guest_address, &original);
            const bool live_valid = memory->contains(guest_address, 4u);
            if (!original_valid && !live_valid)
            {
                continue;
            }
            const uint32_t live = live_valid
                ? memory->read32(guest_address) : 0u;
            std::fprintf(stderr,
                "EE unsupported code: %c pc=0x%08x "
                "ELF=%s0x%08x RAM=%s0x%08x\n",
                relative == 0 ? '>' : ' ', guest_address,
                original_valid ? "" : "unmapped:", original,
                live_valid ? "" : "unmapped:", live);
        }
        for (uint32_t index = 0u; index < 32u; index += 4u)
        {
            std::fprintf(stderr,
                "EE unsupported GPR: r%02u=0x%016llx "
                "r%02u=0x%016llx r%02u=0x%016llx "
                "r%02u=0x%016llx\n",
                index, static_cast<unsigned long long>(cpu.m_gpr[index].m_lo),
                index + 1u,
                static_cast<unsigned long long>(cpu.m_gpr[index + 1u].m_lo),
                index + 2u,
                static_cast<unsigned long long>(cpu.m_gpr[index + 2u].m_lo),
                index + 3u,
                static_cast<unsigned long long>(cpu.m_gpr[index + 3u].m_lo));
        }
        std::fprintf(stderr,
            "guest blocked by an unsupported operation or host service\n");
        return 3;
    }
    if (reason == static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_EXCEPTION) ||
        reason == static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_EXTERNAL_CALL))
    {
        std::fprintf(stderr, "guest blocked by an unsupported operation or host service\n");
        return 3;
    }
#ifdef EE_ENABLE_RAYLIB_WINDOW
    if (raylib_window.closed())
    {
        return 0;
    }
#endif
    // An exit from the linked translated-code set is not a completed PS2 boot.
    std::fprintf(stderr, "guest left compiled code; further runtime/BIOS work required\n");
    return 3;
}
