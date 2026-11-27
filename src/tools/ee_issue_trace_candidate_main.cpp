#include "ee_issue_trace_replay.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace
{
bool parse_latency(const char* text, uint32_t* value)
{
    if (text == nullptr || text[0] == '\0' || text[0] == '-')
    {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' ||
        parsed == 0u || parsed > std::numeric_limits<uint32_t>::max())
    {
        return false;
    }
    *value = static_cast<uint32_t>(parsed);
    return true;
}

struct s_candidate_event
{
    uint64_t m_slot = 0u;
    uint32_t m_pc = 0u;
    uint32_t m_opcode = 0u;
    uint32_t m_kind = 0u;
    uint64_t m_cycle = 0u;
    uint64_t m_completion_cycle = 0u;
};

bool parse_entry(const std::string& line,
                 ee::s_ee_instruction_trace_entry* entry)
{
    unsigned long long attempt = 0u;
    unsigned long long slot = 0u;
    unsigned int pc = 0u;
    unsigned int opcode = 0u;
    unsigned int kind = 0u;
    unsigned int service = 0u;
    int consumed = 0;
    const int parsed = std::sscanf(
        line.c_str(), "%llu,0x%x,0x%x,%u,%llu,%u%n",
        &attempt, &pc, &opcode, &kind, &slot, &service, &consumed
    );
    if (parsed != 6 || consumed != static_cast<int>(line.size()))
    {
        return false;
    }
    entry->m_attempt = static_cast<uint64_t>(attempt);
    entry->m_pc = static_cast<uint32_t>(pc);
    entry->m_opcode_word = static_cast<uint32_t>(opcode);
    entry->m_kind = static_cast<uint32_t>(kind);
    entry->m_reference_slot = static_cast<uint64_t>(slot);
    entry->m_hle_service = static_cast<uint32_t>(service);
    return true;
}
}

int main(int argc, char** argv)
{
    constexpr const char* k_output_option = "--candidate-events=";
    constexpr const char* k_completion_option = "--candidate-completions=";
    const std::string last_arg = argc > 1 ? argv[argc - 1] : "";
    const bool export_events = last_arg.rfind(k_output_option, 0u) == 0u;
    const bool export_completions =
        last_arg.rfind(k_completion_option, 0u) == 0u;
    const bool export_any = export_events || export_completions;
    const int argc_without_export = argc - (export_any ? 1 : 0);
    constexpr const char* k_packed_option = "--packed-mmi-latency=";
    const std::string packed_arg = argc_without_export > 1
        ? argv[argc_without_export - 1] : "";
    const bool explicit_packed =
        packed_arg.rfind(k_packed_option, 0u) == 0u;
    const int argc_without_packed =
        argc_without_export - (explicit_packed ? 1 : 0);
    const bool ordered_waw_overlap =
        argc_without_packed > 1 &&
        std::string(argv[argc_without_packed - 1]) == "--ordered-waw-overlap";
    const int argc_without_overlap =
        argc_without_packed - (ordered_waw_overlap ? 1 : 0);
    constexpr const char* k_throughput_option = "--throughput=";
    const std::string throughput_arg = argc_without_overlap > 1
        ? argv[argc_without_overlap - 1] : "";
    const bool explicit_throughput =
        throughput_arg.rfind(k_throughput_option, 0u) == 0u;
    const int profile_argc = argc_without_overlap - (explicit_throughput ? 1 : 0);
    const std::string output_path = export_any
        ? last_arg.substr(std::char_traits<char>::length(
            export_completions ? k_completion_option : k_output_option))
        : std::string{};

    const bool manual_profile = profile_argc == 3 &&
        std::string(argv[2]) == "--manual-cache-hit";
    if ((!manual_profile && profile_argc != 4 && profile_argc != 8 && profile_argc != 12 &&
         profile_argc != 14 && profile_argc != 16 &&
         profile_argc != 19) ||
        (manual_profile && explicit_throughput) ||
        (ordered_waw_overlap && !explicit_throughput && !manual_profile) ||
        (export_any && (output_path.empty() || output_path == argv[1])))
    {
        std::fprintf(stderr,
            "usage: ee_issue_trace_candidate <trace.csv> "
            "<integer-result-latency> <load-result-latency> "
            "[<multiply-latency> <divide-latency> <hilo-transfer-latency> "
            "<hilo-accumulate-latency>] "
            "[<fpu-transfer-latency> <fpu-arithmetic-latency> "
            "<fpu-divide-latency> <fpu-accumulate-latency>] "
            "[<fpu-control-latency> <fpu-compare-latency>] "
            "[<fpu-sqrt-latency> <fpu-rsqrt-latency>] "
            "[<cop0-transfer-latency> <vu0-vf-transfer-latency> "
            "<vu0-vi-transfer-latency>] "
            "[--throughput=<mac-mult>,<mac-div>,<mac-acc>,"
            "<cop1-arith>,<cop1-div>,<cop1-sqrt>,<cop1-rsqrt>] "
            "[--ordered-waw-overlap] "
            "[--packed-mmi-latency=<explicit-positive-value>] "
            "[--candidate-events=<path>|--candidate-completions=<path>]\n"
            "or: ee_issue_trace_candidate <trace.csv> --manual-cache-hit "
            "[--ordered-waw-overlap] [--packed-mmi-latency=<value>] "
            "[--candidate-events=<path>|--candidate-completions=<path>]\n"
            "Diagnostic only. Manual cache-hit latencies are NOT a validated R5900 clock.\n");
        return 2;
    }

    ee::s_ee_issue_replay_profile profile = manual_profile
        ? ee::ee_manual_cache_hit_profile() : ee::s_ee_issue_replay_profile{};
    if (explicit_packed)
    {
        const std::string value = packed_arg.substr(
            std::char_traits<char>::length(k_packed_option));
        if (!parse_latency(value.c_str(), &profile.m_packed_mmi_latency))
        {
            std::fprintf(stderr,
                "packed MMI timing requires an explicit positive latency\n");
            return 2;
        }
    }
    if (!manual_profile &&
        (!parse_latency(argv[2], &profile.m_integer_latency) ||
         !parse_latency(argv[3], &profile.m_load_latency)))
    {
        std::fprintf(stderr, "positive, explicit latency values required\n");
        return 2;
    }

    if (profile_argc >= 8 &&
        (!parse_latency(argv[4], &profile.m_hilo_multiply_latency) ||
         !parse_latency(argv[5], &profile.m_hilo_divide_latency) ||
         !parse_latency(argv[6], &profile.m_hilo_transfer_latency) ||
         !parse_latency(argv[7], &profile.m_hilo_accumulate_latency)))
    {
        std::fprintf(stderr, "HI/LO timing requires explicit positive latencies\n");
        return 2;
    }

    if (profile_argc >= 12 &&
        (!parse_latency(argv[8], &profile.m_fpu_transfer_latency) ||
         !parse_latency(argv[9], &profile.m_fpu_arithmetic_latency) ||
         !parse_latency(argv[10], &profile.m_fpu_divide_latency) ||
         !parse_latency(argv[11], &profile.m_fpu_accumulate_latency)))
    {
        std::fprintf(stderr, "COP1 timing requires explicit positive latencies\n");
        return 2;
    }

    if (profile_argc >= 14 &&
        (!parse_latency(argv[12], &profile.m_fpu_control_latency) ||
         !parse_latency(argv[13], &profile.m_fpu_compare_latency)))
    {
        std::fprintf(stderr,
            "FCR31 timing requires explicit positive control/compare latencies\n");
        return 2;
    }

    if (profile_argc >= 16 &&
        (!parse_latency(argv[14], &profile.m_fpu_sqrt_latency) ||
         !parse_latency(argv[15], &profile.m_fpu_rsqrt_latency)))
    {
        std::fprintf(stderr,
            "SQRT.S/RSQRT.S require separate positive latencies\n");
        return 2;
    }

    if (profile_argc == 19 &&
        (!parse_latency(argv[16], &profile.m_cop0_transfer_latency) ||
         !parse_latency(argv[17], &profile.m_vu0_vf_transfer_latency) ||
         !parse_latency(argv[18], &profile.m_vu0_vi_transfer_latency)))
    {
        std::fprintf(stderr,
            "COP0/VU0 register-transfer timing requires three explicit "
            "positive latency values\n");
        return 2;
    }

    if (explicit_throughput)
    {
        const std::string values = throughput_arg.substr(
            std::char_traits<char>::length(k_throughput_option));
        uint32_t intervals[7]{};
        std::size_t cursor = 0u;
        bool complete = true;
        for (uint32_t index = 0u; index < 7u && complete; ++index)
        {
            const std::size_t end = values.find(',', cursor);
            if ((index < 6u && end == std::string::npos) ||
                (index == 6u && end != std::string::npos))
            {
                complete = false;
                break;
            }
            const std::string token = values.substr(
                cursor, end == std::string::npos
                    ? std::string::npos : end - cursor);
            complete = parse_latency(token.c_str(), &intervals[index]);
            cursor = end == std::string::npos ? values.size() : end + 1u;
        }
        if (!complete)
        {
            std::fprintf(stderr,
                "throughput requires seven explicit positive intervals\n");
            return 2;
        }
        profile.m_mac_multiply_interval = intervals[0];
        profile.m_mac_divide_interval = intervals[1];
        profile.m_mac_accumulate_interval = intervals[2];
        profile.m_cop1_arithmetic_interval = intervals[3];
        profile.m_cop1_divide_interval = intervals[4];
        profile.m_cop1_sqrt_interval = intervals[5];
        profile.m_cop1_rsqrt_interval = intervals[6];
        profile.m_require_throughput = true;
    }
    profile.m_allow_ordered_waw_overlap = ordered_waw_overlap;

    std::ifstream input(argv[1]);
    if (!input)
    {
        std::fprintf(stderr, "cannot open issue trace: %s\n", argv[1]);
        return 2;
    }

    std::string line;
    if (!std::getline(input, line))
    {
        std::fprintf(stderr, "empty issue trace\n");
        return 2;
    }
    if (!line.empty() && line.back() == '\r')
    {
        line.pop_back();
    }
    if (line != "attempt,pc,opcode_word,kind,reference_slot,hle_service")
    {
        std::fprintf(stderr, "unsupported issue trace CSV schema\n");
        return 2;
    }

    ee::c_ee_issue_trace_replay replay{};
    if (!replay.configure(profile))
    {
        return 2;
    }

    uint64_t event_count = 0u;
    bool complete_footer = false;
    std::vector<s_candidate_event> candidates;
    while (std::getline(input, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (line.rfind("# truncated=", 0u) == 0u)
        {
            complete_footer = line.rfind(
                "# truncated=false range=", 0u) == 0u;
            break;
        }

        ee::s_ee_instruction_trace_entry entry{};
        if (!parse_entry(line, &entry))
        {
            std::fprintf(stderr, "invalid observation at row %llu\n",
                static_cast<unsigned long long>(event_count + 2u));
            return 3;
        }
        const auto result = replay.observe(entry);
        if (result != ee::e_ee_issue_replay_status::ISSUED &&
            result != ee::e_ee_issue_replay_status::ANNULLED)
        {
            std::fprintf(stderr,
                "unmodeled or incomplete R5900 timing: pc=0x%08x "
                "reference_slot=%llu result=%u "
                "(NO hardware timestamp produced)\n",
                replay.failure_pc(),
                static_cast<unsigned long long>(entry.m_reference_slot),
                static_cast<unsigned>(result));
            return 3;
        }
        if (export_any)
        {
            // Native runner captures at most 65536 observations. Leave
            // generous room for external traces without unbounded memory.
            if (candidates.size() >= 1048576u)
            {
                std::fprintf(stderr, "candidate export event cap exceeded\n");
                return 3;
            }
            candidates.push_back({
                entry.m_reference_slot, entry.m_pc, entry.m_opcode_word,
                entry.m_kind, replay.last_event_candidate_cycle(),
                // Annulled delay slots do not produce a result.
                entry.m_kind == 1u ? 0u
                    : replay.last_candidate_completion_cycle()
            });
        }
        ++event_count;
    }
    if (!complete_footer || std::getline(input, line))
    {
        std::fprintf(stderr,
            "trace is truncated or lacks a complete footer "
            "(NO absolute timing claimed)\n");
        return 3;
    }

    if (export_any)
    {
        FILE* output = std::fopen(output_path.c_str(), "wb");
        if (output == nullptr)
        {
            std::fprintf(stderr, "cannot open output: %s\n", output_path.c_str());
            return 3;
        }
        if (export_completions)
        {
            std::fprintf(output,
                "reference_slot,pc,opcode_word,kind,"
                "candidate_issue_cycle,candidate_completion_cycle\n");
        }
        else
        {
            std::fprintf(output,
                "reference_slot,pc,opcode_word,kind,candidate_issue_cycle\n");
        }
        for (const s_candidate_event& item : candidates)
        {
            std::fprintf(output, "%llu,0x%08x,0x%08x,%u,%llu",
                static_cast<unsigned long long>(item.m_slot),
                item.m_pc, item.m_opcode, item.m_kind,
                static_cast<unsigned long long>(item.m_cycle));
            if (export_completions && item.m_kind != 1u)
            {
                std::fprintf(output, ",%llu",
                    static_cast<unsigned long long>(
                        item.m_completion_cycle));
            }
            else if (export_completions)
            {
                // An annulled slot has no result or retirement event.
                std::fprintf(output, ",");
            }
            std::fprintf(output, "\n");
        }
        const bool written = std::ferror(output) == 0;
        const bool closed = std::fclose(output) == 0;
        if (!written || !closed)
        {
            std::remove(output_path.c_str());
            std::fprintf(stderr, "failed to write complete candidate timeline\n");
            return 3;
        }
    }

    std::printf(
        "candidate_issue_cycles=%llu reached=%llu annulled=%llu "
        "events=%llu profile_integer=%u profile_load=%u "
        "profile_source=%s throughput_mode=%s ordered_waw_overlap=%s "
        "last_candidate_completion=%llu max_candidate_completion=%llu "
        "(EXPERIMENTAL; NOT validated R5900 cycles; NO GS events)\n",
        static_cast<unsigned long long>(replay.candidate_cycles()),
        static_cast<unsigned long long>(replay.executed_instructions()),
        static_cast<unsigned long long>(replay.annulled_slots()),
        static_cast<unsigned long long>(event_count),
        profile.m_integer_latency, profile.m_load_latency,
        manual_profile ? "ee_manual_v6_cache_hit" : "caller",
        profile.m_require_throughput ? "explicit" : "unconfigured",
        profile.m_allow_ordered_waw_overlap ? "enabled" : "disabled",
        static_cast<unsigned long long>(
            replay.last_candidate_completion_cycle()),
        static_cast<unsigned long long>(
            replay.max_candidate_completion_cycle())
    );
    const auto& breakdown = replay.cycle_breakdown();
    std::printf(
        "candidate_single_issue_cycles=%llu candidate_dual_issue_cycles=%llu "
        "candidate_annulled_cycles=%llu candidate_register_idle_cycles=%llu "
        "candidate_unit_idle_cycles=%llu candidate_hle_cycles=%llu\n",
        static_cast<unsigned long long>(breakdown.m_single_issue),
        static_cast<unsigned long long>(breakdown.m_dual_issue),
        static_cast<unsigned long long>(breakdown.m_annulled),
        static_cast<unsigned long long>(breakdown.m_register_idle),
        static_cast<unsigned long long>(breakdown.m_unit_idle),
        static_cast<unsigned long long>(breakdown.m_hle));
    return 0;
}
