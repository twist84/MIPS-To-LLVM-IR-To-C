#include "ee_vu1_ir.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::fprintf(stderr, "usage: vu1_recomp CODE.bin BYTE_PC OUTPUT.ll\n");
        return 2;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long start = std::strtoul(argv[2], &end, 0);
    if (errno != 0 || end == argv[2] || *end != '\0' ||
        start >= ee::c_ee_vu1::k_memory_size || (start & 7u) != 0u)
    {
        return 2;
    }
    std::array<uint8_t, ee::c_ee_vu1::k_memory_size> code{};
    std::ifstream input(argv[1], std::ios::binary);
    if (!input.read(reinterpret_cast<char*>(code.data()), code.size()) ||
        input.peek() != std::char_traits<char>::eof())
    {
        std::fprintf(stderr, "VU1 capture must contain exactly 16 KiB\n");
        return 2;
    }
#if defined(_WIN32)
    constexpr bool windows_exports = true;
#else
    constexpr bool windows_exports = false;
#endif
    const auto ir = ee::vu1_aot_module_ir(code, static_cast<uint32_t>(start),
        windows_exports);
    if (ir.empty())
    {
        std::puts("VU1 program outside current supported control-flow/instruction subset");
        return 3;
    }
    std::ofstream output(argv[3], std::ios::binary);
    output << ir;
    output.close();
    return output ? 0 : 1;
}
