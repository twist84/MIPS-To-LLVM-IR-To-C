#include "ee_strict_rom0.h"
#include "ee_decoder.h"
#include "ee_exceptions.h"
#include <cstdio>
#include <fstream>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: ee_rom0_fetch_probe ROM0.bin\n");
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
    if (!file || file.tellg() != static_cast<std::streamoff>(ee::s_ee_strict_rom0_view::k_size))
    {
        std::fprintf(stderr, "expected exactly 4 MiB ROM0\n");
        return 3;
    }
    std::vector<uint8_t> bytes(ee::s_ee_strict_rom0_view::k_size);
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
    {
        return 3;
    }
    const ee::s_ee_strict_rom0_view rom{bytes.data(), bytes.size()};
    const ee::c_ee_tlb tlb{};
    // Classification only: no full reset initialization or retirement.
    constexpr uint32_t status = ee::STATUS_BEV | ee::STATUS_ERL;
    constexpr uint32_t pcs[] = {0xbfc00000u, ee::VEC_TLB_REFILL_BEV1, ee::VEC_COMMON_BEV1};
    for (const uint32_t pc : pcs)
    {
        const auto fetch = ee::ee_plan_instruction_fetch(tlb, pc, status, 0u, 0u, 0u);
        const uint8_t* code = rom.fetch_span(fetch);
        if (code != bytes.data() + (pc - 0xbfc00000u))
        {
            return 4;
        }
        const uint32_t word = ee::ee_read_instruction_word(code);
        const auto ins = ee::decode(pc, word);
        std::printf("EE ROM0 fetch: pc=%08x physical=%08x word=%08x decoded=%s valid=%u\n",
            pc, fetch.m_access.m_physical_address, word, ins.mnemonic,
            static_cast<unsigned>(ins.valid));
    }
    std::puts("EE ROM0 fetch probe: PASS (no instructions retired)");
    return 0;
}
