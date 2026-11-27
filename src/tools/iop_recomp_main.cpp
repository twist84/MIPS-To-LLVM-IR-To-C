// iop_recomp: ahead-of-time translate captured IOP IRX modules to LLVM IR.
//
// The game runner writes CAPTURE_DIR with SDBZ_IOP_AOT_CAPTURE=DIR: for each
// loaded IRX, NAME@BASE.bin holds its relocated, import-linked .text exactly
// as in IOP RAM and NAME@BASE.json its metadata. Translating the linked
// bytes means relocations, import stubs and the load base are already
// applied. At runtime a module activates only when IOP RAM hashes to the
// same bytes (src/iop/iop_aot.cpp); otherwise the interpreter runs it.
//
// Output: OUT_DIR/iop_mod_N.ll (one per module) and OUT_DIR/iop_table.ll.
// tools/build_iop_aot.py compiles and links them into a host library.

#include "iop_aot.h"
#include "iop_ir_lifter.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
bool json_string(const std::string& text, const std::string& key, std::string* value)
{
    const std::string marker = "\"" + key + "\": \"";
    const size_t start = text.find(marker);
    if (start == std::string::npos)
    {
        return false;
    }
    const size_t begin = start + marker.size();
    const size_t end = text.find('"', begin);
    if (end == std::string::npos)
    {
        return false;
    }
    *value = text.substr(begin, end - begin);
    return true;
}

bool json_number(const std::string& text, const std::string& key, uint64_t* value)
{
    const std::string marker = "\"" + key + "\": ";
    const size_t start = text.find(marker);
    if (start == std::string::npos)
    {
        return false;
    }
    char* end = nullptr;
    *value = std::strtoull(text.c_str() + start + marker.size(), &end, 10);
    return end != text.c_str() + start + marker.size();
}

bool read_file(const std::filesystem::path& path, std::string* bytes)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return false;
    }
    bytes->assign(std::istreambuf_iterator<char>(input), {});
    return true;
}

bool write_file(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    return static_cast<bool>(output);
}
// Private plan format emitted from a freshly hash-validated capture.
bool read_regions(const std::filesystem::path& path, uint64_t base,
                  uint64_t size, uint64_t hash,
                  std::vector<std::vector<uint32_t>>* regions)
{
    std::ifstream input(path);
    std::string magic;
    uint64_t plan_base = 0u, plan_size = 0u, plan_hash = 0u, count = 0u;
    if (!(input >> magic >> plan_base >> plan_size >> plan_hash >> count) ||
        magic != "IOPREG1" || base != plan_base || size != plan_size ||
        hash != plan_hash || count == 0u || count > size / 4u)
    {
        return false;
    }
    uint64_t remaining = size / 4u;
    for (uint64_t i = 0u; i < count; ++i)
    {
        uint64_t length = 0u;
        if (!(input >> length) || length == 0u || length > remaining)
        {
            return false;
        }
        std::vector<uint32_t> region;
        for (uint64_t j = 0u; j < length; ++j)
        {
            uint64_t pc = 0u;
            if (!(input >> pc) || pc > UINT32_MAX)
            {
                return false;
            }
            region.push_back(static_cast<uint32_t>(pc));
        }
        regions->push_back(std::move(region));
        remaining -= length;
    }
    std::string trailing;
    return !(input >> trailing) && input.eof();
}
} // namespace

int main(int argc, char** argv)
{
    const bool split = argc == 4;
    if (argc != 3 && !split)
    {
        std::fprintf(stderr, "usage: %s CAPTURE_DIR OUT_DIR [REGION_PLAN]\n", argv[0]);
        return 2;
    }
    const std::filesystem::path capture = argv[1];
    const std::filesystem::path output = argv[2];
    std::error_code error;
    std::filesystem::create_directories(output, error);

    std::vector<std::filesystem::path> metas;
    for (const auto& entry : std::filesystem::directory_iterator(capture, error))
    {
        if (entry.path().extension() == ".json")
        {
            metas.push_back(entry.path());
        }
    }
    std::sort(metas.begin(), metas.end());
    if (metas.empty())
    {
        std::fprintf(stderr, "iop_recomp: no captured modules in %s\n", argv[1]);
        return 1;
    }

    if (split && metas.size() != 1u)
    {
        std::fprintf(stderr, "iop_recomp: split pilot requires exactly one capture\n");
        return 1;
    }

#if defined(_WIN32)
    const bool windows_exports = true;
#else
    const bool windows_exports = false;
#endif
    size_t modules = 0u;
    uint64_t instructions = 0u;
    for (const auto& meta_path : metas)
    {
        std::string meta;
        std::string bytes;
        std::filesystem::path bin = meta_path;
        bin.replace_extension(".bin");
        uint64_t base = 0u;
        std::string name;
        std::string hash_text;
        if (!read_file(meta_path, &meta) || !read_file(bin, &bytes) ||
            !json_string(meta, "name", &name) ||
            !json_number(meta, "base", &base) ||
            !json_string(meta, "hash", &hash_text))
        {
            std::fprintf(stderr, "iop_recomp: unreadable capture %s\n",
                         meta_path.string().c_str());
            return 1;
        }
        iop::s_iop_ir_module module;
        module.m_name = name;
        module.m_base = static_cast<uint32_t>(base);
        module.m_text.assign(bytes.begin(), bytes.end());
        module.m_text.resize(module.m_text.size() & ~size_t(3));
        const uint64_t hash = iop::iop_aot_hash(module.m_text.data(),
                                                module.m_text.size());
        if (std::strtoull(hash_text.c_str(), nullptr, 16) !=
            iop::iop_aot_hash(reinterpret_cast<const uint8_t*>(bytes.data()),
                              bytes.size()))
        {
            std::fprintf(stderr, "iop_recomp: capture hash mismatch for %s\n",
                         meta_path.string().c_str());
            return 1;
        }
        // One library per IRX: STEM.ll (code) + STEM.table.ll (exports),
        // where STEM is the capture name NAME@BASE.
        const std::string stem = meta_path.stem().string();
        const std::string symbol = "iop_aot_fn";
        const std::filesystem::path ll = output / (stem + ".ll");
        const std::filesystem::path table_ll = output / (stem + ".table.ll");
        std::string entry_ir;
        if (split)
        {
            std::vector<std::vector<uint32_t>> regions;
            std::vector<std::string> code;
            if (base > UINT32_MAX || bytes.size() != module.m_text.size() ||
                !read_regions(argv[3], base, bytes.size(), hash, &regions) ||
                !iop::iop_lift_regions_ir(module, regions, symbol, &code, &entry_ir))
            {
                std::fprintf(stderr, "iop_recomp: refused split ownership plan\n");
                return 1;
            }
            for (size_t i = 0u; i < code.size(); ++i)
            {
                if (!write_file(output / (stem + ".region." + std::to_string(i) + ".ll"),
                                code[i]))
                {
                    return 1;
                }
            }
            std::printf("iop_recomp: %s: %zu split regions\n", name.c_str(), code.size());
        }
        else
        {
            entry_ir = iop::iop_lift_module_ir(module, symbol);
        }
        if (!write_file(ll, entry_ir) ||
            !write_file(table_ll, iop::iop_table_module_ir(
                {{name, module.m_base, static_cast<uint32_t>(module.m_text.size()),
                  hash, symbol}}, windows_exports)))
        {
            std::fprintf(stderr, "iop_recomp: cannot write %s\n", ll.string().c_str());
            return 1;
        }
        ++modules;
        instructions += module.m_text.size() / 4u;
        std::printf("iop_recomp: %s at 0x%08x, %zu instructions -> %s\n",
                    name.c_str(), module.m_base, module.m_text.size() / 4u,
                    ll.filename().string().c_str());
    }
    std::printf("iop_recomp: %zu module(s), %llu instructions\n", modules,
                static_cast<unsigned long long>(instructions));
    return 0;
}
