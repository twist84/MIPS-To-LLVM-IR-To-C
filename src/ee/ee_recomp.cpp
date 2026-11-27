#include "ee_recomp.h"
#include "ee_instruction.h"
#include "ee_isa_inventory.h"
#include "ee_opcode.h"
#include "ee_decoder.h"
#include "ee_runtime.h"
#include "ee_state.h"
#include "elf_loader.h"
#include "function_analyzer.h"
#include "instruction_lifter.h"
#include "llvm_context.h"
#include "module_builder.h"

#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>
#include <vector>

namespace ee {

namespace
{
std::string sanitize_name(const std::string& name)
{
    std::string out;
    out.reserve(name.size());
    for (char c : name)
    {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '.')
        {
            out.push_back(c);
        }
        else
        {
            out.push_back('_');
        }
    }
    if (out.empty()) out = "fn";
    return out;
}

std::string json_escape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

bool opcode_supported(const s_ee_instruction& ins)
{
    if (ins.valid != e_ee_valid::VALID) return false;
    s_lifted_op op = lift(ins);
    return op.kind != LOP_UNSUPPORTED;
}

std::string u32hex(uint32_t v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%x", v);
    return buf;
}

std::string native_function_symbol(uint32_t guest_entry)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "ee_fn_%08x", guest_entry);
    return buf;
}


// A full discovery produces an immutable snapshot of decoded PCs and
// function boundaries. Re-emitting one guest function should not repeat
// fixed-point JALR discovery and thousands of unrelated CFG analyses.
// Cache files are local binary artifacts, tied to the exact input ELF.
// They are never accepted on a fingerprint or format mismatch.
uint64_t elf_fingerprint(const char* path)
{
    std::FILE* input = std::fopen(path, "rb");
    if (!input)
    {
        return 0;
    }

    uint64_t hash = UINT64_C(14695981039346656037);
    unsigned char buffer[65536];
    size_t count = 0;
    while ((count = std::fread(buffer, 1, sizeof(buffer), input)) != 0)
    {
        for (size_t i = 0; i < count; ++i)
        {
            hash ^= buffer[i];
            hash *= UINT64_C(1099511628211);
        }
    }
    const bool valid = !std::ferror(input);
    std::fclose(input);
    return valid ? hash : 0;
}

template<typename t_value>
bool cache_write(std::FILE* output, const t_value& value)
{
    return std::fwrite(&value, sizeof(value), 1, output) == 1;
}

template<typename t_value>
bool cache_read(std::FILE* input, t_value* value)
{
    return std::fread(value, sizeof(*value), 1, input) == 1;
}

bool write_discovery_cache(
    const std::string& path,
    uint64_t fingerprint,
    const std::map<uint32_t, s_ee_instruction>& instructions,
    const std::vector<uint32_t>& entries,
    const std::set<uint32_t>& jalr_targets,
    const std::set<uint32_t>& jalr_only,
    const std::vector<s_static_jalr_site>& sites)
{
    const std::string temporary = path + ".tmp";
    std::FILE* output = std::fopen(temporary.c_str(), "wb");
    if (!output)
    {
        return false;
    }

    const char magic[8] = { 'E', 'E', 'I', 'D', 'X', 'C', '0', '2' };
    const uint32_t insn_count = static_cast<uint32_t>(instructions.size());
    const uint32_t entry_count = static_cast<uint32_t>(entries.size());
    const uint32_t jalr_count = static_cast<uint32_t>(jalr_targets.size());
    const uint32_t jalr_only_count = static_cast<uint32_t>(jalr_only.size());
    const uint32_t site_count = static_cast<uint32_t>(sites.size());

    bool ok = std::fwrite(magic, 1, sizeof(magic), output) == sizeof(magic)
        && cache_write(output, fingerprint)
        && cache_write(output, insn_count)
        && cache_write(output, entry_count)
        && cache_write(output, jalr_count)
        && cache_write(output, jalr_only_count)
        && cache_write(output, site_count);

    for (const auto& kv : instructions)
    {
        ok = ok && cache_write(output, kv.first)
            && cache_write(output, kv.second.opcode_word);
    }
    for (uint32_t entry : entries)
    {
        ok = ok && cache_write(output, entry);
    }
    for (uint32_t target : jalr_targets)
    {
        ok = ok && cache_write(output, target);
    }
    for (uint32_t target : jalr_only)
    {
        ok = ok && cache_write(output, target);
    }
    for (const s_static_jalr_site& site : sites)
    {
        const uint32_t source_register = site.m_source_register;
        const uint32_t source_known = site.m_source_known ? 1u : 0u;
        const uint32_t target_aligned = site.m_target_aligned ? 1u : 0u;
        ok = ok && cache_write(output, site.m_caller_entry)
            && cache_write(output, site.m_pc)
            && cache_write(output, source_register)
            && cache_write(output, source_known)
            && cache_write(output, site.m_source_value)
            && cache_write(output, site.m_target)
            && cache_write(output, target_aligned);
    }

    if (std::fclose(output) != 0)
    {
        ok = false;
    }
    std::error_code ec;
    if (ok)
    {
        // Older cache files may exist after a new full translation.
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(temporary, path, ec);
        ok = !ec;
    }
    if (!ok)
    {
        std::filesystem::remove(temporary, ec);
    }
    return ok;
}

bool read_discovery_cache(
    const std::string& path,
    uint64_t expected_fingerprint,
    std::map<uint32_t, s_ee_instruction>* instructions,
    std::vector<uint32_t>* entries,
    std::set<uint32_t>* jalr_targets,
    std::set<uint32_t>* jalr_only,
    std::vector<s_static_jalr_site>* sites)
{
    std::FILE* input = std::fopen(path.c_str(), "rb");
    if (!input)
    {
        return false;
    }

    char magic[8] = {};
    uint64_t fingerprint = 0;
    uint32_t insn_count = 0;
    uint32_t entry_count = 0;
    uint32_t jalr_count = 0;
    uint32_t jalr_only_count = 0;
    uint32_t site_count = 0;
    bool ok = std::fread(magic, 1, sizeof(magic), input) == sizeof(magic)
        && std::memcmp(magic, "EEIDXC02", 8) == 0
        && cache_read(input, &fingerprint)
        && fingerprint == expected_fingerprint
        && cache_read(input, &insn_count)
        && cache_read(input, &entry_count)
        && cache_read(input, &jalr_count)
        && cache_read(input, &jalr_only_count)
        && cache_read(input, &site_count)
        && insn_count <= 8000000
        && entry_count <= 8000000
        && jalr_count <= 8000000
        && jalr_only_count <= 8000000
        && site_count <= 8000000;

    if (ok)
    {
        for (uint32_t i = 0; i < insn_count && ok; ++i)
        {
            uint32_t pc = 0;
            uint32_t word = 0;
            ok = cache_read(input, &pc) && cache_read(input, &word)
                && (pc & 3u) == 0
                && instructions->emplace(pc, decode(pc, word)).second;
        }
        for (uint32_t i = 0; i < entry_count && ok; ++i)
        {
            uint32_t entry = 0;
            ok = cache_read(input, &entry);
            if (ok)
            {
                entries->push_back(entry);
            }
        }
        for (uint32_t i = 0; i < jalr_count && ok; ++i)
        {
            uint32_t target = 0;
            ok = cache_read(input, &target);
            if (ok)
            {
                jalr_targets->insert(target);
            }
        }
        for (uint32_t i = 0; i < jalr_only_count && ok; ++i)
        {
            uint32_t target = 0;
            ok = cache_read(input, &target);
            if (ok)
            {
                jalr_only->insert(target);
            }
        }
        for (uint32_t i = 0; i < site_count && ok; ++i)
        {
            s_static_jalr_site site;
            uint32_t source_register = 0;
            uint32_t source_known = 0;
            uint32_t target_aligned = 0;
            ok = cache_read(input, &site.m_caller_entry)
                && cache_read(input, &site.m_pc)
                && cache_read(input, &source_register)
                && cache_read(input, &source_known)
                && cache_read(input, &site.m_source_value)
                && cache_read(input, &site.m_target)
                && cache_read(input, &target_aligned)
                && source_register < 32
                && source_known <= 1
                && target_aligned <= 1;
            if (ok)
            {
                site.m_source_register = static_cast<uint8_t>(source_register);
                site.m_source_known = source_known != 0;
                site.m_target_aligned = target_aligned != 0;
                sites->push_back(site);
            }
        }
        ok = ok && std::fgetc(input) == EOF && !std::ferror(input)
            && std::is_sorted(entries->begin(), entries->end())
            && std::adjacent_find(entries->begin(), entries->end()) == entries->end();
    }
    std::fclose(input);
    return ok;
}

// Address-taken function seeding for ahead-of-time coverage. Function
// pointer tables (CRI SFD/ADX module tables, callbacks, vtables) and
// lui/addiu-materialized code addresses are otherwise found only when the
// game first calls them, through live cache-miss expansion. Retail SDBZ:
// 1559 of 1713 runtime-only entries were pointer words, 201 lui/addiu.
//
// A candidate must look like a function start (preceded by "jr ra; delay"
// or a J tail call, allowing zero padding, or beginning with a stack
// prologue) and must decode as a defined instruction stream up to its
// first "jr ra" delay slot. False positives only cost compile time: a
// translated entry is dispatched only when the guest actually jumps there.
bool looks_like_function_start(const elf_loader& loader, uint32_t pc)
{
    uint32_t first = 0;
    if ((pc & 3u) != 0u || !loader.is_executable_address(pc) ||
        !loader.read32_at(pc, &first) || first == 0u)
    {
        return false;
    }
    const bool prologue =
        ((first >> 16) == 0x27bdu || (first >> 16) == 0x67bdu) &&
        (first & 0x8000u) != 0u;
    bool after_return = false;
    uint32_t previous = pc;
    for (uint32_t skipped = 0; skipped <= 4u && previous >= 8u; ++skipped)
    {
        previous -= 4u;
        uint32_t word = 0;
        if (!loader.read32_at(previous, &word))
        {
            break;
        }
        if (word == 0u)
        {
            continue;
        }
        // "previous" is the last non-padding word: the delay slot of the
        // preceding function's return or tail jump.
        uint32_t control = 0;
        after_return = loader.read32_at(previous - 4u, &control) &&
            (control == 0x03e00008u || (control >> 26) == 0x02u);
        break;
    }
    if (!prologue && !after_return)
    {
        return false;
    }
    for (uint32_t offset = 0; offset < 4096u * 4u; offset += 4u)
    {
        uint32_t word = 0;
        if (!loader.read32_at(pc + offset, &word))
        {
            return false;
        }
        const s_ee_instruction ins = decode(pc + offset, word);
        if (ins.valid == e_ee_valid::INVALID)
        {
            return false;
        }
        if (word == 0x03e00008u)
        {
            uint32_t delay = 0;
            return loader.read32_at(pc + offset + 4u, &delay) &&
                decode(pc + offset + 4u, delay).valid != e_ee_valid::INVALID;
        }
    }
    return false;
}

std::set<uint32_t> collect_address_taken_entries(const elf_loader& loader)
{
    std::set<uint32_t> candidates;
    for (const auto& segment : loader.segments())
    {
        const uint32_t words = segment.filesz / 4u;
        uint32_t high_value[32] = {};
        uint32_t high_at[32] = {};
        bool high_valid[32] = {};
        for (uint32_t index = 0; index < words; ++index)
        {
            const uint32_t at = segment.vaddr + index * 4u;
            uint32_t word = 0;
            std::memcpy(&word, segment.data.data() + index * 4u, 4u);
            // Aligned pointer words anywhere in the loaded image.
            if ((word & 3u) == 0u && loader.is_executable_address(word))
            {
                candidates.insert(word);
            }
            // lui/addiu (or ori) pairs within 16 instructions.
            const uint32_t op = word >> 26;
            const uint32_t rs = (word >> 21) & 31u;
            const uint32_t rt = (word >> 16) & 31u;
            if (op == 0x0fu)
            {
                high_value[rt] = (word & 0xffffu) << 16;
                high_at[rt] = at;
                high_valid[rt] = true;
            }
            else if ((op == 0x09u || op == 0x0du) && high_valid[rs] &&
                     at - high_at[rs] <= 64u)
            {
                uint32_t low = word & 0xffffu;
                if (op == 0x09u && (low & 0x8000u) != 0u)
                {
                    low |= 0xffff0000u;
                }
                const uint32_t value = high_value[rs] + low;
                if ((value & 3u) == 0u && loader.is_executable_address(value))
                {
                    candidates.insert(value);
                }
            }
        }
    }
    std::set<uint32_t> entries;
    for (uint32_t candidate : candidates)
    {
        if (looks_like_function_start(loader, candidate))
        {
            entries.insert(candidate);
        }
    }
    return entries;
}

} // namespace

struct s_ee_recomp_session_state
{
    elf_loader m_loader;
    std::string m_elf_path;
    std::string m_cache_path;
    uint64_t m_fingerprint = 0;
    bool m_snapshot_loaded = false;
    std::map<uint32_t, s_ee_instruction> m_insns;
    std::vector<uint32_t> m_entries;
    std::set<uint32_t> m_jalr_targets;
    std::set<uint32_t> m_jalr_only;
    std::vector<s_static_jalr_site> m_sites;
};

static bool ee_recomp_impl(const char* elf_path,
                          const char* output_dir,
                          s_recompiled_elf* result,
                          std::string* error,
                          const s_recomp_options& opts,
                          s_ee_recomp_session_state* session)
{
    if (opts.ir_jobs == 0 || opts.ir_jobs > 32)
    {
        if (error)
        {
            *error = "IR worker count must be in 1..32";
        }
        return false;
    }
    const auto started = std::chrono::steady_clock::now();
    std::error_code ec;
    std::filesystem::create_directories(output_dir, ec);

    elf_loader local_loader;
    elf_loader& loader = session ? session->m_loader : local_loader;
    if (!session || session->m_elf_path.empty())
    {
        if (!loader.load_file(elf_path, error))
        {
            return false;
        }
        if (session)
        {
            session->m_elf_path = elf_path;
        }
    }
    else if (session->m_elf_path != elf_path ||
             session->m_cache_path.empty() ||
             session->m_cache_path != opts.discovery_cache_path)
    {
        if (error)
        {
            *error = "reconstruction session ELF/discovery identity changed";
        }
        return false;
    }

    result->path = elf_path;
    result->entry_point = loader.entry_point();
    result->machine = loader.machine();
    result->segment_count = (uint32_t)loader.segments().size();

    if (!opts.only_functions.empty() &&
        (opts.only_function != 0u || opts.max_functions != 0u))
    {
        if (error)
        {
            *error = "batched --functions conflicts with single/capped emission";
        }
        return false;
    }
    const std::vector<uint32_t> requested_functions =
        !opts.only_functions.empty()
            ? opts.only_functions
            : (opts.only_function != 0u
                ? std::vector<uint32_t>{opts.only_function}
                : std::vector<uint32_t>{});
    for (uint32_t entry : requested_functions)
    {
        if (entry == 0u || (entry & 3u) != 0u ||
            !loader.is_executable_address(entry))
        {
            if (error)
            {
                *error = "requested function address is not executable/aligned: "
                    + u32hex(entry);
            }
            return false;
        }
    }
    if (requested_functions.size() >
        std::set<uint32_t>(requested_functions.begin(),
                           requested_functions.end()).size())
    {
        if (error)
        {
            *error = "duplicate requested function entry";
        }
        return false;
    }

    // ------------------------------------------------------------------
    // Worklist-based code discovery: start from entry points and follow
    // control-flow edges. Only decode instructions that are actually
    // reachable from a function entry — data embedded in executable
    // segments is never decoded, so it never pollutes coverage.
    // ------------------------------------------------------------------
    std::map<uint32_t, s_ee_instruction> global_insns;
    std::vector<uint32_t> entries;
    std::set<uint32_t> static_jalr_targets;
    std::set<uint32_t> jalr_only_entries;
    std::vector<s_static_jalr_site> jalr_site_audit;

    if (opts.discovery_cache_path.empty())
    {
    std::set<uint32_t> worklist;
    std::set<uint32_t> discovered;

    auto seed_entry = [&](uint32_t ep) {
        if (ep != 0 && loader.is_executable_address(ep)) {
            worklist.insert(ep);
        }
    };
    seed_entry(loader.entry_point());
    for (uint32_t entry : requested_functions)
    {
        seed_entry(entry);
    }
    for (const auto& sym : loader.symbols())
    {
        if (sym.is_function && !sym.is_undefined && sym.value != 0)
            seed_entry(sym.value);
    }

    auto decode_worklist = [&]() {
        while (!worklist.empty())
        {
            const uint32_t pc = *worklist.begin();
            worklist.erase(worklist.begin());
            if (!discovered.insert(pc).second) continue;

            uint32_t word = 0;
            if (!loader.read32_at(pc, &word)) continue;

            const s_ee_instruction ins = decode(pc, word);
            global_insns[pc] = ins;

            const std::set<uint32_t> successors = collect_successors(ins);
            worklist.insert(successors.begin(), successors.end());
        }
    };
    decode_worklist();

    // Seed entries from the ELF entry point and all defined STT_FUNC symbols
    // in executable segments. Calls discovered below are added to this set.
    if (loader.entry_point() != 0 &&
        loader.is_executable_address(loader.entry_point())) {
        entries.push_back(loader.entry_point());
    }
    for (const auto& sym : loader.symbols())
    {
        if (sym.is_function && !sym.is_undefined && sym.value != 0 &&
            loader.is_executable_address(sym.value)) {
            entries.push_back(sym.value);
        }
    }

    std::sort(entries.begin(), entries.end());
    entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
    std::set<uint32_t> entry_set(entries.begin(), entries.end());

    // Seed address-taken functions so they are compiled ahead of time
    // instead of being discovered by live cache-miss expansion.
    {
        const std::set<uint32_t> address_taken =
            collect_address_taken_entries(loader);
        size_t added = 0;
        for (uint32_t target : address_taken)
        {
            if (entry_set.insert(target).second)
            {
                entries.push_back(target);
                worklist.insert(target);
                ++added;
            }
        }
        std::sort(entries.begin(), entries.end());
        decode_worklist();
        std::printf("  address-taken entries: %zu candidates, %zu new\n",
                    address_taken.size(), added);
    }

    // Iterate to a fixed point. Direct JAL/branch-and-link targets are
    // harvested as before. JALR targets are added only when CFG constant
    // propagation proves the target register has one value on every path.
    // Newly decoded callees may themselves contain additional indirect calls.
    bool added_entries = true;
    while (added_entries)
    {
        added_entries = false;
        std::set<uint32_t> direct_call_targets;
        std::set<uint32_t> call_targets;

        for (const auto& kv : global_insns)
        {
            const s_ee_instruction& ins = kv.second;
            const bool direct_jal =
                ins.flow == e_ee_flow::FLOW_CALL &&
                ins.opcode == e_ee_opcode::OP_JAL;
            const bool regimm_link =
                ins.valid == e_ee_valid::VALID &&
                ins.opcode == e_ee_opcode::OP_REGIMM &&
                (ins.rt == static_cast<uint8_t>(e_ee_opcode::REGIMM_BLTZAL) ||
                 ins.rt == static_cast<uint8_t>(e_ee_opcode::REGIMM_BGEZAL) ||
                 ins.rt == static_cast<uint8_t>(e_ee_opcode::REGIMM_BLTZALL) ||
                 ins.rt == static_cast<uint8_t>(e_ee_opcode::REGIMM_BGEZALL));
            if (!direct_jal && !regimm_link)
            {
                continue;
            }

            const uint32_t target = direct_jal
                ? ins.jump_target
                : static_cast<uint32_t>(ins.branch_target);
            if (target != 0 && loader.is_executable_address(target))
            {
                direct_call_targets.insert(target);
                call_targets.insert(target);
            }
        }

        const std::set<uint32_t> block_starts =
            collect_function_block_starts(global_insns, entries);
        jalr_site_audit.clear();
        const std::set<uint32_t> indirect_targets =
            collect_static_jalr_targets(
                global_insns, entries, entries, &block_starts,
                &jalr_site_audit, &loader
            );
        for (uint32_t target : indirect_targets)
        {
            if (loader.is_executable_address(target))
            {
                static_jalr_targets.insert(target);
                call_targets.insert(target);
            }
        }

        for (uint32_t target : call_targets)
        {
            if (entry_set.insert(target).second)
            {
                entries.push_back(target);
                if (indirect_targets.count(target) != 0 &&
                    direct_call_targets.count(target) == 0)
                {
                    jalr_only_entries.insert(target);
                }
                worklist.insert(target);
                added_entries = true;
            }
        }

        if (added_entries)
        {
            std::sort(entries.begin(), entries.end());
            decode_worklist();
        }
    }

        // The full discovery is expensive, but identical for every
        // single-function translation of the same ELF. Save it once.
        const uint64_t fingerprint = elf_fingerprint(elf_path);
        const std::string cache_path = std::string(output_dir)
            + "/ee_discovery_cache.bin";
        if (fingerprint == 0 ||
            !write_discovery_cache(
                cache_path, fingerprint, global_insns, entries,
                static_jalr_targets, jalr_only_entries, jalr_site_audit))
        {
            std::fprintf(stderr,
                "ee_recomp: warning: discovery snapshot not saved\n");
            if (session)
            {
                if (error)
                {
                    *error = "reconstruction session requires a complete discovery snapshot";
                }
                return false;
            }
        }
        else if (session)
        {
            session->m_cache_path = cache_path;
            session->m_fingerprint = fingerprint;
        }
    }
    else
    {
        if (requested_functions.empty())
        {
            if (error)
            {
                *error = "--discovery-cache requires explicit function entries";
            }
            return false;
        }

        bool loaded = false;
        if (session)
        {
            if (!session->m_snapshot_loaded)
            {
                session->m_snapshot_loaded = read_discovery_cache(
                    opts.discovery_cache_path, session->m_fingerprint,
                    &session->m_insns, &session->m_entries,
                    &session->m_jalr_targets, &session->m_jalr_only,
                    &session->m_sites);
            }
            loaded = session->m_snapshot_loaded;
            if (loaded)
            {
                // Each batch starts from exactly the same immutable snapshot.
                // Never retain dynamic discoveries or entry boundaries.
                global_insns = session->m_insns;
                entries = session->m_entries;
                static_jalr_targets = session->m_jalr_targets;
                jalr_only_entries = session->m_jalr_only;
                jalr_site_audit = session->m_sites;
            }
        }
        else
        {
            const uint64_t fingerprint = elf_fingerprint(elf_path);
            loaded = fingerprint != 0 && read_discovery_cache(
                opts.discovery_cache_path, fingerprint, &global_insns,
                &entries, &static_jalr_targets, &jalr_only_entries,
                &jalr_site_audit);
        }
        if (!loaded)
        {
            if (error)
            {
                *error = "discovery cache missing, stale or invalid; "
                         "rerun without --discovery-cache";
            }
            return false;
        }

        // A dynamic dispatcher entry need not have been reachable from
        // static ELF discovery. Decode newly reached instructions locally,
        // without re-running all-ELF direct/indirect call discovery.
        std::set<uint32_t> pending(
            requested_functions.begin(), requested_functions.end());
        std::set<uint32_t> seen;
        const std::set<uint32_t> known_entries(entries.begin(), entries.end());
        const std::set<uint32_t> selected_entries(
            requested_functions.begin(), requested_functions.end());
        while (!pending.empty())
        {
            const uint32_t pc = *pending.begin();
            pending.erase(pending.begin());
            if (!seen.insert(pc).second ||
                (selected_entries.count(pc) == 0 &&
                 known_entries.count(pc) != 0))
            {
                continue;
            }
            if (global_insns.count(pc) != 0)
            {
                continue;
            }
            uint32_t word = 0;
            if (!loader.read32_at(pc, &word))
            {
                continue;
            }
            const s_ee_instruction ins = decode(pc, word);
            global_insns.emplace(pc, ins);
            for (uint32_t successor : collect_successors(ins))
            {
                if (ins.flow == e_ee_flow::FLOW_CALL &&
                    successor == ins.jump_target)
                {
                    continue;
                }
                if (known_entries.count(successor) == 0)
                {
                    pending.insert(successor);
                }
            }
        }
        std::printf("  discovery cache: %zu decoded PCs, %zu entries\n",
            global_insns.size(), entries.size());
    }

    const auto discovery_done = std::chrono::steady_clock::now();
    const std::vector<uint32_t> discovered_entries = entries;
    // Publish the discovery-derived expected IR file count *before* lengthy
    // emission. The Windows HLE launcher polls this small marker and counts
    // completed .ll files, updating one console line without forwarding the
    // per-function diagnostic log to the screen. The file is advisory only:
    // it does not change translation/discovery or cache provenance.
    {
        const std::filesystem::path progress_path =
            std::filesystem::path(output_dir) / "ee_expected_ll_count.txt";
        const std::filesystem::path temporary = progress_path.string() + ".tmp";
        std::FILE* progress = std::fopen(temporary.string().c_str(), "wb");
        if (progress)
        {
            const bool written = std::fprintf(progress, "%zu\n",
                discovered_entries.size()) > 0;
            const bool closed = std::fclose(progress) == 0;
            if (written && closed)
            {
                std::error_code progress_error;
                std::filesystem::remove(progress_path, progress_error);
                progress_error.clear();
                std::filesystem::rename(
                    temporary, progress_path, progress_error);
                if (progress_error)
                {
                    std::filesystem::remove(temporary, progress_error);
                }
            }
            else
            {
                std::error_code progress_error;
                std::filesystem::remove(temporary, progress_error);
            }
        }
    }
    // A batch shares one immutable discovery and one CFG boundary index.
    // Treat all requested dynamic entries as function boundaries so a
    // sibling's successor blocks cannot be spuriously claimed by another.
    // Single-function mode retains its existing discovery/CFG semantics.
    if (!opts.only_functions.empty())
    {
        entries.insert(entries.end(), requested_functions.begin(),
                       requested_functions.end());
        std::sort(entries.begin(), entries.end());
        entries.erase(std::unique(entries.begin(), entries.end()),
                      entries.end());
    }
    function_analyzer analyzer;
    llvm::llvm_context ctx("ee_game");
    const bool parallel_ir = opts.ir_jobs > 1 && !opts.only_functions.empty();

    // ------------------------------------------------------------------
    // Function-scoped coverage: every PC belonging to a CFG built from a
    // discovered entry. Data regions are never decoded, so they never
    // appear here.
    // ------------------------------------------------------------------
    const std::set<uint32_t> block_starts =
        collect_function_block_starts(global_insns, entries);
    // This index is constant for the full coverage and emission passes.
    // Reuse it across functions rather than rebuilding ~8K tree nodes
    // for each of the thousands of function analyses.
    std::set<uint32_t> stop_entries(entries.begin(), entries.end());
    std::set<uint32_t> function_pcs;
    std::set<uint32_t> analyzed_entries;
    const std::vector<uint32_t> coverage_entries =
        !requested_functions.empty() ? requested_functions : entries;
    for (uint32_t ep : coverage_entries)
    {
        if (!analyzed_entries.insert(ep).second) continue;
        std::vector<s_ee_function> fns =
            analyzer.analyze(
                global_insns, {ep}, entries, &block_starts, &stop_entries
            );
        if (fns.empty()) continue;
        for (const auto& bb : fns[0].blocks)
        {
            for (uint32_t pc = bb.start_pc; pc < bb.end_pc; pc += 4)
            {
                if (global_insns.count(pc))
                    function_pcs.insert(pc);
            }
        }
    }

    uint32_t total_insns = 0, total_supported = 0, total_unsupported = 0;
    c_ee_isa_inventory m2_inventory;
    std::map<uint16_t, std::pair<uint32_t, uint32_t>> coverage;
    std::map<std::tuple<uint16_t, uint16_t, uint16_t>, std::pair<uint32_t, uint32_t>> sub_cov;

    auto record_sub = [&](const s_ee_instruction& ins, bool ok) {
        uint16_t op = (uint16_t)ins.opcode;
        uint16_t a = 0, b = 0;
        switch (ins.opcode) {
        case e_ee_opcode::OP_SPECIAL:
        case e_ee_opcode::OP_MMI:
            a = ins.funct; b = 0; break;
        case e_ee_opcode::OP_REGIMM:
            a = ins.rt; b = 0; break;
        case e_ee_opcode::OP_COP0:
        case e_ee_opcode::OP_COP1:
        case e_ee_opcode::OP_COP2:
            a = ins.rs; b = ins.funct; break;
        default:
            return; // no sub-histogram
        }
        auto& cell = sub_cov[{op, a, b}];
        cell.first++;
        if (ok) cell.second++;
    };

    for (uint32_t pc : function_pcs)
    {
        auto it = global_insns.find(pc);
        if (it == global_insns.end()) continue;
        const s_ee_instruction& ins = it->second;
        total_insns++;
        coverage[(uint16_t)ins.opcode].first++;
        bool ok = opcode_supported(ins);
        m2_inventory.record(ins, ok);
        if (ok) total_supported++; else total_unsupported++;
        coverage[(uint16_t)ins.opcode].second += ok ? 1u : 0u;
        record_sub(ins, ok);
    }

    const auto coverage_done = std::chrono::steady_clock::now();
    std::set<uint32_t> emitted_blocks;
    struct s_ir_job
    {
        s_ee_function m_function;
        std::string m_host_symbol;
        std::string m_path;
    };
    std::vector<s_ir_job> ir_jobs;
    bool serial_write_failed = false;

    auto emit_entry = [&](uint32_t entry) -> bool {
        // Pass full entry set so CFGs stop at other functions (stop-at-other-entry).
        std::vector<s_ee_function> fns =
            analyzer.analyze(
                global_insns, {entry}, entries, &block_starts, &stop_entries
            );
        if (fns.empty()) return false;
        s_ee_function& fn = fns[0];
        fn.name = loader.find_symbol(entry) ?
                  loader.find_symbol(entry)->name : ("sub_" + u32hex(entry));

        // Emit only blocks not emitted for a previous entry.
        bool any_new = false;
        for (const auto& bb : fn.blocks)
        {
            if (!emitted_blocks.insert(bb.start_pc).second) continue;
            any_new = true;
        }
        if (!any_new) return false;

        // Keep the guest display name separate from the native linker symbol.
        // ELF symbol names are not guaranteed to be unique across aliases or
        // stripped/partially-symbolized images; the guest entry VA is.
        const std::string host_symbol = native_function_symbol(entry);
        std::string ir_name = sanitize_name(fn.name) + "_" + u32hex(entry) + ".ll";
        std::string ir_path = std::string(output_dir) + "/" + ir_name;
        if (!parallel_ir)
        {
            llvm::module_builder builder(ctx, opts.opt_level);
            builder.emit_function_cfg(fn, global_insns, host_symbol.c_str());
            if (!ctx.write_ir_to_file(ir_path.c_str()))
            {
                serial_write_failed = true;
                return false;
            }
        }

        s_recompiled_fn rfn;
        rfn.name = fn.name;
        rfn.host_symbol = host_symbol;
        rfn.vaddr = entry;
        rfn.ir_path = ir_path;
        rfn.block_count = (uint32_t)fn.blocks.size();
        rfn.block_pcs.reserve(fn.blocks.size());
        for (const auto& bb : fn.blocks)
            rfn.block_pcs.push_back(bb.start_pc);
        rfn.insn_count = 0;
        rfn.supported_count = 0;
        rfn.unsupported_count = 0;
        constexpr size_t k_max_unsupported_samples = 16u;
        std::vector<s_ee_instruction> unsupported_samples;
        for (const auto& bb : fn.blocks)
        {
            for (uint32_t pc = bb.start_pc; pc < bb.end_pc; pc += 4)
            {
                auto it = global_insns.find(pc);
                if (it == global_insns.end()) continue;
                rfn.insn_count++;
                if (opcode_supported(it->second))
                {
                    ++rfn.supported_count;
                }
                else
                {
                    ++rfn.unsupported_count;
                    if (unsupported_samples.size() < k_max_unsupported_samples)
                    {
                        unsupported_samples.push_back(it->second);
                    }
                }
            }
        }
        if (!fn.blocks.empty()) {
            rfn.size = (fn.blocks.rbegin()->start_pc + 4) - entry;
        }

        result->functions.push_back(std::move(rfn));

        std::printf("  %-48s pc=0x%08x insns=%u supported=%u unsupported=%u blocks=%u\n",
                    result->functions.back().name.c_str(), entry,
                    result->functions.back().insn_count,
                    result->functions.back().supported_count,
                    result->functions.back().unsupported_count,
                    result->functions.back().block_count);
        // These are static decode/lift observations, not evidence that
        // guest execution ever reached the instruction. Retain exact PC
        // and opcode so later boot audits can distinguish classification
        // gaps from runtime blocks without re-inspecting the ELF.
        for (const s_ee_instruction& insn : unsupported_samples)
        {
            const s_lifted_op lifted = lift(insn);
            const char* classification = insn.valid == e_ee_valid::INVALID
                ? "invalid" : (insn.valid == e_ee_valid::UNSUPPORTED
                    ? "unsupported" : "valid_lift_gap");
            std::printf(
                "    not-lowered pc=0x%08x opcode=0x%08x mnemonic=%s "
                "classification=%s lift_unsupported=%u\n",
                insn.pc, insn.opcode_word, insn.mnemonic,
                classification, lifted.kind == LOP_UNSUPPORTED ? 1u : 0u);
        }
        if (result->functions.back().unsupported_count > unsupported_samples.size())
        {
            std::printf("    not-lowered more=%u (details capped at %u per function)\n",
                result->functions.back().unsupported_count -
                    static_cast<uint32_t>(unsupported_samples.size()),
                static_cast<uint32_t>(k_max_unsupported_samples));
        }
        if (parallel_ir)
        {
            ir_jobs.push_back({std::move(fn), host_symbol, ir_path});
        }
        return true;
    };

    result->functions_discovered =
        static_cast<uint32_t>(discovered_entries.size());

    if (!requested_functions.empty())
    {
        for (uint32_t entry : requested_functions)
        {
            // Preserve legacy single-entry behaviour for a dynamic address:
            // its own CFG may be translated without harvesting new callees.
            if (opts.only_functions.empty() &&
                stop_entries.insert(entry).second)
            {
                entries.push_back(entry);
                std::sort(entries.begin(), entries.end());
                entries.erase(std::unique(entries.begin(), entries.end()),
                              entries.end());
            }
            emit_entry(entry);
        }
    }
    else
    {
        for (uint32_t ep : entries)
        {
            if (opts.max_functions != 0 &&
                result->functions.size() >= opts.max_functions)
            {
                std::printf("  (capped at --max-functions=%u; %zu of %u entries emitted)\n",
                            opts.max_functions, result->functions.size(),
                            (unsigned)entries.size());
                break;
            }
            emit_entry(ep);
        }
    }

    if (serial_write_failed)
    {
        if (error)
        {
            *error = "serial IR emission failed";
        }
        return false;
    }
    if (parallel_ir && !ir_jobs.empty())
    {
        // Planning, ownership and result order were established above.
        // Workers only read the frozen instruction map, and each owns its
        // builder/context and a distinct output path. Bound memory by the
        // existing explicit batch size and the selected worker count.
        const auto& instructions = std::as_const(global_insns);
        const auto& jobs = std::as_const(ir_jobs);
        std::atomic<size_t> next{0};
        std::atomic<bool> failed{false};
        const size_t workers = std::min<size_t>(opts.ir_jobs, jobs.size());
        {
            std::vector<std::jthread> threads;
            threads.reserve(workers);
            for (size_t i = 0; i < workers; ++i)
            {
                threads.emplace_back([&]()
                {
                    try
                    {
                        llvm::llvm_context worker_ctx("ee_game");
                        for (; !failed.load();)
                        {
                            const size_t index = next.fetch_add(1);
                            if (index >= jobs.size())
                            {
                                break;
                            }
                            const auto& job = jobs[index];
                            llvm::module_builder builder(worker_ctx, opts.opt_level);
                            builder.emit_function_cfg(job.m_function, instructions,
                                                      job.m_host_symbol.c_str());
                            if (!worker_ctx.write_ir_to_file(job.m_path.c_str()))
                            {
                                failed.store(true);
                            }
                        }
                    }
                    catch (...)
                    {
                        failed.store(true);
                    }
                });
            }
        } // Join every worker before inspecting failure or publishing metadata.
        if (failed.load())
        {
            if (error)
            {
                *error = "parallel IR emission failed";
            }
            return false;
        }
        std::printf("ee_recomp parallel IR: workers=%zu modules=%zu\n",
                    workers, jobs.size());
    }

    const auto emission_done = std::chrono::steady_clock::now();
    const auto milliseconds = [](auto start, auto end) {
        return std::chrono::duration<double, std::milli>(end - start).count();
    };
    std::printf("ee_recomp timings: discovery=%.1fms coverage=%.1fms IR=%.1fms\n",
        milliseconds(started, discovery_done),
        milliseconds(discovery_done, coverage_done),
        milliseconds(coverage_done, emission_done));
    result->function_count = (uint32_t)result->functions.size();
    result->function_scoped_pcs = (uint32_t)function_pcs.size();
    result->total_insns = total_insns;
    result->total_supported = total_supported;
    result->total_unsupported = total_unsupported;
    // Linear sweep is now identical to function-scoped: both only cover
    // actually-decoded code. The old "full segment sweep" no longer exists.
    result->linear_insns = total_insns;
    result->linear_supported = total_supported;
    result->linear_unsupported = total_unsupported;
    for (auto& kv : coverage)
        result->coverage.push_back({kv.first, kv.second});
    std::sort(result->coverage.begin(), result->coverage.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& kv : sub_cov)
    {
        s_recompiled_elf::s_sub_cov sc;
        sc.opcode = std::get<0>(kv.first);
        sc.sub = std::get<1>(kv.first);
        sc.sub2 = std::get<2>(kv.first);
        sc.total = kv.second.first;
        sc.supported = kv.second.second;
        result->sub_coverage.push_back(sc);
    }
    // Sort sub-coverage by unsupported descending for report convenience.
    std::sort(result->sub_coverage.begin(), result->sub_coverage.end(),
              [](const s_recompiled_elf::s_sub_cov& a,
                 const s_recompiled_elf::s_sub_cov& b) {
                  const uint32_t ua = a.total - a.supported;
                  const uint32_t ub = b.total - b.supported;
                  if (ua != ub) return ua > ub;
                  return a.total > b.total;
              });

    // Write ee_functions.json
    {
        std::ostringstream os;
        os << "{\n";
        os << "  \"elf\": \"" << json_escape(elf_path) << "\",\n";
        os << "  \"entry_point\": \"" << u32hex(result->entry_point) << "\",\n";
        os << "  \"machine\": " << result->machine << ",\n";
        os << "  \"segment_count\": " << result->segment_count << ",\n";
        os << "  \"function_count\": " << result->function_count << ",\n";
        os << "  \"functions_discovered\": " << result->functions_discovered << ",\n";
        os << "  \"scope\": \"function\",\n";
        os << "  \"total_insns\": " << result->total_insns << ",\n";
        os << "  \"total_supported\": " << result->total_supported << ",\n";
        os << "  \"total_unsupported\": " << result->total_unsupported << ",\n";
        os << "  \"linear_insns\": " << result->linear_insns << ",\n";
        os << "  \"linear_supported\": " << result->linear_supported << ",\n";
        os << "  \"linear_unsupported\": " << result->linear_unsupported << ",\n";
        os << "  \"discovered_entries\": [";
        size_t entry_index = 0;
        for (uint32_t entry : discovered_entries)
        {
            if (entry_index++ != 0) os << ", ";
            os << "\"" << u32hex(entry) << "\"";
        }
        os << "],\n";
        os << "  \"static_jalr_targets\": [";
        size_t jalr_index = 0;
        for (uint32_t target : static_jalr_targets)
        {
            if (jalr_index++ != 0) os << ", ";
            os << "\"" << u32hex(target) << "\"";
        }
        os << "],\n";
        os << "  \"jalr_sites\": [\n";
        for (size_t i = 0; i < jalr_site_audit.size(); ++i)
        {
            const auto& site = jalr_site_audit[i];
            const bool target_executable =
                site.m_target_aligned &&
                loader.is_executable_address(site.m_target);
            const char* status = !site.m_source_known
                ? "unknown_source"
                : !site.m_target_aligned
                    ? "invalid_target"
                    : target_executable
                        ? "resolved_executable"
                        : "resolved_non_executable";

            os << "    { \"caller_entry\": \""
               << u32hex(site.m_caller_entry)
               << "\", \"pc\": \"" << u32hex(site.m_pc)
               << "\", \"source_register\": "
               << static_cast<unsigned>(site.m_source_register)
               << ", \"source_known\": "
               << (site.m_source_known ? "true" : "false")
               << ", \"source_value\": "
               << site.m_source_value
               << ", \"target\": \"" << u32hex(site.m_target)
               << "\", \"target_aligned\": "
               << (site.m_target_aligned ? "true" : "false")
               << ", \"target_executable\": "
               << (target_executable ? "true" : "false")
               << ", \"status\": \"" << status << "\" }"
               << (i + 1u < jalr_site_audit.size() ? "," : "") << "\n";
        }
        os << "  ],\n";
        os << "  \"jalr_only_entries\": [";
        size_t jalr_only_index = 0;
        for (uint32_t entry : jalr_only_entries)
        {
            if (jalr_only_index++ != 0) os << ", ";
            os << "\"" << u32hex(entry) << "\"";
        }
        os << "],\n";
        os << "  \"functions\": [\n";
        for (size_t i = 0; i < result->functions.size(); ++i)
        {
            const auto& f = result->functions[i];
            os << "    {\n";
            os << "      \"name\": \"" << json_escape(f.name) << "\",\n";
            os << "      \"host_symbol\": \"" << json_escape(f.host_symbol) << "\",\n";
            os << "      \"vaddr\": \"" << u32hex(f.vaddr) << "\",\n";
            os << "      \"size\": " << f.size << ",\n";
            os << "      \"insn_count\": " << f.insn_count << ",\n";
            os << "      \"supported_count\": " << f.supported_count << ",\n";
            os << "      \"unsupported_count\": " << f.unsupported_count << ",\n";
            os << "      \"block_count\": " << f.block_count << ",\n";
            os << "      \"block_pcs\": [";
            for (size_t j = 0; j < f.block_pcs.size(); ++j)
            {
                if (j != 0) os << ", ";
                os << "\"" << u32hex(f.block_pcs[j]) << "\"";
            }
            os << "],\n";
            os << "      \"ir_path\": \"" << json_escape(f.ir_path) << "\"\n";
            os << "    }" << (i + 1 < result->functions.size() ? "," : "") << "\n";
        }
        os << "  ]\n";
        os << "}\n";
        std::string json_path = std::string(output_dir) + "/ee_functions.json";
        std::FILE* f = std::fopen(json_path.c_str(), "wb");
        if (f) {
            std::fwrite(os.str().data(), 1, os.str().size(), f);
            std::fclose(f);
        }
    }

    // Write ee_coverage.json (function-scoped primary + linear + sub-ops)
    {
        std::ostringstream os;
        os << "{\n";
        os << "  \"scope\": \"function\",\n";
        os << "  \"functions_discovered\": " << result->functions_discovered << ",\n";
        os << "  \"function_scoped_pcs\": " << result->function_scoped_pcs << ",\n";
        os << "  \"total_insns\": " << result->total_insns << ",\n";
        os << "  \"total_supported\": " << result->total_supported << ",\n";
        os << "  \"total_unsupported\": " << result->total_unsupported << ",\n";
        if (result->total_insns)
            os << "  \"supported_pct\": "
               << (100.0 * result->total_supported / result->total_insns) << ",\n";
        os << "  \"linear_insns\": " << result->linear_insns << ",\n";
        os << "  \"linear_supported\": " << result->linear_supported << ",\n";
        os << "  \"linear_unsupported\": " << result->linear_unsupported << ",\n";
        if (result->linear_insns)
            os << "  \"linear_supported_pct\": "
               << (100.0 * result->linear_supported / result->linear_insns) << ",\n";
        os << "  \"m2_instruction_inventory\": ";
        m2_inventory.write_json(os, !requested_functions.empty());
        os << ",\n";
        os << "  \"opcode_coverage\": [\n";
        for (size_t i = 0; i < result->coverage.size(); ++i)
        {
            const auto& c = result->coverage[i];
            os << "    {\"opcode\": " << (uint32_t)c.first
               << ", \"total\": " << c.second.first
               << ", \"supported\": " << c.second.second << "}"
               << (i + 1 < result->coverage.size() ? "," : "") << "\n";
        }
        os << "  ],\n";
        os << "  \"linear_opcode_coverage\": [\n";
        for (size_t i = 0; i < result->linear_coverage.size(); ++i)
        {
            const auto& c = result->linear_coverage[i];
            os << "    {\"opcode\": " << (uint32_t)c.first
               << ", \"total\": " << c.second.first
               << ", \"supported\": " << c.second.second << "}"
               << (i + 1 < result->linear_coverage.size() ? "," : "") << "\n";
        }
        os << "  ],\n";
        os << "  \"sub_coverage\": [\n";
        for (size_t i = 0; i < result->sub_coverage.size(); ++i)
        {
            const auto& s = result->sub_coverage[i];
            os << "    {\"opcode\": " << (uint32_t)s.opcode
               << ", \"sub\": " << (uint32_t)s.sub
               << ", \"sub2\": " << (uint32_t)s.sub2
               << ", \"total\": " << s.total
               << ", \"supported\": " << s.supported << "}"
               << (i + 1 < result->sub_coverage.size() ? "," : "") << "\n";
        }
        os << "  ]\n";
        os << "}\n";
        std::string cov_path = std::string(output_dir) + "/ee_coverage.json";
        std::FILE* f = std::fopen(cov_path.c_str(), "wb");
        if (f) {
            std::fwrite(os.str().data(), 1, os.str().size(), f);
            std::fclose(f);
        }
    }

    // Human-readable markdown report.
    {
        std::ostringstream os;
        const double pct = result->total_insns
            ? 100.0 * result->total_supported / result->total_insns : 0.0;
        const double lpct = result->linear_insns
            ? 100.0 * result->linear_supported / result->linear_insns : 0.0;
        os << "# Opcode coverage — function-scoped\n\n";
        os << "ELF: `" << elf_path << "`\n\n";
        os << "| Metric | Value |\n|--------|------:|\n";
        os << "| Functions discovered | " << result->functions_discovered << " |\n";
        os << "| Unique CFG PCs | " << result->function_scoped_pcs << " |\n";
        os << "| **Function-scoped supported** | ** "
           << result->total_supported << " / " << result->total_insns
           << " (" << pct << "%)** |\n";
        os << "| Linear sweep supported | "
           << result->linear_supported << " / " << result->linear_insns
           << " (" << lpct << "%) |\n\n";

        os << "## Primary opcodes (function-scoped)\n\n";
        os << "| Opcode | Total | Supported | Unsup | Pct |\n";
        os << "|-------:|------:|----------:|------:|----:|\n";
        auto cov_sorted = result->coverage;
        std::sort(cov_sorted.begin(), cov_sorted.end(),
                  [](const auto& a, const auto& b) {
                      return (a.second.first - a.second.second) >
                             (b.second.first - b.second.second);
                  });
        for (const auto& c : cov_sorted)
        {
            uint32_t t = c.second.first, s = c.second.second, u = t - s;
            if (t == 0) continue;
            double p = 100.0 * s / t;
            os << "| 0x" << std::hex << (unsigned)c.first << std::dec
               << " | " << t << " | " << s << " | " << u
               << " | " << p << "% |\n";
        }

        os << "\n## Top unsupported sub-ops (SPECIAL/REGIMM/COP*/MMI)\n\n";
        os << "| Opcode | sub | sub2 | Total | Unsup |\n";
        os << "|-------:|----:|-----:|------:|------:|\n";
        size_t shown = 0;
        for (const auto& s : result->sub_coverage)
        {
            uint32_t u = s.total - s.supported;
            if (u == 0) continue;
            os << "| 0x" << std::hex << (unsigned)s.opcode
               << " | 0x" << (unsigned)s.sub
               << " | 0x" << (unsigned)s.sub2 << std::dec
               << " | " << s.total << " | " << u << " |\n";
            if (++shown >= 40) break;
        }
        std::string md_path = std::string(output_dir) + "/OPCODE_COVERAGE.md";
        std::FILE* f = std::fopen(md_path.c_str(), "wb");
        if (f) {
            std::fwrite(os.str().data(), 1, os.str().size(), f);
            std::fclose(f);
        }
    }

    return true;
}

bool ee_recomp(const char* elf_path, const char* output_dir,
               s_recompiled_elf* result, std::string* error,
               const s_recomp_options& opts)
{
    return ee_recomp_impl(elf_path, output_dir, result, error, opts, nullptr);
}

c_ee_recomp_session::c_ee_recomp_session()
    : m_state(std::make_unique<s_ee_recomp_session_state>())
{
}

c_ee_recomp_session::~c_ee_recomp_session() = default;

bool c_ee_recomp_session::recompile(const char* elf_path, const char* output_dir,
                                    s_recompiled_elf* result, std::string* error,
                                    const s_recomp_options& opts)
{
    return ee_recomp_impl(elf_path, output_dir, result, error, opts, m_state.get());
}

} // namespace ee