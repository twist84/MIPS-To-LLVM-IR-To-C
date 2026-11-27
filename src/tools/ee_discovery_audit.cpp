// Read-only provenance reconstruction from the existing EEIDXC02 snapshot.
// This audit-only executable shares the production translation unit so that
// address-taken classification and cache validation cannot drift. It never
// calls ee_recomp(), emits IR, or changes the production cache format.
#include "../ee/ee_recomp.cpp"

namespace ee
{
int audit_discovery(const char* elf_path, const char* cache_path)
{
    elf_loader loader;
    std::string error;
    if (!loader.load_file(elf_path, &error))
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    std::map<uint32_t, s_ee_instruction> instructions;
    std::vector<uint32_t> entries;
    std::set<uint32_t> indirect_targets;
    std::set<uint32_t> indirect_only;
    std::vector<s_static_jalr_site> sites;
    const uint64_t fingerprint = elf_fingerprint(elf_path);
    if (fingerprint == 0 ||
        !read_discovery_cache(cache_path, fingerprint, &instructions,
            &entries, &indirect_targets, &indirect_only, &sites))
    {
        std::fprintf(stderr, "Invalid or stale EE discovery snapshot\n");
        return 1;
    }

    std::map<uint32_t, std::set<std::string>> sources;
    for (uint32_t entry : entries)
    {
        sources.emplace(entry, std::set<std::string>{});
    }
    auto record = [&](uint32_t pc, const char* source)
    {
        const auto it = sources.find(pc);
        if (it != sources.end())
        {
            it->second.insert(source);
        }
    };

    record(loader.entry_point(), "elf_entry");
    for (const auto& sym : loader.symbols())
    {
        if (sym.is_function && !sym.is_undefined && sym.value != 0 &&
            loader.is_executable_address(sym.value))
        {
            record(sym.value, "stt_func");
        }
    }
    for (uint32_t target : collect_address_taken_entries(loader))
    {
        record(target, "address_taken_heuristic");
    }
    for (uint32_t target : indirect_targets)
    {
        record(target, "static_indirect_call");
    }

    // Match the fixed-point discovery predicate on the final decoded image.
    for (const auto& kv : instructions)
    {
        const auto& ins = kv.second;
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
        if (direct_jal || regimm_link)
        {
            const uint32_t target = direct_jal
                ? ins.jump_target
                : static_cast<uint32_t>(ins.branch_target);
            if (target != 0 && loader.is_executable_address(target))
            {
                record(target, "direct_call");
            }
        }
    }

    std::printf("{\n  \"source\": \"EEIDXC02\",\n  \"entry_sources\": {\n");
    size_t index = 0;
    for (const auto& kv : sources)
    {
        std::printf("    \"%s\": [", u32hex(kv.first).c_str());
        size_t source_index = 0;
        for (const auto& source : kv.second)
        {
            std::printf("%s\"%s\"", source_index++ ? ", " : "", source.c_str());
        }
        std::printf("]%s\n", ++index < sources.size() ? "," : "");
    }
    std::printf("  }\n}\n");
    return 0;
}
} // namespace ee

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::fprintf(stderr, "Usage: ee_discovery_audit ELF ee_discovery_cache.bin\n");
        return 2;
    }
    return ee::audit_discovery(argv[1], argv[2]);
}
