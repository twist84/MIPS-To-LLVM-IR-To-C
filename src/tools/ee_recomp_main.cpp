#include "ee_recomp.h"
#include "module_builder.h"
#include <cerrno>
#include <cstdio>
#include <set>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>

static void print_usage(const char* argv0)
{
    std::fprintf(stderr,
        "usage: %s <elf.elf> [output_dir] [options]\n"
        "\n"
        "options:\n"
        "  --max-functions=N   Emit at most N functions (0 = unlimited)\n"
        "  --function=0xADDR   Emit only the function at guest address ADDR\n"
        "  --functions=ADDR,...   Emit a bounded list of guest entry PCs in one pass\n"
        "  --opt-level=N       IR optimisations 0-3 (default 0):\n"
        "                        0/1 safe baseline, 2 +CSE, 3 +dead-temp\n"

        "  --discovery-cache=FILE   Reuse matching full-ELF discovery for explicit entries\n"
        "  --serve-reconstruction  Persistent isolated batch session (ELF only)\n"
        "  -h, --help          Show this help\n",
        argv0);
}

static uint32_t parse_u32(const std::string& s)
{
    return (uint32_t)std::strtoul(s.c_str(), nullptr, 0);
}

static bool parse_function_addresses(const std::string& text,
                                     std::vector<uint32_t>& addresses)
{
    if (text.empty())
    {
        return false;
    }
    std::set<uint32_t> seen;
    size_t offset = 0;
    do
    {
        const size_t separator = text.find(',', offset);
        const std::string token = text.substr(
            offset, separator == std::string::npos
                ? std::string::npos : separator - offset);
        if (token.empty() || token[0] == '-' || token[0] == '+')
        {
            return false;
        }
        errno = 0;
        char* end = nullptr;
        const unsigned long long value = std::strtoull(
            token.c_str(), &end, 0);
        if (errno != 0 || end == token.c_str() || *end != '\0' ||
            value == 0 || value > UINT32_MAX || (value & 3u) != 0 ||
            !seen.insert(static_cast<uint32_t>(value)).second)
        {
            return false;
        }
        addresses.push_back(static_cast<uint32_t>(value));
        if (addresses.size() > 128u)
        {
            return false;
        }
        if (separator == std::string::npos)
        {
            break;
        }
        offset = separator + 1u;
    }
    while (true);
    return true;
}

// Line protocol used only by isolated reconstruction: seed/batch, output,
// comma-separated PCs. A failed request terminates the session.
static int serve_reconstruction(const char* elf_path, uint32_t ir_jobs)
{
    ee::c_ee_recomp_session session;
    std::string cache_path;
    std::string line;
    for (; std::getline(std::cin, line);)
    {
        if (line == "quit")
        {
            return cache_path.empty() ? 2 : 0;
        }
        const size_t first = line.find('\t');
        const size_t second = first == std::string::npos
            ? std::string::npos : line.find('\t', first + 1);
        if (second == std::string::npos || line.find('\t', second + 1) != std::string::npos)
        {
            return 2;
        }
        const std::string mode = line.substr(0, first);
        const std::string output = line.substr(first + 1, second - first - 1);
        std::vector<uint32_t> addresses;
        if (output.empty() ||
            !parse_function_addresses(line.substr(second + 1), addresses))
        {
            return 2;
        }
        ee::s_recomp_options opts;
        opts.ir_jobs = ir_jobs;
        if (mode == "seed" && cache_path.empty() && addresses.size() == 1)
        {
            opts.only_function = addresses.front();
        }
        else if (mode == "batch" && !cache_path.empty())
        {
            opts.only_functions = std::move(addresses);
            opts.discovery_cache_path = cache_path;
        }
        else
        {
            return 2;
        }
        ee::s_recompiled_elf result;
        std::string error;
        if (!session.recompile(elf_path, output.c_str(), &result, &error, opts))
        {
            std::fprintf(stderr, "ee_recomp session: %s\n", error.c_str());
            return 1;
        }
        if (cache_path.empty())
        {
            cache_path = output + "/ee_discovery_cache.bin";
        }
        std::printf("ee_recomp session result: ok\n");
        std::fflush(stdout);
    }
    return 2;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        print_usage(argv[0]);
        return 2;
    }

    if (argc >= 3 && std::strcmp(argv[2], "--serve-reconstruction") == 0)
    {
        uint32_t ir_jobs = 1;
        if (argc == 4)
        {
            const std::string arg = argv[3];
            if (!arg.starts_with("--ir-jobs="))
            {
                return 2;
            }
            const std::string value = arg.substr(10);
            if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos ||
                value.size() > 2)
            {
                return 2;
            }
            ir_jobs = parse_u32(value);
            if (ir_jobs == 0 || ir_jobs > 32)
            {
                return 2;
            }
        }
        else if (argc != 3)
        {
            return 2;
        }
        return serve_reconstruction(argv[1], ir_jobs);
    }

    const char* elf_path = nullptr;
    const char* output_dir = "build/recompiled";
    ee::s_recomp_options opts;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg.compare(0, 16, "--max-functions=") == 0) {
            opts.max_functions = parse_u32(arg.substr(16));
            continue;
        }
        if (arg.compare(0, 11, "--function=") == 0) {
            opts.only_function = parse_u32(arg.substr(11));
            continue;
        }
        if (arg.compare(0, 12, "--functions=") == 0) {
            if (!opts.only_functions.empty() ||
                !parse_function_addresses(arg.substr(12), opts.only_functions))
            {
                std::fprintf(stderr, "invalid --functions list (1..128 unique aligned PCs)\n");
                return 2;
            }
            continue;
        }
        if (arg.compare(0, 18, "--discovery-cache=") == 0) {
            opts.discovery_cache_path = arg.substr(18);
            continue;
        }
        if (arg.compare(0, 12, "--opt-level=") == 0) {
            opts.opt_level = (int)parse_u32(arg.substr(12));
            continue;
        }
        if (arg[0] == '-') {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            print_usage(argv[0]);
            return 2;
        }
        if (!elf_path) {
            elf_path = argv[i];
        } else {
            output_dir = argv[i];
        }
    }

    if (!elf_path) {
        print_usage(argv[0]);
        return 2;
    }
    if (!opts.only_functions.empty() &&
        (opts.only_function != 0u || opts.max_functions != 0u))
    {
        std::fprintf(stderr, "--functions cannot be combined with --function or --max-functions\n");
        return 2;
    }

    std::printf("ee_recomp: loading %s -> %s", elf_path, output_dir);
    if (opts.max_functions)
        std::printf(" (max-functions=%u)", opts.max_functions);
    if (opts.only_function)
        std::printf(" (function=0x%08x)", opts.only_function);
    if (!opts.only_functions.empty())
        std::printf(" (functions=%zu)", opts.only_functions.size());
    if (opts.opt_level)
        std::printf(" (opt-level=%d)", opts.opt_level);
    std::printf("\n");
    std::fflush(stdout);

    ee::s_recompiled_elf result;
    std::string error;
    if (!ee::ee_recomp(elf_path, output_dir, &result, &error, opts)) {
        std::fprintf(stderr, "ee_recomp: %s\n", error.c_str());
        return 1;
    }
    std::printf("ee_recomp: done\n");
    std::fflush(stdout);

    std::printf("\n== ee_recomp summary ==\n");
    std::printf("elf: %s\n", result.path.c_str());
    std::printf("entry: 0x%08x  machine: %u  segments: %u\n",
        result.entry_point, result.machine, result.segment_count);
    std::printf("functions emitted: %u  discovered: %u\n",
        result.function_count, result.functions_discovered);
    std::printf("function-scoped: pcs=%u  insns=%u  supported=%u  unsupported=%u",
        result.function_scoped_pcs, result.total_insns,
        result.total_supported, result.total_unsupported);
    if (result.total_insns > 0) {
        double pct = 100.0 * result.total_supported / result.total_insns;
        std::printf("  (%.1f%%)", pct);
    }
    std::printf("\n");
    std::printf("linear sweep:    insns=%u  supported=%u  unsupported=%u",
        result.linear_insns, result.linear_supported, result.linear_unsupported);
    if (result.linear_insns > 0) {
        double pct = 100.0 * result.linear_supported / result.linear_insns;
        std::printf("  (%.1f%%)", pct);
    }
    std::printf("\n");
    return 0;
}
