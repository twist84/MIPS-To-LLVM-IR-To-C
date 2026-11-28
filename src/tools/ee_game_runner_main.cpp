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
    case ee::e_ee_exit_reason::EE_EXIT_EVENT_YIELD: return "EVENT_YIELD";
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
        if (dispatch_trace_every > 1u)
        {
            std::printf(
                "EE dispatch trace cadence: first 64, every %u, "
                "and every non-DISPATCH/non-RETURN or failing transition "
                "(host-only; full hotspots/history retained)\n",
                dispatch_trace_every);
        }
    }

    ee::elf_loader image;
    std::string error;
    if (!image.load_file(elf_path, &error))
    {
        std::fprintf(stderr, "ELF load failed: %s\n", error.c_str());
        return 1;
    }
    // EM_MIPS is the machine identity of PS2 EE executables.
    if (image.machine() != 8u)
    {
        std::fprintf(stderr, "unsupported ELF machine %u (expected MIPS 8)\n",
                     image.machine());
        return 1;
    }

    const uint32_t entry = override_entry ? entry_override : image.entry_point();
    if (!image.is_executable_address(entry))
    {
        std::fprintf(stderr, "entry 0x%08x is not in an executable PT_LOAD segment\n", entry);
        return 1;
    }

    std::unique_ptr<ee::ee_memory> memory(new ee::ee_memory(k_ee_ram_size));
    if (!memory->contains(0, k_ee_ram_size))
    {
        std::fprintf(stderr, "failed to allocate EE RAM\n");
        return 1;
    }

    memory->set_kseg_ram_aliases(kseg_ram);

    std::printf("EE ELF entry 0x%08x; RAM %u MiB; stack 0x%08x\n",
                entry, k_ee_ram_size >> 20, stack);
    if (kernel_ram_path != nullptr &&
        !load_kernel_ram_snapshot(kernel_ram_path, memory.get()))
    {
        return 1;
    }
    if (!load_segments(image, memory.get()))
    {
        return 1;
    }

    c_native_module module;
    if (!module.open(library_path))
    {
        module.print_error();
        return 1;
    }
    t_game_run native_run = module.entry();
    if (native_run == nullptr)
    {
        std::fprintf(stderr, "native library has no ee_game_run export\n");
        return 1;
    }

    std::string direct_dispatch_environment;
    const char* direct_dispatch_setting = read_host_environment(
        "SDBZ_EE_DIRECT_DISPATCH", &direct_dispatch_environment);
    if (direct_dispatch_setting != nullptr && *direct_dispatch_setting != '\0' &&
        std::strcmp(direct_dispatch_setting, "0") != 0 &&
        std::strcmp(direct_dispatch_setting, "1") != 0)
    {
        std::fprintf(stderr, "invalid SDBZ_EE_DIRECT_DISPATCH (expected 0 or 1)\n");
        return 2;
    }
    const bool direct_dispatch = direct_dispatch_setting != nullptr &&
        std::strcmp(direct_dispatch_setting, "1") == 0;
    t_game_resolve_entry native_resolve = direct_dispatch ? module.resolver() : nullptr;
    t_game_resolve_owner native_owner_resolve = module.owner_resolver();
    t_game_alu_words native_alu_certificate = module.alu_certificate();
    ee::c_native_target_cache native_targets(direct_dispatch);
    uint64_t direct_dispatches = 0u;
    if (direct_dispatch)
    {
        std::printf("EE direct dispatch: %s (one architectural boundary per entry)\n",
            native_resolve != nullptr ? "enabled" : "legacy library fallback");
    }

    // Host models INTC/D_STAT and synchronous normal-mode SPR DMA only.
    // There is no general hardware scheduler, BIOS, chain DMA, MFIFO,
    // or complete exception lifecycle.
    ee::s_ee_cpu_state cpu{};
    ee::s_ee_vu0_state vu0{};
    ee::c_ee_hw_bus hardware;
    hardware.bind_cpu(&cpu);
    hardware.bind_memory(memory.get());
    // Real channel-1 VIF packets and channel-2 GIF PATH3 DMA share
    // the existing DMAC sink contract, but have different consumers.
    // Both GIF paths deliver original GS events to the same state.
    ee::c_ee_gif_path3 gif_path3{};
    ee::c_ee_gif_path3 gif_path2{};
    ee::c_ee_gif_path3 gif_path1{};
    std::string gif_digest_environment;
    const char* gif_digest = read_host_environment(
        "SDBZ_GIF_IMAGE_DIGEST", &gif_digest_environment);
    if (gif_digest != nullptr && std::strcmp(gif_digest, "0") == 0)
    {
        gif_path1.set_image_payload_digest(false);
        gif_path2.set_image_payload_digest(false);
        gif_path3.set_image_payload_digest(false);
        std::puts("GIF image payload digest: disabled (event and QWC counters retained)");
    }
    ee::c_ee_vif1 vif0{};
    vif0.bind_vu0(&vu0);
    hardware.bind_vif0(&vif0);
    ee::c_ee_vif1 vif1{};
    ee::c_ee_gs_state gs_state{};
    hardware.bind_gs(&gs_state);
    gif_path3.bind_event_sink(&gs_state, &ee::c_ee_gs_state::gif_events);
    gif_path2.bind_event_sink(&gs_state, &ee::c_ee_gs_state::gif_events);
    gif_path1.bind_event_sink(&gs_state, &ee::c_ee_gs_state::gif_events);
    vif1.bind_path2(&gif_path2);
    vif1.bind_path1(&gif_path1);
    vif1.bind_path3(&gif_path3);
    hardware.bind_vif1(&vif1);

    struct s_graphics_dma_routes
    {
        ee::c_ee_vif1* m_vif0 = nullptr;
        ee::c_ee_vif1* m_vif1 = nullptr;
        ee::c_ee_gif_path3* m_path3 = nullptr;
        bool m_rejection_logged = false;
    };
    s_graphics_dma_routes graphics_dma{&vif0, &vif1, &gif_path3, false};
    hardware.bind_device_transfer(
        &graphics_dma,
        [](void* user, uint32_t channel, const uint8_t* data,
           uint32_t qwc) -> bool
        {
            s_graphics_dma_routes* routes =
                static_cast<s_graphics_dma_routes*>(user);
            bool accepted = false;
            if (channel == 0u)
            {
                accepted = routes->m_vif0->consume(data, qwc);
            }
            else if (channel == 1u)
            {
                accepted = routes->m_vif1->consume(data, qwc);
                if (!accepted && !routes->m_rejection_logged)
                {
                    const auto& diagnostic = routes->m_vif1->diagnostic();
                    std::fprintf(stderr,
                        "VIF1 native reject: reason=%u code=0x%08x "
                        "pending_words=%u vu_pc=%04x vu_lo=%08x vu_hi=%08x "
                        "vu_steps=%u staged_mpg=%u staged_unpack=%u "
                        "kick_reject=%u kick_addr=%03x kick_tag_addr=%03x "
                        "kick_packet_qwc=%u kick_staged_qwc=%u kick_tag=%016llx\n",
                        static_cast<uint32_t>(diagnostic.m_reason),
                        diagnostic.m_code, diagnostic.m_pending_words,
                        diagnostic.m_vu_pc, diagnostic.m_vu_lower,
                        diagnostic.m_vu_upper, diagnostic.m_last_rejected_vu_steps,
                        diagnostic.m_last_rejected_staged_mpg,
                        diagnostic.m_last_rejected_staged_unpack,
                        static_cast<uint32_t>(diagnostic.m_last_rejected_vu_issue.m_kick_reject),
                        diagnostic.m_last_rejected_vu_issue.m_kick_address,
                        diagnostic.m_last_rejected_vu_issue.m_kick_tag_address,
                        diagnostic.m_last_rejected_vu_issue.m_kick_packet_qwc,
                        diagnostic.m_last_rejected_vu_issue.m_kick_staged_qwc,
                        static_cast<unsigned long long>(diagnostic.m_last_rejected_vu_issue.m_kick_tag));
                    routes->m_rejection_logged = true;
                }
            }
            else if (channel == 2u)
            {
                accepted = routes->m_path3->consume(data, qwc);
            }
            return accepted;
        });
    hardware.bind_device_download(&gs_state, &ee::c_ee_gs_state::dma_download);

    ee::c_ee_gs_presentation_probe graphics_probe{};
    graphics_probe.set_window_requested(gs_webgpu_window);
    if (gs_field_probe_path == nullptr)
    {
        gs_field_probe_path = read_host_environment(
            "SDBZ_GS_FIELD_PROBE", &gs_field_probe_environment);
    }
#ifndef EE_ENABLE_WEBGPU
    if (gs_field_probe_path != nullptr && *gs_field_probe_path != '\0')
    {
        std::fprintf(stderr, "GS field probe requires EE_ENABLE_WEBGPU=ON\n");
        return 2;
    }
#endif
#ifdef EE_ENABLE_WEBGPU
    std::unique_ptr<FILE, decltype(&std::fclose)> gs_field_probe_file(
        nullptr, &std::fclose);
    s_gs_field_probe gs_field_probe{};
    if (gs_field_probe_path != nullptr)
    {
        if (*gs_field_probe_path == '\0')
        {
            std::fprintf(stderr, "empty GS field probe path\n");
            return 2;
        }
        std::string start_dispatch_environment;
        const char* start_dispatch = read_host_environment(
            "SDBZ_GS_FIELD_PROBE_START_DISPATCH", &start_dispatch_environment);
        if (start_dispatch != nullptr && *start_dispatch != '\0' &&
            !parse_u32(start_dispatch, &gs_field_probe.m_start_dispatch))
        {
            std::fprintf(stderr,
                "invalid SDBZ_GS_FIELD_PROBE_START_DISPATCH: %s\n",
                start_dispatch);
            return 2;
        }
        gs_field_probe_file.reset(
            open_host_file(gs_field_probe_path, "wb"));
        if (!gs_field_probe_file)
        {
            std::fprintf(stderr, "cannot create GS field probe: %s\n",
                gs_field_probe_path);
            return 2;
        }
        gs_field_probe.m_file = gs_field_probe_file.get();
        gs_field_probe.m_prefix = gs_field_probe_path;
        std::fprintf(gs_field_probe.m_file,
            "{\"event\":\"configuration\","
            "\"schema\":\"gs-field-target-provenance-v1\","
            "\"max_field_samples\":64,\"start_dispatch\":%u,"
            "\"ppm_policy\":\"first_readback_and_first_nonblack_per_target\"}\n",
            gs_field_probe.m_start_dispatch);
        if (std::fflush(gs_field_probe.m_file) != 0)
        {
            return 2;
        }
    }
#ifdef EE_ENABLE_RAYLIB_WINDOW
    ee::c_ee_raylib_window raylib_window{};
#endif
    ee::c_ee_webgpu_bridge webgpu_bridge{};
    if (gs_webgpu_snapshot_path != nullptr || gs_webgpu_window ||
        gs_field_probe.m_file != nullptr)
    {
        if (!webgpu_bridge.init())
        {
            std::fprintf(stderr,
                "native WebGPU adapter or shader initialization failed\n");
            return 2;
        }
        gs_state.bind_committed_event_observer(
            &webgpu_bridge, &ee::c_ee_webgpu_bridge::committed_gif_events);
        std::printf("GS native WebGPU: enabled, committed GS event mirror\n");
        std::string gpu_readback_environment;
        const char* gpu_readback = read_host_environment(
            "SDBZ_GS_GPU_READBACK", &gpu_readback_environment);
        std::string gpu_authority_environment;
        const char* gpu_authority = read_host_environment(
            "SDBZ_GS_GPU_VRAM", &gpu_authority_environment);
        if (gpu_authority != nullptr && std::strcmp(gpu_authority, "1") == 0)
        {
            if (!gs_state.enable_gpu_authority(&webgpu_bridge,
                    &ee::c_ee_webgpu_bridge::read_vram, &ee::c_ee_webgpu_bridge::read_clut))
            {
                std::fprintf(stderr, "cannot enable experimental GPU GS VRAM authority\n");
                return 2;
            }
            std::printf("GS native WebGPU: GPU-authoritative VRAM/CLUT (experimental)\n");
        }
        else if (gpu_readback != nullptr && std::strcmp(gpu_readback, "1") == 0)
        {
            gs_state.bind_vram_readback(
                &webgpu_bridge, &ee::c_ee_webgpu_bridge::read_vram);
            std::printf("GS reverse DMA: GPU VRAM readback enabled (CPU reference retained)\n");
        }
    }
#endif
#ifdef EE_ENABLE_RAYLIB_WINDOW
    if (gs_webgpu_window && !raylib_window.initialize())
    {
        std::fprintf(stderr, "native raylib window initialization failed\n");
        return 2;
    }
#endif

    // Optional real guest IOP execution. This is a deterministic polling
    // rendezvous, NOT EE/IOP hardware cycle synchronization: the native
    // dispatcher does not yet expose an accurate EE-cycle clock.
    iop::c_memory_card memory_card;
    std::string memory_card_environment;
    const char* memory_card_path = read_host_environment(
        "SDBZ_MEMCARD", &memory_card_environment);
    std::filesystem::path memory_card_file;
    if (memory_card_path != nullptr && *memory_card_path != '\0')
    {
        memory_card_file = std::filesystem::path(std::u8string(
            memory_card_path, memory_card_path + std::strlen(memory_card_path)));
        if ((disc_folder_path == nullptr && iop_irx_paths.empty() &&
             iop_rom0_path == nullptr) ||
            !memory_card.mount(memory_card_file))
        {
            std::fprintf(stderr,
                "[mc] mounting requires an active IOP and an exact "
                "8650752-byte raw PS2 card image: %s\n", memory_card_path);
            return 2;
        }
        std::printf("[mc] card mounted in slot 1: %s\n", memory_card_path);
    }
    // Declared before the kernel so translated code outlives the IOP CPU.
    std::unique_ptr<iop::c_iop_aot> iop_aot;
    std::unique_ptr<iop::c_kernel> iop_kernel;
    ee::c_ee_host_audio host_audio;
    iop::c_iop_guest_fault_probe iop_fault_probe{};
    std::unique_ptr<iop::c_sif1_dma_transport> iop_sif1_transport;
    std::unique_ptr<iop::c_sif0_dma_transport> iop_sif0_transport;
    std::unique_ptr<iop::c_sifman_bridge> iop_sifman;
    std::unique_ptr<iop::c_sif1_bridge> iop_sif1;
    std::unique_ptr<iop::c_sifcmd_bridge> iop_sifcmd;
    std::unique_ptr<iop::c_disc_loader> iop_disc;
    std::unique_ptr<iop::c_sif_rpc_bridge> iop_rpc;
    std::unique_ptr<iop::c_kernel_hle_bridge> iop_hle;
    uint64_t last_iop_rpc_sequence = 0u;
    uint64_t observed_iop_rpc_calls = 0u;
    uint32_t last_iop_rpc_server = 0u;
    uint32_t last_iop_rpc_method = 0u;
    uint32_t last_iop_rpc_result = 0u;
    std::string last_iop_rpc_path;
    std::unique_ptr<s_cdvd_context> cdvd;
    std::unique_ptr<iop::c_cdvdman_bridge> iop_cdvdman;
    iop::c_folder_sector_source* folder_disc_source = nullptr;
    bool hle_iop_reboot_bootstrap_pending = false;
    bool hle_iop_reboot_ee_reset_seen = false;
    if (!iop_irx_paths.empty() || iop_rom0_path != nullptr ||
        disc_folder_path != nullptr)
    {
        iop_kernel = std::make_unique<iop::c_kernel>();
        // SDBZ_IOP_AOT=LIB loads ahead-of-time translated IRX code;
        // SDBZ_IOP_AOT_CAPTURE=DIR saves each loaded module's relocated,
        // import-linked .text for tools/iop_recomp.py.
        std::string aot_path_storage;
        if (const char* aot_path = read_host_environment("SDBZ_IOP_AOT", &aot_path_storage))
        {
            // SDBZ_IOP_AOT is a directory of per-module libraries
            // (tools/build_iop_aot.py OUT/modules) or one library.
            iop_aot = std::make_unique<iop::c_iop_aot>();
            std::string aot_error;
            const bool aot_loaded = std::filesystem::is_directory(aot_path)
                ? iop_aot->open_directory(aot_path, &aot_error) != 0u
                : iop_aot->open(aot_path, &aot_error);
            if (!aot_error.empty())
            {
                std::fprintf(stderr, "IOP AOT: %s\n", aot_error.c_str());
            }
            if (aot_loaded)
            {
                iop_kernel->cpu().set_aot(iop_aot.get());
                std::fprintf(stderr, "IOP AOT: loaded %zu translated module(s) from %s\n",
                             iop_aot->module_count(), aot_path);
            }
            else
            {
                std::fprintf(stderr, "IOP AOT: no translated modules loaded "
                             "from %s; interpreting IRX code\n", aot_path);
                iop_aot.reset();
            }
        }
        {
            std::string capture_storage;
            const bool capture_enabled = read_host_environment(
                "SDBZ_IOP_AOT_CAPTURE", &capture_storage) != nullptr;
            iop::c_kernel* kernel_for_aot = iop_kernel.get();
            iop::c_iop_aot* aot_for_kernel = iop_aot.get();
            iop_kernel->set_module_loaded_callback(
                [capture_storage, capture_enabled, kernel_for_aot, aot_for_kernel](
                    const iop::s_irx_module& module)
            {
                if (capture_enabled && module.m_text_size != 0u)
                {
                    std::vector<uint8_t> text(module.m_text_size);
                    if (kernel_for_aot->cpu().dma_read(
                            module.m_base, text.data(), module.m_text_size))
                    {
                        std::string name;
                        for (char ch : module.m_name)
                        {
                            name.push_back(std::isalnum(
                                static_cast<unsigned char>(ch)) ? ch : '_');
                        }
                        // Use validated guest export identities for host
                        // naming; the load base is still the linked image base.
                        std::string export_library;
                        if (!module.m_exports.empty())
                        {
                            for (unsigned char ch : module.m_exports.front().m_library)
                            {
                                const bool safe = (ch >= 'A' && ch <= 'Z') ||
                                    (ch >= 'a' && ch <= 'z') ||
                                    (ch >= '0' && ch <= '9') || ch == '_';
                                export_library.push_back(safe ? static_cast<char>(ch) : '_');
                            }
                        }
                        // LOADFILE's original filename is preferable to an
                        // internal module title or a secondary export name.
                        std::string source_name = module.m_source_name;
                        const size_t version = source_name.find(';');
                        if (version != std::string::npos)
                        {
                            source_name.erase(version);
                        }
                        const size_t extension = source_name.rfind('.');
                        if (extension != std::string::npos)
                        {
                            source_name.erase(extension);
                        }
                        for (char& ch : source_name)
                        {
                            const bool safe = (ch >= 'A' && ch <= 'Z') ||
                                (ch >= 'a' && ch <= 'z') ||
                                (ch >= '0' && ch <= '9') || ch == '_';
                            if (!safe)
                            {
                                ch = '_';
                            }
                        }
                        char stem[128];
                        std::snprintf(stem, sizeof(stem), "%s/%s@%08x",
                                      capture_storage.c_str(), name.c_str(), module.m_base);
                        if (FILE* bin = open_host_file(
                                (std::string(stem) + ".bin").c_str(), "wb"))
                        {
                            std::fwrite(text.data(), 1u, text.size(), bin);
                            std::fclose(bin);
                        }
                        if (FILE* meta = open_host_file(
                                (std::string(stem) + ".json").c_str(), "w"))
                        {
                            std::fprintf(meta,
                                "{\"name\": \"%s\", \"base\": %u, "
                                "\"text_size\": %u, \"entry\": %u, "
                                "\"gp\": %u, \"hash\": \"%016llx\", "
                                "\"source_name\": \"%s\", "
                                "\"export_library\": \"%s\", \"exports\": [",
                                name.c_str(), module.m_base, module.m_text_size,
                                module.m_entry, module.m_gp,
                                static_cast<unsigned long long>(
                                    iop::iop_aot_hash(text.data(), text.size())),
                                source_name.c_str(), export_library.c_str());
                            for (size_t index = 0; index < module.m_exports.size();
                                 ++index)
                            {
                                std::fprintf(meta, "%s%u", index ? ", " : "",
                                             module.m_exports[index].m_entry);
                            }
                            std::fprintf(meta, "]}\n");
                            std::fclose(meta);
                        }
                    }
                }
                if (aot_for_kernel != nullptr)
                {
                    aot_for_kernel->refresh(kernel_for_aot->cpu());
                    std::fprintf(stderr,
                        "IOP AOT: module %s loaded at 0x%08x, %zu translated "
                        "module(s) active\n", module.m_name.c_str(),
                        module.m_base, aot_for_kernel->active_count());
                }
            });
        }
        if (memory_card.mounted())
        {
            iop_kernel->hardware().bind_memory_card(&memory_card);
        }
        if (iop_rom0_path != nullptr)
        {
            std::ifstream rom(
                iop_rom0_path, std::ios::binary | std::ios::ate
            );
            if (!rom || rom.tellg() !=
                    static_cast<std::streampos>(iop::c_cpu::k_rom0_size))
            {
                std::fprintf(
                    stderr, "IOP ROM0 must be a readable, exact 4 MiB "
                    "primary BIOS image: %s\n", iop_rom0_path
                );
                return 1;
            }
            std::vector<uint8_t> image(iop::c_cpu::k_rom0_size);
            rom.seekg(0, std::ios::beg);
            if (!rom.read(
                    reinterpret_cast<char*>(image.data()),
                    static_cast<std::streamsize>(image.size())) ||
                !iop_kernel->start_rom0_boot(image))
            {
                std::fprintf(
                    stderr, "IOP BIOS ROM0 load/reset failed: %s\n",
                    iop_rom0_path
                );
                return 1;
            }
            // Both CPUs must observe the same SIF mailbox. A guest
            // ROM0 store now updates the EE-visible SMFLAG/SMCOM without
            // host injection or a high-level replacement for IOPBOOT.
            iop::c_sif_mmio_link::bind(
                iop_kernel->hardware(), hardware.sif()
            );
            // ROM0 DMA is driven by guest EE/IOP register writes and
            // hardware FIFO bytes, not by the IRX/HLE SifSetDma path.
            iop_sif1_transport =
                std::make_unique<iop::c_sif1_dma_transport>(
                    *memory, hardware.dmac(),
                    iop_kernel->cpu(), iop_kernel->hardware()
                );
            iop_sif0_transport =
                std::make_unique<iop::c_sif0_dma_transport>(
                    *memory, hardware.dmac(),
                    iop_kernel->cpu(), iop_kernel->hardware()
                );
            if (disc_image_path != nullptr)
            {
                cdvd = std::make_unique<s_cdvd_context>();
                auto iso_source =
                    std::make_unique<iop::c_iso_sector_source>(
                    std::filesystem::path(disc_image_path)
                );
                if (!iso_source->valid())
                {
                    std::fprintf(
                        stderr, "CDVD ISO must be readable and aligned to "
                        "2048-byte sectors: %s\n", disc_image_path
                    );
                    return 1;
                }
                cdvd->m_source = std::move(iso_source);
                cdvd->m_transport =
                    std::make_unique<iop::c_cdvd_dma_transport>(
                        iop_kernel->cpu(), iop_kernel->hardware(),
                        *cdvd->m_source
                    );
                std::printf(
                    "CDVD media attached: path=%s sectors=%llu "
                    "sector_size=2048\n", disc_image_path,
                    static_cast<unsigned long long>(
                        cdvd->m_source->sector_count())
                );
            }
            const iop::s_cpu_state& boot =
                iop_kernel->cpu().state();
            std::printf(
                "IOP ROM0 reset: pc=0x%08x cp0_status=0x%08x "
                "cp0_prid=0x%08x (real firmware, not an IRX thread)\n",
                boot.m_pc, boot.m_cop0_status, boot.m_cop0_prid
            );
            std::printf(
                "IOP ROM0 schedule: quantum=%u guest instructions per "
                "unready SMFLAG poll (deterministic, not cycle-accurate)\n",
                iop_quantum
            );
        }
        // Authentic ROM0 has its own sifcmd/sifman implementations in
        // guest RAM. Installing the IRX compatibility stubs here would
        // overwrite firmware-owned memory and create competing services.
        if (iop_rom0_path == nullptr)
        {
            if (!iop_kernel->attach_hle_hardware())
            {
                std::fprintf(stderr, "IOP HLE MMIO attachment failed\n");
                return 1;
            }
            if (pad_poll.enabled())
            {
                iop_kernel->hardware().bind_pad0_poll(
                    &pad_poll, iop::c_pad_poll_replay::poll);
            }
            if (pad0_cross_poll_count != 0u)
            {
                iop_kernel->hardware().configure_sio2_pad0_cross_pulse(
                    pad0_cross_first_poll, pad0_cross_poll_count);
                std::printf(
                    "IOP SIO2 opt-in pad0 Cross pulse: first_poll=%u "
                    "hold_polls=%u (only real port0 0x42 replies)\n",
                    pad0_cross_first_poll, pad0_cross_poll_count);
            }
            if (!pad0_script.empty())
            {
                iop_kernel->hardware().configure_sio2_pad0_script(pad0_script);
                std::printf(
                    "IOP SIO2 pad0 input script: %zu step(s) "
                    "(only real port0 0x42 replies)\n", pad0_script.size());
            }
            iop_hle = std::make_unique<iop::c_kernel_hle_bridge>(
                *iop_kernel);
            if (!iop_hle->install())
            {
                std::fprintf(stderr,
                             "IOP HLE kernel imports installation failed\n");
                return 1;
            }
            iop_sifcmd = std::make_unique<iop::c_sifcmd_bridge>(
                *iop_kernel, hardware.sif(), memory.get()
            );
            if (!iop_sifcmd->install())
            {
                std::fprintf(stderr, "IOP sifcmd service installation failed\n");
                return 1;
            }
            // LOADFILE is backed by actual game disc files, but nothing
            // is loaded until a guest-initialized IOP endpoint receives
            // a valid EE RPC packet through SIF1.
            const std::filesystem::path disc_root =
                disc_folder_path != nullptr
                    ? std::filesystem::path(disc_folder_path)
                    : std::filesystem::path(elf_path).parent_path();
            iop_disc = std::make_unique<iop::c_disc_loader>(
                *iop_kernel, disc_root
            );
            if (disc_folder_path != nullptr)
            {
                auto folder_source =
                    std::make_unique<iop::c_folder_sector_source>(
                        disc_root, memory_card.mounted()
                            ? memory_card_file
                            : std::filesystem::path{});
                if (!folder_source->valid())
                {
                    std::fprintf(
                        stderr, "[disc] invalid extracted media: %s\n",
                        folder_source->error().c_str()
                    );
                    return 1;
                }
                // Only this explicit no-ROM0 folder mode publishes
                // cdvdman HLE. Raw ISO firmware retains its own library.
                iop_cdvdman = std::make_unique<iop::c_cdvdman_bridge>(
                    *iop_kernel, *folder_source
                );
                if (!iop_cdvdman->install())
                {
                    std::fprintf(
                        stderr, "IOP cdvdman search service install failed\n"
                    );
                    return 1;
                }
                folder_disc_source = folder_source.get();
                iop_hle->attach_disc_source(folder_disc_source);
                cdvd = std::make_unique<s_cdvd_context>();
                cdvd->m_source = std::move(folder_source);
                cdvd->m_transport =
                    std::make_unique<iop::c_cdvd_dma_transport>(
                        iop_kernel->cpu(), iop_kernel->hardware(),
                        *cdvd->m_source
                    );
                std::printf(
                    "[disc] HLE folder mounted: %s "
                    "(virtual ISO9660 sectors=%llu; direct LOADFILE paths)\n",
                    disc_root.string().c_str(),
                    static_cast<unsigned long long>(
                        cdvd->m_source->sector_count())
                );
            }
            iop_rpc = std::make_unique<iop::c_sif_rpc_bridge>(
                *iop_disc, *iop_kernel, *iop_sifcmd,
                *memory, hardware.dmac(),
                cdvd != nullptr ? cdvd->m_source.get() : nullptr
            );
            iop_sif1 = std::make_unique<iop::c_sif1_bridge>(
                *memory, iop_kernel->cpu(), hardware.dmac(),
                iop_sifcmd.get(), iop_rpc.get()
            );
            iop_sifman = std::make_unique<iop::c_sifman_bridge>(
                *iop_kernel, hardware.sif(),
                &hardware.dmac(), memory.get()
            );
            if (!iop_sifman->install())
            {
                std::fprintf(stderr, "IOP sifman service installation failed\n");
                return 1;
            }
            if (disc_folder_path != nullptr)
            {
                // HLE-first firmware replacement: the model starts its
                // installed IOP kernel/SIFCMD services ready, matching
                // ps2-web-recomp's SDBZ HLE bootstrap. This is not a
                // guest-produced SMFLAG, IOP module load, or RPC reply.
                if (!iop_sifcmd->initialize_hle_bootstrap())
                {
                    std::fprintf(
                        stderr, "IOP HLE SIFCMD bootstrap failed\n"
                    );
                    return 1;
                }
                std::printf(
                    "[iop] HLE bootstrap: sifcmd=ready "
                    "subaddr=0x%08x smflag=0x00060000 "
                    "(modeled HLE services; no IOP modules started)\n",
                    iop::c_sifcmd_bridge::k_command_buffer
                );
            }
            iop_kernel->bind_syscall([&](iop::c_kernel& active, uint32_t service)
            {
                return iop_hle->handle_syscall(active, service) ||
                       iop_sifman->handle_syscall(active, service) ||
                       iop_sifcmd->handle_syscall(active, service) ||
                       (iop_cdvdman != nullptr &&
                        iop_cdvdman->handle_syscall(active, service));
            });
        }

        else
        {
            std::printf(
                "IOP ROM0 compatibility bridges: disabled "
                "(sifcmd/sifman owned by guest firmware)\n"
            );
        }

        // The optional ROM0 branch has no bootstrap IRX list: the
        // firmware must load and start its own IOP modules.
        // Load the complete explicit IRX list before starting any thread.
        // Each provider registers its relocated guest exports for the next
        // module's import resolver. Nothing runs if a later import fails.
        std::vector<uint32_t> iop_module_ids;
        for (size_t index = 0u; index < iop_irx_paths.size(); ++index)
        {
            const std::string& path = iop_irx_paths[index];
            std::ifstream input(path.c_str(), std::ios::binary | std::ios::ate);
            if (!input)
            {
                std::fprintf(stderr, "IOP IRX cannot be opened: %s\n", path.c_str());
                return 1;
            }
            const std::streampos file_end = input.tellg();
            if (file_end <= 0 ||
                file_end > static_cast<std::streampos>(iop::c_cpu::k_ram_size))
            {
                std::fprintf(stderr, "IOP IRX invalid file size: %s\n", path.c_str());
                return 1;
            }
            std::vector<uint8_t> bytes(static_cast<size_t>(file_end));
            input.seekg(0, std::ios::beg);
            if (!input.read(reinterpret_cast<char*>(bytes.data()),
                            static_cast<std::streamsize>(bytes.size())))
            {
                std::fprintf(stderr, "IOP IRX read failed: %s\n", path.c_str());
                return 1;
            }
            const iop::s_module_load_result loaded = iop_kernel->load_module(
                bytes, std::filesystem::path(path).filename().string());
            if (loaded.m_status != iop::e_irx_load_result::success)
            {
                std::fprintf(stderr,
                             "IOP IRX load failed: index=%zu path=%s status=%u\n",
                             index + 1u, path.c_str(),
                             static_cast<uint32_t>(loaded.m_status));
                return 1;
            }
            iop_module_ids.push_back(loaded.m_id);
            std::printf("IOP IRX linked: %zu/%zu path=%s module=%u\n",
                        index + 1u, iop_irx_paths.size(),
                        path.c_str(), loaded.m_id);
        }

        // The final IRX is the entry module. Providers are linked but not
        // run by default; authentic module startup requires explicit HLE
        // services and the opt-in --iop-start-all mode.
        for (size_t index = 0u; index < iop_irx_paths.size(); ++index)
        {
            if (!iop_start_all && index + 1u != iop_irx_paths.size())
            {
                continue;
            }
            const uint32_t thread_id = iop_kernel->create_module_thread(
                iop_module_ids[index], 1u
            );
            if (thread_id == 0u || !iop_kernel->start_thread(thread_id))
            {
                std::fprintf(stderr, "IOP IRX thread start failed: %s\n",
                             iop_irx_paths[index].c_str());
                return 1;
            }
            std::printf("IOP guest loaded: %s thread=%u quantum=%u "
                        "(SIF-poll rendezvous, not cycle accurate)\n",
                        iop_irx_paths[index].c_str(), thread_id, iop_quantum);
        }
    }

    std::string audio_profile_environment;
    const char* audio_profile_value = read_host_environment(
        "SDBZ_AUDIO_PROFILE", &audio_profile_environment);
    const bool audio_profile_enabled = audio_profile_value != nullptr &&
        std::strcmp(audio_profile_value, "1") == 0;
    std::string effect_fast_path_environment;
    const char* effect_fast_path_value = read_host_environment(
        "SDBZ_SPU2_EFFECT_FAST_PATH", &effect_fast_path_environment);
    const bool effect_fast_path_enabled = effect_fast_path_value == nullptr ||
        std::strcmp(effect_fast_path_value, "0") != 0;
    if (iop_kernel != nullptr)
    {
        iop_kernel->hardware().enable_spu2_audio_profile(audio_profile_enabled);
        iop_kernel->hardware().enable_spu2_effect_fast_path(effect_fast_path_enabled);
        std::printf("IOP SPU2 effect fast path: %s\n",
            effect_fast_path_enabled ? "enabled" : "disabled (reference)");
    }
    if (host_audio_enabled)
    {
        host_audio.start(iop_kernel != nullptr ? &iop_kernel->hardware() : nullptr,
            audio_profile_enabled);
    }

    if (instruction_trace_window_seen && instruction_trace_path == nullptr)
    {
        std::fprintf(stderr,
                     "instruction trace window requires --trace-instructions\n");
        return 2;
    }

    // Bounded, host-owned instruction stream. Recording executes no guest
    // opcodes and changes neither EE timing nor MMIO/interrupt scheduling.
    std::vector<ee::s_ee_instruction_trace_entry> instruction_trace;
    if (instruction_trace_path != nullptr)
    {
        // SDBZ_TRACE_LIMIT=N raises the bounded entry count (diagnostic).
        std::string limit_storage;
        const char* limit_text = read_host_environment("SDBZ_TRACE_LIMIT", &limit_storage);
        const unsigned long long limit = limit_text != nullptr
            ? std::strtoull(limit_text, nullptr, 0) : 0ull;
        instruction_trace.resize(limit != 0ull && limit <= 0x10000000ull
            ? static_cast<size_t>(limit) : k_instruction_trace_limit);
    }
    // Preserve the most recent guest stores, not just the first 64
    // printed matches. This ring is host-owned and disabled by default.
    std::vector<ee::s_ee_guest_store_trace_entry> recent_guest_stores;
    if (has_trace_store)
    {
        recent_guest_stores.resize(64u);
    }
    ee::s_ee_runtime runtime{};
    if (hle_startup && compatibility_clock)
    {
        if (!runtime.m_ee_cycle_scheduler.install_reference_compatibility_source())
        {
            std::fprintf(stderr, "EE compatibility clock installation failed\n");
            return 1;
        }
        // ps2-web-recomp starts the PAL field schedule at boot even
        // before SetGsCrt (system.cpp::boot, Options::pal = true).
        // Only the explicitly enabled compatibility clock owns this
        // bootstrap schedule; a later guest SetGsCrt supersedes it.
        if (!hardware.start_hle_scanout(
                runtime.m_ee_cycle_scheduler.current_cycle()))
        {
            std::fprintf(stderr, "EE HLE startup scanout initialization failed\n");
            return 1;
        }
        // ps2-web-recomp runs guest execution against an event budget.
        // Preserve real safepoints for due GS fields; do not derive time
        // from host dispatches, loop visits, or wall-clock duration.
        if (loop_budget != 0u && loop_yield_resumes == 0u)
        {
            loop_yield_resumes = 256u;
        }
        std::printf("EE clock: reference-compatible guest-execution budget "
                    "(approximate, not R5900 calibrated)\n");
    }
    runtime.m_user = memory.get();
    std::string fastmem_word_environment;
    const char* fastmem_word_setting = read_host_environment(
        "SDBZ_EE_FASTMEM_WORD", &fastmem_word_environment);
    if (fastmem_word_setting != nullptr && *fastmem_word_setting != '\0' &&
        std::strcmp(fastmem_word_setting, "0") != 0 &&
        std::strcmp(fastmem_word_setting, "1") != 0)
    {
        std::fprintf(stderr, "invalid SDBZ_EE_FASTMEM_WORD (expected 0 or 1)\n");
        return 2;
    }
    std::string fastmem_scalar_environment;
    const char* fastmem_scalar_setting = read_host_environment(
        "SDBZ_EE_FASTMEM_SCALAR", &fastmem_scalar_environment);
    if (fastmem_scalar_setting != nullptr && *fastmem_scalar_setting != '\0' &&
        std::strcmp(fastmem_scalar_setting, "0") != 0 &&
        std::strcmp(fastmem_scalar_setting, "1") != 0)
    {
        std::fprintf(stderr, "invalid SDBZ_EE_FASTMEM_SCALAR (expected 0 or 1)\n");
        return 2;
    }
    const bool fastmem_word = fastmem_word_setting != nullptr &&
        std::strcmp(fastmem_word_setting, "1") == 0;
    const bool fastmem_scalar = fastmem_scalar_setting != nullptr &&
        std::strcmp(fastmem_scalar_setting, "1") == 0;
    std::string fastmem_quad_environment;
    const char* fastmem_quad_setting = read_host_environment(
        "SDBZ_EE_FASTMEM_QUAD", &fastmem_quad_environment);
    if (fastmem_quad_setting != nullptr && *fastmem_quad_setting != '\0' &&
        std::strcmp(fastmem_quad_setting, "0") != 0 &&
        std::strcmp(fastmem_quad_setting, "1") != 0)
    {
        std::fprintf(stderr, "invalid SDBZ_EE_FASTMEM_QUAD (expected 0 or 1)\n");
        return 2;
    }
    const bool fastmem_quad = fastmem_quad_setting != nullptr &&
        std::strcmp(fastmem_quad_setting, "1") == 0;
    if (fastmem_word || fastmem_scalar || fastmem_quad)
    {
#if defined(_WIN32) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
        // Restrict this prototype to the current HLE direct physical RAM
        // model. Do not enable it for ROM0 or a future strict MMU path.
        if (!hle_startup)
        {
            std::fprintf(stderr, "EE fastmem word requires HLE startup\n");
            return 2;
        }
        static_assert(k_ee_ram_size <= 0x10000000u, "fastmem RAM below devices");
        uint8_t* ram = memory->physical_ram_span(0u, k_ee_ram_size);
        if (ram == nullptr)
        {
            std::fprintf(stderr, "EE fastmem word RAM span rejected\n");
            return 2;
        }
        if (fastmem_word || fastmem_scalar)
        {
            runtime.m_fastmem_word_base = ram;
            runtime.m_fastmem_word_size = k_ee_ram_size;
            std::printf("EE fastmem word: enabled (direct physical RAM only)\n");
        }
        if (fastmem_scalar)
        {
            runtime.m_fastmem_other_scalar_base = ram;
            runtime.m_fastmem_other_scalar_size = k_ee_ram_size;
            std::printf("EE fastmem scalar: enabled (8/16/32/64-bit physical RAM)\n");
        }
        if (fastmem_quad)
        {
            runtime.m_fastmem_quad_base = ram;
            runtime.m_fastmem_quad_size = k_ee_ram_size;
            std::printf("EE fastmem quad: enabled (128-bit physical RAM)\n");
        }
#else
        std::fprintf(stderr, "EE fastmem word requires little-endian host\n");
        return 2;
#endif
    }
    runtime.m_vu0_state = &vu0;
    runtime.m_vu0_sync = runner_vu0_sync;
    s_runner_vu0_control vu0_control{&vif1, &hardware.dmac(), &runtime};
    vu0_control.m_scheduler.m_user = &vu0_control;
    vu0_control.m_scheduler.m_execute = runner_vu0_execute;
    vu0_control.m_scheduler.m_vu1_catch_up = runner_vu0_vu1_catch_up;
    vu0_control.m_scheduler.m_schedule_event = runner_vu0_schedule_event;
    runtime.m_vu0_user = &vu0_control;
    runtime.m_vu0_start = runner_vu0_start;
    runtime.m_vu0_control_user = &vu0_control;
    runtime.m_vu0_control = runner_vu0_control;
    // The timers share the hardware MMIO bus but own their clocked state
    // in this host runner, leaving cached translated IR unchanged.
    ee::c_ee_host_timers host_timers(&runtime, &hardware);
    runtime.m_mmio_user = &host_timers;
    runtime.m_mmio_read = &ee::c_ee_host_timers::mmio_read;
    runtime.m_mmio_write = &ee::c_ee_host_timers::mmio_write;
    runtime.m_loop_safepoint_limit = loop_budget;
    runtime.m_loop_safepoint_remaining = loop_budget;
    runtime.m_loop_safepoint_yield_enabled = loop_yield_resumes != 0u ? 1u : 0u;
    runtime.m_instruction_trace = instruction_trace.empty() ? nullptr :
        instruction_trace.data();
    runtime.m_instruction_trace_capacity =
        static_cast<uint32_t>(instruction_trace.size());
    runtime.m_instruction_trace_first_pc = instruction_trace_first_pc;
    runtime.m_instruction_trace_last_pc = instruction_trace_last_pc;
    runtime.m_guest_store_trace_enabled = has_trace_store;
    runtime.m_guest_store_trace_word = trace_store;
    runtime.m_guest_store_trace_remaining = has_trace_store ? 64u : 0u;
    runtime.m_guest_store_trace_recent = recent_guest_stores.empty() ?
        nullptr : recent_guest_stores.data();
    runtime.m_guest_store_trace_capacity =
        static_cast<uint32_t>(recent_guest_stores.size());
    runtime.m_guest_v0_trace_first_pc = trace_v0_first_pc;
    runtime.m_guest_v0_trace_last_pc = trace_v0_last_pc;
    runtime.m_guest_v0_trace_remaining = has_trace_v0 ? 64u : 0u;
    cpu.m_pc = entry;
    cpu.m_gpr[29].m_lo = stack;
    // Startup HLE runs without a guest EE BIOS. Match the kernel's
    // initial interrupt-capable main-thread COP0 context rather than
    // leaving IE/IM bits zero; otherwise genuine DMA5 completions
    // assert Cause.IP3 but can never invoke the guest SIF0 handler.
    if (hle_startup)
    {
        cpu.m_cop0[12] = 0x70010c01u;
    }

    // Watchpoints observe *committed* architectural memory between host
    // dispatches. They never modify guest RAM or trigger a guest exception.
    // An unbacked address is a usage error; reading zeros would conceal
    // the same invalid-memory condition being investigated.
    if (has_trace_store && !memory->contains(trace_store, 4u))
    {
        std::fprintf(stderr, "trace-store address is not backed: 0x%08x\n",
                     trace_store);
        return 2;
    }
    if (has_trace_store)
    {
        std::printf("EE guest-store watch: addr=0x%08x max_events=64\n",
                    trace_store);
    }
    if (has_trace_v0)
    {
        std::printf(
            "EE guest-v0 watch: first_pc=0x%08x last_pc=0x%08x max_events=64\n",
            trace_v0_first_pc, trace_v0_last_pc
        );
    }
    if (has_watch_word && !memory->contains(watch_word, 4u))
    {
        std::fprintf(stderr, "watch-word address is not backed: 0x%08x\n",
                     watch_word);
        return 2;
    }
    if (has_watch_word)
    {
        std::printf("EE watch initial: addr=0x%08x value=0x%08x\n",
                    watch_word, memory->read32(watch_word));
    }
    cpu.m_exit_reason = static_cast<uint32_t>(
        ee::e_ee_exit_reason::EE_EXIT_DISPATCH
    );

    uint32_t startup_stack_top = stack;
    uint32_t startup_stack_size = 0;
    // EE libc's heap is shared by the executable's guest threads.
    uint32_t startup_heap_end = 0u;
    bool startup_thread_set = false;

    struct s_kernel_semaphore
    {
        bool m_used = false;
        uint32_t m_count = 0;
        uint32_t m_max_count = 0;
        uint32_t m_initial_count = 0;
        std::vector<uint32_t> m_waiters;
        uint32_t m_attr = 0;
        uint32_t m_option = 0;
    };
    s_kernel_semaphore semaphores[64]{};

    // DECI2 is the EE debugging channel. PCSX2's HLE records OPEN's
    // descriptor and handler, acknowledges REQSEND by clearing word 3 in
    // the registered descriptor, and has no active outbound transport.
    // No registered guest callback is invoked by these operations.
    uint32_t deci2_descriptor_address = 0u;
    uint32_t deci2_handler_pc = 0u;

    // Guest-visible 0x73 SetVSyncFlag pointer registration. Event delivery
    // is not implied; this runner has no real GS vertical-blank producer yet.
    ee::c_ee_vsync_flags vsync_flags{};

    // The ELF-derived red-clear loop is a *failure* path entered after
    // waiting for two command-buffer flags. Record one before/after state
    // pair without reading MMIO, changing RAM, or issuing any completion.
    ee::c_ee_sdbz_dma_wait_probe dma_wait_probe{};

    // Original USA game pad-read return continuation at 0x00109ba0.
    // Observe guest EE stack bytes only; do not alter device responses.
    ee::c_ee_sdbz_pad_return_probe pad_return_probe{};
    ee::c_ee_sdbz_spawn_probe spawn_probe{};
    spawn_probe.initialize(memory.get());

    struct s_kernel_thread
    {
        bool m_used = false;
        uint32_t m_status = 16u; // dormant; 1=run, 2=ready, 4=wait, 8=suspend, 12=wait+suspend
        bool m_self_suspend_pending = false;
        uint64_t m_order = 0u;
        bool m_has_context = false;
        uint32_t m_wait_semaphore = 0u;
        uint32_t m_wait_type = 0u; // 0=none, 1=SleepThread, 2=WaitSema
        uint32_t m_wakeups = 0u;
        ee::s_ee_cpu_state m_context{};
        ee::s_ee_vu0_state m_vu0_context{};
        uint32_t m_entry = 0u;
        uint32_t m_stack = 0u;
        uint32_t m_stack_size = 0u;
        uint32_t m_gp = 0u;
        uint32_t m_priority = 0u;
        uint32_t m_initial_priority = 0u;
        uint32_t m_attr = 0u;
        uint32_t m_option = 0u;
    };
    // The loaded executable already runs as the kernel's main thread.
    // Just like ps2-web-recomp::start_main_thread, reserve ID 1.
    // Only real guest syscalls transition READY/WAIT/DORMANT states.
    // The scheduler switches full EE contexts at dispatch boundaries.
    s_kernel_thread threads[256]{};
    threads[1].m_used = true;
    threads[1].m_status = 1u;
    threads[1].m_entry = entry;
    threads[1].m_stack = stack;
    uint32_t current_thread_id = 1u;
    uint64_t next_thread_order = 1u;
    constexpr uint32_t k_thread_return_pc = 0xffffffe0u;
    bool reschedule_requested = false;
    uint64_t idle_started_cycle = 0u;
    uint64_t idle_cycles = 0u;
    const auto make_thread_dormant = [](s_kernel_thread& thread)
    {
        thread.m_status = ee_kernel_thread::k_dormant;
        thread.m_self_suspend_pending = false;
        thread.m_has_context = false;
        thread.m_wait_type = 0u;
        thread.m_wait_semaphore = 0u;
        thread.m_wakeups = 0u;
        thread.m_priority = thread.m_initial_priority;
    };
    const auto schedule_threads = [&]() -> bool
    {
        ee::c_runtime_profile_scope profile_scope(ee::e_runtime_profile_id::thread_schedule);
        // A self-suspend reports SUSPEND until a real scheduling boundary,
        // but the thread still owns the running CPU. Reconcile that owner
        // before selecting the next runnable thread. Never release another
        // thread's suspension or override an actual WAIT/DORMANT transition.
        s_kernel_thread& running = threads[current_thread_id];
        if (running.m_self_suspend_pending)
        {
            running.m_self_suspend_pending = false;
            if (running.m_status == ee_kernel_thread::k_suspend)
            {
                running.m_status = ee_kernel_thread::k_run;
                if (trace)
                {
                    std::printf("HLE EE self-suspend reconciled: id=%u\n",
                                current_thread_id);
                }
            }
        }
        uint32_t next_id = 0u;
        for (uint32_t id = 1u; id < 256u; ++id)
        {
            const s_kernel_thread& candidate = threads[id];
            if (!candidate.m_used ||
                (candidate.m_status != 1u && candidate.m_status != 2u))
            {
                continue;
            }
            if (next_id == 0u ||
                candidate.m_priority < threads[next_id].m_priority ||
                (candidate.m_priority == threads[next_id].m_priority &&
                 candidate.m_order < threads[next_id].m_order))
            {
                next_id = id;
            }
        }
        if (next_id == 0u &&
            runtime.m_ee_cycle_scheduler.source() !=
                ee::e_ee_cycle_source::REFERENCE_COMPATIBILITY)
        {
            std::fprintf(stderr,
                "EE thread scheduler: no runnable guest threads and no idle clock\n");
            return false;
        }
        if (next_id != current_thread_id)
        {
            // Attribute outstanding elapsed budget to the outgoing
            // privilege mode before replacing its context.
            ee::ee_cop0_clock_sync(&cpu, &runtime);
            s_kernel_thread& current = threads[current_thread_id];
            if (current.m_used && current.m_status != ee_kernel_thread::k_dormant)
            {
                current.m_context = cpu;
                current.m_vu0_context = vu0;
                current.m_has_context = true;
                if (current.m_status == 1u)
                {
                    current.m_status = 2u;
                    current.m_order = ++next_thread_order;
                }
            }
            if (next_id == 0u)
            {
                current_thread_id = 0u;
                idle_started_cycle = runtime.m_ee_cycle_scheduler.current_cycle();
                reschedule_requested = false;
                if (trace)
                {
                    std::printf("HLE EE scheduler idle: no runnable guest threads\n");
                }
                return true;
            }
            s_kernel_thread& next = threads[next_id];
            if (!next.m_has_context)
            {
                std::fprintf(stderr,
                    "EE thread scheduler: thread %u has no CPU context\n",
                    next_id);
                return false;
            }
            cpu = next.m_context;
            ee::ee_cop0_clock_sync(&cpu, &runtime);
            vu0 = next.m_vu0_context;
            next.m_status = 1u;
            current_thread_id = next_id;
            hardware.intc().bind_cpu(&cpu);
            hardware.dmac().bind_cpu(&cpu);
            cpu.m_exit_reason = static_cast<uint32_t>(
                ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
            if (trace)
            {
                std::printf("HLE EE thread switch: id=%u pc=0x%08x\n",
                            next_id, cpu.m_pc);
            }
        }
        reschedule_requested = false;
        return true;
    };
    // EE kernel OSD configuration word. Defaults to the normal 4:3
    // console configuration until an explicit guest SetOsdConfigParam.
    uint32_t osd_config = 0u;
    uint32_t installed_syscall_handlers[k_kernel_syscall_table_entries]{};
    std::vector<s_kernel_interrupt_handler> intc_handlers;
    std::vector<s_kernel_interrupt_handler> dmac_handlers;
    uint32_t next_interrupt_handler_id = 1u;

    // A registered guest callback must return through this host-only
    // sentinel. Any other stop is propagated without claiming success.
    constexpr uint32_t k_callback_return_pc = 0xfffffff0u;
    bool callback_active = false;
    uint32_t callback_resume_pc = 0u;
    ee::s_ee_u128 callback_saved_ra{};

    // HLE EE kernel dispatches registered guest INTC/DMAC callbacks at
    // native safepoints. DMA5 pending comes from real IOP DMA transport;
    // INTC pending comes from masked hardware status, including scanout.
    // Neither path fabricates callbacks, completion or guest flags.
    bool irq_callback_active = false;
    bool irq_callback_is_intc = false;
    uint32_t irq_callback_cause = 0u;
    size_t irq_callback_index = 0u;
    std::vector<s_kernel_interrupt_handler> irq_callback_handlers;
    ee::s_ee_cpu_state irq_interrupted_cpu{};
    ee::s_ee_vu0_state irq_interrupted_vu0{};
    constexpr uint32_t k_irq_kernel_stack = 0x8007fff0u;
    constexpr uint32_t k_slot_callback_limit = 16u;
    uint32_t slot_callback_samples = 0u;
    bool slot_callback_armed = false;
    ee::c_ee_sdbz_slot_callback_probe::s_slot_observation slot_callback_before{};
    const auto enter_irq_callback = [&](const s_kernel_interrupt_handler& handler)
    {
        // Observe only the identified VIF1 handler while the retail
        // wait is suspended, with a fixed log bound per replay.
        slot_callback_armed = report_path != nullptr &&
            slot_callback_samples < k_slot_callback_limit &&
            ee::c_ee_sdbz_slot_callback_probe::is_slot_callback(
                irq_callback_is_intc, irq_callback_cause,
                handler.m_address, irq_interrupted_cpu.m_pc) &&
            ee::c_ee_sdbz_slot_callback_probe::sample_slots(
                memory.get(), slot_callback_before);
        ee::ee_cop0_clock_sync(&cpu, &runtime);
        cpu.m_cop0[12] = (irq_interrupted_cpu.m_cop0[12] & ~0x10001u) | 2u;
        cpu.m_gpr[4] = {irq_callback_cause, 0u};
        cpu.m_gpr[5] = {handler.m_arg, 0u};
        cpu.m_gpr[6] = {0u, 0u};
        cpu.m_gpr[7] = {0u, 0u};
        if (handler.m_gp != 0u)
        {
            cpu.m_gpr[28] = {handler.m_gp, 0u};
        }
        cpu.m_gpr[29] = {
            static_cast<uint64_t>(static_cast<int64_t>(
                static_cast<int32_t>(k_irq_kernel_stack))), 0u
        };
        cpu.m_gpr[31] = {static_cast<uint64_t>(-16ll), 0u};
        cpu.m_pc = handler.m_address;
        cpu.m_exit_reason = static_cast<uint32_t>(
            ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
        if (trace)
        {
            if (irq_callback_is_intc)
            {
                std::printf("HLE INTC callback enter: cause=%u "
                            "handler=0x%08x arg=0x%08x gp=0x%08x\n",
                            irq_callback_cause, handler.m_address,
                            handler.m_arg, handler.m_gp);
            }
            else
            {
                // Keep the established ROM0/DMA5 diagnostic contract.
                std::printf("HLE DMAC callback enter: channel=%u "
                            "handler=0x%08x arg=0x%08x gp=0x%08x\n",
                            irq_callback_cause, handler.m_address,
                            handler.m_arg, handler.m_gp);
            }
        }
    };
    std::vector<s_syscall_sample> syscall_history;
    uint32_t callback_sample_index = 0;
    if (report_path != nullptr)
    {
        syscall_history.reserve(32u);
    }

    uint32_t remaining = budget;
    uint64_t dispatched = 0;
    uint64_t event_yield_count = 0u;
    uint32_t hot_expand_sequence = 0u;
    // Live cache-miss overlays hold only newly translated functions. They are
    // consulted after the base module reports an unowned PC, so a miss links
    // a few objects instead of relinking the whole translated image.
    std::vector<std::unique_ptr<c_native_module>> overlay_modules;
    std::vector<t_game_run> overlay_runs;
    std::vector<t_game_resolve_entry> overlay_resolvers;
    std::vector<t_game_resolve_owner> overlay_owner_resolvers;
    std::vector<t_game_alu_words> overlay_alu_certificates;
    std::unordered_map<uint32_t, size_t> overlay_owner_by_pc;
    // Diagnose an unready IOP SIF handshake. Real ROM0 execution advances
    // on each read; the guard never manufactures a readiness value.
    uint32_t last_sif_smflag_epc = 0u;
    uint64_t last_sif_smflag_dispatch = 0u;
    uint32_t repeated_sif_smflag_reads = 0u;
    // A zero-count EE semaphore can be completed by a genuine IOP->EE
    // DMA5 callback, even when there is no separate runnable EE thread.
    // Count bounded IOP turns at the trapped WaitSema site; this is a
    // diagnostic/scheduling guard, never a synthetic signal.
    uint32_t iop_semaphore_wait_turns = 0u;
    uint64_t iop_semaphore_wait_last_sif0 = 0u;
    // All IOP work runs against elapsed EE guest time, not only
    // when a guest RPC call is outstanding. Retain fractional clocks
    // and a backlog after bounded native-dispatch safepoints.
    ee::c_ee_iop_clock iop_clock;
    uint64_t iop_clock_turns = 0u;
    ee::c_iop_service_attribution iop_attribution;
    std::string iop_attribution_environment;
    const char* iop_attribution_value = read_host_environment(
        "SDBZ_PROFILE_IOP_SERVICE", &iop_attribution_environment);
    // Opt-in: coalesce only provably idle IOP turns within the existing
    // EE checkpoint. Do not defer IOP time across native EE execution.
    std::string iop_coalesce_environment;
    const char* iop_coalesce_value = read_host_environment(
        "SDBZ_IOP_COALESCE_IDLE_TURNS", &iop_coalesce_environment);
    const bool iop_coalesce = iop_coalesce_value != nullptr &&
        std::strcmp(iop_coalesce_value, "1") == 0;
    std::string iop_fast_quiet_environment;
    const char* iop_fast_quiet_value = read_host_environment(
        "SDBZ_IOP_FAST_QUIET_TURNS", &iop_fast_quiet_environment);
    const bool iop_fast_quiet = iop_fast_quiet_value != nullptr &&
        std::strcmp(iop_fast_quiet_value, "1") == 0;
    // Exact checkpoint trace for bounded differential fixtures, not a
    // performance profile. Disabled by default; no host time is sampled.
    std::string iop_checkpoint_environment;
    const char* iop_checkpoint_value = read_host_environment(
        "SDBZ_IOP_CHECKPOINT_TRACE", &iop_checkpoint_environment);
    const bool iop_checkpoint_trace = iop_checkpoint_value != nullptr &&
        std::strcmp(iop_checkpoint_value, "1") == 0;
    uint64_t iop_checkpoint_executed = 0u;
    uint64_t iop_quiet_attempts = 0u;
    uint64_t iop_quiet_advances = 0u;
    uint64_t iop_quiet_cycles = 0u;
    uint64_t iop_clock_runs = 0u;
    uint64_t iop_coalesced_batches = 0u;
    bool guest_rpc_idle_diagnostic_reported = false;
    uint64_t next_iop_guest_rpc_snapshot = 50000u;
    std::string hotspots_environment;
    const char* hotspots_value = read_host_environment(
        "SDBZ_REPORT_HOTSPOTS", &hotspots_environment);
    const bool report_hotspots = hotspots_value == nullptr ||
        std::strcmp(hotspots_value, "0") != 0;
    ee::c_ee_dispatch_hotspots hotspots(
        ee::c_ee_dispatch_hotspots::k_pc_capacity,
        ee::c_ee_dispatch_hotspots::k_edge_capacity, report_hotspots);
    std::string boundary_timing_environment;
    const char* boundary_timing_value = read_host_environment(
        "SDBZ_REPORT_BOUNDARY_TIMING", &boundary_timing_environment);
    const bool report_boundary_timing = boundary_timing_value == nullptr ||
        std::strcmp(boundary_timing_value, "0") != 0;
    if (report_path != nullptr)
    {
        std::printf("EE report options: hotspots=%u boundary_timing=%u "
            "(history and guest checkpoint cadence retained)\n",
            report_hotspots ? 1u : 0u, report_boundary_timing ? 1u : 0u);
    }
    ee::c_bounded_diagnostic_history<s_dispatch_sample, k_report_history_limit> history;
    uint32_t status = 0;
    uint32_t reason = cpu.m_exit_reason;
    uint32_t completed_loop_yields = 0u;
    // A total-session cap was wrong: genuine VBlank, GIF and IOP RPC
    // progress must reopen a finite set of native watchdog resumes.
    ee::c_host_loop_yield_budget loop_yield_budget{};
    uint64_t last_snapshot_dispatch = 0;
    uint64_t last_progress_dispatch = 0;
    bool last_snapshot_callback_active = false;
    // Optional host-only probe pacing. Preserve the normal per-128-dispatch
    // checkpoints unless explicitly overridden. This only changes disk I/O:
    // the guest still returns to the host after EVERY native dispatch while
    // --report is enabled, so DMA/interrupt/callback scheduling is unchanged.
    uint32_t report_snapshot_milliseconds = 0u;
    std::string snapshot_environment;
    const char* snapshot_interval = read_host_environment(
        "SDBZ_REPORT_SNAPSHOT_MS", &snapshot_environment);
    if (snapshot_interval != nullptr && *snapshot_interval != '\0')
    {
        char* end = nullptr;
        errno = 0;
        const unsigned long parsed = std::strtoul(snapshot_interval, &end, 10);
        if (errno != 0 || end == snapshot_interval || *end != '\0' ||
            parsed > 60000ul)
        {
            std::fprintf(stderr,
                         "invalid SDBZ_REPORT_SNAPSHOT_MS (expected 0..60000): %s\n",
                         snapshot_interval);
            return 1;
        }
        report_snapshot_milliseconds = static_cast<uint32_t>(parsed);
    }
    if (report_path != nullptr && report_snapshot_milliseconds != 0u)
    {
        std::fprintf(stderr,
                     "EE report snapshots: every %u ms (native dispatch slice unchanged)\n",
                     report_snapshot_milliseconds);
    }
    std::chrono::steady_clock::time_point last_snapshot_time{};
    const auto wall_start = std::chrono::steady_clock::now();
    std::string runtime_profile_environment;
    const char* runtime_profile_value = read_host_environment(
        "SDBZ_RUNTIME_PROFILE", &runtime_profile_environment);
    ee::runtime_profile_begin(runtime_profile_value != nullptr &&
        std::strcmp(runtime_profile_value, "1") == 0);
    const bool attribute_iop = ee::g_runtime_profile.m_enabled &&
        iop_attribution_value != nullptr &&
        std::strcmp(iop_attribution_value, "1") == 0;
    std::string iop_kernel_profile_environment;
    const char* iop_kernel_profile_value = read_host_environment(
        "SDBZ_PROFILE_IOP_KERNEL", &iop_kernel_profile_environment);
    const bool attribute_iop_kernel = attribute_iop &&
        iop_kernel_profile_value != nullptr &&
        std::strcmp(iop_kernel_profile_value, "1") == 0;
    iop::s_kernel_phase_sample iop_phase_sample{};
    iop::s_kernel_phase_sample iop_phase_totals[2]{};
    uint64_t iop_phase_samples[2]{};
    if (ee::g_runtime_profile.m_enabled)
    {
        runtime.m_vu0_sync = runner_vu0_profiled_sync;
    }
    if (report_path != nullptr && !report_boundary_timing &&
        !ee::g_runtime_profile.m_enabled)
    {
        std::puts("EE report boundary timing disabled: legacy native_ms/iop_ms "
            "and slowest_native fields are unavailable (zero unless PC profiling requested)");
    }

    std::string gs_async_environment;
    const char* gs_async_setting = read_host_environment(
        "SDBZ_GS_ASYNC", &gs_async_environment);
    if (gs_async_setting != nullptr && *gs_async_setting != '\0' &&
        std::strcmp(gs_async_setting, "0") != 0 &&
        std::strcmp(gs_async_setting, "1") != 0)
    {
        std::fprintf(stderr, "invalid SDBZ_GS_ASYNC (expected 0 or 1)\n");
        return 2;
    }
    const bool async_gs = gs_async_setting != nullptr &&
        std::strcmp(gs_async_setting, "1") == 0;
    if (async_gs)
    {
#ifdef EE_ENABLE_WEBGPU
        if (!webgpu_bridge.enable_async())
        {
            std::fprintf(stderr, "GS async requires an initialized WebGPU bridge\n");
            return 2;
        }
        std::puts("GS async: enabled (bounded ordered worker, fenced presentation/readback)");
#else
        std::fprintf(stderr, "GS async requires -DEE_ENABLE_WEBGPU=ON\n");
        return 2;
#endif
    }
    std::unique_ptr<ee::c_ee_vu1_aot> vu1_aot;
    std::string vu1_aot_environment;
    std::string vu1_capture_environment;
    std::string vu1_capture_misses_environment;
    const char* vu1_path = read_host_environment(
        "SDBZ_VU1_AOT", &vu1_aot_environment);
    const char* vu1_capture = read_host_environment(
        "SDBZ_VU1_AOT_CAPTURE", &vu1_capture_environment);
    const char* vu1_capture_misses = read_host_environment(
        "SDBZ_VU1_AOT_CAPTURE_MISSES", &vu1_capture_misses_environment);
    const bool misses_only = (vu1_capture == nullptr || *vu1_capture == '\0') &&
        vu1_capture_misses != nullptr && *vu1_capture_misses != '\0';
    if (misses_only)
    {
        vu1_capture = vu1_capture_misses;
    }
    if ((vu1_path != nullptr && *vu1_path != '\0') ||
        (vu1_capture != nullptr && *vu1_capture != '\0'))
    {
        vu1_aot = std::make_unique<ee::c_ee_vu1_aot>();
        std::string error;
        if (vu1_path != nullptr && *vu1_path != '\0')
        {
            const auto count = vu1_aot->open_directory(vu1_path, &error);
            std::fprintf(stderr, "VU1 AOT: %zu native programs loaded\n", count);
            if (!error.empty())
            {
                std::fprintf(stderr, "VU1 AOT: %s\n", error.c_str());
            }
        }
        if (vu1_capture != nullptr && *vu1_capture != '\0')
        {
            error.clear();
            if (!vu1_aot->enable_capture(vu1_capture, &error, misses_only))
            {
                std::fprintf(stderr, "VU1 capture disabled: %s\n", error.c_str());
            }
        }
    }
    std::string vu1_async_environment;
    const char* vu1_async_value = read_host_environment(
        "SDBZ_VU1_ASYNC", &vu1_async_environment);
    if (vu1_async_value != nullptr && *vu1_async_value != '\0' &&
        std::strcmp(vu1_async_value, "0") != 0 &&
        std::strcmp(vu1_async_value, "1") != 0)
    {
        std::fprintf(stderr, "invalid SDBZ_VU1_ASYNC (expected 0 or 1)\n");
        return 2;
    }
    const bool async_vu1 = vu1_async_value != nullptr &&
        std::strcmp(vu1_async_value, "1") == 0;
    std::string vu1_overlap_environment;
    const char* vu1_overlap_value = read_host_environment(
        "SDBZ_VU1_NATIVE_OVERLAP_PROFILE", &vu1_overlap_environment);
    if (vu1_overlap_value != nullptr && *vu1_overlap_value != '\0' &&
        std::strcmp(vu1_overlap_value, "0") != 0 &&
        std::strcmp(vu1_overlap_value, "1") != 0)
    {
        std::fprintf(stderr, "invalid SDBZ_VU1_NATIVE_OVERLAP_PROFILE (expected 0 or 1)\n");
        return 2;
    }
    const bool vu1_native_overlap_profile = vu1_overlap_value == nullptr ||
        std::strcmp(vu1_overlap_value, "0") != 0;
    // Opt-in worker-only attribution, independent of the EE main-thread
    // profiler. Preserve the historical default when no override is set.
    std::string vu1_worker_profile_environment;
    const char* vu1_worker_profile_value = read_host_environment(
        "SDBZ_VU1_WORKER_PROFILE", &vu1_worker_profile_environment);
    if (vu1_worker_profile_value != nullptr && *vu1_worker_profile_value != '\0' &&
        std::strcmp(vu1_worker_profile_value, "0") != 0 &&
        std::strcmp(vu1_worker_profile_value, "1") != 0)
    {
        std::fprintf(stderr, "invalid SDBZ_VU1_WORKER_PROFILE (expected 0 or 1)\n");
        return 2;
    }
    const bool vu1_worker_profile = vu1_worker_profile_value == nullptr ||
        *vu1_worker_profile_value == '\0' ? ee::g_runtime_profile.m_enabled :
        std::strcmp(vu1_worker_profile_value, "1") == 0;
    std::string vu1_profile_window_environment;
    const char* vu1_profile_window = read_host_environment(
        "SDBZ_VU1_WORKER_PROFILE_WINDOW", &vu1_profile_window_environment);
    unsigned long long vu1_profile_begin = 0u, vu1_profile_end = 0u;
    unsigned vu1_profile_snapshots = 0u;
    if (vu1_profile_window != nullptr && *vu1_profile_window != '\0')
    {
        char trailing = 0;
        if (!async_vu1 || !vu1_worker_profile ||
            std::sscanf(vu1_profile_window, "%llu,%llu%c", &vu1_profile_begin,
                &vu1_profile_end, &trailing) != 2 ||
            vu1_profile_begin == 0u || vu1_profile_end <= vu1_profile_begin ||
            vu1_profile_end > 0xffffffffull)
        {
            std::fprintf(stderr, "invalid SDBZ_VU1_WORKER_PROFILE_WINDOW (requires async VU1, profiling, and begin,end dispatches)\n");
            return 2;
        }
    }
    if (async_vu1)
    {
        std::fprintf(stderr, "[vu1] worker_profile=%u\n", vu1_worker_profile ? 1u : 0u);
        if (!vif1.enable_deferred(std::move(vu1_aot),
                vu1_worker_profile, vu1_native_overlap_profile))
        {
            std::fprintf(stderr, "failed to enable deferred VU1 worker\n");
            return 1;
        }
        hardware.dmac().bind_deferred_vif1(&vif1);
        std::puts("VU1 async: enabled (bounded worker; ordered EE-thread publication)");
    }
    ee::c_vu1_aot_binding vu1_binding(vu1_aot.get());
    double report_time_ms = 0.0;
    uint64_t native_time_ns = 0u;
    // Host-cost breakdown for reports: IOP emulation between EE dispatches,
    // and time paused waiting for live cache-miss compilation.
    uint64_t iop_time_ns = 0u;
    double compile_wait_ms = 0.0;
    // SDBZ_PROFILE_PCS=1: reuse the native-boundary clock; rank by
    // inclusive native time, excluding live compilation and outer-loop work.
    // Accounted guest attempts include charged idle-loop instructions.
    std::string profile_pcs_environment;
    const char* profile_pcs_value = read_host_environment(
        "SDBZ_PROFILE_PCS", &profile_pcs_environment);
    bool profile_pcs = profile_pcs_value != nullptr &&
        std::strcmp(profile_pcs_value, "0") != 0;
    uint64_t profile_pcs_first = 1u;
    uint64_t profile_pcs_last = UINT32_MAX;
    const auto read_profile_bound = [&](const char* name, uint64_t& bound)
    {
        std::string environment;
        const char* value = read_host_environment(name, &environment);
        if (value == nullptr || *value == '\0')
        {
            return true;
        }
        uint64_t parsed = 0u;
        for (const char* digit = value; *digit != '\0'; ++digit)
        {
            if (*digit < '0' || *digit > '9' || parsed > UINT32_MAX / 10u)
            {
                return false;
            }
            parsed = parsed * 10u + static_cast<uint64_t>(*digit - '0');
        }
        if (parsed == 0u || parsed > UINT32_MAX)
        {
            return false;
        }
        bound = parsed;
        return true;
    };
    if (!read_profile_bound("SDBZ_PROFILE_PCS_FIRST", profile_pcs_first) ||
        !read_profile_bound("SDBZ_PROFILE_PCS_LAST", profile_pcs_last) ||
        profile_pcs_first > profile_pcs_last)
    {
        std::fprintf(stderr, "invalid PC profile dispatch window (expected 1..UINT32_MAX)\n");
        return 2;
    }
    if (profile_pcs)
    {
        std::printf("EE PC profile window: first=%llu last=%llu (native entry ordinal)\n",
            static_cast<unsigned long long>(profile_pcs_first),
            static_cast<unsigned long long>(profile_pcs_last));
    }
    ee::c_ee_pc_profile<> pc_profile(profile_pcs);
    // Observe the existing schedule without changing guest execution.
    std::string horizon_environment;
    const char* horizon_value = read_host_environment(
        "SDBZ_PROFILE_HORIZON", &horizon_environment);
    const bool profile_horizon = horizon_value != nullptr &&
        std::strcmp(horizon_value, "1") == 0;
    ee::c_native_alu_identity_cache alu_identities(profile_horizon && direct_dispatch);
    if (profile_horizon)
    {
        // Only the opt-in shadow path wraps the native runtime callbacks.
        // Count attempts, including failed or no-op synchronization.
        runtime.m_vu0_sync = [](void* user, ee::s_ee_cpu_state* cpu,
            ee::s_ee_vu0_state* vu0, uint32_t mode, uint32_t pc)
        {
            ++static_cast<s_runner_vu0_control*>(user)->m_callback_generation;
            return ee::g_runtime_profile.m_enabled ?
                runner_vu0_profiled_sync(user, cpu, vu0, mode, pc) :
                runner_vu0_sync(user, cpu, vu0, mode, pc);
        };
        runtime.m_vu0_start = [](void* user, ee::s_ee_cpu_state* cpu,
            ee::s_ee_vu0_state* vu0, uint32_t address, uint32_t pc)
        {
            ++static_cast<s_runner_vu0_control*>(user)->m_callback_generation;
            return runner_vu0_start(user, cpu, vu0, address, pc);
        };
        runtime.m_vu0_control = [](void* user, ee::s_ee_cpu_state* cpu,
            ee::s_ee_vu0_state* vu0, uint32_t reg, uint32_t value, uint32_t pc)
        {
            ++static_cast<s_runner_vu0_control*>(user)->m_callback_generation;
            return runner_vu0_control(user, cpu, vu0, reg, value, pc);
        };
    }
    constexpr size_t horizon_sources = std::to_underlying(
        ee::e_ee_horizon_source::count);
    uint64_t horizon_samples[horizon_sources]{};
    uint64_t horizon_before[horizon_sources]{};
    uint64_t horizon_reached[horizon_sources]{};
    uint64_t horizon_unknown = 0u;
    uint64_t horizon_normal_before = 0u;
    uint64_t horizon_without_iop_before = 0u;
    uint64_t horizon_iop_lookahead_before = 0u;
    uint64_t horizon_iop_idle_candidates = 0u;
    uint64_t horizon_iop_rpc_barriers = 0u;
    uint64_t horizon_gs_worker_barriers = 0u;
    uint64_t horizon_dmac_started_barriers = 0u;
    constexpr uint32_t horizon_dmac_mask_count = 1u << ee::c_ee_dmac::k_channel_count;
    uint64_t horizon_dmac_masks[horizon_dmac_mask_count]{};
    uint64_t horizon_dmac_sif0_only = 0u;
    uint64_t horizon_dmac_passive_sif0 = 0u;
    uint64_t horizon_entry_masks[32]{};
    uint64_t horizon_vu_masks[8]{};
    std::unordered_map<uint32_t, uint32_t> horizon_alu_bounds;
    uint64_t horizon_alu_candidates = 0u;
    uint64_t horizon_alu_matched = 0u;
    uint64_t horizon_alu_mismatch = 0u;
    uint64_t horizon_alu_owner_known = 0u;
    uint64_t horizon_alu_owner_matched = 0u;
    uint64_t horizon_alu_certified = 0u;
    uint64_t horizon_alu_certified_matched = 0u;
    uint64_t horizon_clock_barriers = 0u;
    uint64_t horizon_lookahead_mmio_free = 0u;
    uint64_t horizon_lookahead_mmio_touched = 0u;
    uint64_t horizon_vu_callback_boundaries = 0u;
    uint64_t horizon_cop0_changed_boundaries = 0u;
    uint64_t horizon_mmio_free_direct_touched = 0u;
    uint64_t horizon_mmio_free_direct_quiet = 0u;
    // SDBZ_IDLE_SKIP=1: exact host-side acceleration of translated guest
    // poll loops at host or scheduler yield boundaries (see
    // ee_idle_loop_skip.h).
    std::string idle_skip_environment;
    const char* idle_skip_value = read_host_environment(
        "SDBZ_IDLE_SKIP", &idle_skip_environment);
    const bool idle_skip = idle_skip_value != nullptr &&
        std::strcmp(idle_skip_value, "0") != 0;
    // Experimental forward CFG support is independent of the validated
    // straight-line poll-loop skip path.
    std::string idle_forward_environment;
    const char* idle_forward_value = read_host_environment(
        "SDBZ_IDLE_SKIP_FORWARD_BRANCHES", &idle_forward_environment);
    const bool idle_forward_branches = idle_forward_value != nullptr &&
        std::strcmp(idle_forward_value, "1") == 0;
    std::unordered_map<uint32_t, ee::s_idle_loop_shape> idle_loop_shapes;
    // Guarded loop headers observed at scheduler or host yields. Any later
    // native entry at one of them may probe; the one-iteration probe validates
    // itself before the runner skips repeated guest iterations.
    std::unordered_set<uint32_t> idle_analyzed_headers;
    std::vector<uint32_t> idle_probe_headers;  // at most four armed headers
    uint32_t idle_probe_slots[4]{};
    const auto refresh_idle_probe_slots = [&]()
    {
        std::fill(std::begin(idle_probe_slots), std::end(idle_probe_slots), 0u);
        std::copy(idle_probe_headers.begin(), idle_probe_headers.end(),
                  std::begin(idle_probe_slots));
    };
    const auto analyze_idle_loop_header = [&](uint32_t header)
    {
        if (!idle_skip || !idle_analyzed_headers.insert(header).second)
        {
            return;
        }
        const auto shape = idle_loop_shapes.emplace(header, ee::analyze_idle_loop(
            header,
            [&](uint32_t pc, uint32_t* word)
            {
                if (!image.is_executable_address(pc) ||
                    !memory->contains(pc, 4u))
                {
                    return false;
                }
                *word = memory->read32(pc);
                return true;
            }, idle_forward_branches)).first;
        if (shape->second.eligible && idle_probe_headers.size() < 4u)
        {
            idle_probe_headers.push_back(header);
            refresh_idle_probe_slots();
        }
    };
    uint64_t idle_skip_slices = 0u;
    uint64_t idle_skip_instructions = 0u;
    uint64_t idle_skip_mispredictions = 0u;
    uint64_t slowest_native_ns = 0u;
    uint32_t slowest_native_pc = 0u;
#ifdef EE_ENABLE_RAYLIB_WINDOW
    uint32_t window_pump_stride = 1u;
    std::string window_pump_environment;
    const char* window_pump_value = read_host_environment(
        "SDBZ_WINDOW_PUMP_STRIDE", &window_pump_environment);
    if (window_pump_value != nullptr &&
        (!parse_u32(window_pump_value, &window_pump_stride) ||
         window_pump_stride == 0u || window_pump_stride > 64u))
    {
        std::fprintf(stderr, "invalid SDBZ_WINDOW_PUMP_STRIDE (expected 1..64)\n");
        return 2;
    }
    ee::c_ee_window_pump_gate window_pump_gate(window_pump_stride);
    if (gs_webgpu_window)
    {
        std::printf("GS host window pump stride: %u (all observed field advances serviced)\n",
            window_pump_stride);
    }
    ee::c_ee_frame_pacer frame_pacer(gs_webgpu_window && speed_percent != 0u);
    uint64_t paced_field = 0u;
    uint64_t pace_cycle = 0u;
    uint64_t sample_cycle = 0u;
    auto pace_time = std::chrono::steady_clock::now();
    auto sample_time = pace_time;
#endif
    // Read diagnostic configuration once, outside the device service hot
    // path. The CPU GS display readback also supports headless builds.
    const std::pair<std::string, uint64_t> s_frame_dump = []
    {
        std::string storage;
        const char* text = read_host_environment("SDBZ_FRAME_DUMP", &storage);
        if (text == nullptr)
        {
            return std::pair<std::string, uint64_t>{};
        }
        const std::string spec(text);
        const size_t colon = spec.rfind(':');
        if (colon == std::string::npos || colon == 0u)
        {
            return std::pair<std::string, uint64_t>{};
        }
        return std::pair<std::string, uint64_t>{
            spec.substr(0u, colon),
            std::strtoull(spec.c_str() + colon + 1u, nullptr, 0)};
    }();
    uint64_t s_frame_dump_next = 0u;
    enum class e_device_service { continued, window_closed, failed };
    const auto service_devices = [&]() -> e_device_service
    {
        if (runtime.m_ee_cycle_scheduler.source() ==
            ee::e_ee_cycle_source::REFERENCE_COMPATIBILITY)
        {
            uint64_t cycle = 0u;
            if (!ee::ee_reference_compatibility_cycle(runtime, cycle) ||
                !runtime.m_ee_cycle_scheduler.publish_cycle(cycle))
            {
                std::fprintf(stderr, "EE compatibility cycle publication rejected\n");
                return e_device_service::failed;
            }
        }
        ee::ee_cop0_clock_sync(&cpu, &runtime);
        if (runtime.m_perf_compat_fault)
        {
            std::fprintf(stderr, "EE COP0 compatibility performance event/overflow unsupported\n");
            return e_device_service::failed;
        }
        ee::c_runtime_profile_scope profile_scope(ee::e_runtime_profile_id::device_service);
        if (vif1.cmsar1_running() &&
            reason != static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_UNSUPPORTED) &&
            !runner_service_cmsar1(vu0_control, vu0))
        {
            std::fprintf(stderr, "VU1 CMSAR1 slice rejected\n");
            return e_device_service::failed;
        }
        if (vu0.m_running != 0u &&
            reason != static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_UNSUPPORTED) &&
            runtime.m_guest_instruction_attempts >= vu0_control.m_next_attempt)
        {
            const auto synced = runner_vu0_sync(&vu0_control, &cpu, &vu0,
                static_cast<uint32_t>(ee::e_ee_vu0_sync_mode::CATCH_UP_TO_EE_CYCLE),
                cpu.m_pc);
            if (synced != static_cast<uint32_t>(ee::e_ee_vu0_sync_result::COMPLETE))
            {
                std::fprintf(stderr, "VU0 micro execution stopped at byte PC=%04x result=%u\n",
                    vu0.m_micro_pc, synced);
                return e_device_service::failed;
            }
        }
        if (hardware.dmac().deferred_vif1_pending())
        {
            // Present only committed GS state at the next field boundary.
            // Otherwise poll without blocking the independent EE/IOP path.
            const bool field_due = hardware.scanout().enabled() &&
                hardware.scanout().next_field_cycle() <=
                    runtime.m_ee_cycle_scheduler.current_cycle();
            (void)hardware.dmac().service_deferred_vif1(field_due);
        }
        if (async_vu1 && !graphics_dma.m_rejection_logged &&
            vif1.diagnostic().m_rejected_batches != 0u)
        {
            const auto& detail = vif1.diagnostic();
            std::fprintf(stderr,
                "VIF1 deferred reject: reason=%u code=0x%08x vu_pc=%04x steps=%u\n",
                static_cast<uint32_t>(detail.m_reason), detail.m_code,
                detail.m_vu_pc, detail.m_last_rejected_vu_steps);
            graphics_dma.m_rejection_logged = true;
        }
#ifdef EE_ENABLE_WEBGPU
        if (async_gs && !webgpu_bridge.worker_healthy())
        {
            std::fprintf(stderr, "GS async worker failed after committed GIF batch\n");
            return e_device_service::failed;
        }
#endif
        // Service guest-programmed timer compare/overflow events against
        // the same reached-instruction compatibility clock as scanout.
        // Timer COUNT reads inside a long native loop sample the live
        // instruction budget rather than this after-dispatch checkpoint.
        host_timers.service();
        const auto scanout_result = runtime.m_ee_cycle_scheduler.service_scanout(
            hardware, vsync_flags, *memory);
        if (scanout_result == ee::e_ee_scheduled_event_result::INVALID_TIMESTAMP_OR_DELIVERY)
        {
            std::fprintf(stderr, "EE scheduler rejected scanout delivery at cycle=%llu\n",
                static_cast<unsigned long long>(runtime.m_ee_cycle_scheduler.current_cycle()));
            return e_device_service::failed;
        }
        // Deliver original IOP start/end callbacks in the scanout
        // clock's event order; never infer edges from wall time.
        if (iop_hle != nullptr)
        {
            for (uint8_t start_edge :
                 hardware.scanout().delivered_vblank_edges())
            {
                iop_hle->signal_vblank_edge(start_edge != 0u);
            }
        }
        // SDBZ_FRAME_DUMP=DIR:EVERY_FIELDS writes a read-only GS display
        // capture every EVERY_FIELDS guest scanout fields. Prefer the
        // WebGPU raster when available; headless builds use CPU GS VRAM.
        {
            if (s_frame_dump.second != 0u)
            {
                const uint64_t field = hardware.scanout().fields_started();
                if (field != 0u && field >= s_frame_dump_next)
                {
                    s_frame_dump_next = field + s_frame_dump.second;
                    std::vector<uint32_t> pixels;
                    uint32_t width = 0u;
                    uint32_t height = 0u;
                    bool readback = false;
                    const char* source = "CPU GS VRAM";
#ifdef EE_ENABLE_WEBGPU
                    if (webgpu_bridge.active())
                    {
                        readback = webgpu_bridge.readback(
                            hardware.gs_display_registers(),
                            hardware.gs_crt_mode(), pixels, width, height);
                        source = "WebGPU raster";
                    }
#endif
                    if (!readback)
                    {
                        readback = ee::c_ee_gs_display::readback(
                            gs_state, hardware.gs_display_registers(),
                            hardware.gs_crt_mode(), pixels, width, height);
                        source = "CPU GS VRAM";
                    }
                    if (readback)
                    {
                        char name[64];
                        std::snprintf(name, sizeof(name), "/field-%08llu.ppm",
                                      static_cast<unsigned long long>(field));
                        if (!write_gs_ppm(
                                (s_frame_dump.first + name).c_str(),
                                pixels, width, height))
                        {
                            std::fprintf(stderr,
                                "EE frame dump write failed: field=%llu path=%s%s\n",
                                static_cast<unsigned long long>(field),
                                s_frame_dump.first.c_str(), name);
                        }
                        else
                        {
                            std::fprintf(stderr,
                                "EE frame dump: field=%llu size=%ux%u source=%s\n",
                                static_cast<unsigned long long>(field),
                                width, height, source);
                        }
                    }
                    else
                    {
                        std::fprintf(stderr,
                            "EE frame dump unavailable: field=%llu no supported display\n",
                            static_cast<unsigned long long>(field));
                    }
                }
            }
        }
#ifdef EE_ENABLE_RAYLIB_WINDOW
        if (gs_webgpu_window)
        {
            const uint64_t field = hardware.scanout().fields_started();
            if (field != paced_field)
            {
                using t_clock = std::chrono::steady_clock;
                const uint64_t cycle = runtime.m_ee_cycle_scheduler.current_cycle();
                constexpr double clock_hz = ee::c_ee_scanout_clock::k_ee_clock_hz;
                auto now = t_clock::now();
                if (paced_field == 0u || cycle < pace_cycle)
                {
                    pace_cycle = sample_cycle = cycle;
                    pace_time = sample_time = now;
                }
                else if (speed_percent != 0u)
                {
                    const auto target = pace_time + std::chrono::duration_cast<t_clock::duration>(
                        std::chrono::duration<double>((cycle - pace_cycle) * 100.0 /
                            (clock_hz * speed_percent)));
                    if (target > now && target - now <= std::chrono::milliseconds(500))
                    {
                        {
                            ee::c_runtime_profile_scope profile_scope(ee::e_runtime_profile_id::frame_pacing_sleep);
                            frame_pacer.wait_until(target);
                        }
                        now = t_clock::now();
                    }
                    // Re-anchor after a stall or a large guest clock jump.
                    if (now - target > std::chrono::milliseconds(100) ||
                        target - now > std::chrono::milliseconds(500))
                    {
                        pace_cycle = cycle;
                        pace_time = now;
                    }
                }
                const double elapsed = std::chrono::duration<double>(now - sample_time).count();
                if (elapsed >= 1.0)
                {
                    const double actual = (cycle - sample_cycle) * 100.0 / (clock_hz * elapsed);
                    if (speed_percent == 0u)
                    {
                        std::printf("EE speed: %.0f%% actual / Unlimited\n", actual);
                    }
                    else
                    {
                        std::printf("EE speed: %.0f%% actual / %u%% target\n", actual, speed_percent);
                    }
                    std::fflush(stdout);
                    sample_cycle = cycle;
                    sample_time = now;
                }
                paced_field = field;
            }
            // The window already throttles host input polls to 120 Hz.
            // Avoid checking its host clock on every EE boundary, while
            // preserving immediate service on every observed field advance.
            if (window_pump_gate.should_pump(field))
            {
                ee::c_runtime_profile_scope profile_scope(ee::e_runtime_profile_id::window_pump);
                ee::s_ee_performance_counters counters{};
                counters.m_cycles = runtime.m_ee_cycle_scheduler.current_cycle();
                counters.m_dispatches = dispatched;
                counters.m_instructions = runtime.m_guest_instruction_attempts;
                counters.m_draws = webgpu_bridge.rendered_primitives();
                counters.m_target_speed = speed_percent;
                counters.m_clock_available = runtime.m_ee_cycle_scheduler.source() !=
                    ee::e_ee_cycle_source::UNAVAILABLE;
                counters.m_profile_enabled = ee::g_runtime_profile.m_enabled;
                if (counters.m_profile_enabled)
                {
                    counters.m_ee_host_ns = ee::g_runtime_profile.m_counters[
                        static_cast<size_t>(ee::e_runtime_profile_id::ee_native_boundary)].m_exclusive_ns;
                    counters.m_iop_host_ns = ee::g_runtime_profile.m_counters[
                        static_cast<size_t>(ee::e_runtime_profile_id::iop_service)].m_exclusive_ns;
                }
                raylib_window.pump(
                    webgpu_bridge,
                    hardware.gs_display_registers(),
                    hardware.gs_crt_mode(),
                    hardware.scanout().fields_started(), &graphics_probe,
                    iop_kernel != nullptr ? &iop_kernel->hardware() : nullptr,
                    counters);
                if (raylib_window.consume_pc_profile_toggle())
                {
                    profile_pcs = !profile_pcs;
                    pc_profile.set_enabled(profile_pcs);
                }
            }
            if (raylib_window.closed())
            {
                return e_device_service::window_closed;
            }
        }
#endif
#ifdef EE_ENABLE_WEBGPU
        if (gs_field_probe.m_file != nullptr &&
            hardware.scanout().fields_started() > gs_field_probe.m_last_field &&
            !capture_gs_field(
                gs_field_probe, webgpu_bridge, gs_state, graphics_probe,
                hardware.gs_display_registers(),
                hardware.gs_crt_mode(),
                hardware.scanout().fields_started(), dispatched,
                runtime.m_ee_cycle_scheduler.current_cycle(), cpu.m_pc))
        {
            std::fprintf(stderr, "failed to write GS field probe: %s\n",
                gs_field_probe_path);
            return e_device_service::failed;
        }
#endif
        return e_device_service::continued;
    };
    enum class e_intc_service { not_pending, handled, failed };
    const auto service_intc = [&](uint32_t exit_reason) -> e_intc_service
    {
        // Normal dispatch/return boundaries cannot bridge an interrupt.
        // Keep their rejection outside the profiler; this scope now
        // measures exception-path INTC service only.
        if (!hle_startup || irq_callback_active || callback_active ||
            exit_reason != static_cast<uint32_t>(
                ee::e_ee_exit_reason::EE_EXIT_EXCEPTION))
        {
            return e_intc_service::not_pending;
        }
        ee::c_runtime_profile_scope profile_scope(ee::e_runtime_profile_id::intc_service);
        // ps2-web-recomp/kernel.cpp::run_intc acknowledges the actual
        // masked INTC cause and runs the registered guest callbacks, then
        // restores the interrupted EE thread. The ROM0 exception vector
        // is not executable in HLE-first mode, so bridge only an observed,
        // architecturally taken INTC exception; all other exceptions
        // remain unsupported. Never manufacture a VBlank or interrupt.
        if (((cpu.m_cop0[13] >> 2u) & 31u) == ee::EX_Int &&
            cpu.m_pc == ee::ee_exception_vector(
                cpu.m_cop0[12] & ~ee::STATUS_EXL, ee::EX_Int) &&
            (cpu.m_cop0[12] & (ee::STATUS_EXL | ee::STATUS_IE |
                                 ee::STATUS_EIE)) ==
                (ee::STATUS_EXL | ee::STATUS_IE | ee::STATUS_EIE) &&
            (cpu.m_cop0[12] & ee::STATUS_ERL) == 0u &&
            (cpu.m_cop0[13] & ee::CAUSE_BD) == 0u &&
            (ee::ee_pending_interrupts(&cpu) &
                (1u << (ee::CAUSE_IP_SHIFT + ee::INT_INTC))) != 0u &&
            hardware.intc().pending() != 0u)
        {
            const uint32_t pending = hardware.intc().pending();
            uint32_t cause = 0u;
            while (cause < 32u && (pending & (1u << cause)) == 0u)
            {
                ++cause;
            }
            if (cause >= 32u)
            {
                return e_intc_service::failed;
            }

            irq_callback_handlers.clear();
            for (const s_kernel_interrupt_handler& handler : intc_handlers)
            {
                if (handler.m_cause == cause)
                {
                    irq_callback_handlers.push_back(handler);
                }
            }
            if (!irq_callback_handlers.empty() &&
                !memory->contains(k_irq_kernel_stack - 0x1000u, 0x1000u))
            {
                std::fprintf(stderr,
                    "HLE INTC callback refused unmapped kernel stack\n");
                return e_intc_service::failed;
            }

            const ee::s_ee_u128 acknowledge{uint64_t{1} << cause, 0u};
            if (!ee::c_ee_intc::mmio_write(
                    &hardware.intc(), ee::c_ee_intc::k_stat_address,
                    4u, &acknowledge))
            {
                std::fprintf(stderr, "HLE INTC acknowledgement failed\n");
                return e_intc_service::failed;
            }
            // Resume at EPC, not EPC+4. The interrupted instruction did
            // not execute, and a branch-delay interrupt is excluded above.
            ee::s_ee_cpu_state resume_cpu = cpu;
            resume_cpu.m_pc = cpu.m_cop0[14];
            resume_cpu.m_cop0[12] &= ~ee::STATUS_EXL;
            resume_cpu.m_exit_reason = static_cast<uint32_t>(
                ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
            if (trace)
            {
                std::printf("HLE INTC IRQ: cause=%u pending=0x%08x "
                            "handlers=%zu epc=0x%08x\n",
                            cause, pending, irq_callback_handlers.size(),
                            resume_cpu.m_pc);
            }
            if (irq_callback_handlers.empty())
            {
                cpu = resume_cpu;
                hardware.intc().bind_cpu(&cpu);
                hardware.dmac().bind_cpu(&cpu);
                return e_intc_service::handled;
            }
            irq_interrupted_cpu = resume_cpu;
            irq_interrupted_vu0 = vu0;
            irq_callback_is_intc = true;
            irq_callback_cause = cause;
            irq_callback_index = 0u;
            irq_callback_active = true;
            enter_irq_callback(irq_callback_handlers[irq_callback_index++]);
            return e_intc_service::handled;
        }
        return e_intc_service::not_pending;
    };
    // Verify, without changing guest state, that the next iteration of an
    // eligible poll loop returns to its header and reads only values that
    // cannot change inside a native slice: RAM/scratchpad and status
    // registers the host updates between slices (never timers).
    const auto idle_iteration_loops = [&](const ee::s_idle_loop_shape& shape,
                                         uint32_t& instruction_count)
    {
        uint64_t gpr[32];
        for (uint32_t i = 0u; i < 32u; ++i)
        {
            gpr[i] = cpu.m_gpr[i].m_lo;
        }
        return ee::predict_idle_iteration(
            shape, gpr,
            [&](uint32_t address, uint32_t width, uint64_t* raw)
            {
                const uint32_t physical = memory->translate_direct_address(address);
                // IPU registers only change on EE writes or DMA starts,
                // never while a pure loop spins, and reads are side-effect
                // free in this model.
                const bool status_register =
                    width == 4u &&
                    (physical == 0x1000f000u ||   // INTC_STAT
                     physical == 0x1000f010u ||   // INTC_MASK
                     physical == 0x1000e010u ||   // D_STAT
                     physical == 0x12001000u ||   // GS CSR
                     physical == 0x10002000u ||   // IPU_CMD
                     physical == 0x10002004u ||   // IPU_CMD busy
                     physical == 0x10002010u ||   // IPU_CTRL
                     physical == 0x10002020u ||   // IPU_BP
                     physical == 0x10002030u ||   // IPU_TOP
                     physical == 0x10002034u);    // IPU_TOP busy
                if (!memory->contains(address, width) && !status_register)
                {
                    return false;
                }
                switch (width)
                {
                case 1u: *raw = ee::ee_mem_read8(&runtime, address); break;
                case 2u: *raw = ee::ee_mem_read16(&runtime, address); break;
                case 4u: *raw = ee::ee_mem_read32(&runtime, address); break;
                default: *raw = ee::ee_mem_read64(&runtime, address); break;
                }
                return true;
            }, &instruction_count) == ee::e_idle_iteration::loops;
    };

    while (remaining != 0)
    {
        if (pad_poll.failed())
        {
            pad_poll.summary(stderr);
            std::fprintf(stderr, "PAD0 input stream rejected a guest poll\n");
            return 1;
        }
        if (hardware.dmac().deferred_vif1_pending())
        {
            (void)hardware.dmac().service_deferred_vif1();
        }
        if (pad0_cross_poll_count != 0u && iop_kernel != nullptr)
        {
            iop_kernel->hardware().set_pad_trace_dispatch(dispatched);
        }
        // This catches HLE handoffs and interpreter observations that
        // returned via a host continuation rather than native dispatch.
        if (runtime.m_timing_observer_failed != 0u)
        {
            std::fprintf(stderr,
                "EE timing observer refused an event "
                "(guest execution halted, no hardware timing inferred)\n");
            return 1;
        }

        // HLE and RAM callbacks can continue directly to this loop. Publish
        // their elapsed budget before the next guest instruction or IOP slice.
        if (runtime.m_ee_cycle_scheduler.source() ==
            ee::e_ee_cycle_source::REFERENCE_COMPATIBILITY)
        {
            uint64_t cycle = 0u;
            if (!ee::ee_reference_compatibility_cycle(runtime, cycle))
            {
                return 1;
            }
            if (cycle != runtime.m_ee_cycle_scheduler.current_cycle())
            {
                const auto device_result = service_devices();
                if (device_result == e_device_service::failed)
                {
                    return 1;
                }
                if (device_result == e_device_service::window_closed)
                {
                    break;
                }
            }
        }

        // The primary SDBZ reference advances the IOP from every
        // elapsed EE cycle, including polling, timer, and idle periods.
        // Dividing by eight is reference compatibility timing, not an
        // assertion of verified R5900 cycle accuracy.
        ee::c_runtime_profile_scope iop_profile_scope(
            ee::e_runtime_profile_id::iop_service, 0u, nullptr,
            report_path != nullptr && report_boundary_timing);
        if (hle_startup && iop_rom0_path == nullptr &&
            iop_kernel != nullptr)
        {
            const uint64_t ee_cycle =
                runtime.m_ee_cycle_scheduler.current_cycle();
            const auto clock_result = iop_clock.observe(ee_cycle);
            if (clock_result != ee::c_ee_iop_clock::e_result::accepted)
            {
                std::fprintf(stderr, "%s\n",
                    clock_result == ee::c_ee_iop_clock::e_result::nonmonotonic
                        ? "IOP clock refused nonmonotonic EE timestamp"
                        : "IOP clock refused accumulated cycle overflow");
                return 1;
            }
            if (attribute_iop)
            {
                iop_attribution.checkpoint(iop_clock.due() != 0u);
            }
            uint32_t turns = 0u;
            // Never drop guest cycles to make a host checkpoint fast.
            // A large translated function may leave a bounded backlog.
            while (iop_clock.due() != 0u && turns < 64u)
            {
                const bool sample_iop = attribute_iop && iop_attribution.begin_batch();
                using t_iop_clock = std::chrono::steady_clock;
                const auto planner_start = sample_iop ? t_iop_clock::now() :
                    t_iop_clock::time_point{};
                ee::s_ee_iop_service_batch batch{
                    static_cast<uint32_t>(std::min<uint64_t>(iop_clock.due(), 256u)),
                    1u};
                uint64_t kernel_before = 0u;
                const bool may_coalesce =
                    iop_coalesce && iop_clock.due() > 256u && turns < 63u;
                // Both paths share the same ownership gate. The planner
                // and optional SIF trace read below do not publish events.
                // Re-evaluate on every physical service turn, never across
                // an IOP advance, RPC pump or guest checkpoint.
                const bool may_optimize_idle = (may_coalesce || iop_fast_quiet) &&
                    runtime.m_ee_cycle_scheduler.source() ==
                        ee::e_ee_cycle_source::REFERENCE_COMPATIBILITY &&
                    runtime.m_timing_observer == nullptr &&
                    !hle_iop_reboot_bootstrap_pending &&
                    (iop_rpc == nullptr || !iop_rpc->scheduler_barrier());
                if (may_coalesce && may_optimize_idle)
                {
                    // No runnable guest, IRQ, software wake, DMA completion,
                    // SPU2 sample or Timer5 edge may occur in a combined batch.
                    // With no queued RPC work, intervening pumps are no-ops.
                    const auto candidate = ee::ee_iop_service_batch(
                        iop_clock.due(), 64u - turns,
                        iop_kernel->idle_event_cycles_remaining());
                    if (candidate.m_turns > 1u)
                    {
                        kernel_before = iop_kernel->cycles();
                        const auto& spu2 = iop_kernel->hardware().spu2();
                        if (candidate.m_cycles <= UINT64_MAX - kernel_before &&
                            (!spu2.clock_active() ||
                                candidate.m_cycles <= UINT64_MAX - spu2.clock_cycles()))
                        {
                            batch = candidate;
                        }
                    }
                }
                const uint32_t cycles = batch.m_cycles;
                uint32_t prior_smflag = 0u;
                const bool trace_smflag = trace &&
                    hardware.sif().get_reg(4u, &prior_smflag) && prior_smflag == 0u;
                const auto kernel_start = sample_iop ? t_iop_clock::now() :
                    t_iop_clock::time_point{};
                bool quiet_advanced = false;
                if (iop_fast_quiet && may_optimize_idle)
                {
                    ++iop_quiet_attempts;
                    quiet_advanced = iop_kernel->try_advance_quiet_cycles(cycles);
                    if (quiet_advanced)
                    {
                        ++iop_quiet_advances;
                        iop_quiet_cycles += cycles;
                    }
                }
                // Both idle and budget_exhausted are accepted scheduled
                // service outcomes. The fast path has no guest execution.
                iop::s_kernel_phase_sample* phase_sample = nullptr;
                if (sample_iop && attribute_iop_kernel && !quiet_advanced)
                {
                    iop_phase_sample = {};
                    phase_sample = &iop_phase_sample;
                }
                const iop::s_kernel_run_result progressed = quiet_advanced
                    ? iop::s_kernel_run_result{} : iop_kernel->run(cycles, 128u, phase_sample);
                const auto kernel_end = sample_iop ? t_iop_clock::now() :
                    t_iop_clock::time_point{};
                if (phase_sample != nullptr)
                {
                    const uint32_t kind = progressed.m_executed != 0u ? 1u : 0u;
                    ++iop_phase_samples[kind];
                    for (uint32_t i = 0u; i < iop::s_kernel_phase_sample::k_count; ++i)
                    {
                        iop_phase_totals[kind].m_calls[i] += phase_sample->m_calls[i];
                        iop_phase_totals[kind].m_ns[i] += phase_sample->m_ns[i];
                    }
                }
                if (pad_poll.failed())
                {
                    pad_poll.summary(stderr);
                    std::fprintf(stderr, "PAD0 input stream rejected a guest poll\n");
                    return 1;
                }
                if (batch.m_turns > 1u &&
                    (progressed.m_executed != 0u ||
                        iop_kernel->cycles() - kernel_before != cycles ||
                        (progressed.m_stop != iop::e_kernel_stop::idle &&
                            progressed.m_stop != iop::e_kernel_stop::budget_exhausted)))
                {
                    std::fprintf(stderr, "IOP idle coalescing violated quiet interval\n");
                    // Do not retry or claim a valid result after guest state
                    // may have been changed by an unexpected event.
                    return 1;
                }
                if (iop_checkpoint_trace)
                {
                    iop_checkpoint_executed += progressed.m_executed;
                }
                uint32_t current_smflag = 0u;
                if (trace_smflag && progressed.m_executed != 0u &&
                    hardware.sif().get_reg(4u, &current_smflag) && current_smflag != 0u)
                {
                    std::printf("IOP guest published SMFLAG: value=0x%08x instructions=%llu\n",
                        current_smflag, static_cast<unsigned long long>(progressed.m_executed));
                }
                const auto rpc_start = sample_iop && iop_rpc != nullptr ?
                    t_iop_clock::now() : t_iop_clock::time_point{};
                if (iop_rpc != nullptr)
                {
                    (void)iop_rpc->pump();
                }
                if (attribute_iop)
                {
                    const auto rpc_end = sample_iop && iop_rpc != nullptr ?
                        t_iop_clock::now() : t_iop_clock::time_point{};
                    const auto elapsed_ns = [](auto start, auto end) -> uint64_t
                    {
                        return static_cast<uint64_t>(std::chrono::duration_cast<
                            std::chrono::nanoseconds>(end - start).count());
                    };
                    iop_attribution.finish_batch(cycles, progressed.m_executed,
                        quiet_advanced, iop_rpc != nullptr, sample_iop,
                        elapsed_ns(planner_start, kernel_start),
                        elapsed_ns(kernel_start, kernel_end),
                        elapsed_ns(rpc_start, rpc_end));
                }
                if (!iop_clock.consume(cycles))
                {
                    std::fprintf(stderr, "IOP clock refused invalid cycle consumption\n");
                    return 1;
                }
                turns += batch.m_turns;
                iop_clock_turns += batch.m_turns;
                if (iop_coalesce)
                {
                    ++iop_clock_runs;
                    if (batch.m_turns > 1u)
                    {
                        ++iop_coalesced_batches;
                    }
                }
                if (progressed.m_stop !=
                        iop::e_kernel_stop::budget_exhausted &&
                    progressed.m_stop != iop::e_kernel_stop::idle)
                {
                    std::fprintf(stderr,
                        "IOP clock guest execution stopped: "
                        "turn=%llu thread=%u reason=%u pc=0x%08x "
                        "opcode=0x%08x\n",
                        static_cast<unsigned long long>(
                            iop_clock_turns),
                        progressed.m_thread_id,
                        static_cast<uint32_t>(progressed.m_stop),
                        progressed.m_guest_pc, progressed.m_opcode);
                    // Preserve the actual IOP failure, not the EE PC
                    // where host scheduling happened to observe it.
                    if (iop_fault_probe.capture(*iop_kernel, progressed))
                    {
                        iop_fault_probe.write_text(stderr);
                    }
                    reason = static_cast<uint32_t>(
                        ee::e_ee_exit_reason::EE_EXIT_UNSUPPORTED);
                    status = 0u;
                    cpu.m_exit_reason = reason;
                    break;
                }
            }
            // Compare at the same EE service checkpoint, not at physical
            // kernel invocations (which differ when quiet turns are combined).
            if (iop_checkpoint_trace)
            {
                const auto& iop_hw = iop_kernel->hardware();
                const auto& spu2 = iop_hw.spu2();
                const auto device = iop_hw.scheduler_checkpoint();
                uint32_t ee_mscom = 0u, ee_smcom = 0u;
                uint32_t ee_msflag = 0u, ee_smflag = 0u;
                (void)hardware.sif().get_reg(1u, &ee_mscom);
                (void)hardware.sif().get_reg(2u, &ee_smcom);
                (void)hardware.sif().get_reg(3u, &ee_msflag);
                (void)hardware.sif().get_reg(4u, &ee_smflag);
                std::fprintf(stderr,
                    "IOP checkpoint: dispatch=%llu pc=%08x ee=%llu iop=%llu "
                    "turns=%llu executed=%llu debt=%llu next=%llu "
                    "irq=%08x dma=%u spu2=%llu frames=%llu "
                    "i_stat=%08x i_mask=%08x i_ctrl=%08x icr=%08x icr2=%08x "
                    "timer_count=%u timer_mode=%08x timer_target=%u timer_rem=%u timer_armed=%u "
                    "dma4=%u dma7=%u auto4=%u auto7=%u cdvd=%u "
                    "ee_dmac=%08x ee_intc=%08x mscom=%08x smcom=%08x msflag=%08x smflag=%08x "
                    "rpc_pending=%u reason=%u\n",
                    static_cast<unsigned long long>(dispatched), cpu.m_pc,
                    static_cast<unsigned long long>(ee_cycle),
                    static_cast<unsigned long long>(iop_kernel->cycles()),
                    static_cast<unsigned long long>(iop_clock_turns),
                    static_cast<unsigned long long>(iop_checkpoint_executed),
                    static_cast<unsigned long long>(iop_clock.due()),
                    static_cast<unsigned long long>(iop_clock.next_cycle()),
                    iop_hw.pending_interrupts(),
                    iop_hw.spu2_dma_cycles_remaining(),
                    static_cast<unsigned long long>(spu2.clock_cycles()),
                    static_cast<unsigned long long>(spu2.mixed_frames()),
                    device.m_irq_stat, device.m_irq_mask, device.m_irq_ctrl,
                    device.m_dma_icr, device.m_dma_icr2,
                    device.m_timer_count, device.m_timer_mode, device.m_timer_target,
                    device.m_timer_remainder, static_cast<unsigned>(device.m_timer_armed),
                    device.m_dma4_remaining, device.m_dma7_remaining,
                    static_cast<unsigned>(device.m_auto_dma4),
                    static_cast<unsigned>(device.m_auto_dma7),
                    static_cast<unsigned>(device.m_cdvd_pending),
                    hardware.dmac().status(), hardware.intc().pending(),
                    ee_mscom, ee_smcom, ee_msflag, ee_smflag,
                    static_cast<unsigned>(iop_rpc != nullptr &&
                        iop_rpc->pending_guest_calls() != 0u),
                    reason);
            }
            if (reason == static_cast<uint32_t>(
                    ee::e_ee_exit_reason::EE_EXIT_UNSUPPORTED))
            {
                break;
            }
            const bool rpc_pending =
                iop_rpc != nullptr && iop_rpc->pending_guest_calls() != 0u;
            if (rpc_pending &&
                !guest_rpc_idle_diagnostic_reported &&
                !iop_kernel->has_guest_work())
            {
                guest_rpc_idle_diagnostic_reported = true;
                uint32_t dma4_chcr = 0u;
                uint32_t dicr = 0u;
                uint32_t i_stat = 0u;
                (void)iop_kernel->hardware().read(
                    iop::c_hw::k_spu2_dma4_base + 8u,
                    4u, &dma4_chcr);
                (void)iop_kernel->hardware().read(
                    iop::c_hw::k_dma_icr, 4u, &dicr);
                (void)iop_kernel->hardware().read(
                    iop::c_hw::k_i_stat, 4u, &i_stat);
                std::printf(
                    "IOP HLE guest RPC stalled without IOP work: "
                    "ee_cycle=%llu pending_calls=%zu "
                    "dma4_chcr=0x%08x dicr=0x%08x i_stat=0x%08x\n",
                    static_cast<unsigned long long>(ee_cycle),
                    iop_rpc->pending_guest_calls(),
                    dma4_chcr, dicr, i_stat);
                for (const auto& item : iop_kernel->threads())
                {
                    const iop::s_kernel_thread& thread = item.second;
                    if (thread.m_status ==
                        iop::e_thread_status::dormant)
                    {
                        continue;
                    }
                    std::printf(
                        "IOP HLE pending thread: id=%u status=%u "
                        "pc=0x%08x wait_type=%u wake_cycle=%llu\n",
                        item.first,
                        static_cast<uint32_t>(thread.m_status),
                        thread.m_cpu.m_pc, thread.m_wait_type,
                        static_cast<unsigned long long>(
                            thread.m_wake_cycle));
                }
            }
            if (!rpc_pending)
            {
                guest_rpc_idle_diagnostic_reported = false;
            }
        }
        const uint64_t iop_duration_ns = iop_profile_scope.stop();
        if (report_path != nullptr)
        {
            iop_time_ns += iop_duration_ns;
        }

        // Kernel HLE services completed device interrupts before handing
        // control back to its main thread. There is no synthetic DMA
        // status: only actual masked D_STAT completion bits can enter here.
        // An EE WaitSema may be blocked on a real IOP->EE DMA5 reply.
        // Its syscall exception keeps EXL set, but the kernel must still
        // enter the registered DMA callback that supplies iSignalSema.
        const bool external_iop_wait = iop_semaphore_wait_turns != 0u;
        if (hle_startup && !irq_callback_active && !callback_active &&
            (ee::ee_interrupts_enabled_inline(&cpu) || external_iop_wait) &&
            hardware.dmac().pending() != 0u)
        {
            const uint32_t pending = hardware.dmac().pending();
            uint32_t channel = 0u;
            while (channel < 10u && (pending & (1u << channel)) == 0u)
            {
                ++channel;
            }
            if (channel >= 10u || !hardware.dmac().acknowledge(channel))
            {
                reason = static_cast<uint32_t>(
                    ee::e_ee_exit_reason::EE_EXIT_UNSUPPORTED);
                status = 0u;
                break;
            }
            irq_callback_handlers.clear();
            for (const s_kernel_interrupt_handler& handler : dmac_handlers)
            {
                if (handler.m_cause == channel)
                {
                    irq_callback_handlers.push_back(handler);
                }
            }
            if (trace)
            {
                std::printf("HLE DMAC IRQ: channel=%u pending=0x%08x "
                            "handlers=%zu\n", channel, pending,
                            irq_callback_handlers.size());
            }
            if (irq_callback_handlers.empty())
            {
                continue;
            }
            if (!memory->contains(k_irq_kernel_stack - 0x1000u, 0x1000u))
            {
                std::fprintf(stderr,
                    "HLE DMAC callback refused unmapped kernel stack\n");
                reason = static_cast<uint32_t>(
                    ee::e_ee_exit_reason::EE_EXIT_UNSUPPORTED);
                status = 0u;
                break;
            }
            irq_interrupted_cpu = cpu;
            irq_interrupted_vu0 = vu0;
            irq_callback_is_intc = false;
            irq_callback_cause = channel;
            if (channel == 5u && iop_rpc != nullptr)
            {
                iop_rpc->set_ee_receive_busy(true);
            }
            irq_callback_index = 0u;
            irq_callback_active = true;
            enter_irq_callback(irq_callback_handlers[irq_callback_index++]);
            continue;
        }
        if (report_path != nullptr &&
            (report_snapshot_milliseconds != 0u
                ? (last_snapshot_time == std::chrono::steady_clock::time_point{} ||
                   std::chrono::steady_clock::now() - last_snapshot_time >=
                       std::chrono::milliseconds(report_snapshot_milliseconds))
                : (dispatched == 0 ||
                   dispatched - last_snapshot_dispatch >= k_report_snapshot_interval ||
                   callback_active != last_snapshot_callback_active)))
        {
            // Durable progress state survives an external wall-clock kill.
            // Default snapshots preserve the previous 128-dispatch/callback
            // policy. Probe mode writes at bounded wall-clock intervals to
            // avoid repeated JSON I/O in fast guest paths, without using host
            // time to advance any EE, IOP, DMA, scanout or interrupt state.
            const auto report_start = std::chrono::steady_clock::now();
            if (trace && trace_buffered)
            {
                std::fflush(stdout);
            }
#ifdef EE_ENABLE_WEBGPU
            graphics_probe.set_gpu_progress(
                webgpu_bridge.active(), webgpu_bridge.rendered_primitives());
#endif
            const bool report_ok = write_report(
                report_path, image, cpu, entry, dispatched,
                runtime.m_guest_instruction_attempts,
                runtime, hardware, vsync_flags,
                reason, status, history, syscall_history, gs_state.state(),
                gif_path1, gif_path2, gif_path3, vif1,
                graphics_probe, iop_fault_probe, iop_kernel.get(),
                dma_wait_probe, pad_return_probe, spawn_probe, hotspots, true
            );
            report_time_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - report_start
            ).count();
            if (!report_ok)
            {
                return 1;
            }
            last_snapshot_dispatch = dispatched;
            last_snapshot_callback_active = callback_active;
            last_snapshot_time = std::chrono::steady_clock::now();
        }
        if (current_thread_id == 0u && !irq_callback_active && !callback_active)
        {
            if (!schedule_threads())
            {
                return 1;
            }
            if (current_thread_id != 0u)
            {
                continue;
            }

            // Idle has no guest instruction to take the interrupt.
            // Sample the real masked INTC line at this kernel boundary.
            if (hardware.intc().pending() != 0u &&
                (ee::ee_pending_interrupts(&cpu) &
                    (1u << (ee::CAUSE_IP_SHIFT + ee::INT_INTC))) != 0u &&
                ee::ee_check_interrupts(&cpu, &runtime, cpu.m_pc, 0u) != 0u)
            {
                if (service_intc(cpu.m_exit_reason) != e_intc_service::handled)
                {
                    reason = static_cast<uint32_t>(
                        ee::e_ee_exit_reason::EE_EXIT_UNSUPPORTED);
                    status = 0u;
                    break;
                }
                continue;
            }

            // Keep one idle turn below the normal IOP catch-up limit.
            // Device events continue on the emulated clock, even though
            // there are no EE instructions to charge to the guest.
            constexpr uint64_t k_idle_slice = 65536u;
            constexpr uint64_t k_max_idle_cycles = ee::c_ee_scanout_clock::k_ee_clock_hz;
            const uint64_t now = runtime.m_ee_cycle_scheduler.current_cycle();
            if (now - idle_started_cycle >= k_max_idle_cycles ||
                now > std::numeric_limits<uint64_t>::max() - k_idle_slice)
            {
                std::fprintf(stderr,
                    "EE scheduler idle limit: no guest thread woke at cycle=%llu\n",
                    static_cast<unsigned long long>(now));
                reason = static_cast<uint32_t>(
                    ee::e_ee_exit_reason::EE_EXIT_UNSUPPORTED);
                status = 0u;
                break;
            }
            uint64_t next = std::min(now + k_idle_slice, host_timers.next_event());
            if (hardware.scanout().enabled())
            {
                next = std::min(next, hardware.scanout().next_event_cycle());
            }
            // The preceding service has already handled all due events.
            // A device with no future deadline must not stall idle time.
            if (next <= now)
            {
                next = now + k_idle_slice;
            }
            idle_cycles += next - now;
            host_timers.set_idle_cycles(idle_cycles);
            if (!runtime.m_ee_cycle_scheduler.publish_cycle(next))
            {
                return 1;
            }
            reason = static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
            status = 1u;
            const auto device_result = service_devices();
            if (device_result == e_device_service::failed)
            {
                return 1;
            }
            if (device_result == e_device_service::window_closed)
            {
                break;
            }
            continue;
        }
        if (callback_active && !image.is_executable_address(cpu.m_pc))
        {
            // Executable RAM handlers (e.g. SDBZ's relocated
            // 0x80076000 syscall) cannot be found in the ELF-only
            // native PC map. Execute an explicitly bounded,
            // fail-closed MIPS instruction subset instead. Never
            // manufacture a successful return for unsupported code.
            constexpr uint32_t k_ram_instruction_budget = 65536u;
            const uint32_t ram_entry = cpu.m_pc;
            const ee::s_ee_ram_execution result =
                ee::ee_execute_ram_callback(
                    &cpu, memory.get(), ram_entry,
                    k_callback_return_pc, k_ram_instruction_budget,
                    &runtime
                );
            if (result.m_result != ee::e_ee_ram_result::RETURNED)
            {
                std::fprintf(stderr,
                    "registered syscall HLE RAM execution stopped: "
                    "entry=0x%08x pc=0x%08x instruction=0x%08x "
                    "steps=%u reason=%s\n",
                    ram_entry, result.m_pc, result.m_opcode,
                    result.m_instructions,
                    ee::ee_ram_result_name(result.m_result)
                );
                reason = static_cast<uint32_t>(
                    ee::e_ee_exit_reason::EE_EXIT_UNSUPPORTED
                );
                status = 0u;
                cpu.m_exit_reason = reason;
                break;
            }

            if (report_path != nullptr &&
                callback_sample_index < syscall_history.size())
            {
                syscall_history[callback_sample_index].m_v0 =
                    static_cast<uint32_t>(cpu.m_gpr[2].m_lo);
                syscall_history[callback_sample_index].m_returned = true;
            }
            cpu.m_gpr[31] = callback_saved_ra;
            cpu.m_cop0[12] &= ~2u;
            cpu.m_pc = callback_resume_pc;
            cpu.m_exit_reason = static_cast<uint32_t>(
                ee::e_ee_exit_reason::EE_EXIT_DISPATCH
            );
            callback_active = false;
            if (trace)
            {
                std::printf(
                    "HLE guest RAM syscall returned: entry=0x%08x "
                    "resume=0x%08x v0=0x%08x instructions=%u tlb_writes=%u\n",
                    ram_entry, cpu.m_pc,
                    static_cast<uint32_t>(cpu.m_gpr[2].m_lo),
                    result.m_instructions, result.m_tlb_writes
                );
            }
            continue;
        }

        const uint32_t from_pc = cpu.m_pc;
        if (state_trace_first != 0u && dispatched + 1u >= state_trace_first &&
            dispatched + 1u <= state_trace_last)
        {
            // Rolling fingerprints include all observed pre-native entries
            // between samples, even if dispatch PCs later reconverge.
            const uint64_t trace_dispatch = dispatched + 1u;
            const auto mix_trace = [](uint64_t hash, uint64_t value)
            {
                return (hash ^ value) * 1099511628211ull;
            };
            ++state_trace_observations;
            state_trace_path = mix_trace(state_trace_path, trace_dispatch);
            state_trace_path = mix_trace(state_trace_path, from_pc);
            state_trace_path = mix_trace(state_trace_path, current_thread_id);
            state_trace_clocks = mix_trace(state_trace_clocks,
                runtime.m_ee_cycle_scheduler.current_cycle());
            state_trace_clocks = mix_trace(state_trace_clocks,
                runtime.m_guest_instruction_attempts);
            state_trace_clocks = mix_trace(state_trace_clocks,
                iop_kernel ? iop_kernel->cycles() : 0u);
            state_trace_clocks = mix_trace(state_trace_clocks, iop_clock.due());
            state_trace_clocks = mix_trace(state_trace_clocks,
                hardware.scanout().fields_started());
            if (state_trace_every == 1u || trace_dispatch >= state_trace_next)
            {
                state_trace_next = static_cast<uint64_t>(state_trace_first) +
                    ((trace_dispatch - state_trace_first) / state_trace_every + 1u) *
                    state_trace_every;
                // Passive pre-native state, independent of host timestamps.
                // Hash only defined architectural GPR fields, not struct padding.
                uint64_t gpr_digest = 14695981039346656037ull;
                for (const auto& reg : cpu.m_gpr)
                {
                    gpr_digest = (gpr_digest ^ reg.m_lo) * 1099511628211ull;
                    gpr_digest = (gpr_digest ^ reg.m_hi) * 1099511628211ull;
                }
                std::fprintf(stdout,
                    "EE state: dispatch=%llu pc=%08x thread=%u gpr=%016llx "
                    "status=%08x cause=%08x epc=%08x "
                    "ee=%llu attempts=%llu iop=%llu debt=%llu "
                    "intc=%08x/%08x dmac=%08x/%08x fields=%llu rpc=%llu pad=%u "
                    "observations=%llu path=%016llx clocks=%016llx\n",
                    static_cast<unsigned long long>(dispatched + 1u),
                    from_pc, current_thread_id,
                    static_cast<unsigned long long>(gpr_digest),
                    cpu.m_cop0[12u], cpu.m_cop0[13u], cpu.m_cop0[14u],
                    static_cast<unsigned long long>(runtime.m_ee_cycle_scheduler.current_cycle()),
                    static_cast<unsigned long long>(runtime.m_guest_instruction_attempts),
                    static_cast<unsigned long long>(iop_kernel ? iop_kernel->cycles() : 0u),
                    static_cast<unsigned long long>(iop_clock.due()),
                    hardware.intc().status(), hardware.intc().mask(),
                    hardware.dmac().status(), hardware.dmac().pending(),
                    static_cast<unsigned long long>(hardware.scanout().fields_started()),
                    static_cast<unsigned long long>(last_iop_rpc_sequence),
                    iop_kernel ? iop_kernel->hardware().pad0_polls() : 0u,
                    static_cast<unsigned long long>(state_trace_observations),
                    static_cast<unsigned long long>(state_trace_path),
                    static_cast<unsigned long long>(state_trace_clocks));
                if (state_trace_detail)
                {
                    // Full architectural operands, not structure bytes/padding.
                    std::fprintf(stdout,
                        "EE detail: dispatch=%llu pc=%08x hi=%016llx lo=%016llx "
                        "hi1=%016llx lo1=%016llx sa=%08x fcsr=%08x "
                        "fpu_acc=%08x fpu_acc_overflow=%u\n",
                        static_cast<unsigned long long>(trace_dispatch), from_pc,
                        static_cast<unsigned long long>(cpu.m_hi),
                        static_cast<unsigned long long>(cpu.m_lo),
                        static_cast<unsigned long long>(cpu.m_hi1),
                        static_cast<unsigned long long>(cpu.m_lo1),
                        cpu.m_sa, cpu.m_fcsr, cpu.m_fpu_acc, cpu.m_fpu_acc_overflow);
                    for (uint32_t index = 0u; index < 32u; ++index)
                    {
                        std::fprintf(stdout,
                            "EE detail reg: dispatch=%llu index=%02u "
                            "gpr=%016llx:%016llx fpr=%016llx vf=%016llx:%016llx cop0=%08x\n",
                            static_cast<unsigned long long>(trace_dispatch), index,
                            static_cast<unsigned long long>(cpu.m_gpr[index].m_hi),
                            static_cast<unsigned long long>(cpu.m_gpr[index].m_lo),
                            static_cast<unsigned long long>(cpu.m_fpr[index]),
                            static_cast<unsigned long long>(cpu.m_vu0[index].m_hi),
                            static_cast<unsigned long long>(cpu.m_vu0[index].m_lo),
                            cpu.m_cop0[index]);
                    }
                    // Static ELF instruction window; does not read MMIO or
                    // claim that every word in the window was executed.
                    for (uint32_t index = 0u; index < 16u; ++index)
                    {
                        const uint64_t pc = static_cast<uint64_t>(from_pc) + index * 4u;
                        uint32_t word = 0u;
                        if (pc > UINT32_MAX ||
                            !image.read32_at(static_cast<uint32_t>(pc), &word))
                        {
                            break;
                        }
                        std::fprintf(stdout,
                            "EE detail code: dispatch=%llu pc=%08x elf=%08x\n",
                            static_cast<unsigned long long>(trace_dispatch),
                            static_cast<uint32_t>(pc), word);
                    }
                }
            }
        }
        if (report_path != nullptr)
        {
            spawn_probe.observe(from_pc, dispatched + 1u, cpu, memory.get());
            const bool sampled = pad_return_probe.observe(
                from_pc, dispatched + 1u,
                cpu.m_gpr[29u].m_lo, cpu.m_gpr[2u].m_lo,
                memory.get());
            if (sampled && iop_kernel != nullptr &&
                iop_kernel->hardware().pad_trace_window())
            {
                const auto& sample = pad_return_probe.last_sample();
                std::fprintf(stderr,
                    "PAD EE dispatch=%llu poll=%u buffer=%08x return=%u bytes=%02x %02x %02x %02x\n",
                    static_cast<unsigned long long>(sample.m_dispatch),
                    iop_kernel->hardware().pad0_polls(), sample.m_buffer_address,
                    sample.m_return_value, sample.m_bytes[0], sample.m_bytes[1],
                    sample.m_bytes[2], sample.m_bytes[3]);
            }
        }
        if (report_path != nullptr &&
            dma_wait_probe.observe(
                from_pc, dispatched + 1u, current_thread_id,
                memory.get(), &hardware.dmac()))
        {
            const bool newly_pending =
                from_pc == ee::c_ee_sdbz_dma_wait_probe::k_wait_pc &&
                dma_wait_probe.pending_wait().m_observed &&
                dma_wait_probe.pending_wait().m_dispatch == dispatched + 1u;
            const auto& sample = newly_pending ?
                dma_wait_probe.pending_wait() :
                from_pc == ee::c_ee_sdbz_dma_wait_probe::k_wait_pc ?
                dma_wait_probe.wait() : dma_wait_probe.red_loop();
            std::printf(
                "EE SDBZ command wait snapshot: dispatch=%llu pc=0x%08x "
                "slot=%u flags=[0x%08x,0x%08x] "
                "vif1=[chcr=%08x madr=%08x qwc=%u tadr=%08x] "
                "dstat=%08x pending=%08x "
                "(read-only; completion producer unproven)\n",
                static_cast<unsigned long long>(sample.m_dispatch),
                sample.m_pc, sample.selected_slot(),
                sample.flag0(), sample.flag1(),
                sample.m_vif1_chcr, sample.m_vif1_madr,
                sample.m_vif1_qwc, sample.m_vif1_tadr,
                sample.m_dmac_status, sample.m_dmac_pending
            );
        }
        if (has_trace_entry && from_pc == trace_entry)
        {
            const uint32_t a0 = static_cast<uint32_t>(cpu.m_gpr[4].m_lo);
            const uint32_t a1 = static_cast<uint32_t>(cpu.m_gpr[5].m_lo);
            std::printf(
                "EE trace-entry: dispatch=%llu pc=0x%08x "
                "ra=0x%08x sp=0x%08x "
                "a0=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x\n",
                static_cast<unsigned long long>(dispatched + 1u),
                from_pc,
                static_cast<uint32_t>(cpu.m_gpr[31].m_lo),
                static_cast<uint32_t>(cpu.m_gpr[29].m_lo),
                a0, a1,
                static_cast<uint32_t>(cpu.m_gpr[6].m_lo),
                static_cast<uint32_t>(cpu.m_gpr[7].m_lo)
            );
            // Capture preserved registers at a *native dispatch* boundary.
            // This is particularly useful when a translated call resumes
            // in the middle of its parent function (e.g. after JAL).
            // Both 64-bit halves of each EE GPR remain available in cpu;
            // the low halves are enough for 32-bit pointers and lengths.
            std::printf(
                "EE trace-entry saved: "
                "s0=%08x s1=%08x s2=%08x s3=%08x "
                "s4=%08x s5=%08x s6=%08x s7=%08x "
                "fp=%08x gp=%08x v0=%08x v1=%08x\n",
                static_cast<uint32_t>(cpu.m_gpr[16].m_lo),
                static_cast<uint32_t>(cpu.m_gpr[17].m_lo),
                static_cast<uint32_t>(cpu.m_gpr[18].m_lo),
                static_cast<uint32_t>(cpu.m_gpr[19].m_lo),
                static_cast<uint32_t>(cpu.m_gpr[20].m_lo),
                static_cast<uint32_t>(cpu.m_gpr[21].m_lo),
                static_cast<uint32_t>(cpu.m_gpr[22].m_lo),
                static_cast<uint32_t>(cpu.m_gpr[23].m_lo),
                static_cast<uint32_t>(cpu.m_gpr[30].m_lo),
                static_cast<uint32_t>(cpu.m_gpr[28].m_lo),
                static_cast<uint32_t>(cpu.m_gpr[2].m_lo),
                static_cast<uint32_t>(cpu.m_gpr[3].m_lo)
            );
            // Dump eight adjacent 32-bit pairs beginning at SP+0x10.
            // These are generic stack words, not interpreted as a
            // game-specific iovec. This is a *read-only* diagnostic
            // bounded by the guest memory mapper.
            const uint32_t sp = static_cast<uint32_t>(cpu.m_gpr[29].m_lo);
            const uint64_t pairs_address = static_cast<uint64_t>(sp) + 16u;
            if (pairs_address <= UINT32_MAX &&
                memory->contains(static_cast<uint32_t>(pairs_address), 64u))
            {
                for (uint32_t index = 0; index < 8u; ++index)
                {
                    const uint32_t address =
                        static_cast<uint32_t>(pairs_address) + index * 8u;
                    std::printf(
                        "EE trace-entry stack-pair: index=%u addr=0x%08x "
                        "word0=0x%08x word1=0x%08x\n",
                        index, address,
                        memory->read32(address), memory->read32(address + 4u)
                    );
                }
            }
            // Show the descriptor content without interpreting the memory
            // as a particular game's C structure or dereferencing a raw
            // host pointer. The caller can determine which fields matter.
            for (const uint32_t address : {a0, a1})
            {
                if (memory->contains(address, 16u))
                {
                    std::printf(
                        "EE trace-entry memory: addr=0x%08x "
                        "words=%08x,%08x,%08x,%08x\n",
                        address, memory->read32(address),
                        memory->read32(address + 4u),
                        memory->read32(address + 8u),
                        memory->read32(address + 12u)
                    );
                }
            }
        }
        const uint32_t watched_before = has_watch_word
            ? memory->read32(watch_word) : 0u;
        const uint32_t watch_s3_before = has_watch_word
            ? static_cast<uint32_t>(cpu.m_gpr[19].m_lo) : 0u;
        const uint32_t watch_s5_before = has_watch_word
            ? static_cast<uint32_t>(cpu.m_gpr[21].m_lo) : 0u;
        const uint32_t watch_fp_before = has_watch_word
            ? static_cast<uint32_t>(cpu.m_gpr[30].m_lo) : 0u;
        uint32_t count = 0;
        uint32_t out_pc = from_pc;
        // HLE services the IOP and pending callbacks between EE dispatches.
        // Batching across this boundary changes guest timing in a quiet run.
        const uint32_t slice =
            (hot_expand_dir != nullptr || hle_startup || trace ||
             report_path != nullptr) ? 1u : remaining;
        // Retrying here (rather than restarting the outer guest loop)
        // preserves the exact scheduler boundary: no second IOP quantum,
        // controller poll, VBlank, DMA, or GS presentation is delivered.
        // The history needs only three pre-entry registers. A full
        // snapshot is required solely for a possible live cache miss.
        const uint32_t before_s0 = static_cast<uint32_t>(cpu.m_gpr[16].m_lo);
        const uint32_t before_s1 = static_cast<uint32_t>(cpu.m_gpr[17].m_lo);
        const uint32_t before_s2 = static_cast<uint32_t>(cpu.m_gpr[18].m_lo);
        std::optional<ee::s_ee_cpu_state> cpu_before_dispatch;
        const uint64_t instructions_before_dispatch =
            runtime.m_guest_instruction_attempts;
        const uint64_t idle_before_dispatch = idle_skip_instructions;
        const bool profile_this_pc = profile_pcs &&
            dispatched + 1u >= profile_pcs_first &&
            dispatched + 1u <= profile_pcs_last;
        uint64_t pc_native_ns = 0u;
        uint64_t pc_nested_ns = 0u;
        // Shadow measurement of known deadlines at the existing
        // native boundary. Does not prove the next block is pure or
        // that MMIO cannot change a deadline while it executes.
        ee::s_ee_execution_horizon horizon{};
        ee::s_ee_execution_horizon horizon_without_iop{};
        ee::s_ee_execution_horizon horizon_iop_lookahead{};
        const bool horizon_sample = profile_horizon &&
            runtime.m_ee_cycle_scheduler.source() ==
                ee::e_ee_cycle_source::REFERENCE_COMPATIBILITY;
        const uint64_t horizon_start = horizon_sample
            ? runtime.m_ee_cycle_scheduler.current_cycle() : 0u;
        const uint64_t horizon_mmio_start = horizon_sample
            ? host_timers.mmio_generation() : 0u;
        const uint64_t horizon_vu_start = horizon_sample
            ? vu0_control.m_callback_generation : 0u;
        const auto horizon_cop0_start = horizon_sample
            ? ee::ee_horizon_cop0_snapshot(cpu, runtime)
            : ee::t_ee_horizon_cop0_snapshot{};
        uint32_t horizon_alu_slots = 0u;
        uint32_t horizon_alu_owner = 0u;
        bool horizon_alu_certificate = false;
        if (horizon_sample)
        {
            const bool iop_scheduled = hle_startup &&
                iop_rom0_path == nullptr && iop_kernel != nullptr;
            const uint64_t iop_deadline = iop_scheduled
                ? iop_clock.next_cycle() : ee::k_ee_no_deadline;
            const bool entry_callback = callback_active || irq_callback_active;
            const bool entry_idle = current_thread_id == 0u;
            const bool entry_irq = hardware.dmac().pending() != 0u ||
                hardware.intc().pending() != 0u ||
                ee::ee_pending_interrupts(&cpu) != 0u;
            const uint32_t vu_mask =
                static_cast<uint32_t>(hardware.dmac().deferred_vif1_pending()) |
                (static_cast<uint32_t>(vif1.cmsar1_running()) << 1u) |
                (static_cast<uint32_t>(vu0.m_running != 0u) << 2u);
            ++horizon_vu_masks[vu_mask];
            const bool entry_vu = vu_mask != 0u;
            const bool entry_other = iop_rom0_path != nullptr ||
                (runtime.m_perf_control & 0x80000000u) != 0u ||
                runtime.m_perf_compat_fault ||
                runtime.m_timing_observer != nullptr;
            // These independent observations can overlap; do not sum
            // them as exclusive blocking reasons.
            const uint32_t entry_mask = static_cast<uint32_t>(entry_callback) |
                (static_cast<uint32_t>(entry_idle) << 1u) |
                (static_cast<uint32_t>(entry_irq) << 2u) |
                (static_cast<uint32_t>(entry_vu) << 3u) |
                (static_cast<uint32_t>(entry_other) << 4u);
            ++horizon_entry_masks[entry_mask];
            bool barrier = entry_mask != 0u;
            if (!barrier && !ee::ee_horizon_checkpoint_ready(runtime, iop_clock, iop_scheduled))
            {
                barrier = true;
                ++horizon_clock_barriers;
            }
            // D_STAT pending only represents enabled completion IRQs.
            // Count the original STR mask at otherwise eligible entries.
            // An idle HLE SIF0 receiver can remain armed indefinitely;
            // only its external producer can make guest-visible progress.
            if (!barrier)
            {
                const uint32_t started = hardware.dmac().started_channels();
                ++horizon_dmac_masks[started];
                if (started == (1u << 5u))
                {
                    ++horizon_dmac_sif0_only;
                }
                const bool passive_sif0 = started == (1u << 5u) &&
                    iop_scheduled && iop_rpc != nullptr &&
                    !hle_iop_reboot_bootstrap_pending &&
                    !iop_rpc->scheduler_barrier() &&
                    hardware.dmac().sif0_receive_armed_only();
                if (passive_sif0)
                {
                    ++horizon_dmac_passive_sif0;
                }
                else if (started != 0u)
                {
                    barrier = true;
                    ++horizon_dmac_started_barriers;
                }
            }
#ifdef EE_ENABLE_WEBGPU
            // The renderer has no guest event timestamp. Preserve the
            // existing worker/fault observation cadence while committed
            // batches are queued or executing. Shadow measurement only.
            if (!barrier && async_gs && !webgpu_bridge.worker_quiescent())
            {
                barrier = true;
                ++horizon_gs_worker_barriers;
            }
#endif
            const uint64_t timer_deadline = host_timers.next_event();
            const uint64_t scanout_deadline = hardware.scanout().next_event_cycle();
            const uint64_t cop0_deadline =
                ee::ee_cop0_compare_deadline(runtime, horizon_start);
            horizon = ee::ee_execution_horizon(horizon_start, timer_deadline,
                scanout_deadline, cop0_deadline, iop_deadline, barrier);
            horizon_without_iop = ee::ee_execution_horizon(horizon_start,
                timer_deadline, scanout_deadline, cop0_deadline,
                ee::k_ee_no_deadline, barrier);

            uint64_t lookahead_deadline = iop_deadline;
            if (iop_scheduled && !barrier && iop_clock.due() == 0u)
            {
                // HLE reboot can still own the mailbox even when
                // its old RPC queues have been cleared.
                const bool rpc_pending = hle_iop_reboot_bootstrap_pending ||
                    (iop_rpc != nullptr && iop_rpc->scheduler_barrier());
                if (rpc_pending)
                {
                    ++horizon_iop_rpc_barriers;
                }
                else
                {
                    const uint64_t interval = iop_kernel->idle_event_cycles_remaining();
                    const uint64_t projected = iop_clock.cycle_after_iop(interval);
                    if (interval != 0u && projected != ee::k_ee_no_deadline &&
                        projected > iop_deadline)
                    {
                        lookahead_deadline = projected;
                        ++horizon_iop_idle_candidates;
                    }
                }
            }
            horizon_iop_lookahead = ee::ee_execution_horizon(horizon_start,
                timer_deadline, scanout_deadline, cop0_deadline,
                lookahead_deadline, barrier);
            if (slice == 1u &&
                ee::ee_horizon_bound_fits(horizon_start, horizon_iop_lookahead.m_cycle, 2u))
            {
                // Analyze immutable ELF bytes once per PC. This is only
                // a source-level candidate, not a certificate that the
                // current native owner has this exact path.
                auto cached = horizon_alu_bounds.find(from_pc);
                if (cached == horizon_alu_bounds.end())
                {
                    const auto slots = ee::ee_horizon_alu_exit_slots(from_pc,
                        [&](uint32_t pc, uint32_t* word)
                        {
                            return image.is_executable_address(pc) &&
                                image.read32_at(pc, word);
                        });
                    cached = horizon_alu_bounds.emplace(from_pc, slots).first;
                }
                if (ee::ee_horizon_bound_fits(horizon_start,
                        horizon_iop_lookahead.m_cycle, cached->second))
                {
                    horizon_alu_slots = cached->second;
                }
            }
        }
        bool hot_expand_budget_reached = false;
        for (;;)
        {
            // Arm the translated loop-header idle probe with known pure,
            // idempotent poll loops (see ee_idle_loop_skip.h).
            bool idle_armed = false;
            const bool event_timed_entry =
                runtime.m_ee_cycle_scheduler.source() ==
                    ee::e_ee_cycle_source::REFERENCE_COMPATIBILITY;
            const bool idle_candidate_entry = std::find(
                idle_probe_headers.begin(), idle_probe_headers.end(), from_pc) !=
                idle_probe_headers.end();
            if (idle_skip && !hardware.dmac().deferred_vif1_pending() &&
                !idle_probe_headers.empty() && slice == 1u &&
                (!event_timed_entry || idle_candidate_entry) &&
                runtime.m_loop_safepoint_limit > 1u &&
                runtime.m_loop_safepoint_yield_enabled != 0u &&
                runtime.m_instruction_trace == nullptr &&
                runtime.m_guest_v0_trace_remaining == 0u &&
                runtime.m_timing_observer == nullptr)
            {
                static_assert(sizeof(runtime.m_idle_probe_pcs) ==
                    sizeof(idle_probe_slots), "idle probe slot count changed");
                std::memcpy(runtime.m_idle_probe_pcs, idle_probe_slots,
                            sizeof(idle_probe_slots));
                runtime.m_idle_probe_state = 0u;
                runtime.m_idle_probe_hit_pc = 0u;
                runtime.m_idle_probe_remaining = 0u;
                idle_armed = true;
            }
            uint32_t safepoint_budget = runtime.m_loop_safepoint_limit;

            // Give translated EE loops a ps2-web-recomp-style bounded
            // compatibility-clock slice, shortened to the next known EE
            // timer, scanout or COP0 event. The loop-header check is cheap
            // when disabled: UINT64_MAX is the no-deadline sentinel, so it
            // avoids a per-header enable load and branch.
            runtime.m_event_budget_yield_enabled = 0u;
            runtime.m_event_budget_slot_deadline = UINT64_MAX;
            if (runtime.m_ee_cycle_scheduler.source() ==
                ee::e_ee_cycle_source::REFERENCE_COMPATIBILITY)
            {
                const uint64_t now =
                    runtime.m_ee_cycle_scheduler.current_cycle();
                uint64_t live = 0u;
                if (!ee::ee_reference_compatibility_cycle(runtime, live) ||
                    live != now)
                {
                    std::fprintf(stderr,
                        "EE event budget refused stale compatibility clock\n");
                    return 1;
                }
                uint64_t next_event = std::min({
                    host_timers.next_event(),
                    hardware.scanout().next_event_cycle(),
                    ee::ee_cop0_compare_deadline(runtime, now)});
                if (hle_startup && iop_kernel != nullptr &&
                    iop_rom0_path == nullptr && iop_clock.due() == 0u)
                {
                    const uint64_t interval =
                        iop_kernel->idle_event_cycles_remaining();
                    const uint64_t iop_event =
                        iop_clock.cycle_after_iop(interval);
                    if (interval != 0u &&
                        iop_event != ee::k_ee_no_deadline)
                    {
                        next_event = std::min(next_event, iop_event);
                    }
                }
                const uint64_t slice_cycles =
                    ee::ee_reference_slice_budget(now, next_event);
                if (runtime.m_reference_instruction_slots >
                    UINT64_MAX - slice_cycles)
                {
                    std::fprintf(stderr,
                        "EE event budget instruction-slot deadline overflow\n");
                    return 1;
                }
                runtime.m_event_budget_slot_deadline =
                    runtime.m_reference_instruction_slots + slice_cycles;
                runtime.m_event_budget_yield_enabled = 1u;
                if (idle_armed && idle_candidate_entry && from_pc == cpu.m_pc)
                {
                    const auto shape = idle_loop_shapes.find(from_pc);
                    uint32_t slots_per_iteration = 0u;
                    if (shape != idle_loop_shapes.end() &&
                        idle_iteration_loops(shape->second, slots_per_iteration) &&
                        slots_per_iteration != 0u)
                    {
                        const uint64_t slots_to_deadline =
                            runtime.m_event_budget_slot_deadline -
                            runtime.m_reference_instruction_slots;
                        const uint32_t event_budget = ee::idle_loop_iteration_budget(
                            slots_to_deadline, slots_per_iteration,
                            runtime.m_loop_safepoint_limit);
                        if (event_budget != 0u)
                        {
                            // A verified loop can be accelerated only to the
                            // first header at or after this slice deadline.
                            safepoint_budget = event_budget;
                        }
                    }
                }
            }

            // One translated function invocation per native guest boundary.
            runtime.m_loop_safepoint_remaining = safepoint_budget;
            ee::c_runtime_profile_scope native_profile_scope(
                ee::e_runtime_profile_id::ee_native_boundary, 0u, nullptr,
                (report_path != nullptr && report_boundary_timing) || profile_this_pc);
            const uint32_t exit_reason_before = cpu.m_exit_reason;
            const auto resolved = slice == 1u && native_resolve != nullptr ?
                native_targets.resolve(from_pc, [&](uint32_t pc)
                {
                    auto target = native_resolve(pc);
                    for (const auto resolve : overlay_resolvers)
                    {
                        if (target != nullptr)
                        {
                            break;
                        }
                        // An older overlay without the optional export
                        // may own this PC. Preserve first-owner ordering
                        // by falling back to the ordinary dispatcher.
                        if (resolve == nullptr)
                        {
                            break;
                        }
                        target = resolve(pc);
                    }
                    return target;
                }) : nullptr;
            if (horizon_alu_slots != 0u)
            {
                // Exact cached identity is valid only while its native
                // module and the immutable ELF source remain loaded.
                const auto identity = alu_identities.resolve(
                    from_pc, resolved, horizon_alu_slots,
                    [&](uint32_t pc, ee::t_ee_translated_entry entry,
                        uint32_t bound)
                    {
                        ee::s_native_alu_identity result{};
                        auto target = native_resolve(pc);
                        auto identify = native_owner_resolve;
                        auto certify = native_alu_certificate;
                        for (size_t i = 0u; target == nullptr &&
                             i < overlay_resolvers.size(); ++i)
                        {
                            if (overlay_resolvers[i] == nullptr)
                            {
                                break;
                            }
                            target = overlay_resolvers[i](pc);
                            identify = overlay_owner_resolvers[i];
                            certify = overlay_alu_certificates[i];
                        }
                        if (target != entry || identify == nullptr)
                        {
                            return result;
                        }
                        result.m_owner = identify(pc, entry);
                        if (result.m_owner != 0u && certify != nullptr)
                        {
                            uint32_t slots = 0u;
                            const uint32_t* words = certify(pc, entry, &slots);
                            if (words != nullptr && slots == bound)
                            {
                                result.m_certified = true;
                                for (uint32_t i = 0u; i < slots; ++i)
                                {
                                    uint32_t word = 0u;
                                    if (!image.read32_at(pc + 4u * i, &word) ||
                                        word != words[i])
                                    {
                                        result.m_certified = false;
                                        break;
                                    }
                                }
                            }
                        }
                        return result;
                    });
                horizon_alu_owner = identity.m_owner;
                horizon_alu_certificate = identity.m_certified;
            }
            if (resolved == nullptr && hot_expand_dir != nullptr &&
                !cpu_before_dispatch.has_value())
            {
                cpu_before_dispatch.emplace(cpu);
            }
            if (async_vu1 && vu1_native_overlap_profile)
            {
                vif1.native_execution_begin();
            }
            if (resolved != nullptr)
            {
                // The runner's lookup is for from_pc, and neither
                // lookup nor the host-only VU1 timing guard changes PC.
                // Retain the checked failure path if that invariant breaks.
                const auto result = cpu.m_pc == from_pc ?
                    ee::ee_dispatch_run_owned_inline(cpu, &runtime, resolved) :
                    ee::ee_dispatch_run_resolved_inline(
                        &cpu, &runtime, from_pc, resolved);
                reason = result.exit_reason;
                count = result.dispatch_count;
                out_pc = result.guest_pc;
                status = static_cast<uint32_t>(result.status);
                direct_dispatches += count;
            }
            else
            {
                reason = native_run(&cpu, &runtime, slice, &count, &out_pc, &status);
                // A module that does not own the PC returns EXITED with zero
                // dispatches and leaves the CPU, PC and exit reason untouched.
                // Only then consult the live cache-miss overlays (each owns
                // disjoint, newly translated block PCs). A resolved overlay PC
                // is cached so hot overlay code costs one map lookup, not a scan.
                const auto unowned = [&]()
                {
                    return count == 0u && status == 0u &&
                           reason == exit_reason_before &&
                           out_pc == from_pc && cpu.m_pc == from_pc &&
                           runtime.m_guest_instruction_attempts ==
                               instructions_before_dispatch;
                };
                if (!overlay_runs.empty() && unowned())
                {
                    const auto cached = overlay_owner_by_pc.find(from_pc);
                    if (cached != overlay_owner_by_pc.end())
                    {
                        runtime.m_loop_safepoint_remaining = safepoint_budget;
                        reason = overlay_runs[cached->second](
                            &cpu, &runtime, slice, &count, &out_pc, &status);
                    }
                    else
                    {
                        for (size_t overlay = 0;
                             overlay < overlay_runs.size() && unowned();
                             ++overlay)
                        {
                            runtime.m_loop_safepoint_remaining = safepoint_budget;
                            reason = overlay_runs[overlay](
                                &cpu, &runtime, slice, &count, &out_pc, &status);
                            if (!unowned())
                            {
                                overlay_owner_by_pc.emplace(from_pc, overlay);
                            }
                        }
                    }
                }
            }
            if (async_vu1 && vu1_native_overlap_profile)
            {
                vif1.native_execution_end();
            }
            if (idle_armed)
            {
                // The armed header yielded after exactly one real iteration.
                // A verified pure poll loop would have repeated that iteration
                // for every remaining budgeted header visit (nothing it reads
                // can change before the runner regains control) and yielded
                // with no budget left. Charge exactly those iterations.
                const uint32_t hit = runtime.m_idle_probe_hit_pc;
                if (runtime.m_idle_probe_state == 2u &&
                    reason == static_cast<uint32_t>(
                        ee::e_ee_exit_reason::EE_EXIT_HOST_YIELD) &&
                    count == 1u && status == 0u &&
                    out_pc == hit && cpu.m_pc == hit)
                {
                    const auto shape = idle_loop_shapes.find(hit);
                    uint32_t per_iteration = 0u;
                    // A second safepoint inside the path would change
                    // the point where the unaccelerated slice exhausts.
                    // Require exactly one budget decrement between visits.
                    if (shape != idle_loop_shapes.end() &&
                        runtime.m_idle_probe_remaining >= 1u &&
                        runtime.m_loop_safepoint_remaining ==
                            runtime.m_idle_probe_remaining - 1u &&
                        idle_iteration_loops(shape->second, per_iteration) &&
                        per_iteration != 0u)
                    {
                        const uint64_t repeats =
                            runtime.m_idle_probe_remaining - 1u;
                        runtime.m_guest_instruction_attempts += repeats * per_iteration;
                        runtime.m_reference_instruction_slots += repeats * per_iteration;
                        runtime.m_loop_safepoint_remaining = 0u;
                        ++idle_skip_slices;
                        idle_skip_instructions += repeats * per_iteration;
                    }
                    else
                    {
                        // Should not happen for a verified loop; never arm it
                        // again. This one HOST_YIELD stays a host-only resume.
                        ++idle_skip_mispredictions;
                        idle_probe_headers.erase(
                            std::remove(idle_probe_headers.begin(),
                                        idle_probe_headers.end(), hit),
                            idle_probe_headers.end());
                        refresh_idle_probe_slots();
                    }
                }
                std::memset(runtime.m_idle_probe_pcs, 0,
                            sizeof(runtime.m_idle_probe_pcs));
                runtime.m_idle_probe_state = 0u;
            }
            if (runtime.m_timing_observer_failed != 0u)
            {
                std::fprintf(stderr,
                    "EE timing observation producer rejected native instruction "
                    "(no hardware event published)\n");
                return 1;
            }
            const uint64_t native_duration_ns = native_profile_scope.stop();
            if (profile_this_pc)
            {
                pc_native_ns += native_duration_ns;
                pc_nested_ns += native_profile_scope.child_ns();
            }
            if (report_path != nullptr)
            {
                native_time_ns += native_duration_ns;
                if (native_duration_ns > slowest_native_ns)
                {
                    slowest_native_ns = native_duration_ns;
                    slowest_native_pc = from_pc;
                }
            }
            if (count > slice)
            {
                std::fprintf(stderr,
                    "invalid native dispatcher count %u > %u\n", count, slice);
                return 1;
            }
            const bool missing_guest_code =
                hot_expand_dir != nullptr && count == 0u && status == 0u &&
                (reason == static_cast<uint32_t>(
                    ee::e_ee_exit_reason::EE_EXIT_DISPATCH) ||
                 reason == static_cast<uint32_t>(
                    ee::e_ee_exit_reason::EE_EXIT_RETURN));
            if (!missing_guest_code)
            {
                break;
            }
            if (out_pc != from_pc ||
                runtime.m_guest_instruction_attempts !=
                    instructions_before_dispatch)
            {
                std::fprintf(stderr,
                    "EE hot-expand refused non-atomic missing dispatch "
                    "from=0x%08x to=0x%08x\n", from_pc, out_pc);
                return 1;
            }
            std::filesystem::path replacement;
            bool replacement_is_overlay = false;
            std::string failure;
            const auto compile_start = std::chrono::steady_clock::now();
            ee::c_runtime_profile_scope compile_profile_scope(ee::e_runtime_profile_id::compile_wait);
            host_audio.pause();
            const bool expanded = request_hot_expand(
                hot_expand_dir, ++hot_expand_sequence, from_pc,
                &replacement, &replacement_is_overlay, &failure);
            host_audio.resume();
            compile_profile_scope.stop();
            compile_wait_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - compile_start).count();
            if (!expanded)
            {
                std::fprintf(stderr, "EE hot-expand failed: %s\n",
                             failure.c_str());
                return 1;
            }
            if (replacement.empty())
            {
                std::printf(
                    "EE hot-expand: expansion budget reached at pc=0x%08x\n",
                    from_pc);
                hot_expand_budget_reached = true;
                break;
            }
            auto candidate = std::make_unique<c_native_module>();
            if (!candidate->open(replacement.string().c_str()))
            {
                candidate->print_error();
                return 1;
            }
            const t_game_run replacement_entry = candidate->entry();
            if (replacement_entry == nullptr)
            {
                std::fprintf(stderr,
                    "EE hot-expand replacement lacks ee_game_run\n");
                return 1;
            }
            const auto replacement_resolver =
                direct_dispatch ? candidate->resolver() : nullptr;
            if (replacement_is_overlay)
            {
                overlay_modules.push_back(std::move(candidate));
                overlay_runs.push_back(replacement_entry);
                overlay_resolvers.push_back(replacement_resolver);
                overlay_owner_resolvers.push_back(overlay_modules.back()->owner_resolver());
                overlay_alu_certificates.push_back(overlay_modules.back()->alu_certificate());
            }
            else
            {
                // A full replacement already contains every overlay.
                // Native pointers must be invalidated before unloading
                // any owning library, not after a stale cache hit.
                native_targets.clear();
                alu_identities.clear();
                overlay_owner_by_pc.clear();
                overlay_runs.clear();
                overlay_resolvers.clear();
                overlay_owner_resolvers.clear();
                overlay_alu_certificates.clear();
                overlay_modules.clear();
                module.swap(*candidate);
                native_run = replacement_entry;
                native_resolve = replacement_resolver;
                native_owner_resolve = module.owner_resolver();
                native_alu_certificate = module.alu_certificate();
            }
            if (!cpu_before_dispatch.has_value())
            {
                std::fprintf(stderr, "EE hot-expand missing rollback state\n");
                return 1;
            }
            cpu = *cpu_before_dispatch;
            cpu.m_exit_reason = static_cast<uint32_t>(
                ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
            std::printf(
                "EE hot-expand: resumed guest pc=0x%08x "
                "with %s=%s (no replay)\n",
                from_pc, replacement_is_overlay ? "overlay" : "replacement",
                replacement.string().c_str());
            std::fflush(stdout);
        }
        if (hot_expand_budget_reached)
        {
            break;
        }

        if (profile_this_pc)
        {
            // Attribute measured boundary durations across retries, not
            // the time spent compiling, opening libraries or updating reports.
            pc_profile.record(from_pc, count,
                runtime.m_guest_instruction_attempts - instructions_before_dispatch,
                idle_skip_instructions - idle_before_dispatch, pc_native_ns,
                pc_nested_ns);
        }

        if (horizon_alu_slots != 0u)
        {
            ++horizon_alu_candidates;
            horizon_alu_owner_known += horizon_alu_owner != 0u;
            horizon_alu_certified += horizon_alu_certificate;
            uint64_t reached = 0u;
            if (count == 1u &&
                (reason == static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_DISPATCH) ||
                 reason == static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_RETURN)) &&
                ee::ee_reference_compatibility_cycle(runtime, reached) &&
                reached >= horizon_start && reached - horizon_start == horizon_alu_slots &&
                runtime.m_guest_instruction_attempts - instructions_before_dispatch ==
                    horizon_alu_slots &&
                idle_skip_instructions == idle_before_dispatch &&
                host_timers.mmio_generation() == horizon_mmio_start &&
                vu0_control.m_callback_generation == horizon_vu_start &&
                ee::ee_horizon_cop0_snapshot(cpu, runtime) == horizon_cop0_start)
            {
                ++horizon_alu_matched;
                horizon_alu_owner_matched += horizon_alu_owner != 0u;
                horizon_alu_certified_matched += horizon_alu_certificate;
            }
            else
            {
                ++horizon_alu_mismatch;
            }
        }
        if (horizon_sample && count != 0u)
        {
            uint64_t reached = 0u;
            if (!ee::ee_reference_compatibility_cycle(runtime, reached) ||
                reached < horizon_start)
            {
                ++horizon_unknown;
            }
            else
            {
                const size_t source = std::to_underlying(horizon.m_source);
                ++horizon_samples[source];
                const bool vu_touched = vu0_control.m_callback_generation != horizon_vu_start;
                const bool cop0_changed = ee::ee_horizon_cop0_snapshot(cpu, runtime) !=
                    horizon_cop0_start;
                if (vu_touched)
                {
                    ++horizon_vu_callback_boundaries;
                }
                if (cop0_changed)
                {
                    ++horizon_cop0_changed_boundaries;
                }
                if (reached < horizon.m_cycle)
                {
                    ++horizon_before[source];
                }
                else
                {
                    ++horizon_reached[source];
                }
                const bool normal_exit =
                    reason == static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_DISPATCH) ||
                    reason == static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_RETURN);
                if (normal_exit && reached < horizon.m_cycle)
                {
                    ++horizon_normal_before;
                }
                if (normal_exit && reached < horizon_without_iop.m_cycle)
                {
                    ++horizon_without_iop_before;
                }
                if (normal_exit && reached < horizon_iop_lookahead.m_cycle)
                {
                    ++horizon_iop_lookahead_before;
                    if (host_timers.mmio_generation() == horizon_mmio_start)
                    {
                        ++horizon_lookahead_mmio_free;
                        if (vu_touched || cop0_changed)
                        {
                            ++horizon_mmio_free_direct_touched;
                        }
                        else
                        {
                            ++horizon_mmio_free_direct_quiet;
                        }
                    }
                    else
                    {
                        ++horizon_lookahead_mmio_touched;
                    }
                }
            }
        }

        dispatched += count;
        // Observing the guest in the middle and near the end of a
        // bounded retail replay reveals a persistent R3000 spin without
        // recording per-instruction traces or changing guest scheduling.
        if (report_path != nullptr &&
            dispatched >= next_iop_guest_rpc_snapshot)
        {
            if (hle_startup && iop_kernel != nullptr &&
                iop_rpc != nullptr &&
                iop_rpc->pending_guest_calls() != 0u)
            {
                dump_pending_iop_guest_rpc(
                    *iop_kernel, dispatched,
                    iop_rpc->pending_guest_calls(), "checkpoint");
            }
            // Dense early checkpoints, then logarithmic spacing so
            // longer genuine guest work cannot flood the trace.
            if (next_iop_guest_rpc_snapshot < 100000u)
            {
                next_iop_guest_rpc_snapshot += 25000u;
            }
            else if (next_iop_guest_rpc_snapshot < 250000u)
            {
                next_iop_guest_rpc_snapshot = 250000u;
            }
            else if (next_iop_guest_rpc_snapshot <=
                     std::numeric_limits<uint64_t>::max() / 2u)
            {
                next_iop_guest_rpc_snapshot *= 2u;
            }
            else
            {
                next_iop_guest_rpc_snapshot =
                    std::numeric_limits<uint64_t>::max();
            }
        }
        remaining -= count;
        // Level 2 delivery is not supported by this runner. Stop before
        // device service, callback resumption or Level 1 HLE dispatch;
        // preserved ExcCode/EPC may describe a completely different event.
        if (ee::ee_is_level2_exception_exit(reason))
        {
            break;
        }
        const auto device_result = service_devices();
        if (device_result == e_device_service::failed)
        {
            return 1;
        }
        if (device_result == e_device_service::window_closed)
        {
            break;
        }
        if (has_watch_word && count != 0u)
        {
            const uint32_t watched_after = memory->read32(watch_word);
            if (watched_after != watched_before)
            {
                std::printf(
                    "EE watch changed: dispatch=%llu from_pc=0x%08x "
                    "to_pc=0x%08x addr=0x%08x old=0x%08x new=0x%08x\n",
                    static_cast<unsigned long long>(dispatched), from_pc,
                    cpu.m_pc, watch_word, watched_before, watched_after
                );
                std::printf(
                    "EE watch registers: "
                    "s3_before=%08x s3_after=%08x "
                    "s5_before=%08x s5_after=%08x "
                    "fp_before=%08x fp_after=%08x sp=%08x\n",
                    watch_s3_before,
                    static_cast<uint32_t>(cpu.m_gpr[19].m_lo),
                    watch_s5_before,
                    static_cast<uint32_t>(cpu.m_gpr[21].m_lo),
                    watch_fp_before,
                    static_cast<uint32_t>(cpu.m_gpr[30].m_lo),
                    static_cast<uint32_t>(cpu.m_gpr[29].m_lo)
                );
                if (watch_word >= 4u &&
                    memory->contains(watch_word - 4u, 8u))
                {
                    std::printf(
                        "EE watch pair: addr=0x%08x "
                        "word0=0x%08x word1=0x%08x\n",
                        watch_word - 4u,
                        memory->read32(watch_word - 4u),
                        watched_after
                    );
                }
            }
        }
        if (report_path != nullptr && count != 0)
        {
            ee::c_runtime_profile_scope profile_scope(ee::e_runtime_profile_id::dispatch_diagnostics);
            history.push_back({
                dispatched, from_pc, out_pc, reason, status,
                before_s0,
                static_cast<uint32_t>(cpu.m_gpr[16].m_lo),
                before_s1,
                static_cast<uint32_t>(cpu.m_gpr[17].m_lo),
                before_s2,
                static_cast<uint32_t>(cpu.m_gpr[18].m_lo)
            });
            if (report_hotspots)
            {
                // Host read-only observations, not synthetic guest events.
                const auto gif_channel = hardware.dmac().gif_channel_state();
                const ee::s_ee_hot_loop_state state{
                    current_thread_id,
                    cpu.m_cop0[12], cpu.m_cop0[13],
                    hardware.intc().status(), hardware.intc().mask(),
                    hardware.dmac().status(), hardware.dmac().pending(),
                    gif_channel.m_chcr, gif_channel.m_madr,
                    gif_channel.m_qwc, gif_channel.m_tadr,
                    hardware.dmac().gif_trace_count(),
                    static_cast<uint32_t>(hardware.dmac().gif_last_reject()),
                    hardware.scanout().fields_started(), hardware.scanout().csr(),
                    last_iop_rpc_sequence
                };
                hotspots.record(dispatched, from_pc, out_pc, state);
            }
        }

        if (report_path != nullptr &&
            dispatched - last_progress_dispatch >= report_progress_interval)
        {
            ee::c_runtime_profile_scope profile_scope(ee::e_runtime_profile_id::progress_log);
            // Keep stdout traces ahead of the progress line in a merged
            // stdout/stderr capture; report-less traces remain size-bounded.
            if (trace && trace_buffered)
            {
                std::fflush(stdout);
            }
            const double elapsed_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - wall_start
            ).count();
            std::fprintf(
                stderr,
                "EE progress: dispatched=%llu pc=0x%08x "
                "elapsed_ms=%.1f native_ms=%.1f report_ms=%.1f "
                "slowest_native_pc=0x%08x slowest_native_ms=%.3f "
                "iop_ms=%.1f compile_wait_ms=%.1f fields=%llu\n",
                static_cast<unsigned long long>(dispatched),
                cpu.m_pc, elapsed_ms, static_cast<double>(native_time_ns) / 1e6, report_time_ms,
                slowest_native_pc, static_cast<double>(slowest_native_ns) / 1e6,
                static_cast<double>(iop_time_ns) / 1e6, compile_wait_ms,
                static_cast<unsigned long long>(hardware.scanout().fields_started())
            );
            // Only the two requested checkpoints serialize the worker
            // AOT counters. No extra polling or guest completion.
            if (vu1_profile_begin != 0u && vu1_profile_snapshots < 2u &&
                dispatched >= (vu1_profile_snapshots == 0u ?
                    vu1_profile_begin : vu1_profile_end))
            {
                std::fprintf(stderr, "[vu1] worker profile snapshot dispatch=%llu fields=%llu aot=",
                    static_cast<unsigned long long>(dispatched),
                    static_cast<unsigned long long>(hardware.scanout().fields_started()));
                vif1.write_aot_json(stderr);
                std::fputc('\n', stderr);
                ++vu1_profile_snapshots;
            }
            std::fflush(stderr);
            last_progress_dispatch = dispatched;
        }

        // The complete dispatch statistics and last-64 transition
        // history above remain unsampled. Only the verbose text stream
        // is thinned during long-running, non-failing host loops.
        const bool ordinary_transition =
            reason == static_cast<uint32_t>(
                ee::e_ee_exit_reason::EE_EXIT_DISPATCH) ||
            reason == static_cast<uint32_t>(
                ee::e_ee_exit_reason::EE_EXIT_RETURN);
        const bool routine_event_yield = reason == static_cast<uint32_t>(
            ee::e_ee_exit_reason::EE_EXIT_EVENT_YIELD);
        if (routine_event_yield)
        {
            ++event_yield_count;
        }
        const bool trace_stride_hit = routine_event_yield
            ? dispatch_trace_every == 1u || event_yield_count <= 64u ||
                event_yield_count % dispatch_trace_every == 0u
            : dispatch_trace_every == 1u || dispatched <= 64u ||
                dispatched % dispatch_trace_every == 0u;
        if (trace && (trace_stride_hit ||
            (!ordinary_transition && !routine_event_yield) ||
            (status != 1u && !routine_event_yield)))
        {
            ee::c_runtime_profile_scope profile_scope(ee::e_runtime_profile_id::dispatch_trace_log);
            std::printf("dispatch=%llu pc=0x%08x -> 0x%08x reason=%s(%u) status=%u\n",
                        static_cast<unsigned long long>(dispatched), from_pc,
                        out_pc, exit_name(reason), reason, status);
        }

        // Each HOST_YIELD has committed its original guest branch,
        // delay slot and register writes. Bound subsequent native
        // watchdog resumes until a *real* GS field, GIF write, or IOP
        // RPC completion is observed. Guest instruction attempts and
        // dispatch count alone are not a progress signal. The host
        // never manufactures cycles, IRQs, DMA, responses, or frames.
        if (reason == static_cast<uint32_t>(
                ee::e_ee_exit_reason::EE_EXIT_HOST_YIELD))
        {
            const bool idle_event_fast_forwarded =
                idle_skip_instructions != idle_before_dispatch &&
                runtime.m_event_budget_yield_enabled != 0u;
            if (count == 0u || status == 2u ||
                (!idle_event_fast_forwarded && !loop_yield_budget.try_resume(
                    loop_yield_resumes,
                    hardware.scanout().fields_started(),
                    last_iop_rpc_sequence,
                    hardware.dmac().gif_trace_count())))
            {
                break;
            }
            ++completed_loop_yields;
            runtime.m_ee_cycle_scheduler.record_scheduler_yield();
            if (trace)
            {
                std::printf(
                    "EE host loop resume: count=%u/%u total=%u "
                    "progress_resets=%llu pc=0x%08x fields=%llu "
                    "rpc=%llu gif_writes=%llu "
                    "(guest EE cycles unchanged)\n",
                    loop_yield_budget.without_progress(),
                    loop_yield_resumes, completed_loop_yields,
                    static_cast<unsigned long long>(
                        loop_yield_budget.progress_resets()),
                    cpu.m_pc,
                    static_cast<unsigned long long>(
                        hardware.scanout().fields_started()),
                    static_cast<unsigned long long>(
                        last_iop_rpc_sequence),
                    static_cast<unsigned long long>(
                        hardware.dmac().gif_trace_count()));
            }
            cpu.m_exit_reason = static_cast<uint32_t>(
                ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
            analyze_idle_loop_header(cpu.m_pc);
            continue;
        }

        // Event-budget yields are ordinary scheduler boundaries. Device and
        // IOP service already ran above; resume immediately without applying
        // the separate no-progress watchdog used for diagnostic host yields.
        if (reason == static_cast<uint32_t>(
                ee::e_ee_exit_reason::EE_EXIT_EVENT_YIELD))
        {
            // Event-bounded loop yields bypass the legacy HOST_YIELD path
            // that originally discovered poll loops. Analyze this exact
            // committed loop header so a later slice can safely skip to its
            // next scheduler deadline.
            analyze_idle_loop_header(cpu.m_pc);
            cpu.m_exit_reason = static_cast<uint32_t>(
                ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
            continue;
        }

        // A bounded one-dispatch slice reports DISPATCH_LIMIT even when
        // the translated callback just returned to the synthetic sentinel.
        if (irq_callback_active &&
            count != 0u &&
            cpu.m_pc == k_callback_return_pc &&
            status != 2u &&
            (reason == static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_RETURN) ||
             reason == static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_DISPATCH)))
        {
            const int32_t result = static_cast<int32_t>(cpu.m_gpr[2].m_lo);
            if (slot_callback_armed)
            {
                slot_callback_armed = false;
                ee::c_ee_sdbz_slot_callback_probe::s_slot_observation after{};
                if (ee::c_ee_sdbz_slot_callback_probe::sample_slots(memory.get(), after))
                {
                    ++slot_callback_samples;
                    std::fprintf(stderr,
                        "EE SDBZ slot callback: dispatch=%llu "
                        "handler=0x00102a60 resume=0x001025a4 result=%d "
                        "selector=%u->%u flags=[%u,%u]->[%u,%u] "
                        "(read-only callback boundaries)\n",
                        static_cast<unsigned long long>(dispatched), result,
                        slot_callback_before.m_selector, after.m_selector,
                        slot_callback_before.m_flag0, slot_callback_before.m_flag1,
                        after.m_flag0, after.m_flag1);
                }
            }
            if (result >= 0 && irq_callback_index < irq_callback_handlers.size())
            {
                enter_irq_callback(irq_callback_handlers[irq_callback_index++]);
                continue;
            }
            ee::ee_cop0_clock_sync(&cpu, &runtime);
            cpu = irq_interrupted_cpu;
            ee::ee_cop0_clock_sync(&cpu, &runtime);
            vu0 = irq_interrupted_vu0;
            hardware.intc().bind_cpu(&cpu);
            hardware.dmac().bind_cpu(&cpu);
            irq_callback_active = false;
            irq_callback_handlers.clear();
            if (!irq_callback_is_intc && irq_callback_cause == 5u &&
                iop_rpc != nullptr)
            {
                iop_rpc->set_ee_receive_busy(false);
                (void)iop_rpc->pump();
            }
            if (!irq_callback_is_intc && iop_semaphore_wait_turns != 0u)
            {
                // The interrupted EE context is a WaitSema syscall trap,
                // so its architectural PC is the exception vector. Replay
                // only that original syscall at EPC after the real DMA5
                // callback has iSignalSema'd its token.
                cpu.m_pc = cpu.m_cop0[14];
                cpu.m_cop0[12] &= ~2u;
            }
            cpu.m_exit_reason = static_cast<uint32_t>(
                ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
            if (trace)
            {
                if (irq_callback_is_intc)
                {
                    std::printf("HLE INTC callback returned: cause=%u "
                                "v0=%d resume=0x%08x\n",
                                irq_callback_cause, result, cpu.m_pc);
                }
                else
                {
                    std::printf("HLE DMAC callback returned: channel=%u "
                                "v0=%d resume=0x%08x\n",
                                irq_callback_cause, result, cpu.m_pc);
                }
            }
            // A genuine interrupt-safe SignalSema may have promoted an EE
            // waiter to READY. The kernel must finish restoring interrupted
            // EE/VU0 state before scheduling the newly runnable thread.
            if ((reschedule_requested || current_thread_id == 0u ||
                 threads[current_thread_id].m_self_suspend_pending) &&
                !schedule_threads())
            {
                reason = static_cast<uint32_t>(
                    ee::e_ee_exit_reason::EE_EXIT_UNSUPPORTED);
                status = 0u;
                break;
            }
            continue;
        }
        if (callback_active &&
            count != 0u &&
            cpu.m_pc == k_callback_return_pc &&
            status != 2u &&
            (reason == static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_RETURN) ||
             reason == static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_DISPATCH)))
        {
            // The guest callback supplies V0 and memory side effects. The
            // host restores only the interrupted call's return address.
            if (report_path != nullptr && callback_sample_index < syscall_history.size())
            {
                syscall_history[callback_sample_index].m_v0 =
                    static_cast<uint32_t>(cpu.m_gpr[2].m_lo);
                syscall_history[callback_sample_index].m_returned = true;
            }
            cpu.m_gpr[31] = callback_saved_ra;
            cpu.m_cop0[12] &= ~2u;
            cpu.m_pc = callback_resume_pc;
            cpu.m_exit_reason =
                static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
            callback_active = false;
            if (trace)
            {
                std::printf("HLE guest syscall returned: resume=0x%08x v0=0x%08x\n",
                            cpu.m_pc, static_cast<uint32_t>(cpu.m_gpr[2].m_lo));
            }
            continue;
        }

        // Returning from a worker's entry makes it dormant.
        if (count != 0u && cpu.m_pc == k_thread_return_pc &&
            current_thread_id > 1u && !irq_callback_active &&
            !callback_active)
        {
            s_kernel_thread& finished = threads[current_thread_id];
            make_thread_dormant(finished);
            if (trace)
            {
                std::printf("HLE EE thread returned: id=%u\n",
                            current_thread_id);
            }
            if (!schedule_threads())
            {
                break;
            }
            continue;
        }

        // A translated return to PC zero is an exit from the guest code
        // region. Do not invoke another one-dispatch slice with PC zero:
        // the dispatcher reserves zero as an initial-entry sentinel.
        if (count != 0u && cpu.m_pc == 0u)
        {
            status = 0u;
            break;
        }

        const auto intc_result = service_intc(reason);
        if (intc_result == e_intc_service::failed)
        {
            reason = static_cast<uint32_t>(
                ee::e_ee_exit_reason::EE_EXIT_UNSUPPORTED);
            status = 0u;
            break;
        }
        if (intc_result == e_intc_service::handled)
        {
            continue;
        }

        // Experimental PS2 EE startup HLE. Handle only explicitly
        // recognized syscalls; unknown services still stop at the exception.
        if (hle_startup &&
            reason == static_cast<uint32_t>(
                ee::e_ee_exit_reason::EE_EXIT_EXCEPTION) &&
            ((cpu.m_cop0[13] >> 2) & 0x1fu) == 8u)
        {
            ee::c_runtime_profile_scope profile_scope(ee::e_runtime_profile_id::ee_hle_syscall);
            const int32_t signed_service =
                static_cast<int32_t>(cpu.m_gpr[3].m_lo);
            // Reject all unknown negative syscall encodings. An
            // interrupt-safe service is accepted only while a real
            // INTC/DMAC callback owns the saved EE interrupt context.
            // The retail 17:45 Cross replay reaches -0x31 from a genuine
            // VBlank callback at guest PC 0x00174bb4.
            const ee_irq_syscall::s_decoded_service decoded =
                ee_irq_syscall::decode(signed_service, irq_callback_active);
            const uint32_t service = decoded.m_number;
            const bool interrupt_safe_service = decoded.m_interrupt_safe;
            const bool interrupt_sif_service = interrupt_safe_service &&
                (service == 0x76u || service == 0x77u || service == 0x78u);
            if (!callback_active && !interrupt_safe_service &&
                service < 256u &&
                installed_syscall_handlers[service] != 0u)
            {
                // In interrupt context R5900 EPC still refers to the
                // interrupted thread, not the nested syscall. The runtime
                // separately records the actual trap instruction PC.
                const uint32_t epc = irq_callback_active
                    ? cpu.m_pad : cpu.m_cop0[14];
                const bool trap_in_bd_slot = irq_callback_active
                    ? runtime.m_last_exception_in_bd_slot
                    : (cpu.m_cop0[13] & ee::CAUSE_BD) != 0u;
                const uint32_t handler = installed_syscall_handlers[service];
                uint32_t opcode = 0;
                if (!image.read32_at(epc, &opcode) ||
                    (opcode & 0xfc00003fu) != 0x0000000cu ||
                    trap_in_bd_slot ||
                    epc > 0xfffffffbu)
                {
                    std::fprintf(stderr,
                        "registered syscall HLE refused invalid context "
                        "number=0x%02x epc=0x%08x handler=0x%08x\n",
                        service, epc, handler);
                    break;
                }
                if (!image.is_executable_address(handler) &&
                    !memory->contains(handler, sizeof(uint32_t)))
                {
                    std::fprintf(stderr,
                        "registered syscall HLE refused unmapped RAM "
                        "handler number=0x%02x handler=0x%08x\n",
                        service, handler);
                    break;
                }
                // The guest handler runs natively, but handing control to
                // it through host syscall dispatch has an uncalibrated cost.
                ee::ee_record_hle_timing_boundary(runtime, epc, opcode, service);
                callback_saved_ra = cpu.m_gpr[31];
                callback_resume_pc = epc + 4u;
                if (report_path != nullptr)
                {
                    if (syscall_history.size() == 32u)
                    {
                        syscall_history.erase(syscall_history.begin());
                    }
                    syscall_history.push_back({
                        dispatched, service, handler, callback_resume_pc,
                        static_cast<uint32_t>(cpu.m_gpr[4].m_lo),
                        static_cast<uint32_t>(cpu.m_gpr[5].m_lo),
                        static_cast<uint32_t>(cpu.m_gpr[6].m_lo),
                        static_cast<uint32_t>(cpu.m_gpr[7].m_lo),
                        0u, false
                    });
                    callback_sample_index = static_cast<uint32_t>(syscall_history.size() - 1u);
                }
                callback_active = true;
                cpu.m_gpr[31].m_lo = static_cast<uint64_t>(-16ll);
                cpu.m_gpr[31].m_hi = 0u;
                cpu.m_pc = handler;
                cpu.m_exit_reason =
                    static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
                if (trace)
                {
                    std::printf(
                        "HLE guest syscall: number=0x%02x handler=0x%08x "
                        "resume=0x%08x\n",
                        service, handler, callback_resume_pc);
                }
                continue;
            }
            if (service == 0x02u ||
                (service >= 0x10u && service <= 0x17u) ||
                (service >= 0x1au && service <= 0x1du) ||
                (service >= 0x20u && service <= 0x25u) ||
                service == 0x29u || service == 0x2au || service == 0x2bu ||
                service == 0x2du || service == 0x2eu ||
                service == 0x2fu || service == 0x30u || service == 0x31u ||
                (service >= 0x32u && service <= 0x37u) ||
                service == 0x39u ||
                service == 0x3cu || service == 0x3du || service == 0x3eu ||
                (service >= 0x40u && service <= 0x48u) ||
                service == 0x4au || service == 0x4bu ||
                service == 0x64u ||
                service == 0x68u || service == 0x6bu ||
                service == 0x71u || service == 0x73u ||
                service == 0x74u ||
                service == 0x76u || service == 0x77u ||
                service == 0x78u || service == 0x79u ||
                service == 0x7au || service == 0x7cu ||
                service == 0x7fu)
            {
                // In interrupt context R5900 EPC still refers to the
                // interrupted thread, not the nested syscall. The runtime
                // separately records the actual trap instruction PC.
                const uint32_t epc = irq_callback_active
                    ? cpu.m_pad : cpu.m_cop0[14];
                const bool trap_in_bd_slot = irq_callback_active
                    ? runtime.m_last_exception_in_bd_slot
                    : (cpu.m_cop0[13] & ee::CAUSE_BD) != 0u;
                uint32_t opcode = 0;
                if (!image.read32_at(epc, &opcode) ||
                    (opcode & 0x3fu) != 0x0cu ||
                    (opcode >> 26) != 0u ||
                    trap_in_bd_slot ||
                    epc > 0xfffffffbu)
                {
                    std::fprintf(stderr, "startup HLE refused invalid syscall context\n");
                    break;
                }

                // Explicitly mark entry to modeled host kernel services.
                // The reference clock charges the upstream default syscall budget.
                ee::ee_record_hle_timing_boundary(runtime, epc, opcode, service);

                if (interrupt_sif_service && trace)
                {
                    std::printf(
                        "HLE interrupt SIF syscall: pc=0x%08x "
                        "architectural_epc=0x%08x resume=0x%08x\n",
                        epc, cpu.m_cop0[14], epc + 4u
                    );
                }

                if (service == 0x02u)
                {
                    // SetGsCrt configures the privileged GS display timing
                    // registers. The runner preserves the configuration but
                    // does not fabricate scanout, vblank or presentation.
                    const uint32_t interlace =
                        static_cast<uint32_t>(cpu.m_gpr[4].m_lo);
                    const uint32_t mode =
                        static_cast<uint32_t>(cpu.m_gpr[5].m_lo);
                    const uint32_t ffmd =
                        static_cast<uint32_t>(cpu.m_gpr[6].m_lo);
                    if (!hardware.set_gs_crt(
                            interlace, mode, ffmd,
                            runtime.m_ee_cycle_scheduler.current_cycle()))
                    {
                        std::fprintf(stderr,
                            "SetGsCrt HLE refused invalid configuration "
                            "interlace=%u mode=%u ffmd=%u\n",
                            interlace, mode, ffmd);
                        break;
                    }
                    if (trace)
                    {
                        std::printf(
                            "HLE SetGsCrt: interlace=%u mode=%u ffmd=%u "
                            "result=void\n",
                            interlace, mode, ffmd);
                    }
                }

                else if (service >= 0x10u && service <= 0x13u)
                {
                    const bool intc = service == 0x10u || service == 0x11u;
                    std::vector<s_kernel_interrupt_handler>& handlers =
                        intc ? intc_handlers : dmac_handlers;
                    const uint32_t cause =
                        static_cast<uint32_t>(cpu.m_gpr[4].m_lo);
                    const uint32_t address_or_id =
                        static_cast<uint32_t>(cpu.m_gpr[5].m_lo);
                    const bool add = service == 0x10u || service == 0x12u;

                    if (add)
                    {
                        const uint32_t next =
                            static_cast<uint32_t>(cpu.m_gpr[6].m_lo);
                        const uint32_t argument =
                            static_cast<uint32_t>(cpu.m_gpr[7].m_lo);
                        const uint32_t gp =
                            static_cast<uint32_t>(cpu.m_gpr[28].m_lo);
                        const uint32_t id = kernel_add_interrupt_handler(
                            handlers, next_interrupt_handler_id, cause,
                            address_or_id, next, argument, gp
                        );
                        cpu.m_gpr[2].m_lo = id;
                        cpu.m_gpr[2].m_hi = 0u;
                        if (trace)
                        {
                            std::printf(
                                "HLE Add%sHandler: id=%u cause=%u "
                                "handler=0x%08x next=0x%08x "
                                "arg=0x%08x gp=0x%08x\n",
                                intc ? "Intc" : "Dmac", id, cause,
                                address_or_id, next, argument, gp
                            );
                        }
                    }
                    else
                    {
                        const uint32_t remaining =
                            kernel_remove_interrupt_handler(
                                handlers, cause, address_or_id
                            );
                        cpu.m_gpr[2].m_lo = remaining;
                        cpu.m_gpr[2].m_hi = 0u;
                        if (trace)
                        {
                            std::printf(
                                "HLE Remove%sHandler: cause=%u "
                                "id=%u remaining=%u\n",
                                intc ? "Intc" : "Dmac",
                                cause, address_or_id, remaining
                            );
                        }
                    }
                    // Registration alone does not invoke guest code;
                    // DMAC callbacks execute on genuine masked hardware
                    // completions at the dispatcher safepoint.
                }

                else if ((service >= 0x14u && service <= 0x17u) ||
                         (service >= 0x1au && service <= 0x1du))
                {
                    // EE kernel enables/disables a single INTC cause or
                    // DMAC channel. Return 1 only when the mask changed.
                    // The hardware registers are write-one-to-toggle;
                    // use their actual MMIO handlers to keep COP0 IRQ
                    // levels and subsequent guest MMIO reads coherent.
                    const uint32_t cause =
                        static_cast<uint32_t>(cpu.m_gpr[4].m_lo);
                    const uint32_t action = service >= 0x1au
                        ? service - 0x06u : service;
                    const bool intc =
                        action == 0x14u || action == 0x15u;
                    const bool enabled =
                        action == 0x14u || action == 0x16u;
                    const uint32_t maximum = intc ? 32u : 10u;
                    if (cause >= maximum)
                    {
                        std::fprintf(
                            stderr,
                            "interrupt mask HLE refused invalid "
                            "%s cause/channel %u\n",
                            intc ? "INTC" : "DMAC", cause
                        );
                        break;
                    }

                    const uint32_t bit = intc
                        ? (1u << cause) : (1u << (16u + cause));
                    uint32_t old_mask = 0u;
                    bool valid = true;
                    if (intc)
                    {
                        old_mask = hardware.intc().mask();
                    }
                    else
                    {
                        ee::s_ee_u128 stat{};
                        valid = ee::c_ee_dmac::mmio_read(
                            &hardware.dmac(), ee::c_ee_dmac::k_stat_address,
                            4u, &stat
                        );
                        old_mask = static_cast<uint32_t>(stat.m_lo);
                    }
                    if (!valid)
                    {
                        std::fprintf(
                            stderr, "interrupt mask HLE could not read "
                            "guest hardware mask state\n"
                        );
                        break;
                    }

                    const bool previously_enabled =
                        (old_mask & bit) != 0u;
                    const bool changed = previously_enabled != enabled;
                    if (changed)
                    {
                        const ee::s_ee_u128 toggle{bit, 0u};
                        if (intc)
                        {
                            valid = ee::c_ee_intc::mmio_write(
                                &hardware.intc(),
                                ee::c_ee_intc::k_mask_address, 4u, &toggle
                            );
                        }
                        else
                        {
                            valid = ee::c_ee_dmac::mmio_write(
                                &hardware.dmac(),
                                ee::c_ee_dmac::k_stat_address, 4u, &toggle
                            );
                        }
                        if (!valid)
                        {
                            std::fprintf(
                                stderr, "interrupt mask HLE could not "
                                "update guest hardware mask\n"
                            );
                            break;
                        }
                    }
                    cpu.m_gpr[2].m_lo = changed ? 1u : 0u;
                    cpu.m_gpr[2].m_hi = 0u;
                    if (trace)
                    {
                        std::printf(
                            "HLE %s%s: cause=%u changed=%u "
                            "mask=0x%08x\n",
                            enabled ? "Enable" : "Disable",
                            intc ? "Intc" : "Dmac",
                            cause, changed ? 1u : 0u,
                            old_mask ^ (changed ? bit : 0u)
                        );
                    }
                }

                else if (service == 0x20u)
                {
                    // ps2-web-recomp's CreateThread reads a nine-word
                    // ee_thread_t parameter block and allocates the lowest
                    // unused thread ID in [1, 255]. Thread 1 is the
                    // already-running ELF main thread. Creation alone does
                    // not run a callback or schedule a new CPU context.
                    const uint32_t parameter_address =
                        static_cast<uint32_t>(cpu.m_gpr[4].m_lo);
                    if ((parameter_address & 3u) != 0u ||
                        !memory->contains(parameter_address, 36u))
                    {
                        std::fprintf(stderr,
                            "CreateThread HLE refused unbacked or "
                            "unaligned parameter block 0x%08x\n",
                            parameter_address
                        );
                        break;
                    }

                    uint32_t id = 0u;
                    for (uint32_t candidate = 1u; candidate < 256u;
                         ++candidate)
                    {
                        if (!threads[candidate].m_used)
                        {
                            id = candidate;
                            break;
                        }
                    }
                    if (id == 0u)
                    {
                        // The reference kernel returns -1 when all
                        // 255 EE thread identifiers are in use.
                        cpu.m_gpr[2].m_lo = static_cast<uint64_t>(-1ll);
                        cpu.m_gpr[2].m_hi = 0u;
                    }
                    else
                    {
                        s_kernel_thread& thread = threads[id];
                        thread.m_used = true;
                        thread.m_entry = memory->read32(parameter_address + 4u);
                        thread.m_stack = memory->read32(parameter_address + 8u);
                        thread.m_stack_size =
                            memory->read32(parameter_address + 12u);
                        thread.m_gp = memory->read32(parameter_address + 16u);
                        thread.m_priority =
                            memory->read32(parameter_address + 20u);
                        thread.m_initial_priority = thread.m_priority;
                        thread.m_attr = memory->read32(parameter_address + 28u);
                        thread.m_option =
                            memory->read32(parameter_address + 32u);
                        cpu.m_gpr[2].m_lo = id;
                        cpu.m_gpr[2].m_hi = 0u;
                        if (trace)
                        {
                            std::printf(
                                "HLE CreateThread: id=%u entry=0x%08x "
                                "stack=0x%08x size=0x%08x gp=0x%08x "
                                "priority=%u attr=0x%08x option=0x%08x "
                                "parameters=0x%08x dormant\n",
                                id, thread.m_entry, thread.m_stack,
                                thread.m_stack_size, thread.m_gp,
                                thread.m_priority, thread.m_attr,
                                thread.m_option, parameter_address
                            );
                        }
                    }
                }

                else if (service == 0x21u || service == 0x25u)
                {
                    // DeleteThread only frees dormant threads. TerminateThread
                    // makes an existing non-current active thread dormant.
                    const uint32_t id = static_cast<uint32_t>(cpu.m_gpr[4].m_lo);
                    const bool deleting = service == 0x21u;
                    s_kernel_thread* target = id > 0u && id < 256u &&
                        threads[id].m_used ? &threads[id] : nullptr;
                    const bool allowed = target != nullptr && (deleting
                        ? ee_kernel_thread::can_delete(target->m_status, id == current_thread_id)
                        : ee_kernel_thread::can_terminate(target->m_status, id == current_thread_id));
                    if (allowed)
                    {
                        if (deleting)
                        {
                            *target = {};
                        }
                        else
                        {
                            // Detach semaphore waiters before changing thread state.
                            // Never leave a dead thread in a live wait queue.
                            if (target->m_wait_type == 2u)
                            {
                                const uint32_t sema = target->m_wait_semaphore;
                                if (sema == 0u || sema > 64u ||
                                    !semaphores[sema - 1u].m_used)
                                {
                                    std::fprintf(stderr, "TerminateThread HLE refused invalid semaphore\n");
                                    break;
                                }
                                auto& waiters = semaphores[sema - 1u].m_waiters;
                                const auto waiter = std::find(waiters.begin(), waiters.end(), id);
                                if (waiter == waiters.end())
                                {
                                    std::fprintf(stderr, "TerminateThread HLE refused untracked waiter\n");
                                    break;
                                }
                                waiters.erase(waiter);
                            }
                            make_thread_dormant(*target);
                        }
                    }
                    cpu.m_gpr[2].m_lo = allowed ? id : static_cast<uint64_t>(-1ll);
                    cpu.m_gpr[2].m_hi = 0u;
                    if (trace)
                    {
                        std::printf("HLE %sThread: id=%u result=%d\n",
                            deleting ? "Delete" : "Terminate", id,
                            allowed ? static_cast<int>(id) : -1);
                    }
                }

                else if (service == 0x22u)
                {
                    const uint32_t id =
                        static_cast<uint32_t>(cpu.m_gpr[4].m_lo);
                    const uint32_t argument =
                        static_cast<uint32_t>(cpu.m_gpr[5].m_lo);
                    s_kernel_thread* thread = id > 1u && id < 256u &&
                        threads[id].m_used ? &threads[id] : nullptr;
                    if (thread == nullptr || thread->m_status != 16u)
                    {
                        cpu.m_gpr[2].m_lo = static_cast<uint64_t>(-1ll);
                        cpu.m_gpr[2].m_hi = 0u;
                    }
                    else
                    {
                        const uint64_t stack_end =
                            static_cast<uint64_t>(thread->m_stack) +
                            thread->m_stack_size;
                        if (thread->m_stack_size < 0x2a0u ||
                            stack_end > UINT32_MAX ||
                            !memory->contains(thread->m_stack,
                                              thread->m_stack_size) ||
                            !image.is_executable_address(thread->m_entry))
                        {
                            std::fprintf(stderr,
                                "StartThread HLE refused invalid entry/stack "
                                "id=%u pc=0x%08x stack=0x%08x size=%u\n",
                                id, thread->m_entry, thread->m_stack,
                                thread->m_stack_size);
                            break;
                        }
                        thread->m_context = {};
                        thread->m_self_suspend_pending = false;
                        thread->m_wait_semaphore = 0u;
                        thread->m_wait_type = 0u;
                        thread->m_wakeups = 0u;
                        thread->m_context.m_pc = thread->m_entry;
                        thread->m_context.m_cop0[12] =
                            cpu.m_cop0[12] & ~2u;
                        thread->m_context.m_gpr[4].m_lo = argument;
                        thread->m_context.m_gpr[28].m_lo =
                            static_cast<uint64_t>(static_cast<int64_t>(
                                static_cast<int32_t>(thread->m_gp)));
                        thread->m_context.m_gpr[29].m_lo =
                            static_cast<uint64_t>(stack_end - 0x2a0u);
                        thread->m_context.m_gpr[31].m_lo =
                            static_cast<uint64_t>(static_cast<int64_t>(
                                static_cast<int32_t>(k_thread_return_pc)));
                        thread->m_context.m_exit_reason =
                            static_cast<uint32_t>(
                                ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
                        thread->m_vu0_context = vu0;
                        thread->m_priority = thread->m_initial_priority;
                        thread->m_has_context = true;
                        thread->m_status = 2u;
                        thread->m_order = ++next_thread_order;
                        cpu.m_gpr[2].m_lo = id;
                        cpu.m_gpr[2].m_hi = 0u;
                        reschedule_requested = true;
                        if (trace)
                        {
                            std::printf("HLE StartThread: id=%u entry=0x%08x "
                                        "arg=0x%08x stack=0x%08x priority=%u\n",
                                        id, thread->m_entry, argument,
                                        static_cast<uint32_t>(
                                            thread->m_context.m_gpr[29].m_lo),
                                        thread->m_priority);
                        }
                    }
                }

                else if (service == 0x23u || service == 0x24u)
                {
                    if (current_thread_id == 0u ||
                        current_thread_id >= 256u ||
                        !threads[current_thread_id].m_used)
                    {
                        break;
                    }
                    s_kernel_thread& thread = threads[current_thread_id];
                    make_thread_dormant(thread);
                    if (service == 0x24u)
                    {
                        thread.m_used = false;
                    }
                    reschedule_requested = true;
                    if (trace)
                    {
                        std::printf("HLE %s: id=%u\n",
                                    service == 0x24u ? "ExitDeleteThread"
                                                     : "ExitThread",
                                    current_thread_id);
                    }
                }

                else if (service == 0x29u || service == 0x2au)
                {
                    // ChangeThreadPriority / iChangeThreadPriority.
                    // Reference kernel maps thread ID 0 to the currently
                    // running thread; returns the PREVIOUS priority and
                    // masks the requested priority into [0, 127].
                    // All guest threads other than ID 1 remain dormant
                    // until StartThread and scheduling are implemented.
                    const uint32_t requested_id =
                        static_cast<uint32_t>(cpu.m_gpr[4].m_lo);
                    const uint32_t new_priority =
                        static_cast<uint32_t>(cpu.m_gpr[5].m_lo) & 127u;
                    const uint32_t thread_id = requested_id == 0u
                        ? current_thread_id : requested_id;
                    const bool found = thread_id > 0u &&
                        thread_id < 256u && threads[thread_id].m_used;
                    if (!found)
                    {
                        cpu.m_gpr[2].m_lo =
                            static_cast<uint64_t>(-1ll);
                        cpu.m_gpr[2].m_hi = 0u;
                        if (trace)
                        {
                            std::printf(
                                "HLE %sChangeThreadPriority: id=%u "
                                "not found, result=-1\n",
                                service == 0x2au ? "i" : "",
                                requested_id
                            );
                        }
                    }
                    else
                    {
                        s_kernel_thread& thread = threads[thread_id];
                        const uint32_t previous_priority =
                            thread.m_priority;
                        thread.m_priority = new_priority;
                        reschedule_requested = true;
                        cpu.m_gpr[2].m_lo = previous_priority;
                        cpu.m_gpr[2].m_hi = 0u;
                        if (trace)
                        {
                            std::printf(
                                "HLE %sChangeThreadPriority: id=%u "
                                "old=%u new=%u (no scheduler)\n",
                                service == 0x2au ? "i" : "",
                                thread_id, previous_priority,
                                new_priority
                            );
                        }
                    }
                }

                else if (service == 0x2bu)
                {
                    // The EE kernel masks ready-queue priorities into
                    // [0, 127] and returns the selected priority. If the
                    // caller runs at that priority it yields its own queue
                    // position; otherwise rotate the oldest READY peer.
                    // This matches the reference PS2 EE kernel ordering.
                    const uint32_t priority =
                        static_cast<uint32_t>(cpu.m_gpr[4].m_lo) & 127u;
                    const s_kernel_thread& current =
                        threads[current_thread_id];
                    if (current.m_used && current.m_status == 1u &&
                        current.m_priority == priority)
                    {
                        threads[current_thread_id].m_order =
                            ++next_thread_order;
                    }
                    else
                    {
                        uint32_t oldest_id = 0u;
                        for (uint32_t id = 1u; id < 256u; ++id)
                        {
                            const s_kernel_thread& candidate = threads[id];
                            if (!candidate.m_used ||
                                candidate.m_status != 2u ||
                                candidate.m_priority != priority)
                            {
                                continue;
                            }
                            if (oldest_id == 0u ||
                                candidate.m_order < threads[oldest_id].m_order)
                            {
                                oldest_id = id;
                            }
                        }
                        if (oldest_id != 0u)
                        {
                            threads[oldest_id].m_order = ++next_thread_order;
                        }
                    }
                    cpu.m_gpr[2].m_lo = priority;
                    cpu.m_gpr[2].m_hi = 0u;
                    reschedule_requested = true;
                    if (trace)
                    {
                        std::printf(
                            "HLE RotateThreadReadyQueue: priority=%u "
                            "result=%u\n", priority, priority
                        );
                    }
                }

                else if (service == 0x2du || service == 0x2eu)
                {
                    // ReleaseWaitThread / iReleaseWaitThread. Only an
                    // existing guest wait can be released. A waiting
                    // semaphore thread must be removed from its queue so
                    // a subsequent SignalSema cannot wake it twice.
                    const uint32_t id =
                        static_cast<uint32_t>(cpu.m_gpr[4].m_lo);
                    s_kernel_thread* target = id > 0u && id < 256u &&
                        threads[id].m_used ? &threads[id] : nullptr;
                    const ee_kernel_thread::s_status_transition next =
                        target != nullptr
                        ? ee_kernel_thread::release_wait(target->m_status)
                        : ee_kernel_thread::s_status_transition{};
                    if (target == nullptr || !next.m_changed)
                    {
                        cpu.m_gpr[2].m_lo =
                            static_cast<uint64_t>(-1ll);
                        cpu.m_gpr[2].m_hi = 0u;
                        if (trace)
                        {
                            std::printf(
                                "HLE %sReleaseWaitThread: id=%u "
                                "not waiting, result=-1\n",
                                service == 0x2eu ? "i" : "", id);
                        }
                    }
                    else
                    {
                        const uint32_t previous = target->m_status;
                        if (target->m_wait_type == 2u)
                        {
                            const uint32_t semaphore_id =
                                target->m_wait_semaphore;
                            if (semaphore_id == 0u ||
                                semaphore_id > 64u ||
                                !semaphores[semaphore_id - 1u].m_used)
                            {
                                std::fprintf(stderr,
                                    "ReleaseWaitThread HLE refused "
                                    "missing guest semaphore id=%u\n",
                                    semaphore_id);
                                break;
                            }
                            auto& waiters =
                                semaphores[semaphore_id - 1u].m_waiters;
                            const auto waiter =
                                std::find(waiters.begin(), waiters.end(), id);
                            if (waiter == waiters.end())
                            {
                                std::fprintf(stderr,
                                    "ReleaseWaitThread HLE refused "
                                    "untracked waiter id=%u sema=%u\n",
                                    id, semaphore_id);
                                break;
                            }
                            waiters.erase(waiter);
                        }
                        else if (target->m_wait_type != 1u)
                        {
                            std::fprintf(stderr,
                                "ReleaseWaitThread HLE refused "
                                "unknown guest wait type id=%u type=%u\n",
                                id, target->m_wait_type);
                            break;
                        }
                        target->m_status = next.m_status;
                        target->m_wait_type = 0u;
                        target->m_wait_semaphore = 0u;
                        if (next.m_status == ee_kernel_thread::k_ready)
                        {
                            target->m_order = ++next_thread_order;
                        }
                        // The interrupt-safe service only makes the thread
                        // runnable; the ordinary form may switch immediately.
                        if (service == 0x2du)
                        {
                            reschedule_requested = true;
                        }
                        cpu.m_gpr[2].m_lo = id;
                        cpu.m_gpr[2].m_hi = 0u;
                        if (trace)
                        {
                            std::printf(
                                "HLE %sReleaseWaitThread: id=%u "
                                "old_status=%u new_status=%u result=%u\n",
                                service == 0x2eu ? "i" : "",
                                id, previous, next.m_status, id);
                        }
                    }
                }

                else if (service == 0x2fu)
                {
                    // GetThreadId has no arguments. Creation of a dormant
                    // thread does not switch the CPU's running thread.
                    // Match ps2-web-recomp's current_thread()/GetThreadId.
                    if ((current_thread_id == 0u && !irq_callback_active) ||
                        current_thread_id >= 256u ||
                        (current_thread_id != 0u && !threads[current_thread_id].m_used))
                    {
                        std::fprintf(stderr,
                            "GetThreadId HLE refused invalid running thread\n");
                        break;
                    }
                    cpu.m_gpr[2].m_lo = current_thread_id;
                    cpu.m_gpr[2].m_hi = 0u;
                    if (trace)
                    {
                        std::printf(
                            "HLE %sGetThreadId: id=%u\n",
                            signed_service == -0x2f ? "_i" : "",
                            current_thread_id
                        );
                    }
                }

                else if (service == 0x30u || service == 0x31u)
                {
                    const uint32_t requested_id =
                        static_cast<uint32_t>(cpu.m_gpr[4].m_lo);
                    const uint32_t address =
                        static_cast<uint32_t>(cpu.m_gpr[5].m_lo);
                    const uint32_t thread_id = requested_id == 0u
                        ? current_thread_id : requested_id;
                    const bool valid_id = thread_id > 0u &&
                        thread_id < 256u;
                    const s_kernel_thread* thread =
                        valid_id && threads[thread_id].m_used
                            ? &threads[thread_id] : nullptr;
                    if (address != 0u &&
                        ((address & 3u) != 0u ||
                         !memory->contains(address, 48u)))
                    {
                        std::fprintf(stderr,
                            "ReferThreadStatus HLE refused invalid output "
                            "address=0x%08x\n", address);
                        break;
                    }
                    if (thread != nullptr && address != 0u)
                    {
                        const uint32_t fields[] =
                        {
                            thread->m_status,
                            thread->m_entry, thread->m_stack,
                            thread->m_stack_size, thread->m_gp,
                            thread->m_initial_priority, thread->m_priority,
                            thread->m_attr, thread->m_option,
                            thread->m_wait_type, thread->m_wait_semaphore,
                            thread->m_wakeups
                        };
                        for (uint32_t index = 0u; index < 12u; ++index)
                        {
                            memory->write32(address + index * 4u, fields[index]);
                        }
                    }
                    const int32_t return_code = thread != nullptr
                        ? static_cast<int32_t>(thread->m_status)
                        : (valid_id ? 0 : -1);
                    cpu.m_gpr[2].m_lo = static_cast<uint64_t>(
                        static_cast<int64_t>(return_code));
                    cpu.m_gpr[2].m_hi = 0u;
                    if (trace)
                    {
                        std::printf(
                            "HLE %sReferThreadStatus: id=%u "
                            "address=0x%08x status=%d\n",
                            service == 0x31u ? "i" : "",
                            requested_id, address, return_code);
                    }
                }

                else if (service >= 0x32u && service <= 0x36u)
                {
                    // EE kernel sleep/wakeup support. The wakeup count is
                    // guest-owned and no wakeup is invented by the host.
                    // 0x32 SleepThread, 0x33 WakeupThread,
                    // 0x34 iWakeupThread, 0x35 CancelWakeupThread,
                    // 0x36 iCancelWakeupThread.
                    if (service == 0x32u)
                    {
                        if (callback_active || irq_callback_active ||
                            current_thread_id == 0u ||
                            current_thread_id >= 256u ||
                            !threads[current_thread_id].m_used ||
                            threads[current_thread_id].m_status != 1u)
                        {
                            std::fprintf(stderr,
                                "SleepThread HLE refused invalid running context\n");
                            break;
                        }
                        s_kernel_thread& thread = threads[current_thread_id];
                        cpu.m_gpr[2].m_lo = current_thread_id;
                        cpu.m_gpr[2].m_hi = 0u;
                        if (thread.m_wakeups != 0u)
                        {
                            --thread.m_wakeups;
                            if (trace)
                            {
                                std::printf(
                                    "HLE SleepThread: id=%u consumed wakeup "
                                    "remaining=%u\n",
                                    current_thread_id, thread.m_wakeups);
                            }
                        }
                        else
                        {
                            thread.m_status = 4u;
                            thread.m_wait_type = 1u;
                            thread.m_wait_semaphore = 0u;
                            reschedule_requested = true;
                            if (trace)
                            {
                                std::printf(
                                    "HLE SleepThread: id=%u status=4 "
                                    "waiting for a guest WakeupThread\n",
                                    current_thread_id);
                            }
                        }
                    }
                    else
                    {
                        const uint32_t requested_id =
                            static_cast<uint32_t>(cpu.m_gpr[4].m_lo);
                        const bool found = requested_id > 0u &&
                            requested_id < 256u &&
                            threads[requested_id].m_used;
                        if (!found)
                        {
                            cpu.m_gpr[2].m_lo =
                                static_cast<uint64_t>(-1ll);
                            cpu.m_gpr[2].m_hi = 0u;
                            if (trace)
                            {
                                std::printf(
                                    "HLE %s: id=%u not found, result=-1\n",
                                    service == 0x33u ? "WakeupThread" :
                                    service == 0x34u ? "iWakeupThread" :
                                    service == 0x35u ? "CancelWakeupThread" :
                                                       "iCancelWakeupThread",
                                    requested_id);
                            }
                        }
                        else if ((service == 0x33u || service == 0x34u) &&
                            !ee_kernel_thread::can_wakeup(
                                threads[requested_id].m_status,
                                requested_id == current_thread_id && !irq_callback_active))
                        {
                            // A normal thread cannot wake itself or a dormant
                            // target. Preserve interrupt-context wakeup semantics.
                            cpu.m_gpr[2].m_lo = static_cast<uint64_t>(-1ll);
                            cpu.m_gpr[2].m_hi = 0u;
                            if (trace)
                            {
                                std::printf("HLE %s: id=%u invalid status/context, result=-1\n",
                                    service == 0x33u ? "WakeupThread" : "iWakeupThread",
                                    requested_id);
                            }
                        }
                        else
                        {
                            s_kernel_thread& thread = threads[requested_id];
                            const uint32_t previous_wakeups = thread.m_wakeups;
                            bool released = false;
                            if (service == 0x35u || service == 0x36u)
                            {
                                thread.m_wakeups = 0u;
                                cpu.m_gpr[2].m_lo = previous_wakeups;
                            }
                            else
                            {
                                cpu.m_gpr[2].m_lo = requested_id;
                                if (thread.m_wait_type == 1u &&
                                    (thread.m_status == 4u ||
                                     thread.m_status == 12u))
                                {
                                    thread.m_wait_type = 0u;
                                    thread.m_status =
                                        thread.m_status == 12u ? 8u : 2u;
                                    thread.m_context.m_gpr[2].m_lo =
                                        requested_id;
                                    thread.m_context.m_gpr[2].m_hi = 0u;
                                    if (thread.m_status == 2u)
                                    {
                                        thread.m_order = ++next_thread_order;
                                        if (service == 0x33u)
                                        {
                                            reschedule_requested = true;
                                        }
                                    }
                                    released = true;
                                }
                                else if (thread.m_wakeups !=
                                         std::numeric_limits<uint32_t>::max())
                                {
                                    ++thread.m_wakeups;
                                }
                            }
                            cpu.m_gpr[2].m_hi = 0u;
                            if (trace)
                            {
                                std::printf(
                                    "HLE %s: id=%u status=%u wakeups=%u "
                                    "released=%u result=%u\n",
                                    service == 0x33u ? "WakeupThread" :
                                    service == 0x34u ? "iWakeupThread" :
                                    service == 0x35u ? "CancelWakeupThread" :
                                                       "iCancelWakeupThread",
                                    requested_id, thread.m_status,
                                    thread.m_wakeups, released ? 1u : 0u,
                                    static_cast<uint32_t>(cpu.m_gpr[2].m_lo));
                            }
                        }
                    }
                }

                else if (service == 0x37u || service == 0x39u)
                {
                    // SuspendThread (0x37) and ResumeThread (0x39).
                    // Read the actual EE kernel thread ID from A0; zero
                    // addresses the currently executing guest thread.
                    // An unknown ID returns -1 and changes no state.
                    const uint32_t requested_id =
                        static_cast<uint32_t>(cpu.m_gpr[4].m_lo);
                    const uint32_t thread_id = requested_id == 0u
                        ? current_thread_id : requested_id;
                    const bool found = thread_id > 0u &&
                        thread_id < 256u && threads[thread_id].m_used;
                    if (!found)
                    {
                        cpu.m_gpr[2].m_lo =
                            static_cast<uint64_t>(-1ll);
                        cpu.m_gpr[2].m_hi = 0u;
                        if (trace)
                        {
                            std::printf(
                                "HLE %sThread: id=%u not found, result=-1\n",
                                service == 0x37u ? "Suspend" : "Resume",
                                requested_id);
                        }
                    }
                    else
                    {
                        s_kernel_thread& thread = threads[thread_id];
                        const uint32_t old_status = thread.m_status;
                        const ee_kernel_thread::s_status_transition next =
                            service == 0x37u
                            ? ee_kernel_thread::suspend(old_status)
                            : ee_kernel_thread::resume(
                                old_status, thread_id == current_thread_id);
                        thread.m_status = next.m_status;
                        if (next.m_changed && thread_id == current_thread_id)
                        {
                            thread.m_self_suspend_pending =
                                service == 0x37u &&
                                old_status == ee_kernel_thread::k_run &&
                                !irq_callback_active;
                        }
                        cpu.m_gpr[2].m_lo = next.m_changed
                            ? thread_id : static_cast<uint64_t>(-1ll);
                        cpu.m_gpr[2].m_hi = 0u;
                        if (service == 0x39u && next.m_changed)
                        {
                            // ResumeThread may make a suspended guest
                            // runnable. SuspendThread does NOT force a
                            // reschedule on the original PS2 EE kernel.
                            thread.m_order = ++next_thread_order;
                            reschedule_requested = true;
                        }
                        if (trace)
                        {
                            std::printf(
                                "HLE %sThread: id=%u old_status=%u "
                                "new_status=%u changed=%u result=%d\n",
                                service == 0x37u ? "Suspend" : "Resume",
                                thread_id, old_status, thread.m_status,
                                next.m_changed ? 1u : 0u,
                                next.m_changed ? static_cast<int>(thread_id) : -1);
                        }
                    }
                }

                else if (service == 0x3cu)
                {
                    const uint32_t stack_arg =
                        static_cast<uint32_t>(cpu.m_gpr[5].m_lo);
                    const uint32_t requested_size =
                        static_cast<uint32_t>(cpu.m_gpr[6].m_lo);
                    uint32_t new_stack = 0;
                    if (requested_size < 0x100u ||
                        requested_size > k_ee_ram_size)
                    {
                        std::fprintf(stderr, "SetupThread HLE refused stack size\n");
                        break;
                    }
                    if (stack_arg == 0xffffffffu)
                    {
                        new_stack = stack;
                    }
                    else
                    {
                        const uint64_t end =
                            static_cast<uint64_t>(stack_arg) + requested_size;
                        if (end > k_ee_ram_size)
                        {
                            std::fprintf(stderr, "SetupThread HLE refused stack region\n");
                            break;
                        }
                        new_stack = static_cast<uint32_t>(end) & ~0x0fu;
                    }
                    if (new_stack < requested_size ||
                        !memory->contains(new_stack - requested_size, requested_size))
                    {
                        std::fprintf(stderr, "SetupThread HLE refused unbacked stack\n");
                        break;
                    }
                    startup_stack_top = new_stack;
                    startup_stack_size = requested_size;
                    startup_thread_set = true;
                    cpu.m_gpr[2].m_lo = new_stack;
                    if (trace)
                    {
                        std::printf(
                            "HLE SetupThread: gp=0x%08x stack=0x%08x "
                            "size=0x%08x args=0x%08x\n",
                            static_cast<uint32_t>(cpu.m_gpr[4].m_lo),
                            new_stack, requested_size,
                            static_cast<uint32_t>(cpu.m_gpr[7].m_lo)
                        );
                    }
                }
                else if (service == 0x3du)
                {
                    const uint32_t request_base =
                        static_cast<uint32_t>(cpu.m_gpr[4].m_lo);
                    const uint32_t request_size =
                        static_cast<uint32_t>(cpu.m_gpr[5].m_lo);
                    const uint64_t aligned_base =
                        (static_cast<uint64_t>(request_base) + 15u) & ~15ull;
                    const uint64_t limit = startup_thread_set
                        ? static_cast<uint64_t>(startup_stack_top - startup_stack_size)
                        : k_ee_ram_size;
                    const uint64_t heap_end = request_size == 0xffffffffu
                        ? limit : aligned_base + request_size;

                    if (!startup_thread_set ||
                        aligned_base > limit ||
                        heap_end < aligned_base ||
                        heap_end > limit)
                    {
                        std::fprintf(stderr, "SetupHeap HLE refused overlapping heap/stack\n");
                        break;
                    }
                    // SetupHeap records the process-wide EE heap boundary.
                    // PS2SDK libc shares its allocator between EE threads;
                    // OpenPS2BIOS likewise stores one heap_end in ee/kernel.c.
                    // EndOfHeap must not depend on the caller's thread or A0.
                    // This does not implement malloc/sbrk.
                    if (current_thread_id == 0u ||
                        current_thread_id >= 256u ||
                        !threads[current_thread_id].m_used)
                    {
                        std::fprintf(stderr,
                            "SetupHeap HLE refused invalid running thread\n");
                        break;
                    }
                    const uint32_t heap_end32 = static_cast<uint32_t>(heap_end);
                    startup_heap_end = heap_end32;
                    cpu.m_gpr[2].m_lo = heap_end32;
                    cpu.m_gpr[2].m_hi = 0u;
                    if (trace)
                    {
                        std::printf(
                            "HLE SetupHeap: base=0x%08x end=0x%08x "
                            "size=0x%08x\n",
                            static_cast<uint32_t>(aligned_base),
                            static_cast<uint32_t>(heap_end), request_size
                        );
                    }
                }

                else if (service == 0x3eu)
                {
                    if (current_thread_id == 0u ||
                        current_thread_id >= 256u ||
                        !threads[current_thread_id].m_used)
                    {
                        std::fprintf(stderr,
                            "EndOfHeap HLE refused invalid running thread\n");
                        break;
                    }
                    const uint32_t heap_end = startup_heap_end;
                    cpu.m_gpr[2].m_lo = heap_end;
                    cpu.m_gpr[2].m_hi = 0u;
                    if (trace)
                    {
                        std::printf(
                            "HLE EndOfHeap: id=%u end=0x%08x\n",
                            current_thread_id, heap_end
                        );
                    }
                }

                else if (service == 0x40u)
                {
                    // PS2SDK ee_sema_t is six consecutive 32-bit fields:
                    // count, max_count, init_count, wait_threads, attr,
                    // option. Initial allocation uses max_count/init_count;
                    // count and wait_threads are runtime output fields.
                    const uint32_t parameter_address =
                        static_cast<uint32_t>(cpu.m_gpr[4].m_lo);
                    if ((parameter_address & 3u) != 0 ||
                        !memory->contains(parameter_address, 24u))
                    {
                        std::fprintf(stderr, "CreateSema HLE refused unbacked parameter block\n");
                        break;
                    }
                    const uint32_t max_count =
                        memory->read32(parameter_address + 4u);
                    const uint32_t init_count =
                        memory->read32(parameter_address + 8u);
                    const uint32_t attr =
                        memory->read32(parameter_address + 16u);
                    const uint32_t option =
                        memory->read32(parameter_address + 20u);
                    if (max_count == 0u || max_count > 0x7fffffffu ||
                        init_count > max_count)
                    {
                        std::fprintf(stderr,
                            "CreateSema HLE refused invalid count init=%u max=%u\n",
                            init_count, max_count
                        );
                        break;
                    }
                    uint32_t id = 0u;
                    for (uint32_t candidate = 1u; candidate <= 64u; ++candidate)
                    {
                        if (!semaphores[candidate - 1u].m_used)
                        {
                            id = candidate;
                            break;
                        }
                    }
                    if (id == 0u)
                    {
                        std::fprintf(stderr, "CreateSema HLE exhausted semaphore slots\n");
                        break;
                    }
                    s_kernel_semaphore& semaphore = semaphores[id - 1u];
                    semaphore.m_used = true;
                    semaphore.m_count = init_count;
                    semaphore.m_max_count = max_count;
                    semaphore.m_initial_count = init_count;
                    semaphore.m_attr = attr;
                    semaphore.m_option = option;
                    cpu.m_gpr[2].m_lo = id;
                    if (trace)
                    {
                        std::printf(
                            "HLE CreateSema: id=%u init=%u max=%u attr=0x%08x "
                            "option=0x%08x parameters=0x%08x\n",
                            id, init_count, max_count, attr, option,
                            parameter_address
                        );
                    }
                }

                else if (service >= 0x41u && service <= 0x48u)
                {
                    const uint32_t semaphore_id =
                        static_cast<uint32_t>(cpu.m_gpr[4].m_lo);
                    if (semaphore_id == 0u || semaphore_id > 64u ||
                        !semaphores[semaphore_id - 1u].m_used)
                    {
                        // Unallocated/deleted IDs are normal kernel errors,
                        // not a reason to fabricate semaphore state.
                        cpu.m_gpr[2].m_lo = static_cast<uint64_t>(-1ll);
                        cpu.m_gpr[2].m_hi = 0u;
                        if (trace)
                        {
                            std::printf(
                                "HLE semaphore service=0x%02x id=%u "
                                "not found, result=-1\n",
                                service, semaphore_id
                            );
                        }
                    }
                    else
                    {
                        s_kernel_semaphore& semaphore =
                            semaphores[semaphore_id - 1u];
                        const uint32_t old_count = semaphore.m_count;
                        const bool wait_blocked =
                            service == 0x44u && old_count == 0u;
                        if (wait_blocked)
                        {
                            bool other_ready = false;
                            for (uint32_t id = 1u; id < 256u; ++id)
                            {
                                if (id != current_thread_id &&
                                    threads[id].m_used &&
                                    threads[id].m_status == 2u)
                                {
                                    other_ready = true;
                                    break;
                                }
                            }
                            if (!other_ready || callback_active ||
                                irq_callback_active)
                            {
                                // A real EE SIFRPC BIND can block in
                                // WaitSema before its guest IOP server
                                // registers. Unlike ROM0 DMA, HLE-first
                                // SIF1 is already committed; give only
                                // already-scheduled guest R3000 work a
                                // bounded turn so it can register that
                                // server and produce a genuine SIF0 Rend.
                                const bool guest_rpc_wait =
                                    !other_ready && !callback_active &&
                                    !irq_callback_active &&
                                    iop_rom0_path == nullptr &&
                                    iop_kernel != nullptr &&
                                    iop_rpc != nullptr &&
                                    (iop_rpc->has_waiting_bind_guest_work() ||
                                     ((iop_rpc->pending_guest_calls() != 0u ||
                                       iop_rpc->pending_module_starts() != 0u) &&
                                      iop_kernel->has_guest_work()));
                                if (guest_rpc_wait)
                                {
                                    if (trace &&
                                        iop_semaphore_wait_turns == 0u)
                                    {
                                        dump_iop_rpc_wait_threads(
                                            *iop_kernel, semaphore_id, 0u);
                                    }
                                    if (iop_semaphore_wait_turns >=
                                        sif_poll_limit)
                                    {
                                        std::fprintf(stderr,
                                            "WaitSema HLE pending RPC "
                                            "exhausted guest IOP turns: "
                                            "id=%u turns=%u binds=%zu "
                                            "replies=%zu iop_pc=0x%08x\n",
                                            semaphore_id,
                                            iop_semaphore_wait_turns,
                                            iop_rpc->pending_binds(),
                                            iop_rpc->pending_replies(),
                                            iop_kernel->cpu().state().m_pc);
                                        break;
                                    }
                                    ++iop_semaphore_wait_turns;
                                    const size_t binds_before =
                                        iop_rpc->pending_binds();
                                    const iop::s_kernel_run_result progressed =
                                        iop_kernel->run(iop_quantum, 128u);
                                    if (progressed.m_stop !=
                                            iop::e_kernel_stop::budget_exhausted &&
                                        progressed.m_stop !=
                                            iop::e_kernel_stop::idle)
                                    {
                                        std::fprintf(stderr,
                                            "WaitSema HLE guest IOP stopped: "
                                            "id=%u reason=%u thread=%u "
                                            "pc=0x%08x opcode=0x%08x "
                                            "executed=%llu\n",
                                            semaphore_id,
                                            static_cast<uint32_t>(
                                                progressed.m_stop),
                                            progressed.m_thread_id,
                                            progressed.m_guest_pc,
                                            progressed.m_opcode,
                                            static_cast<unsigned long long>(
                                                progressed.m_executed));
                                        if (progressed.m_stop ==
                                            iop::e_kernel_stop::memory_fault)
                                        {
                                            const iop::s_memory_fault& fault =
                                                iop_kernel->cpu().last_memory_fault();
                                            const iop::s_cpu_state& guest =
                                                iop_kernel->cpu().state();
                                            std::fprintf(stderr,
                                                "WaitSema HLE IOP memory fault: "
                                                "address=0x%08x width=%u "
                                                "write=%u valid=%u "
                                                "ra=0x%08x gp=0x%08x "
                                                "sp=0x%08x\n",
                                                fault.m_address, fault.m_width,
                                                fault.m_write ? 1u : 0u,
                                                fault.m_valid ? 1u : 0u,
                                                guest.m_gpr[31],
                                                guest.m_gpr[28],
                                                guest.m_gpr[29]);
                                        }
                                        dump_iop_rpc_wait_threads(
                                            *iop_kernel, semaphore_id,
                                            iop_semaphore_wait_turns);
                                        break;
                                    }
                                    // Delivery still requires guest
                                    // registration, the armed EE SIF0
                                    // receiver and actual DMAC IRQ. Pumping
                                    // does not manufacture a response.
                                    (void)iop_rpc->pump();
                                    const size_t binds_after =
                                        iop_rpc->pending_binds();
                                    if (trace &&
                                        (iop_semaphore_wait_turns == 1u ||
                                         binds_before != binds_after ||
                                         (hardware.dmac().pending() &
                                             (1u << 5u)) != 0u))
                                    {
                                        std::printf(
                                            "WaitSema HLE guest RPC turn: "
                                            "id=%u turn=%u executed=%llu "
                                            "binds=%zu replies=%zu "
                                            "dma5_pending=%u\n",
                                            semaphore_id,
                                            iop_semaphore_wait_turns,
                                            static_cast<unsigned long long>(
                                                progressed.m_executed),
                                            binds_after,
                                            iop_rpc->pending_replies(),
                                            (hardware.dmac().pending() &
                                                (1u << 5u)) != 0u ? 1u : 0u);
                                    }
                                    // A handful of read-only snapshots
                                    // distinguish guest thread starvation,
                                    // a runaway NOP path and genuine server
                                    // initialization without a 2048-turn log.
                                    if (trace &&
                                        (iop_semaphore_wait_turns == 1u ||
                                         iop_semaphore_wait_turns == 64u ||
                                         iop_semaphore_wait_turns == 512u ||
                                         iop_semaphore_wait_turns ==
                                             sif_poll_limit))
                                    {
                                        dump_iop_rpc_wait_threads(
                                            *iop_kernel, semaphore_id,
                                            iop_semaphore_wait_turns);
                                    }
                                    // The normal top-of-loop interrupt
                                    // delivery executes the original guest
                                    // callback. Only its iSignalSema may
                                    // wake this trapped WaitSema.
                                    continue;
                                }
                                if (!other_ready && !callback_active &&
                                    !irq_callback_active &&
                                    iop_rom0_path != nullptr &&
                                    iop_kernel != nullptr &&
                                    iop_sif0_transport != nullptr)
                                {
                                    if (iop_semaphore_wait_turns >=
                                        sif_poll_limit)
                                    {
                                        uint32_t op0 = 0u;
                                        uint32_t op1 = 0u;
                                        uint32_t sio2_ctrl = 0u;
                                        uint32_t ee5_chcr = 0u;
                                        uint32_t ee5_madr = 0u;
                                        uint32_t ee5_qwc = 0u;
                                        uint32_t iop9_chcr = 0u;
                                        uint32_t iop9_tadr = 0u;
                                        const iop::s_cpu_state& iop_state =
                                            iop_kernel->cpu().state();
                                        (void)iop_kernel->cpu().read32(
                                            iop_state.m_pc, &op0);
                                        (void)iop_kernel->cpu().read32(
                                            iop_state.m_pc + 4u, &op1);
                                        (void)iop_kernel->hardware().read(
                                            iop::c_hw::k_sio2_ctrl, 4u,
                                            &sio2_ctrl);
                                        ee::s_ee_u128 dma_word{};
                                        (void)ee::c_ee_dmac::mmio_read(
                                            &hardware.dmac(),
                                            ee::c_ee_dmac::k_sif0_base, 4u,
                                            &dma_word);
                                        ee5_chcr = static_cast<uint32_t>(
                                            dma_word.m_lo);
                                        (void)ee::c_ee_dmac::mmio_read(
                                            &hardware.dmac(),
                                            ee::c_ee_dmac::k_sif0_base + 0x10u,
                                            4u, &dma_word);
                                        ee5_madr = static_cast<uint32_t>(
                                            dma_word.m_lo);
                                        (void)ee::c_ee_dmac::mmio_read(
                                            &hardware.dmac(),
                                            ee::c_ee_dmac::k_sif0_base + 0x20u,
                                            4u, &dma_word);
                                        ee5_qwc = static_cast<uint32_t>(
                                            dma_word.m_lo);
                                        (void)iop_kernel->hardware().read(
                                            iop::c_hw::k_sif0_dma_base + 8u,
                                            4u, &iop9_chcr);
                                        (void)iop_kernel->hardware().read(
                                            iop::c_hw::k_sif0_dma_base + 12u,
                                            4u, &iop9_tadr);
                                        std::fprintf(stderr,
                                            "WaitSema HLE exhausted IOP "
                                            "completion turns: id=%u "
                                            "count=0 turns=%u iop_pc=%08x "
                                            "opcodes=%08x/%08x v0=%08x "
                                            "a0=%08x a1=%08x t4=%08x "
                                            "sio2_ctrl=%08x ee5=%08x/"
                                            "%08x/%08x iop9=%08x/%08x\n",
                                            semaphore_id,
                                            iop_semaphore_wait_turns,
                                            iop_state.m_pc, op0, op1,
                                            iop_state.m_gpr[2],
                                            iop_state.m_gpr[4],
                                            iop_state.m_gpr[5],
                                            iop_state.m_gpr[12], sio2_ctrl,
                                            ee5_chcr, ee5_madr, ee5_qwc,
                                            iop9_chcr, iop9_tadr);
                                        break;
                                    }
                                    ++iop_semaphore_wait_turns;
                                    const iop::s_kernel_run_result progressed =
                                        iop_kernel->run(iop_quantum, 128u);
                                    if (progressed.m_stop !=
                                            iop::e_kernel_stop::budget_exhausted &&
                                        progressed.m_stop !=
                                            iop::e_kernel_stop::idle)
                                    {
                                        const iop::s_memory_fault& fault =
                                            iop_kernel->cpu()
                                                .last_memory_fault();
                                        std::fprintf(stderr,
                                            "WaitSema HLE IOP producer "
                                            "stopped: reason=%u pc=0x%08x "
                                            "opcode=0x%08x v0=0x%08x "
                                            "a0=0x%08x fault=%u/%u/%u\n",
                                            static_cast<uint32_t>(
                                                progressed.m_stop),
                                            progressed.m_guest_pc,
                                            progressed.m_opcode,
                                            iop_kernel->cpu().state().m_gpr[2],
                                            iop_kernel->cpu().state().m_gpr[4],
                                            fault.m_valid ? 1u : 0u,
                                            fault.m_address,
                                            fault.m_write ? fault.m_width : 0u);
                                        break;
                                    }
                                    const uint64_t sif0_delivered =
                                        iop_sif0_transport->delivered_bytes();
                                    if (trace &&
                                        (iop_semaphore_wait_turns == 1u ||
                                         sif0_delivered !=
                                            iop_semaphore_wait_last_sif0))
                                    {
                                        std::printf(
                                            "WaitSema HLE advanced IOP "
                                            "producer: id=%u turn=%u "
                                            "instructions=%llu "
                                            "sif0_delivered=%llu\n",
                                            semaphore_id,
                                            iop_semaphore_wait_turns,
                                            static_cast<unsigned long long>(
                                                progressed.m_executed),
                                            static_cast<unsigned long long>(
                                                sif0_delivered));
                                    }
                                    iop_semaphore_wait_last_sif0 =
                                        sif0_delivered;
                                    // Preserve the trapped syscall. The
                                    // normal top-of-loop DMA dispatch runs
                                    // any real completion callback; its
                                    // iSignalSema then makes this exact
                                    // WaitSema succeed on re-entry.
                                    continue;
                                }
                                // No runnable peer does not imply a
                                // deadlock: a registered, enabled device
                                // interrupt may signal the semaphore
                                // while the kernel is idle. Preserve the
                                // special IOP RPC waits above, and use
                                // the ordinary bounded idle scheduler
                                // only when a modeled producer exists.
                                const uint32_t status_word = cpu.m_cop0[12];
                                const bool irq_enabled =
                                    (status_word & (ee::STATUS_IE | ee::STATUS_EIE |
                                        ee::STATUS_ERL)) ==
                                        (ee::STATUS_IE | ee::STATUS_EIE);
                                const uint32_t intc_mask = hardware.intc().mask();
                                const uint32_t intc_pending = hardware.intc().pending();
                                const bool scheduled_timer =
                                    host_timers.next_event() !=
                                        std::numeric_limits<uint64_t>::max();
                                const bool intc_producer =
                                    (status_word & (1u << (ee::CAUSE_IP_SHIFT + ee::INT_INTC))) != 0u &&
                                    std::any_of(intc_handlers.begin(), intc_handlers.end(),
                                        [&](const s_kernel_interrupt_handler& handler)
                                        {
                                            if (handler.m_cause >= 32u)
                                            {
                                                return false;
                                            }
                                            const uint32_t bit = 1u << handler.m_cause;
                                            return (intc_mask & bit) != 0u &&
                                                ((intc_pending & bit) != 0u ||
                                                 (handler.m_cause ==
                                                    ee::c_ee_scanout_clock::k_intc_vblank_start &&
                                                  hardware.scanout().enabled()) ||
                                                 (handler.m_cause >= 9u &&
                                                  handler.m_cause <= 12u && scheduled_timer));
                                        });
                                const bool dmac_producer =
                                    std::any_of(dmac_handlers.begin(), dmac_handlers.end(),
                                        [&](const s_kernel_interrupt_handler& handler)
                                        {
                                            return handler.m_cause < 10u &&
                                                (hardware.dmac().pending() &
                                                    (1u << handler.m_cause)) != 0u;
                                        });
                                const bool idle_wait = !other_ready &&
                                    !callback_active && !irq_callback_active &&
                                    current_thread_id != 0u && irq_enabled &&
                                    runtime.m_ee_cycle_scheduler.source() ==
                                        ee::e_ee_cycle_source::REFERENCE_COMPATIBILITY &&
                                    (intc_producer || dmac_producer);
                                if (!idle_wait)
                                {
                                    std::fprintf(stderr,
                                        "WaitSema HLE refused blocking wait: "
                                        "id=%u current_thread=%u count=0 "
                                        "(no runnable peer or modeled IRQ producer)\n",
                                        semaphore_id, current_thread_id);
                                    break;
                                }
                                if (trace)
                                {
                                    std::printf("HLE WaitSema idle: id=%u thread=%u\n",
                                                semaphore_id, current_thread_id);
                                }
                            }
                            semaphore.m_waiters.push_back(current_thread_id);
                            threads[current_thread_id].m_status = 4u;
                            threads[current_thread_id].m_wait_semaphore =
                                semaphore_id;
                            threads[current_thread_id].m_wait_type = 2u;
                            reschedule_requested = true;
                        }

                        if (service == 0x41u)
                        {
                            if (!semaphore.m_waiters.empty())
                            {
                                std::fprintf(stderr,
                                    "DeleteSema HLE refused active waiters\n");
                                break;
                            }
                            semaphore = {};
                        }
                        else if (service == 0x42u || service == 0x43u)
                        {
                            if (semaphore.m_count == semaphore.m_max_count)
                            {
                                cpu.m_gpr[2].m_lo =
                                    static_cast<uint64_t>(-1ll);
                                cpu.m_gpr[2].m_hi = 0u;
                                if (trace)
                                {
                                    std::printf(
                                        "HLE %sSignalSema: id=%u count=%u "
                                        "at maximum=%u result=-1\n",
                                        service == 0x43u ? "i" : "",
                                        semaphore_id, old_count,
                                        semaphore.m_max_count
                                    );
                                }
                                // No state changes on count overflow.
                                cpu.m_cop0[12] &= ~2u;
                                cpu.m_pc = epc + 4u;
                                cpu.m_exit_reason =
                                    static_cast<uint32_t>(
                                        ee::e_ee_exit_reason::EE_EXIT_DISPATCH
                                    );
                                continue;
                            }
                            if (semaphore.m_waiters.empty())
                            {
                                ++semaphore.m_count;
                                // An ordinary signal is a scheduling boundary
                                // for a still-executing self-suspended owner.
                                if (service == 0x42u &&
                                    threads[current_thread_id].m_self_suspend_pending)
                                {
                                    reschedule_requested = true;
                                }
                            }
                            else
                            {
                                const uint32_t wake_id =
                                    semaphore.m_waiters.front();
                                semaphore.m_waiters.erase(
                                    semaphore.m_waiters.begin());
                                if (wake_id == 0u || wake_id >= 256u ||
                                    !threads[wake_id].m_used ||
                                    threads[wake_id].m_status != 4u)
                                {
                                    std::fprintf(stderr,
                                        "SignalSema HLE invalid guest waiter\n");
                                    break;
                                }
                                s_kernel_thread& waiter = threads[wake_id];
                                waiter.m_status = 2u;
                                waiter.m_wait_semaphore = 0u;
                                waiter.m_wait_type = 0u;
                                waiter.m_order = ++next_thread_order;
                                waiter.m_context.m_gpr[2].m_lo = semaphore_id;
                                reschedule_requested = true;
                            }
                        }
                        else if (service == 0x44u ||
                                 service == 0x45u ||
                                 service == 0x46u)
                        {
                            if (semaphore.m_count > 0u)
                            {
                                --semaphore.m_count;
                                iop_semaphore_wait_turns = 0u;
                                iop_semaphore_wait_last_sif0 = 0u;
                            }
                            else if (!wait_blocked)
                            {
                                // PollSema and iPollSema do not block.
                                cpu.m_gpr[2].m_lo =
                                    static_cast<uint64_t>(-1ll);
                                cpu.m_gpr[2].m_hi = 0u;
                                if (trace)
                                {
                                    std::printf(
                                        "HLE %sPollSema: id=%u count=0 "
                                        "result=-1\n",
                                        service == 0x46u ? "i" : "",
                                        semaphore_id
                                    );
                                }
                                cpu.m_cop0[12] &= ~2u;
                                cpu.m_pc = epc + 4u;
                                cpu.m_exit_reason =
                                    static_cast<uint32_t>(
                                        ee::e_ee_exit_reason::EE_EXIT_DISPATCH
                                    );
                                continue;
                            }
                        }
                        else
                        {
                            // ReferSemaStatus/iReferSemaStatus write the
                            // ee_sema_t fields in guest address space.
                            const uint32_t status_address =
                                static_cast<uint32_t>(cpu.m_gpr[5].m_lo);
                            if ((status_address & 3u) != 0u ||
                                !memory->contains(status_address, 24u))
                            {
                                std::fprintf(
                                    stderr,
                                    "ReferSemaStatus HLE refused unbacked "
                                    "or unaligned pointer 0x%08x\n",
                                    status_address
                                );
                                break;
                            }
                            memory->write32(status_address + 0u,
                                            semaphore.m_count);
                            memory->write32(status_address + 4u,
                                            semaphore.m_max_count);
                            memory->write32(status_address + 8u,
                                            semaphore.m_initial_count);
                            memory->write32(status_address + 12u,
                                             static_cast<uint32_t>(
                                                 semaphore.m_waiters.size()));
                            memory->write32(status_address + 16u,
                                            semaphore.m_attr);
                            memory->write32(status_address + 20u,
                                            semaphore.m_option);
                        }

                        cpu.m_gpr[2].m_lo = semaphore_id;
                        cpu.m_gpr[2].m_hi = 0u;
                        if (trace)
                        {
                            const char* name =
                                service == 0x41u ? "DeleteSema" :
                                service == 0x42u ? "SignalSema" :
                                service == 0x43u ? "iSignalSema" :
                                service == 0x44u ? "WaitSema" :
                                service == 0x45u ? "PollSema" :
                                service == 0x46u ? "iPollSema" :
                                service == 0x47u ? "ReferSemaStatus" :
                                                   "iReferSemaStatus";
                            std::printf(
                                "HLE %s: id=%u old_count=%u "
                                "new_count=%u result=%u\n",
                                name, semaphore_id, old_count,
                                service == 0x41u ? 0u : semaphore.m_count,
                                semaphore_id
                            );
                        }
                    }
                }

                else if (service == 0x4au || service == 0x4bu)
                {
                    // SetOsdConfigParam/GetOsdConfigParam share one
                    // guest-visible 32-bit OSD configuration word.
                    // Both services take a pointer in A0, not the word
                    // itself. Invalid pointers must not mutate state.
                    const uint32_t parameter_address =
                        static_cast<uint32_t>(cpu.m_gpr[4].m_lo);
                    if ((parameter_address & 3u) != 0u ||
                        !memory->contains(parameter_address, sizeof(uint32_t)))
                    {
                        std::fprintf(stderr,
                            "%sOsdConfigParam HLE refused unbacked or "
                            "unaligned pointer 0x%08x\\n",
                            service == 0x4au ? "Set" : "Get",
                            parameter_address
                        );
                        break;
                    }

                    if (service == 0x4au)
                    {
                        osd_config = memory->read32(parameter_address);
                    }
                    else
                    {
                        memory->write32(parameter_address, osd_config);
                    }
                    cpu.m_gpr[2].m_lo = 0u;
                    cpu.m_gpr[2].m_hi = 0u;
                    if (trace)
                    {
                        std::printf(
                            "HLE %sOsdConfigParam: pointer=0x%08x "
                            "value=0x%08x result=0\\n",
                            service == 0x4au ? "Set" : "Get",
                            parameter_address, osd_config
                        );
                    }
                }

                else if (service == 0x64u || service == 0x68u)
                {
                    // FlushCache / iFlushCache: ps2-web-recomp's EE
                    // kernel HLE returns zero for both services. The
                    // translated code and ELF segments are currently
                    // immutable, so there is no host instruction cache
                    // to invalidate. Dynamic guest code writes remain
                    // outside the static runner's supported behavior.
                    cpu.m_gpr[2].m_lo = 0u;
                    cpu.m_gpr[2].m_hi = 0u;
                    if (trace)
                    {
                        std::printf(
                            "HLE %s: mode=0x%08x result=0\n",
                            service == 0x64u ? "FlushCache" : "iFlushCache",
                            static_cast<uint32_t>(cpu.m_gpr[4].m_lo)
                        );
                    }
                }

                else if (service == 0x6bu)
                {
                    // sceSifStopDma is void: disable genuine EE SIF0
                    // (IOP -> EE) channel 5 without claiming a transfer.
                    // Preserve CHCR mode/TIE bits while clearing STR.
                    ee::s_ee_u128 chcr{};
                    if (!ee::c_ee_hw_bus::mmio_read(
                            &hardware, ee::c_ee_dmac::k_sif0_base,
                            4u, &chcr))
                    {
                        std::fprintf(stderr,
                            "SifStopDma HLE refused unreadable SIF0 CHCR\n");
                        break;
                    }
                    const uint32_t old_chcr =
                        static_cast<uint32_t>(chcr.m_lo);
                    chcr.m_lo &= ~static_cast<uint64_t>(
                        ee::c_ee_dmac::k_chcr_str);
                    if (!ee::c_ee_hw_bus::mmio_write(
                            &hardware, ee::c_ee_dmac::k_sif0_base,
                            4u, &chcr))
                    {
                        std::fprintf(stderr,
                            "SifStopDma HLE refused unwritable SIF0 CHCR\n");
                        break;
                    }
                    if (trace)
                    {
                        std::printf(
                            "HLE SifStopDma: channel=5 "
                            "old_chcr=0x%08x new_chcr=0x%08x result=void\n",
                            old_chcr,
                            static_cast<uint32_t>(chcr.m_lo));
                    }
                }

                else if (service == 0x71u)
                {
                    // GsPutIMR is a void kernel service for the privileged
                    // GS interrupt-mask register.  Preserve only documented
                    // mask bits in the hardware state; no GS IRQ is invented
                    // by this non-rasterizing runner.
                    const uint64_t requested = cpu.m_gpr[4].m_lo;
                    hardware.set_gs_imr(requested);
                    if (trace)
                    {
                        std::printf(
                            "HLE GsPutIMR: requested=0x%08llx "
                            "stored=0x%08llx result=void\n",
                            static_cast<unsigned long long>(requested),
                            static_cast<unsigned long long>(hardware.gs_imr())
                        );
                    }
                }

                else if (service == 0x73u)
                {
                    // SetVSyncFlag records two guest-RAM pointers for a
                    // future *actual* GS VSync event. Unlike VSync itself,
                    // registration must not touch their current contents,
                    // change CSR, raise an IRQ or advance a frame.
                    const uint32_t flag_address =
                        static_cast<uint32_t>(cpu.m_gpr[4].m_lo);
                    const uint32_t csr_address =
                        static_cast<uint32_t>(cpu.m_gpr[5].m_lo);
                    if (!vsync_flags.register_guest_pointers(
                            memory.get(), flag_address, csr_address))
                    {
                        std::fprintf(stderr,
                            "SetVSyncFlag HLE refused invalid guest pointers "
                            "flag=0x%08x csr=0x%08x\n",
                            flag_address, csr_address);
                        break;
                    }
                    // The kernel's syscall ABI returns V0=0 even though
                    // this registration creates no event or guest RAM write.
                    cpu.m_gpr[2].m_lo = 0u;
                    cpu.m_gpr[2].m_hi = 0u;
                    if (trace)
                    {
                        std::printf(
                            "HLE SetVSyncFlag: flag=0x%08x csr=0x%08x "
                            "result=0, no VSync event\n",
                            flag_address, csr_address);
                    }
                }

                else if (service == 0x76u)
                {
                    // SifDmaStat reports only real transfers accepted by
                    // the connected EE-to-IOP transport. Until cycle-level
                    // scheduling exists, those copies finish synchronously.
                    const uint32_t id =
                        static_cast<uint32_t>(cpu.m_gpr[4].m_lo);
                    int32_t dma_status = 0;
                    const bool known_id = iop_rom0_path != nullptr
                        ? iop_sif1_transport != nullptr &&
                          iop_sif1_transport->dma_stat(id, &dma_status)
                        : iop_sif1 != nullptr &&
                          iop_sif1->dma_stat(id, &dma_status);
                    if (!known_id)
                    {
                        std::fprintf(
                            stderr,
                            "SifDmaStat refused untracked SIF1 DMA id=%u\n",
                            id
                        );
                        break;
                    }
                    cpu.m_gpr[2].m_lo = static_cast<uint64_t>(
                        static_cast<int64_t>(dma_status)
                    );
                    cpu.m_gpr[2].m_hi = 0u;
                    if (trace)
                    {
                        std::printf(
                            "HLE SifDmaStat: id=%u status=%d\n",
                            id, dma_status
                        );
                    }
                }

                else if (service == 0x77u)
                {
                    // SifSetDma accepts guest descriptor lists, not an
                    // assumed IOP firmware or fabricated RPC completion.
                    const uint32_t descriptors =
                        static_cast<uint32_t>(cpu.m_gpr[4].m_lo);
                    const uint32_t count =
                        static_cast<uint32_t>(cpu.m_gpr[5].m_lo);
                    uint32_t dma_id = 0u;
                    iop::s_sif1_dma_diagnostic dma_diagnostic{};
                    const bool transferred = iop_rom0_path != nullptr
                        ? iop_sif1_transport != nullptr &&
                          iop_sif1_transport->set_dma(
                              descriptors, count, &dma_id)
                        : iop_sif1 != nullptr &&
                          iop_sif1->set_dma(
                              descriptors, count, &dma_id,
                              &dma_diagnostic
                          );
                    if (!transferred)
                    {
                        std::fprintf(
                            stderr,
                            "SifSetDma refused unconnected or unsupported "
                            "IOP transfer: descriptors=0x%08x count=%u\n",
                            descriptors, count
                        );
                        if (iop_rom0_path == nullptr)
                        {
                            std::fprintf(
                                stderr,
                                "IOP HLE SIF1 DMA rejection: reason=%s "
                                "index=%u src=0x%08x dst=0x%08x "
                                "bytes=%u attrs=0x%08x "
                                "packet_size=0x%08x packet_cid=0x%08x "
                                "packet_dest=0x%08x packet_opt=0x%08x "
                                "packet_arg=0x%08x rpc_server=0x%08x "
                                "rpc_client=0x%08x rpc_rejection=%s sifcmd_initialized=%u\n",
                                iop::c_sif1_bridge::failure_name(
                                    dma_diagnostic.m_failure
                                ),
                                dma_diagnostic.m_descriptor_index,
                                dma_diagnostic.m_source,
                                dma_diagnostic.m_destination,
                                dma_diagnostic.m_size,
                                dma_diagnostic.m_attributes,
                                dma_diagnostic.m_packet_size,
                                dma_diagnostic.m_packet_cid,
                                dma_diagnostic.m_packet_dest,
                                dma_diagnostic.m_packet_opt,
                                dma_diagnostic.m_packet_arg,
                                dma_diagnostic.m_rpc_server,
                                dma_diagnostic.m_rpc_client,
                                iop::c_sif_rpc_bridge::rejection_name(
                                    dma_diagnostic.m_rpc_rejection
                                ),
                                iop_sifcmd != nullptr &&
                                    iop_sifcmd->initialized() ? 1u : 0u
                            );
                        }
                        if (iop_rom0_path == nullptr &&
                            dma_diagnostic.m_rpc_packet_word_count != 0u)
                        {
                            std::fprintf(
                                stderr,
                                "IOP HLE rejected RPC payload: "
                                "source=0x%08x bytes=%u header_words=%u\n",
                                dma_diagnostic.m_rpc_payload_source,
                                dma_diagnostic.m_rpc_payload_length,
                                dma_diagnostic.m_rpc_packet_word_count
                            );
                            std::fprintf(stderr, "IOP HLE rejected RPC words:");
                            for (uint32_t word = 0u;
                                 word < dma_diagnostic.m_rpc_packet_word_count;
                                 ++word)
                            {
                                std::fprintf(
                                    stderr, " %08x",
                                    dma_diagnostic.m_rpc_packet_words[word]
                                );
                            }
                            std::fprintf(stderr, "\n");
                        }
                        if (iop_rom0_path != nullptr && count != 0u &&
                            memory->contains(descriptors, 16u))
                        {
                            const uint32_t source =
                                memory->read32(descriptors);
                            const uint32_t destination =
                                memory->read32(descriptors + 4u);
                            const uint32_t bytes =
                                memory->read32(descriptors + 8u);
                            const uint32_t attributes =
                                memory->read32(descriptors + 12u);
                            uint32_t receiver_chcr = 0u;
                            (void)iop_kernel->hardware().read(
                                iop::c_hw::k_sif1_dma_base + 8u,
                                4u, &receiver_chcr);
                            std::fprintf(
                                stderr,
                                "IOP ROM0 SifSetDma diagnostic: "
                                "src=%08x dst=%08x bytes=%u attr=%08x "
                                "receiver_chcr=%08x sif1_faulted=%u\n",
                                source, destination, bytes, attributes,
                                receiver_chcr,
                                iop_sif1_transport->faulted() ? 1u : 0u
                            );
                        }
                        break;
                    }
                    if (disc_folder_path != nullptr &&
                        iop_sifcmd != nullptr)
                    {
                        std::string reboot_argument;
                        if (iop_sifcmd->take_reboot_request(&reboot_argument))
                        {
                            std::string reboot_error;
                            if (folder_disc_source == nullptr ||
                                !validate_hle_iop_reboot(
                                    reboot_argument, *folder_disc_source,
                                    &reboot_error))
                            {
                                std::fprintf(
                                    stderr, "IOP HLE reboot cannot start: %s "
                                    "(request=%s)\n",
                                    reboot_error.c_str(),
                                    reboot_argument.c_str()
                                );
                                break;
                            }

                            // Only the already-validated SIFCMD packet
                            // requests this reset. It destroys previous IOP
                            // IRX/module/thread/RPC state, then installs
                            // actual host-backed HLE services anew.
                            // Detach CDVD transport before resetting
                            // its IOP MMIO registers. Rebind to the same
                            // mounted sector source after the kernel reset.
                            if (cdvd != nullptr)
                            {
                                cdvd->m_transport.reset();
                            }
                            host_audio.pause();
                            iop_kernel->reset();
                            if (!iop_kernel->attach_hle_hardware())
                            {
                                std::fprintf(stderr,
                                    "[iop] HLE MMIO reattachment failed\n");
                                break;
                            }
                            host_audio.resume();
                            if (pad_poll.enabled())
                            {
                                iop_kernel->hardware().bind_pad0_poll(
                                    &pad_poll, iop::c_pad_poll_replay::poll);
                            }
                            if (pad0_cross_poll_count != 0u)
                            {
                                iop_kernel->hardware()
                                    .configure_sio2_pad0_cross_pulse(
                                        pad0_cross_first_poll,
                                        pad0_cross_poll_count);
                            }
                            if (!pad0_script.empty())
                            {
                                iop_kernel->hardware()
                                    .configure_sio2_pad0_script(pad0_script);
                            }
                            if (cdvd != nullptr && cdvd->m_source != nullptr)
                            {
                                cdvd->m_transport =
                                    std::make_unique<iop::c_cdvd_dma_transport>(
                                        iop_kernel->cpu(),
                                        iop_kernel->hardware(),
                                        *cdvd->m_source
                                    );
                            }
                            iop_sifcmd->reset_for_reboot();
                            iop_hle->reset_for_reboot();
                            iop_rpc->reset_for_reboot();
                            // Metadata is read from the real mounted IOPRP,
                            // never inferred from its name or the ELF catalog.
                            if (!iop_rpc->configure_reboot_image_version(
                                    reboot_argument))
                            {
                                std::fprintf(
                                    stderr,
                                    "[iop] IOPRP lacks a verified LOADFILE "
                                    "version; version RPC unsupported\n");
                            }
                            hardware.sif().begin_iop_reboot();

                            if (!iop_hle->install() ||
                                !iop_sifcmd->install() ||
                                !iop_sifman->install() ||
                                (iop_cdvdman != nullptr &&
                                 !iop_cdvdman->install()))
                            {
                                std::fprintf(
                                    stderr,
                                    "IOP HLE reboot failed service reinstall\n"
                                );
                                break;
                            }
                            iop_kernel->bind_syscall(
                                [&](iop::c_kernel& active, uint32_t service)
                                {
                                    return iop_hle->handle_syscall(active, service) ||
                                           iop_sifman->handle_syscall(active, service) ||
                                           iop_sifcmd->handle_syscall(active, service) ||
                                           (iop_cdvdman != nullptr &&
                                            iop_cdvdman->handle_syscall(active, service));
                                }
                            );
                            // Defer publication to the EE's subsequent
                            // boot-status poll, not the SIF1 DMA itself.
                            // ROM0 boot remains a separate guest-code path.
                            hle_iop_reboot_bootstrap_pending = true;
                            hle_iop_reboot_ee_reset_seen = false;
                            std::printf(
                                "[iop] HLE reboot requested: %s "
                                "(services reinstalled; boot flags pending)\n",
                                reboot_argument.c_str()
                            );
                        }
                    }
                    // Expose accepted RPC identity and actual backend
                    // result, not only unsupported packet diagnostics.
                    // Collapse repeated calls in normal logs; trace mode
                    // intentionally retains every committed observation.
                    if (iop_rom0_path == nullptr && iop_rpc != nullptr)
                    {
                        const auto& event = iop_rpc->last_observation();
                        if (event.m_sequence != 0u &&
                            event.m_sequence != last_iop_rpc_sequence)
                        {
                            last_iop_rpc_sequence = event.m_sequence;
                            ++observed_iop_rpc_calls;
                            const bool changed =
                                event.m_server_id != last_iop_rpc_server ||
                                event.m_method != last_iop_rpc_method ||
                                event.m_return_word != last_iop_rpc_result ||
                                event.m_guest_path != last_iop_rpc_path;
                            if (trace || observed_iop_rpc_calls <= 24u ||
                                changed || observed_iop_rpc_calls % 128u == 0u)
                            {
                                std::printf(
                                    "IOP HLE RPC committed: seq=%llu "
                                    "cid=0x%08x server=0x%08x "
                                    "method=0x%08x client=0x%08x "
                                    "send=%u recv=%u result=0x%08x "
                                    "path=%s load_status=%u link_status=%u "
                                    "module=%u thread=%u started=%u "
                                    "irx_entry=%08x irx_gp=%08x "
                                    "irx_image=%08x unresolved=%s#%u\n",
                                    static_cast<unsigned long long>(
                                        event.m_sequence),
                                    event.m_cid, event.m_server_id,
                                    event.m_method, event.m_client,
                                    event.m_send_size, event.m_recv_size,
                                    event.m_return_word,
                                    event.m_guest_path.empty() ? "(none)"
                                        : event.m_guest_path.c_str(),
                                    static_cast<uint32_t>(
                                        event.m_load_status),
                                    static_cast<uint32_t>(
                                        event.m_link_status),
                                    event.m_module_id, event.m_thread_id,
                                    event.m_module_started ? 1u : 0u,
                                    event.m_irx_entry_offset,
                                    event.m_irx_gp_offset,
                                    event.m_irx_image_size,
                                    event.m_unresolved_library.empty()
                                        ? "(none)"
                                        : event.m_unresolved_library.c_str(),
                                    event.m_unresolved_ordinal);
                            }
                            last_iop_rpc_server = event.m_server_id;
                            last_iop_rpc_method = event.m_method;
                            last_iop_rpc_result = event.m_return_word;
                            last_iop_rpc_path = event.m_guest_path;

                            // Once a real LOADFILE module is ready, it
                            // must continue to run on later EE/IOP
                            // transport turns too. A single load-time
                            // quantum is insufficient for genuine IRX code
                            // and EE WaitSema may never block at all.
                            // This is a bounded guest R3000 slice, not
                            // a synthetic IRX completion or IOP clock.
                            if (iop_kernel->has_guest_work())
                            {
                                const iop::s_kernel_run_result progressed =
                                    iop_kernel->run(iop_quantum, 128u);
                                if (progressed.m_stop !=
                                        iop::e_kernel_stop::budget_exhausted &&
                                    progressed.m_stop !=
                                        iop::e_kernel_stop::idle)
                                {
                                    std::fprintf(
                                        stderr,
                                        "IOP HLE guest service stopped: "
                                        "rpc_server=0x%08x rpc_method=0x%08x "
                                        "module=%u thread=%u reason=%u "
                                        "pc=0x%08x opcode=0x%08x "
                                        "executed=%llu\n",
                                        event.m_server_id, event.m_method,
                                        event.m_module_id,
                                        progressed.m_thread_id,
                                        static_cast<uint32_t>(
                                            progressed.m_stop),
                                        progressed.m_guest_pc,
                                        progressed.m_opcode,
                                        static_cast<unsigned long long>(
                                            progressed.m_executed));
                                     if (progressed.m_stop ==
                                         iop::e_kernel_stop::memory_fault)
                                     {
                                         const iop::s_memory_fault& fault =
                                             iop_kernel->cpu().last_memory_fault();
                                         const iop::s_cpu_state& state =
                                             iop_kernel->cpu().state();
                                         std::fprintf(stderr,
                                             "IOP HLE guest memory fault: "
                                             "address=0x%08x width=%u write=%u "
                                             "valid=%u v0=0x%08x a0=0x%08x "
                                             "gp=0x%08x sp=0x%08x ra=0x%08x\n",
                                             fault.m_address, fault.m_width,
                                             fault.m_write ? 1u : 0u,
                                             fault.m_valid ? 1u : 0u,
                                             state.m_gpr[2], state.m_gpr[4],
                                             state.m_gpr[28], state.m_gpr[29],
                                             state.m_gpr[31]);
                                     }
                                    break;
                                }
                                if (trace && progressed.m_executed != 0u)
                                {
                                    std::printf(
                                        "IOP HLE guest service turn: "
                                        "rpc_server=0x%08x method=0x%08x "
                                        "thread=%u instructions=%llu "
                                        "stop=%u\n",
                                        event.m_server_id, event.m_method,
                                        progressed.m_thread_id,
                                        static_cast<unsigned long long>(
                                            progressed.m_executed),
                                        static_cast<uint32_t>(
                                            progressed.m_stop));
                                }
                            }
                        }
                    }
                    cpu.m_gpr[2].m_lo = dma_id;
                    cpu.m_gpr[2].m_hi = 0u;
                    if (trace)
                    {
                        std::printf(
                            "HLE SifSetDma: descriptors=0x%08x "
                            "count=%u id=%u (%s)\n",
                            descriptors, count, dma_id,
                            iop_rom0_path != nullptr
                                ? "real DMA6/FIFO/DMA10"
                                : "synchronous IRX SIF1"
                        );
                    }
                    if (iop_rom0_path != nullptr)
                    {
                        // Only guest R3000 instructions may process the
                        // packet. Advance a deterministic, bounded IOP
                        // slice after the DMA completes; no host LOADFILE
                        // implementation or fabricated RPC reply runs.
                        const iop::s_kernel_run_result progressed =
                            iop_kernel->run(iop_quantum, 128u);
                        if (progressed.m_stop !=
                                iop::e_kernel_stop::budget_exhausted &&
                            progressed.m_stop != iop::e_kernel_stop::idle)
                        {
                            std::fprintf(
                                stderr,
                                "IOP ROM0 stopped after SifSetDma: "
                                "reason=%u pc=0x%08x opcode=0x%08x\n",
                                static_cast<uint32_t>(progressed.m_stop),
                                progressed.m_guest_pc, progressed.m_opcode
                            );
                            break;
                        }
                        if (trace)
                        {
                            std::printf(
                                "IOP ROM0 processed SIF1 turn: "
                                "instructions=%llu pc=0x%08x "
                                "sif1_delivered=%llu sif0_delivered=%llu\n",
                                static_cast<unsigned long long>(
                                    progressed.m_executed),
                                iop_kernel->cpu().state().m_pc,
                                static_cast<unsigned long long>(
                                    iop_sif1_transport->delivered_bytes()),
                                static_cast<unsigned long long>(
                                    iop_sif0_transport->delivered_bytes())
                            );
                        }
                    }
                }

                else if (service == 0x78u)
                {
                    // sceSifSetDChain: initialize the EE SIF0 receive DMA
                    // channel 5, QWC=0 and CHCR=0x184 (chain/TIE/STR).
                    // The syscall is void: keep the incoming V0 unchanged.
                    // A real IOP DMA producer/scheduler is not yet present;
                    // the hardware model retains STR without fabricating
                    // received bytes, completion or an interrupt.
                    const ee::s_ee_u128 zero{0u, 0u};
                    const ee::s_ee_u128 chcr{0x184u, 0u};
                    if (!ee::c_ee_hw_bus::mmio_write(
                            &hardware,
                            ee::c_ee_dmac::k_sif0_base + 0x20u,
                            4u, &zero) ||
                        !ee::c_ee_hw_bus::mmio_write(
                            &hardware,
                            ee::c_ee_dmac::k_sif0_base,
                            4u, &chcr))
                    {
                        std::fprintf(
                            stderr,
                            "SifSetDChain HLE refused missing SIF0 DMA "
                            "MMIO register backing\n"
                        );
                        break;
                    }

                    // An IOP LOADFILE server may have a validated
                    // response pending until EE SIF0 is armed.
                    if (iop_rpc != nullptr)
                    {
                        (void)iop_rpc->pump();
                    }
                    if (trace)
                    {
                        std::printf(
                            "HLE %sSifSetDChain: channel=5 "
                            "qwc=0 chcr=0x00000184 "
                            "result=void (%s)\n",
                            interrupt_sif_service ? "i" : "",
                            iop_rpc == nullptr
                                ? "no IOP producer"
                                : "IOP LOADFILE RPC connected"
                        );
                    }
                }

                else if (service == 0x79u || service == 0x7au)
                {
                    // sceSifSetReg/GetReg: four EE-visible hardware
                    // mailboxes and three software-only SIF_SYSREG slots.
                    // SIF_SYSREG_SUBADDR is initially zero. Only an IOP
                    // producer can publish the physical SUBADDR/SMFLAG;
                    // neither syscall fakes IOP initialization.
                    const uint32_t id =
                        static_cast<uint32_t>(cpu.m_gpr[4].m_lo);
                    if (service == 0x79u)
                    {
                        const uint32_t value =
                            static_cast<uint32_t>(cpu.m_gpr[5].m_lo);
                        if (!hardware.sif().set_reg(id, value))
                        {
                            std::fprintf(
                                stderr,
                                "SifSetReg HLE refused unsupported "
                                "register=0x%08x value=0x%08x\n",
                                id, value
                            );
                            break;
                        }

                        // The EE explicitly tears down its previous
                        // SIF SYSREG after a reset request. Only after
                        // this handshake can the new IOP HLE bootstrap
                        // publish its replacement readiness flags.
                        if (hle_iop_reboot_bootstrap_pending &&
                            id == 0x80000002u && value == 0u)
                        {
                            hle_iop_reboot_ee_reset_seen = true;
                        }
                        cpu.m_gpr[2].m_lo = 0u;
                        cpu.m_gpr[2].m_hi = 0u;
                        if (trace)
                        {
                            std::printf(
                                "HLE SifSetReg: register=0x%08x "
                                "value=0x%08x result=0\n",
                                id, value
                            );
                        }
                    }
                    else
                    {
                        // The HLE reboot has rebuilt and installed its
                        // real host-backed IOP services. Publish modeled
                        // readiness only when the EE subsequently polls
                        // the boot flag, never from receipt of DMA itself.
                        if (id == 4u &&
                            hle_iop_reboot_bootstrap_pending &&
                            hle_iop_reboot_ee_reset_seen)
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
    if (!pc_profile.entries().empty())
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
        // Preserve the full combat-selected hot set for offline analysis.
        // Export only at shutdown; no extra work in the dispatch loop.
        if (report_path != nullptr)
        {
            const std::string profile_path = std::string(report_path) + ".pc-profile.csv";
            std::ofstream profile(profile_path, std::ios::out | std::ios::trunc);
            profile << "pc,calls,dispatches,instructions,idle_charged,native_boundary_ns,nested_ns\n";
            for (const auto& [pc, item] : ranked)
            {
                profile << "0x" << std::hex << pc << std::dec << ','
                        << item.m_calls << ',' << item.m_dispatches << ','
                        << item.m_instructions << ',' << item.m_idle_instructions << ','
                        << item.m_wall_ns << ',' << item.m_nested_ns << '\n';
            }
            profile.close();
            if (!profile)
            {
                std::fprintf(stderr, "EE profile export failed: %s\n", profile_path.c_str());
            }
            else
            {
                std::fprintf(stderr, "EE profile export: %zu PCs to %s\n",
                    ranked.size(), profile_path.c_str());
            }
        }
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
