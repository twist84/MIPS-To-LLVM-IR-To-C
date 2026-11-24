#include "ee_sdbz_pad_return_probe.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

int main()
{
    ee::ee_memory memory(8u * 1024u * 1024u);
    ee::c_ee_sdbz_pad_return_probe probe{};
    constexpr uint32_t stack = 0x00020000u;
    constexpr uint32_t result = stack + 0x40u;
    if (probe.observe(0x00109b98u, 1u, stack, 4u, &memory))
    {
        return 1;
    }

    // The original caller combines +2 as the high byte and +3 as
    // the low byte, then complements to recover pressed buttons.
    memory.write8(result + 0u, 0u);
    memory.write8(result + 1u, 0x41u);
    memory.write8(result + 2u, 0xffu);
    memory.write8(result + 3u, 0xffu);
    if (!probe.observe(
            ee::c_ee_sdbz_pad_return_probe::k_return_pc,
            2u, stack, 4u, &memory))
    {
        return 1;
    }

    // The swapped pattern must not be identified as Cross.
    memory.write8(result + 2u, 0xbfu);
    if (!probe.observe(
            ee::c_ee_sdbz_pad_return_probe::k_return_pc,
            3u, stack, 4u, &memory))
    {
        return 1;
    }

    memory.write8(result + 2u, 0xffu);
    memory.write8(result + 3u, 0xbfu);
    if (!probe.observe(probe.k_return_pc, 4u, stack, 32u, &memory) ||
        probe.last_sample().m_pressed_mask != 0x0040u)
    {
        return 1;
    }

    // A failed guest helper status cannot be claimed as an input.
    memory.write8(result + 0u, 1u);
    if (!probe.observe(
            ee::c_ee_sdbz_pad_return_probe::k_return_pc,
            5u, stack, 4u, &memory) ||
        probe.observe(
            ee::c_ee_sdbz_pad_return_probe::k_return_pc,
            6u, 0x02000000u, 4u, &memory) ||
        memory.read8(result + 0u) != 1u ||
        memory.read8(result + 1u) != 0x41u ||
        memory.read8(result + 2u) != 0xffu ||
        memory.read8(result + 3u) != 0xbfu)
    {
        return 1;
    }

    memory.write8(result, 0u);
    memory.write8(result + 3u, 0xffu);
    if (!probe.observe(probe.k_return_pc, 7u, stack, 32u, &memory) ||
        probe.last_sample().m_raw_buttons != 0xffffu)
    {
        return 1;
    }

    // Analog reports distinguish valid neutral axes from missing data.
    memory.write8(result + 1u, 0x79u);
    for (uint32_t i = 4u; i < 8u; ++i)
    {
        memory.write8(result + i, 128u);
    }
    memory.write8(result + 4u, 92u); // -36, inside
    memory.write8(result + 5u, 164u); // +36, inside
    if (!probe.observe(probe.k_return_pc, 8u, stack, 32u, &memory) ||
        !probe.last_sample().m_analog_valid)
    {
        return 1;
    }
    memory.write8(result + 6u, 91u); // -37, outside
    if (!probe.observe(probe.k_return_pc, 9u, stack, 32u, &memory) ||
        probe.last_sample().m_analog[2] != 91u ||
        !probe.observe(probe.k_return_pc, 10u, stack, 4u, &memory) ||
        probe.last_sample().m_analog_valid)
    {
        return 1;
    }

    FILE* output = std::tmpfile();
    if (output == nullptr)
    {
        return 1;
    }
    probe.write_json(output);
    std::fflush(output);
    std::rewind(output);
    char data[4096]{};
    const size_t count = std::fread(
        data, 1u, sizeof(data) - 1u, output);
    const bool closed = std::fclose(output) == 0;
    if (count == 0u || !closed ||
        std::strstr(data, "\"return_entries\":9") == nullptr ||
        std::strstr(data, "\"ram_samples\":8") == nullptr ||
        std::strstr(data, "\"caller_candidate_samples\":7") == nullptr ||
        std::strstr(data, "\"cross_bit_low_candidates\":1") == nullptr ||
        std::strstr(data, "\"first_cross_candidate\":{\"dispatch\":4,") == nullptr ||
        std::strstr(data, "\"raw_buttons\":65471") == nullptr ||
        std::strstr(data, "\"pressed_mask\":64") == nullptr ||
        std::strstr(data, "\"analog_samples\":2") == nullptr ||
        std::strstr(data, "\"analog_outside_deadzone\":1") == nullptr ||
        std::strstr(data, "\"first_analog_outside_deadzone\":{\"dispatch\":9,") == nullptr)
    {
        std::fputs("SDBZ original EE pad return probe regression\n", stderr);
        return 1;
    }

    std::puts("SDBZ original EE pad return read-only probe: PASS");
    return 0;
}
