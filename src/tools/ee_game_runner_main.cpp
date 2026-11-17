#include "ee_runtime.h"
#include "ee_exceptions.h"
#include "ee_memory.h"
#include "ee_ram_interpreter.h"
#include "ee_hw_bus.h"
#include "elf_loader.h"
#include "iop_kernel.h"
#include "iop_cdvd.h"
#include "iop_sifman_bridge.h"
#include "iop_sif1_bridge.h"
#include "iop_sif1_dma_transport.h"
#include "iop_sif0_dma_transport.h"
#include "iop_sifcmd_bridge.h"
#include "iop_disc_loader.h"
#include "iop_sif_rpc_bridge.h"
#include "iop_sif_mmio_link.h"

#include <algorithm>
#include <cerrno>
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
#include <string>
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

constexpr uint32_t k_ee_ram_size = 32u * 1024u * 1024u;
constexpr uint32_t k_default_stack = 0x01fff000u;
constexpr uint32_t k_default_dispatch_budget = 10000u;
constexpr uint32_t k_report_history_limit = 64u;
constexpr uint64_t k_report_snapshot_interval = 128u;
constexpr uint64_t k_report_progress_interval = 256u;
constexpr uint32_t k_kernel_ram_snapshot_size = 0x80000u;
// ps2-web-recomp runtime/src/kernel.cpp uses this guest-visible HLE
// syscall table. Kernel addresses 0x80001800..0x80001BFF alias it.
constexpr uint32_t k_kernel_syscall_table_base = 0x00001800u;
constexpr uint32_t k_kernel_syscall_table_entries = 0x100u;

using t_game_run = uint32_t (*)(
    ee::s_ee_cpu_state*,
    ee::s_ee_runtime*,
    uint32_t,
    uint32_t*,
    uint32_t*,
    uint32_t*
);

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

void usage(const char* program)
{
    std::printf(
        "usage: %s <game.elf> <linked-native-library> [options]\n"
        "  --max-dispatches=N    Native function dispatch budget (default 10000)\n"
        "  --entry=0xADDR       Override ELF entry PC\n"
        "  --stack=0xADDR       Initial EE stack pointer (default 0x01fff000)\n"
        "  --loop-budget=N       Stop at a guest CFG loop after N header entries (0=off)\n"
        "  --sif-poll-limit=N    Diagnose N repeated IOP SMFLAG zero reads (0=off)\n"
        "  --iop-irx=FILE       Repeat in dependency order; last IRX starts by default\n"
        "  --iop-rom0=FILE      Boot caller-supplied 4 MiB PS2 BIOS ROM at BFC00000\n"
        "                        Also accepts SDBZ_IOP_ROM0 environment variable\n"
        "  --disc-image=FILE    Attach a raw 2048-byte-sector ISO to CDVD\n"
        "  --iop-start-all      Start all loaded IOP modules (default: last only)\n"
        "  --iop-quantum=N      Max R3000 instructions per SIF poll (default 256)\n"
        "  --trace              Print guest PC, reason and dispatch count per step\n"
        "  --trace-entry=0xPC   Print arguments and pointed-to words at a guest entry\n"
        "  --watch-word=0xADDR  Report guest word changes between native dispatches\n"
        "  --trace-store=0xADDR Trace individual guest stores overlapping this word (64 max)\n"
        "  --trace-v0=0xSTART:0xEND Trace low-64 V0 changes by guest PC (64 max)\n"
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
    std::unique_ptr<iop::c_iso_sector_source> m_source;
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
};

bool write_report(
    const char* path,
    const ee::elf_loader& image,
    const ee::s_ee_cpu_state& cpu,
    uint32_t entry,
    uint64_t dispatched,
    uint32_t reason,
    uint32_t status,
    const std::vector<s_dispatch_sample>& history,
    const std::vector<s_syscall_sample>& syscall_history,
    bool in_progress
)
{
    FILE* output = std::fopen(path, "wb");
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
            " \"to_pc\": %u, \"reason\": %u, \"status\": %u}",
            i ? "," : "", static_cast<unsigned long long>(item.m_index),
            item.m_from, item.m_to, item.m_reason, item.m_status);
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
    std::fputs(syscall_history.empty() ? "]\n}\n" : "\n  ]\n}\n", output);
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
    FILE* input = std::fopen(path, "rb");
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
    bool kseg_ram = false;
    const char* kernel_ram_path = nullptr;
    const char* report_path = nullptr;
    std::vector<std::string> iop_irx_paths;
    const char* iop_rom0_path = nullptr;
    const char* disc_image_path = nullptr;
    bool iop_start_all = false;
    uint32_t budget = k_default_dispatch_budget;
    uint32_t loop_budget = 0u;
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
        if (argument == "--trace")
        {
            trace = true;
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
        if (argument == "--hle-startup")
        {
            hle_startup = true;
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

    if (iop_rom0_path == nullptr)
    {
        // Host-only boot configuration: run_game.py can pass this
        // through unchanged without invalidating the existing EE
        // translation provenance / --no-recompile cache.
        iop_rom0_path = std::getenv("SDBZ_IOP_ROM0");
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
        const char* configured = std::getenv("SDBZ_IOP_ROM0_QUANTUM");
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
        stack >= k_ee_ram_size ||
        (has_trace_entry && (trace_entry & 3u) != 0u) ||
        (has_watch_word && (watch_word & 3u) != 0u) ||
        (has_trace_store && (trace_store & 3u) != 0u))
    {
        usage(argv[0]);
        return 2;
    }

    if (trace)
    {
        // Preserve recent trace lines when the Python host kills a timed-out
        // process: redirected stdout is normally fully buffered on Windows.
        std::setvbuf(stdout, nullptr, _IONBF, 0);
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
    const t_game_run native_run = module.entry();
    if (native_run == nullptr)
    {
        std::fprintf(stderr, "native library has no ee_game_run export\n");
        return 1;
    }

    // Host models INTC/D_STAT and synchronous normal-mode SPR DMA only.
    // There is no general hardware scheduler, BIOS, chain DMA, MFIFO,
    // or complete exception lifecycle.
    ee::s_ee_cpu_state cpu{};
    ee::s_ee_vu0_state vu0{};
    ee::c_ee_hw_bus hardware;
    hardware.bind_cpu(&cpu);
    hardware.bind_memory(memory.get());

    // Optional real guest IOP execution. This is a deterministic polling
    // rendezvous, NOT EE/IOP hardware cycle synchronization: the native
    // dispatcher does not yet expose an accurate EE-cycle clock.
    std::unique_ptr<iop::c_kernel> iop_kernel;
    std::unique_ptr<iop::c_sif1_dma_transport> iop_sif1_transport;
    std::unique_ptr<iop::c_sif0_dma_transport> iop_sif0_transport;
    std::unique_ptr<iop::c_sifman_bridge> iop_sifman;
    std::unique_ptr<iop::c_sif1_bridge> iop_sif1;
    std::unique_ptr<iop::c_sifcmd_bridge> iop_sifcmd;
    std::unique_ptr<iop::c_disc_loader> iop_disc;
    std::unique_ptr<iop::c_sif_rpc_bridge> iop_rpc;
    std::unique_ptr<s_cdvd_context> cdvd;
    if (!iop_irx_paths.empty() || iop_rom0_path != nullptr)
    {
        iop_kernel = std::make_unique<iop::c_kernel>();
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
                cdvd->m_source =
                    std::make_unique<iop::c_iso_sector_source>(
                    std::filesystem::path(disc_image_path)
                );
                if (!cdvd->m_source->valid())
                {
                    std::fprintf(
                        stderr, "CDVD ISO must be readable and aligned to "
                        "2048-byte sectors: %s\n", disc_image_path
                    );
                    return 1;
                }
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
            iop_sifcmd = std::make_unique<iop::c_sifcmd_bridge>(
                *iop_kernel, hardware.sif()
            );
            if (!iop_sifcmd->install())
            {
                std::fprintf(stderr, "IOP sifcmd service installation failed\n");
                return 1;
            }
            // LOADFILE is backed by actual game disc files, but nothing
            // is loaded until a guest-initialized IOP endpoint receives
            // a valid EE RPC packet through SIF1.
            iop_disc = std::make_unique<iop::c_disc_loader>(
                *iop_kernel, std::filesystem::path(elf_path).parent_path()
            );
            iop_rpc = std::make_unique<iop::c_sif_rpc_bridge>(
                *iop_disc, *iop_sifcmd, *memory, hardware.dmac()
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
            iop_kernel->bind_syscall([&](iop::c_kernel& active, uint32_t service)
            {
                return iop_sifman->handle_syscall(active, service) ||
                       iop_sifcmd->handle_syscall(active, service);
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
            const iop::s_module_load_result loaded = iop_kernel->load_module(bytes);
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

    ee::s_ee_runtime runtime{};
    runtime.m_user = memory.get();
    runtime.m_vu0_state = &vu0;
    runtime.m_mmio_user = &hardware;
    runtime.m_mmio_read = &ee::c_ee_hw_bus::mmio_read;
    runtime.m_mmio_write = &ee::c_ee_hw_bus::mmio_write;
    runtime.m_loop_safepoint_limit = loop_budget;
    runtime.m_loop_safepoint_remaining = loop_budget;
    runtime.m_guest_store_trace_enabled = has_trace_store;
    runtime.m_guest_store_trace_word = trace_store;
    runtime.m_guest_store_trace_remaining = has_trace_store ? 64u : 0u;
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

    struct s_kernel_thread
    {
        bool m_used = false;
        uint32_t m_status = 16u; // dormant; 1=run, 2=ready, 4=wait
        uint64_t m_order = 0u;
        bool m_has_context = false;
        uint32_t m_wait_semaphore = 0u;
        ee::s_ee_cpu_state m_context{};
        ee::s_ee_vu0_state m_vu0_context{};
        uint32_t m_entry = 0u;
        uint32_t m_stack = 0u;
        uint32_t m_stack_size = 0u;
        uint32_t m_gp = 0u;
        uint32_t m_priority = 0u;
        uint32_t m_initial_priority = 0u;
        uint32_t m_heap_end = 0u;
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
    const auto schedule_threads = [&]() -> bool
    {
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
        if (next_id == 0u)
        {
            std::fprintf(stderr,
                "EE thread scheduler: no runnable guest threads\n");
            return false;
        }
        if (next_id != current_thread_id)
        {
            s_kernel_thread& current = threads[current_thread_id];
            if (current.m_used)
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
            s_kernel_thread& next = threads[next_id];
            if (!next.m_has_context)
            {
                std::fprintf(stderr,
                    "EE thread scheduler: thread %u has no CPU context\n",
                    next_id);
                return false;
            }
            cpu = next.m_context;
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

    // The HLE EE kernel dispatches *registered guest* DMAC callbacks at
    // native safepoints. DMA5's pending level originates only from the
    // hardware-backed IOP DMA9 -> FIFO -> EE DMA5 transport; the host
    // does not fabricate any completion, response or guest memory flag.
    bool dmac_callback_active = false;
    uint32_t dmac_callback_channel = 0u;
    size_t dmac_callback_index = 0u;
    std::vector<s_kernel_interrupt_handler> dmac_callback_handlers;
    ee::s_ee_cpu_state dmac_interrupted_cpu{};
    ee::s_ee_vu0_state dmac_interrupted_vu0{};
    constexpr uint32_t k_dmac_kernel_stack = 0x8007fff0u;
    const auto enter_dmac_callback = [&](const s_kernel_interrupt_handler& handler)
    {
        cpu.m_cop0[12] = (dmac_interrupted_cpu.m_cop0[12] & ~0x10001u) | 2u;
        cpu.m_gpr[4] = {dmac_callback_channel, 0u};
        cpu.m_gpr[5] = {handler.m_arg, 0u};
        cpu.m_gpr[6] = {0u, 0u};
        cpu.m_gpr[7] = {0u, 0u};
        if (handler.m_gp != 0u)
        {
            cpu.m_gpr[28] = {handler.m_gp, 0u};
        }
        cpu.m_gpr[29] = {
            static_cast<uint64_t>(static_cast<int64_t>(
                static_cast<int32_t>(k_dmac_kernel_stack))), 0u
        };
        cpu.m_gpr[31] = {static_cast<uint64_t>(-16ll), 0u};
        cpu.m_pc = handler.m_address;
        cpu.m_exit_reason = static_cast<uint32_t>(
            ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
        if (trace)
        {
            std::printf("HLE DMAC callback enter: channel=%u "
                        "handler=0x%08x arg=0x%08x gp=0x%08x\n",
                        dmac_callback_channel, handler.m_address,
                        handler.m_arg, handler.m_gp);
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
    // Diagnose an unready IOP SIF handshake. Real ROM0 execution advances
    // on each read; the guard never manufactures a readiness value.
    uint32_t last_sif_smflag_epc = 0u;
    uint64_t last_sif_smflag_dispatch = 0u;
    uint32_t repeated_sif_smflag_reads = 0u;
    std::vector<s_dispatch_sample> history;
    if (report_path != nullptr)
    {
        history.reserve(k_report_history_limit);
    }
    uint32_t status = 0;
    uint32_t reason = cpu.m_exit_reason;
    uint64_t last_snapshot_dispatch = 0;
    uint64_t last_progress_dispatch = 0;
    bool last_snapshot_callback_active = false;
    const auto wall_start = std::chrono::steady_clock::now();
    double report_time_ms = 0.0;
    double native_time_ms = 0.0;
    double slowest_native_ms = 0.0;
    uint32_t slowest_native_pc = 0u;
    while (remaining != 0)
    {
        // Kernel HLE services completed device interrupts before handing
        // control back to its main thread. There is no synthetic DMA
        // status: only actual masked D_STAT completion bits can enter here.
        if (hle_startup && !dmac_callback_active && !callback_active &&
            ee::ee_interrupts_enabled(&cpu) &&
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
            dmac_callback_handlers.clear();
            for (const s_kernel_interrupt_handler& handler : dmac_handlers)
            {
                if (handler.m_cause == channel)
                {
                    dmac_callback_handlers.push_back(handler);
                }
            }
            if (trace)
            {
                std::printf("HLE DMAC IRQ: channel=%u pending=0x%08x "
                            "handlers=%zu\n", channel, pending,
                            dmac_callback_handlers.size());
            }
            if (dmac_callback_handlers.empty())
            {
                continue;
            }
            if (!memory->contains(k_dmac_kernel_stack - 0x1000u, 0x1000u))
            {
                std::fprintf(stderr,
                    "HLE DMAC callback refused unmapped kernel stack\n");
                reason = static_cast<uint32_t>(
                    ee::e_ee_exit_reason::EE_EXIT_UNSUPPORTED);
                status = 0u;
                break;
            }
            dmac_interrupted_cpu = cpu;
            dmac_interrupted_vu0 = vu0;
            dmac_callback_channel = channel;
            dmac_callback_index = 0u;
            dmac_callback_active = true;
            enter_dmac_callback(dmac_callback_handlers[dmac_callback_index++]);
            continue;
        }
        if (report_path != nullptr &&
            (dispatched == 0 ||
             dispatched - last_snapshot_dispatch >= k_report_snapshot_interval ||
             callback_active != last_snapshot_callback_active))
        {
            // Durable progress state survives an external wall-clock kill.
            // Report every 128 dispatches instead of reopening and writing
            // the full JSON report every 16 dispatches. Guest code can still
            // loop internally without returning to the dispatcher.
            const auto report_start = std::chrono::steady_clock::now();
            const bool report_ok = write_report(
                report_path, image, cpu, entry, dispatched,
                reason, status, history, syscall_history, true
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
                    k_callback_return_pc, k_ram_instruction_budget
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
                    "resume=0x%08x v0=0x%08x instructions=%u\n",
                    ram_entry, cpu.m_pc,
                    static_cast<uint32_t>(cpu.m_gpr[2].m_lo),
                    result.m_instructions
                );
            }
            continue;
        }

        const uint32_t from_pc = cpu.m_pc;
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
        const uint32_t slice = (trace || report_path != nullptr) ? 1u : remaining;
        const auto native_start = std::chrono::steady_clock::now();
        // A given native function can branch internally forever without
        // returning to the dispatcher. Reset a per-invocation loop-header
        // budget before execution; instrumented CFG blocks exit precisely
        // with LOOP_BUDGET if this count is exhausted.
        runtime.m_loop_safepoint_remaining = runtime.m_loop_safepoint_limit;
        reason = native_run(&cpu, &runtime, slice, &count, &out_pc, &status);
        if (report_path != nullptr)
        {
            const double duration_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - native_start
            ).count();
            native_time_ms += duration_ms;
            if (duration_ms > slowest_native_ms)
            {
                slowest_native_ms = duration_ms;
                slowest_native_pc = from_pc;
            }
        }

        if (count > slice)
        {
            std::fprintf(stderr, "invalid native dispatcher count %u > %u\n", count, slice);
            return 1;
        }

        dispatched += count;
        remaining -= count;
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
            if (history.size() == k_report_history_limit)
            {
                history.erase(history.begin());
            }
            history.push_back({dispatched, from_pc, out_pc, reason, status});
        }

        if (report_path != nullptr &&
            dispatched - last_progress_dispatch >= k_report_progress_interval)
        {
            const double elapsed_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - wall_start
            ).count();
            std::fprintf(
                stderr,
                "EE progress: dispatched=%llu pc=0x%08x "
                "elapsed_ms=%.1f native_ms=%.1f report_ms=%.1f "
                "slowest_native_pc=0x%08x slowest_native_ms=%.3f\n",
                static_cast<unsigned long long>(dispatched),
                cpu.m_pc, elapsed_ms, native_time_ms, report_time_ms,
                slowest_native_pc, slowest_native_ms
            );
            std::fflush(stderr);
            last_progress_dispatch = dispatched;
        }

        if (trace)
        {
            std::printf("dispatch=%llu pc=0x%08x -> 0x%08x reason=%s(%u) status=%u\n",
                        static_cast<unsigned long long>(dispatched), from_pc,
                        out_pc, exit_name(reason), reason, status);
        }

        // A bounded one-dispatch slice reports DISPATCH_LIMIT even when
        // the translated callback just returned to the synthetic sentinel.
        if (dmac_callback_active &&
            count != 0u &&
            cpu.m_pc == k_callback_return_pc &&
            status != 2u &&
            (reason == static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_RETURN) ||
             reason == static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_DISPATCH)))
        {
            const int32_t result = static_cast<int32_t>(cpu.m_gpr[2].m_lo);
            if (result >= 0 && dmac_callback_index < dmac_callback_handlers.size())
            {
                enter_dmac_callback(dmac_callback_handlers[dmac_callback_index++]);
                continue;
            }
            cpu = dmac_interrupted_cpu;
            vu0 = dmac_interrupted_vu0;
            hardware.intc().bind_cpu(&cpu);
            hardware.dmac().bind_cpu(&cpu);
            dmac_callback_active = false;
            dmac_callback_handlers.clear();
            cpu.m_exit_reason = static_cast<uint32_t>(
                ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
            if (trace)
            {
                std::printf("HLE DMAC callback returned: channel=%u "
                            "v0=%d resume=0x%08x\n",
                            dmac_callback_channel, result, cpu.m_pc);
            }
            // A genuine interrupt-safe SignalSema may have promoted an EE
            // waiter to READY. The kernel must finish restoring interrupted
            // EE/VU0 state before scheduling the newly runnable thread.
            if (reschedule_requested && !schedule_threads())
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
            current_thread_id > 1u && !dmac_callback_active &&
            !callback_active)
        {
            s_kernel_thread& finished = threads[current_thread_id];
            finished.m_status = 16u;
            finished.m_has_context = false;
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

        // Experimental PS2 EE startup HLE. Handle only explicitly
        // recognized syscalls; unknown services still stop at the exception.
        if (hle_startup &&
            reason == static_cast<uint32_t>(
                ee::e_ee_exit_reason::EE_EXIT_EXCEPTION) &&
            ((cpu.m_cop0[13] >> 2) & 0x1fu) == 8u)
        {
            const int32_t signed_service =
                static_cast<int32_t>(cpu.m_gpr[3].m_lo);
            // The EE kernel exposes interrupt-context SIF variants with
            // negative syscall numbers. Only map the genuine DMA/SIF
            // services when invoked from a registered guest DMAC callback;
            // keep unknown negative syscalls unsupported.
            const bool interrupt_sif_service = dmac_callback_active &&
                (signed_service == -0x76 || signed_service == -0x77 ||
                 signed_service == -0x78);
            // Genuine EE DMA5 SIFCMD handlers signal semaphores via the
            // kernel's negative interrupt-context iSignalSema number.
            const bool interrupt_semaphore_service = dmac_callback_active &&
                signed_service == -0x43;
            const bool interrupt_safe_service = interrupt_sif_service ||
                interrupt_semaphore_service;
            const uint32_t service = interrupt_safe_service
                ? static_cast<uint32_t>(-signed_service)
                : static_cast<uint32_t>(signed_service);
            if (!callback_active && !interrupt_safe_service &&
                service < 256u &&
                installed_syscall_handlers[service] != 0u)
            {
                // In interrupt context R5900 EPC still refers to the
                // interrupted thread, not the nested syscall. The runtime
                // separately records the actual trap instruction PC.
                const uint32_t epc = dmac_callback_active
                    ? cpu.m_pad : cpu.m_cop0[14];
                const uint32_t handler = installed_syscall_handlers[service];
                uint32_t opcode = 0;
                if (!image.read32_at(epc, &opcode) ||
                    (opcode & 0xfc00003fu) != 0x0000000cu ||
                    (cpu.m_cop0[13] & 0x80000000u) != 0u ||
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
            if ((service >= 0x10u && service <= 0x17u) ||
                (service >= 0x1au && service <= 0x1du) ||
                (service >= 0x20u && service <= 0x24u) ||
                service == 0x29u || service == 0x2au || service == 0x2bu ||
                service == 0x2fu || service == 0x30u || service == 0x31u ||
                service == 0x3cu || service == 0x3du || service == 0x3eu ||
                (service >= 0x40u && service <= 0x48u) ||
                service == 0x4au || service == 0x4bu ||
                service == 0x64u ||
                service == 0x68u || service == 0x6bu ||
                service == 0x74u ||
                service == 0x76u || service == 0x77u ||
                service == 0x78u || service == 0x79u ||
                service == 0x7au || service == 0x7cu)
            {
                // In interrupt context R5900 EPC still refers to the
                // interrupted thread, not the nested syscall. The runtime
                // separately records the actual trap instruction PC.
                const uint32_t epc = dmac_callback_active
                    ? cpu.m_pad : cpu.m_cop0[14];
                uint32_t opcode = 0;
                if (!image.read32_at(epc, &opcode) ||
                    (opcode & 0x3fu) != 0x0cu ||
                    (opcode >> 26) != 0u ||
                    (cpu.m_cop0[13] & 0x80000000u) != 0u ||
                    epc > 0xfffffffbu)
                {
                    std::fprintf(stderr, "startup HLE refused invalid syscall context\n");
                    break;
                }

                if (interrupt_sif_service && trace)
                {
                    std::printf(
                        "HLE interrupt SIF syscall: pc=0x%08x "
                        "architectural_epc=0x%08x resume=0x%08x\n",
                        epc, cpu.m_cop0[14], epc + 4u
                    );
                }

                if (service >= 0x10u && service <= 0x13u)
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
                    thread.m_status = 16u;
                    thread.m_has_context = false;
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

                else if (service == 0x2fu)
                {
                    // GetThreadId has no arguments. Creation of a dormant
                    // thread does not switch the CPU's running thread.
                    // Match ps2-web-recomp's current_thread()/GetThreadId.
                    if (current_thread_id == 0u ||
                        current_thread_id >= 256u ||
                        !threads[current_thread_id].m_used)
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
                            "HLE GetThreadId: id=%u\n", current_thread_id
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
                            thread->m_attr, thread->m_option, 0u, 0u, 0u
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
                    // SetupHeap records the current thread's heap boundary
                    // and returns it. EndOfHeap must query this same value;
                    // it must not infer the boundary from the caller's A0.
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
                    threads[current_thread_id].m_heap_end = heap_end32;
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
                    const uint32_t heap_end =
                        threads[current_thread_id].m_heap_end;
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
                                dmac_callback_active)
                            {
                                std::fprintf(stderr,
                                    "WaitSema HLE refused blocking wait: "
                                    "id=%u current_thread=%u count=0 "
                                    "(no runnable peer)\n",
                                    semaphore_id, current_thread_id);
                                break;
                            }
                            semaphore.m_waiters.push_back(current_thread_id);
                            threads[current_thread_id].m_status = 4u;
                            threads[current_thread_id].m_wait_semaphore =
                                semaphore_id;
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
                    const bool transferred = iop_rom0_path != nullptr
                        ? iop_sif1_transport != nullptr &&
                          iop_sif1_transport->set_dma(
                              descriptors, count, &dma_id)
                        : iop_sif1 != nullptr &&
                          iop_sif1->set_dma(descriptors, count, &dma_id);
                    if (!transferred)
                    {
                        std::fprintf(
                            stderr,
                            "SifSetDma refused unconnected or unsupported "
                            "IOP transfer: descriptors=0x%08x count=%u\n",
                            descriptors, count
                        );
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
                                    uint32_t dma10_chcr = 0u;
                                    (void)hardware.sif().get_reg(2u, &subaddr);
                                    (void)iop_kernel->hardware().read(
                                        iop::c_hw::k_sif1_dma_base + 8u,
                                        4u, &dma10_chcr
                                    );
                                    std::fprintf(
                                        stderr,
                                        "IOP ROM0 stall state: pc=0x%08x "
                                        "instructions=%llu quantum=%u "
                                        "subaddr=0x%08x dma10_chcr=0x%08x "
                                        "sif1_delivered=%llu sif0_delivered=%llu\n",
                                        iop_kernel->cpu().state().m_pc,
                                        static_cast<unsigned long long>(
                                            iop_kernel->cycles()),
                                        iop_quantum, subaddr, dma10_chcr,
                                        static_cast<unsigned long long>(
                                            iop_sif1_transport->delivered_bytes()),
                                        static_cast<unsigned long long>(
                                            iop_sif0_transport->delivered_bytes())
                                    );
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

                else
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

                // Syscall HLE returns to the interrupted guest callback.
                // Do not clear EXL for an interrupt-safe SIF syscall:
                // the registered DMA5 callback still owns its kernel
                // context until it returns via the callback sentinel.
                if (!dmac_callback_active)
                {
                    cpu.m_cop0[12] &= ~2u;
                }
                cpu.m_pc = epc + 4u;
                cpu.m_exit_reason = static_cast<uint32_t>(
                    ee::e_ee_exit_reason::EE_EXIT_DISPATCH
                );
                if (reschedule_requested && !dmac_callback_active &&
                    !callback_active && !schedule_threads())
                {
                    reason = static_cast<uint32_t>(
                        ee::e_ee_exit_reason::EE_EXIT_UNSUPPORTED);
                    status = 0u;
                    break;
                }
                continue;
            }
        }

        if (status != 1u || count == 0u)
        {
            break;
        }
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
            "slowest_native_ms=%.3f\n",
            static_cast<unsigned long long>(dispatched), elapsed_ms,
            native_time_ms, report_time_ms,
            slowest_native_pc, slowest_native_ms
        );
    }

    if (callback_active)
    {
        std::fprintf(stderr,
            "registered syscall handler did not return: pc=0x%08x resume=0x%08x\n",
            cpu.m_pc, callback_resume_pc);
    }

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
    if (report_path != nullptr &&
        !write_report(report_path, image, cpu, entry, dispatched,
                      reason, status, history, syscall_history, false))
    {
        return 1;
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
    if (reason == static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_LOOP_BUDGET))
    {
        std::fprintf(
            stderr,
            "guest loop budget exhausted: next_pc=0x%08x budget=%u; "
            "inspect guest state and translated loop instead of "
            "increasing the wall-clock timeout\n",
            cpu.m_pc, runtime.m_loop_safepoint_limit
        );
        return 3;
    }
    if (reason == static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_UNSUPPORTED) ||
        reason == static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_EXCEPTION) ||
        reason == static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_EXTERNAL_CALL))
    {
        std::fprintf(stderr, "guest blocked by an unsupported operation or host service\n");
        return 3;
    }
    // An exit from the linked translated-code set is not a completed PS2 boot.
    std::fprintf(stderr, "guest left compiled code; further runtime/BIOS work required\n");
    return 3;
}
