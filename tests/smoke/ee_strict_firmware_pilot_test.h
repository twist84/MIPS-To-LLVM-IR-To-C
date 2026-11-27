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

bool test_strict_firmware_interrupts()
{
    using boundary = ee::e_ee_firmware_boundary;
    using result = ee::e_ee_reset_prefix_result;
    constexpr uint32_t base = 0xbfc00000u;
    constexpr uint32_t bev_vector = 0xbfc00400u;
    constexpr uint32_t normal_vector = 0x80000200u;
    constexpr uint32_t enabled = ee::STATUS_BEV | ee::STATUS_IE | ee::STATUS_EIE | 0x400u;
    for (uint32_t variant = 0u; variant < 7u; ++variant)
    {
        // Timer0 reaches compare at 14 (or 16 in the branch test).
        // Variants: same-boundary Status, already pending, masked source,
        // BEV0, branch completion, unacknowledged ERET, masked CPU IM.
        auto rom = make_firmware_pilot_rom({
            0x40806000u, 0x3c04b000u, variant == 4u ? 0x34020004u : 0x34020003u,
            0xac820020u, variant == 2u ? 0x34020000u : 0x34020200u,
            0x3485f010u, 0xaca20000u, 0x34020180u, 0xac820010u,
            variant == 3u ? 0x3c020001u : 0x3c020041u,
            variant == 6u ? 0x34420001u : 0x34420401u,
            0u, 0u, variant == 1u ? 0u : 0x40826000u,
            variant == 4u ? 0x10000001u : variant == 1u ? 0x40826000u : 0u,
            0x34030007u, 0u});
        const auto put = [&](uint32_t offset, uint32_t word)
        {
            for (uint32_t i = 0u; i < 4u; ++i)
            {
                rom[offset + i] = static_cast<uint8_t>(word >> (8u * i));
            }
        };
        put(0x400u, 0x3485f000u); // ORI a1,a0,INTC_STAT
        put(0x404u, 0x34030200u); // ORI v1,zero,Timer0 source
        put(0x408u, variant == 5u ? 0u : 0xaca30000u); // acknowledge (or deliberately not)
        put(0x40cu, 0x42000018u); // ERET
        ee::c_ee_strict_firmware_pilot pilot(std::move(rom));
        for (uint32_t i = 0u; i < 13u; ++i)
        {
            const auto step = pilot.step(1u);
            if (step.m_boundary != boundary::RETIRED || step.m_interrupt_vector != 0u)
            {
                return false;
            }
        }
        if (pilot.cycle() != 13u || pilot.cpu().m_cop0[ee::COP0_Count] != 13u)
        {
            return false;
        }
        auto step = pilot.step(1u);
        if (step.m_boundary != boundary::RETIRED || pilot.retired() != 14u ||
            step.m_timer_serviced != (variant != 4u))
        {
            return false;
        }
        if (variant == 1u || variant == 4u)
        {
            if (step.m_interrupt_vector != 0u ||
                (variant == 1u && pilot.intc_status() != 0x200u))
            {
                return false;
            }
            step = pilot.step(variant == 4u ? 2u : 1u);
        }
        const uint32_t resume = base + (variant == 4u ? 64u : variant == 1u ? 60u : 56u);
        if (variant == 2u || variant == 6u)
        {
            if (step.m_interrupt_vector != 0u || pilot.cpu().m_pc != resume ||
                (pilot.cpu().m_cop0[ee::COP0_Status] & ee::STATUS_EXL) != 0u ||
                pilot.intc_status() != 0x200u ||
                ((pilot.cpu().m_cop0[ee::COP0_Cause] & 0x400u) != 0u) != (variant == 6u))
            {
                return false;
            }
            continue;
        }
        const uint32_t vector = variant == 3u ? normal_vector : bev_vector;
        const uint32_t expected_status = (variant == 3u ? enabled & ~ee::STATUS_BEV : enabled) | ee::STATUS_EXL;
        if (step.m_boundary != boundary::RETIRED || step.m_interrupt_vector != vector ||
            pilot.cpu().m_pc != vector || pilot.cpu().m_cop0[ee::COP0_EPC] != resume ||
            pilot.cpu().m_cop0[ee::COP0_Status] != expected_status ||
            pilot.cpu().m_cop0[ee::COP0_Cause] != 0x400u ||
            pilot.cpu().m_exit_reason != static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_EXCEPTION) ||
            pilot.retired() != (resume - base) / 4u ||
            (variant == 4u && pilot.cpu().m_gpr[3].m_lo != 7u))
        {
            return false;
        }
        if (variant == 3u)
        {
            // BEV0 is correctly selected, but cached handler execution
            // remains unsupported. Refusal preserves architectural entry.
            if (pilot.step(1u).m_boundary != boundary::HOST_REFUSAL ||
                pilot.cpu().m_pc != vector ||
                pilot.cpu().m_exit_reason != static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_EXCEPTION))
            {
                return false;
            }
            continue;
        }
        for (uint32_t i = 0u; i < 3u; ++i)
        {
            const auto handler = pilot.step(1u);
            if (handler.m_boundary != boundary::RETIRED || handler.m_interrupt_vector != 0u)
            {
                return false;
            }
        }
        const auto eret = pilot.step(1u);
        if (eret.m_boundary != boundary::RETIRED ||
            eret.m_interrupt_vector != (variant == 5u ? bev_vector : 0u) ||
            pilot.cpu().m_pc != (variant == 5u ? bev_vector : resume) ||
            pilot.cpu().m_cop0[ee::COP0_EPC] != resume ||
            pilot.cpu().m_cop0[ee::COP0_Status] != (variant == 5u ? expected_status : enabled) ||
            pilot.intc_status() != (variant == 5u ? 0x200u : 0u))
        {
            return false;
        }
    }
    // Refuse Status/ERET in a JAL slot without leaking link or time.
    for (const uint32_t word : {0x40806000u, 0x42000018u})
    {
        ee::c_ee_strict_firmware_pilot pilot(make_firmware_pilot_rom({
            0x40806000u, 0x0ff00003u, word, 0u}));
        if (pilot.step(1u).m_boundary != boundary::RETIRED)
        {
            return false;
        }
        const auto refused = pilot.step(2u);
        if (refused.m_boundary != boundary::HOST_REFUSAL ||
            refused.m_instruction.m_result != result::UNSUPPORTED_OPCODE ||
            refused.m_instruction.m_detail_pc != base + 8u ||
            pilot.cpu().m_pc != base + 4u || pilot.cpu().m_gpr[31].m_lo != 0u ||
            pilot.cycle() != 1u || pilot.retired() != 1u)
        {
            return false;
        }
    }
    return true;
}

bool test_strict_firmware_cop0_clock()
{
    using boundary = ee::e_ee_firmware_boundary;
    constexpr uint32_t base = 0xbfc00000u;
    constexpr uint32_t vector = 0xbfc00400u;
    // Timer0 and Count/Compare both cross at slot 14. The guest
    // enables IE/EIE and both masks on that same retirement.
    auto rom = make_firmware_pilot_rom({
        0x40806000u, 0x3c04b000u, 0x34020003u, 0xac820020u,
        0x34020200u, 0x3485f010u, 0xaca20000u, 0x34020180u,
        0xac820010u, 0x3402000eu, 0x40825800u, 0x3c020041u,
        0x34428401u, 0x40826000u, 0u});
    const uint32_t handler[] = {
        0x40036800u, // MFC0 v1,Cause
        0x40805800u, // MTC0 zero,Compare: clear IP7 only
        0x40086800u, // MFC0 t0,Cause
        0x3485f000u, 0x34030200u, 0xaca30000u, // acknowledge INTC
        0x42000018u};
    size_t offset = 0x400u;
    for (const uint32_t word : handler)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            rom[offset++] = static_cast<uint8_t>(word >> (i * 8u));
        }
    }
    {
        ee::c_ee_strict_firmware_pilot pilot(std::move(rom));
        for (uint32_t i = 0u; i < 13u; ++i)
        {
            const auto step = pilot.step(1u);
            if (step.m_boundary != boundary::RETIRED || step.m_interrupt_vector != 0u ||
                step.m_cop0_compare_crossed || step.m_timer_serviced)
            {
                return false;
            }
        }
        if (pilot.next_compare_event() != 14u || pilot.next_timer_event() != 14u)
        {
            return false;
        }
        const auto interrupt = pilot.step(1u);
        if (interrupt.m_boundary != boundary::RETIRED || !interrupt.m_cop0_compare_crossed ||
            !interrupt.m_timer_serviced || interrupt.m_interrupt_vector != vector ||
            pilot.cpu().m_cop0[ee::COP0_Cause] != 0x8400u ||
            pilot.cpu().m_cop0[ee::COP0_EPC] != base + 56u ||
            pilot.cpu().m_cop0[ee::COP0_Count] != 14u || pilot.intc_status() != 0x200u)
        {
            return false;
        }
        for (uint32_t i = 0u; i < 7u; ++i)
        {
            const auto step = pilot.step(1u);
            if (step.m_boundary != boundary::RETIRED || step.m_interrupt_vector != 0u)
            {
                return false;
            }
            if (i == 0u && pilot.cpu().m_gpr[3].m_lo != 0x8400u)
            {
                return false;
            }
            if (i == 1u && (pilot.cpu().m_cop0[ee::COP0_Cause] != 0x400u ||
                           pilot.intc_status() != 0x200u))
            {
                return false;
            }
        }
        if (pilot.cpu().m_gpr[8].m_lo != 0x400u || pilot.cpu().m_pc != base + 56u ||
            pilot.cpu().m_cop0[ee::COP0_Cause] != 0u ||
            (pilot.cpu().m_cop0[ee::COP0_Status] & ee::STATUS_EXL) != 0u ||
            pilot.intc_status() != 0u || pilot.cycle() != 21u)
        {
            return false;
        }
    }
    // Count wraps from ffffffff to zero; MFC0 observes the start
    // timestamp, sign extends the low word, and the Compare latch
    // survives a Count write but is cleared by a Compare write.
    {
        ee::c_ee_strict_firmware_pilot pilot(make_firmware_pilot_rom({
            0x40806000u, 0x2402fffeu, 0x40824800u, 0x40034800u,
            0x40804800u, 0x40805800u, 0x40055800u, 0x40004800u, 0xffffffffu}));
        for (uint32_t i = 0u; i < 8u; ++i)
        {
            const auto step = pilot.step(1u);
            if (step.m_boundary != boundary::RETIRED ||
                step.m_cop0_compare_crossed != (i == 3u))
            {
                return false;
            }
            if (i == 2u && (pilot.cpu().m_cop0[ee::COP0_Count] != 0xffffffffu ||
                           pilot.next_compare_event() != 4u))
            {
                return false;
            }
            if (i == 3u && (pilot.cpu().m_cop0[ee::COP0_Count] != 0u ||
                           pilot.cpu().m_gpr[3].m_lo != 0xffffffffffffffffull))
            {
                return false;
            }
            if (i == 4u && (pilot.cpu().m_cop0[ee::COP0_Cause] & 0x8000u) == 0u)
            {
                return false;
            }
        }
        if (pilot.cpu().m_gpr[5].m_lo != 0u || pilot.cpu().m_gpr[0].m_lo != 0u ||
            pilot.cpu().m_cop0[ee::COP0_Cause] != 0u || pilot.cycle() != 8u ||
            pilot.step(1u).m_boundary != boundary::HOST_REFUSAL || pilot.cycle() != 8u)
        {
            return false;
        }
    }
    // Compare deadline strictly inside a branch refuses; equality at
    // the completed branch boundary is allowed, with no link leakage.
    for (uint32_t compare : {5u, 6u})
    {
        ee::c_ee_strict_firmware_pilot pilot(make_firmware_pilot_rom({
            0x40806000u, 0x34020000u | compare, 0x40825800u, 0u,
            0x0ff00006u, 0x34030007u, 0u}));
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            if (pilot.step(1u).m_boundary != boundary::RETIRED)
            {
                return false;
            }
        }
        const auto step = pilot.step(2u);
        if (compare == 5u)
        {
            if (step.m_boundary != boundary::TIMER_DEADLINE || pilot.cycle() != 4u ||
                pilot.cpu().m_pc != base + 16u || pilot.cpu().m_gpr[31].m_lo != 0u ||
                pilot.cpu().m_gpr[3].m_lo != 0u || pilot.cpu().m_cop0[ee::COP0_Cause] != 0u)
            {
                return false;
            }
        }
        else if (step.m_boundary != boundary::RETIRED || !step.m_cop0_compare_crossed ||
                 pilot.cycle() != 6u || pilot.cpu().m_pc != base + 24u ||
                 pilot.cpu().m_gpr[3].m_lo != 7u ||
                 pilot.cpu().m_cop0[ee::COP0_Cause] != 0x8000u)
        {
            return false;
        }
    }
    return true;
}
} // namespace
