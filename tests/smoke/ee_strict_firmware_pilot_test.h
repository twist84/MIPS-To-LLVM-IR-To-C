#pragma once

#include "ee_strict_firmware_pilot.h"
#include <initializer_list>

namespace
{
std::vector<uint8_t> make_firmware_pilot_rom(std::initializer_list<uint32_t> words)
{
    std::vector<uint8_t> rom(ee::s_ee_strict_rom0_view::k_size, 0u);
    size_t offset = 0u;
    for (const uint32_t word : words)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            rom[offset++] = static_cast<uint8_t>(word >> (i * 8u));
        }
    }
    return rom;
}

bool test_strict_firmware_pilot()
{
    using boundary = ee::e_ee_firmware_boundary;
    using result = ee::e_ee_reset_prefix_result;
    // Guest writes program Timer0 compare=3 and unmask INTC source 9.
    const auto program = [](uint32_t word48, uint32_t word52)
    {
        return make_firmware_pilot_rom({
            0x40806000u, 0x3c04b000u, 0x34020003u, 0xac820020u,
            0x34020200u, 0x3485f010u, 0xaca20000u, 0x34020180u,
            0xac820010u, 0x10000001u, 0x34030007u, 0u,
            word48, word52, 0x0ff00011u, 0xac820010u, 0u, 0u});
    };
    // Enable occurs at cycle 8, deadline 14. Branch at cycle 9->11.
    for (uint32_t variant = 0u; variant < 3u; ++variant)
    {
        ee::c_ee_strict_firmware_pilot pilot(program(
            variant == 1u ? 0x10000001u : 0u,
            variant == 2u ? 0x10000001u : 0u));
        if (pilot.cycle() != 0u || pilot.retired() != 0u ||
            pilot.source() != ee::e_ee_cycle_source::REFERENCE_COMPATIBILITY ||
            pilot.cpu().m_pc != 0xbfc00000u ||
            pilot.step(0u).m_boundary != boundary::BUDGET)
        {
            return false;
        }
        for (uint32_t i = 0u; i < 9u; ++i)
        {
            const auto action = pilot.step(1u);
            if (action.m_boundary != boundary::RETIRED || action.m_timer_serviced ||
                pilot.cycle() != i + 1u || pilot.retired() != i + 1u)
            {
                return false;
            }
        }
        if (pilot.next_timer_event() != 14u || pilot.intc_mask() != 0x200u ||
            pilot.step(1u).m_boundary != boundary::BUDGET || pilot.cycle() != 9u ||
            pilot.cpu().m_pc != 0xbfc00024u || pilot.cpu().m_gpr[3].m_lo != 0u)
        {
            return false;
        }
        const auto branch = pilot.step(2u);
        if (branch.m_boundary != boundary::RETIRED || branch.m_instruction.m_retired != 2u ||
            pilot.cycle() != 11u || pilot.retired() != 11u ||
            pilot.cpu().m_pc != 0xbfc0002cu || pilot.cpu().m_gpr[3].m_lo != 7u ||
            pilot.intc_status() != 0u || pilot.step(1u).m_boundary != boundary::RETIRED)
        {
            return false;
        }
        if (variant != 1u)
        {
            if (pilot.step(1u).m_boundary != boundary::RETIRED || pilot.cycle() != 13u)
            {
                return false;
            }
        }
        if (variant == 2u)
        {
            // Timer due between branch and slot: no partial retirement,
            // pending interrupt, PC movement or register mutation.
            if (pilot.step(2u).m_boundary != boundary::TIMER_DEADLINE ||
                pilot.cycle() != 13u || pilot.retired() != 13u ||
                pilot.cpu().m_pc != 0xbfc00034u || pilot.intc_status() != 0u ||
                (pilot.timer_mode(0u) & 0x400u) != 0u)
            {
                return false;
            }
            continue;
        }
        const auto event = pilot.step(2u);
        if (event.m_boundary != boundary::RETIRED || !event.m_timer_serviced ||
            event.m_instruction.m_retired != (variant == 1u ? 2u : 1u) ||
            pilot.cycle() != 14u || pilot.retired() != 14u ||
            pilot.intc_status() != 0x200u || pilot.timer_count(0u) != 3u ||
            (pilot.timer_mode(0u) & 0x400u) == 0u ||
            (pilot.cpu().m_cop0[ee::COP0_Cause] & 0x400u) == 0u ||
            pilot.cpu().m_cop0[ee::COP0_Status] != 0u ||
            pilot.cpu().m_pc != 0xbfc00038u)
        {
            return false;
        }
        const auto refused = pilot.step(2u);
        if (refused.m_boundary != boundary::HOST_REFUSAL ||
            refused.m_instruction.m_result != result::DATA_UNAVAILABLE ||
            refused.m_instruction.m_detail_pc != 0xbfc0003cu ||
            pilot.cycle() != 14u || pilot.retired() != 14u ||
            pilot.cpu().m_pc != 0xbfc00038u || pilot.cpu().m_gpr[31].m_lo != 0u ||
            pilot.intc_status() != 0x200u || pilot.timer_count(0u) != 3u)
        {
            return false;
        }
    }

    // ROM ownership, live RAM writes/reads, and unsupported instruction
    // refusal with no clock advancement.
    auto rom = make_firmware_pilot_rom({
        0x40806000u, 0x3c04a000u, 0x34021234u, 0xac820000u,
        0x8c830000u, 0xffffffffu});
    ee::c_ee_strict_firmware_pilot pilot(rom);
    rom[0] = 0xffu;
    for (uint32_t i = 0u; i < 5u; ++i)
    {
        if (pilot.step(1u).m_boundary != boundary::RETIRED)
        {
            return false;
        }
    }
    const auto refused = pilot.step(1u);
    return refused.m_boundary == boundary::HOST_REFUSAL &&
        refused.m_instruction.m_result == result::UNSUPPORTED_OPCODE &&
        pilot.cycle() == 5u && pilot.retired() == 5u &&
        pilot.cpu().m_pc == 0xbfc00014u && pilot.cpu().m_gpr[3].m_lo == 0x1234u &&
        ee::ee_read_instruction_word(pilot.ram().data()) == 0x1234u;
}
} // namespace
