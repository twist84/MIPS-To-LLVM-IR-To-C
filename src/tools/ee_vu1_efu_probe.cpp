#include "ee_vu1.h"

#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

// One request per line: hexadecimal opcode index, component, and four VF words.
// One response per line: hexadecimal result word and decimal latency.
// Exercise the same EFU helper used by the interpreter and native AOT callback.
int main()
{
    std::string line;
    size_t line_number = 0u;
    int result = 0;
    for (; std::getline(std::cin, line); )
    {
        ++line_number;
        std::istringstream input(line);
        std::array<uint64_t, 6> fields{};
        input >> std::hex;
        for (auto& field : fields)
        {
            input >> field;
        }
        std::string extra;
        if (!input || (input >> extra) || fields[0] > 0xffu || fields[1] > 3u ||
            fields[2] > UINT32_MAX || fields[3] > UINT32_MAX ||
            fields[4] > UINT32_MAX || fields[5] > UINT32_MAX)
        {
            std::cerr << "Invalid request at line " << line_number << '\n';
            result = 1;
            break;
        }
        const std::array<uint32_t, 4> source{
            static_cast<uint32_t>(fields[2]), static_cast<uint32_t>(fields[3]),
            static_cast<uint32_t>(fields[4]), static_cast<uint32_t>(fields[5])};
        uint32_t value = 0u;
        uint32_t latency = 0u;
        if (!ee::c_ee_vu1::efu(source, static_cast<uint32_t>(fields[0]),
                static_cast<uint32_t>(fields[1]), value, latency))
        {
            std::cerr << "Unsupported EFU operation at line " << line_number << '\n';
            result = 1;
            break;
        }
        std::cout << std::hex << std::setfill('0') << std::setw(8) << value
                  << ' ' << std::dec << latency << '\n';
    }
    if (std::cin.bad() || !std::cout)
    {
        result = 1;
    }
    return result;
}
