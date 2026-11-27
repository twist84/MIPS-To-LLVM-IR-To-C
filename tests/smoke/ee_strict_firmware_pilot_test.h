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

bool test_strict_firmware_spr_dma()
{
    using boundary = ee::e_ee_firmware_boundary;
    using result = ee::e_ee_reset_prefix_result;
    constexpr uint32_t base = 0xbfc00000u;
    constexpr uint32_t vector = 0xbfc00400u;
    // Fill source RAM, set DMAE and mask both SPR channels, program
    // toSPR then fromSPR using real guest writes. Both use SADR=3ff0
    // and two QWC, so the production engine wraps through SPR zero.
    const auto program = [](uint32_t enable, uint32_t source, uint32_t start)
    {
        return make_firmware_pilot_rom({
            0x40806000u, 0x3c04a000u, 0x34021234u, 0xac820000u,
            0x34025678u, 0xac820010u, 0x3c05b000u, 0x34a6e000u,
            0x34020000u | enable, 0xacc20000u,
            0x3c020300u, 0xacc20010u, 0x34a7d400u,
            0x34020000u | source, 0xace20010u, 0x34020002u, 0xace20020u,
            0x34023ff0u, 0xace20080u, 0x34020000u | start, 0xace20000u,
            0x8ce80020u, 0x8ce90010u, 0x8cea0080u, 0x8ccb0010u,
            0x34a7d000u, 0x34020020u, 0xace20010u, 0x34020002u, 0xace20020u,
            0x34023ff0u, 0xace20080u, 0x34020100u, 0xace20000u,
            0x8c8c0020u, 0x8c8d0030u, 0x3c020041u, 0x34420801u, 0x40826000u, 0u});
    };
    {
        auto rom = program(1u, 0u, 0x101u);
        const uint32_t handler[] = {
            0x34020300u, 0xacc20010u, 0x42000018u};
        size_t offset = 0x400u;
        for (const uint32_t word : handler)
        {
            for (uint32_t i = 0u; i < 4u; ++i)
            {
                rom[offset++] = static_cast<uint8_t>(word >> (i * 8u));
            }
        }
        ee::c_ee_strict_firmware_pilot pilot(std::move(rom));
        for (uint32_t i = 0u; i < 39u; ++i)
        {
            const auto step = pilot.step(1u);
            if (step.m_boundary != boundary::RETIRED ||
                step.m_dma_completed != (i == 20u || i == 33u) ||
                step.m_interrupt_vector != (i == 38u ? vector : 0u))
            {
                return false;
            }
            if (i == 20u && (pilot.dmac_status() != 0x03000200u ||
                pilot.dmac_started() != 0u || (pilot.cpu().m_cop0[ee::COP0_Cause] & 0x800u) == 0u))
            {
                return false;
            }
        }
        if (pilot.dmac_control() != 1u || pilot.dmac_status() != 0x03000300u ||
            pilot.cpu().m_gpr[8].m_lo != 0u || pilot.cpu().m_gpr[9].m_lo != 32u ||
            pilot.cpu().m_gpr[10].m_lo != 16u || pilot.cpu().m_gpr[11].m_lo != 0x03000200u ||
            pilot.cpu().m_gpr[12].m_lo != 0x1234u || pilot.cpu().m_gpr[13].m_lo != 0x5678u ||
            ee::ee_read_instruction_word(pilot.ram().data() + 0x20u) != 0x1234u ||
            ee::ee_read_instruction_word(pilot.ram().data() + 0x30u) != 0x5678u ||
            pilot.cpu().m_cop0[ee::COP0_EPC] != base + 39u * 4u ||
            pilot.cpu().m_cop0[ee::COP0_Cause] != 0x800u)
        {
            return false;
        }
        for (uint32_t i = 0u; i < 3u; ++i)
        {
            const auto step = pilot.step(1u);
            if (step.m_boundary != boundary::RETIRED || step.m_interrupt_vector != 0u)
            {
                return false;
            }
        }
        if (pilot.cpu().m_pc != base + 39u * 4u || pilot.dmac_status() != 0x03000000u ||
            pilot.cpu().m_cop0[ee::COP0_Cause] != 0u || pilot.cycle() != 42u)
        {
            return false;
        }
    }

    // Disabled DMAE, misaligned physical MADR, and chain mode must all
    // refuse the guest start without latching STR, completion or time.
    for (uint32_t variant = 0u; variant < 3u; ++variant)
    {
        ee::c_ee_strict_firmware_pilot pilot(program(variant == 0u ? 0u : 1u,
            variant == 1u ? 1u : 0u, variant == 2u ? 0x105u : 0x101u));
        for (uint32_t i = 0u; i < 20u; ++i)
        {
            if (pilot.step(1u).m_boundary != boundary::RETIRED)
            {
                return false;
            }
        }
        const auto refused = pilot.step(1u);
        if (refused.m_boundary != boundary::HOST_REFUSAL ||
            refused.m_instruction.m_result != result::DATA_UNAVAILABLE ||
            refused.m_dma_completed || pilot.cycle() != 20u || pilot.retired() != 20u ||
            pilot.cpu().m_pc != base + 80u || pilot.dmac_started() != 0u ||
            pilot.dmac_status() != 0x03000000u || pilot.cpu().m_cop0[ee::COP0_Cause] != 0u)
        {
            return false;
        }
    }
    return true;
}

bool test_strict_firmware_sif_vif()
{
    using boundary = ee::e_ee_firmware_boundary;
    using result = ee::e_ee_reset_prefix_result;
    // SIF mailbox ownership: EE publishes MSCOM and ORs MSFLAG,
    // reads zero IOP-owned fields, acknowledges no nonexistent flags.
    {
        ee::c_ee_strict_firmware_pilot pilot(make_firmware_pilot_rom({
            0x40806000u, 0x3c04b000u, 0x3484f200u,
            0x34021234u, 0xac820000u, 0x34020001u, 0xac820020u,
            0x34020004u, 0xac820020u, 0x8c830000u, 0x8c850020u,
            0x8c860010u, 0xac820030u, 0x8c870030u, 0xac820010u}));
        for (uint32_t i = 0u; i < 14u; ++i)
        {
            if (pilot.step(1u).m_boundary != boundary::RETIRED)
            {
                return false;
            }
        }
        const auto refused = pilot.step(1u);
        if (pilot.cpu().m_gpr[3].m_lo != 0x1234u || pilot.cpu().m_gpr[5].m_lo != 5u ||
            pilot.cpu().m_gpr[6].m_lo != 0u || pilot.cpu().m_gpr[7].m_lo != 0u ||
            pilot.intc_status() != 0u || pilot.dmac_status() != 0u ||
            refused.m_boundary != boundary::HOST_REFUSAL ||
            refused.m_instruction.m_result != result::DATA_UNAVAILABLE || pilot.cycle() != 14u)
        {
            return false;
        }
    }
    // Guest RAM packet: STCYCL 0x0101, MARK.I 0x1234, NOP, NOP.
    // Run both VIF channels and verify real VIF/DMAC source latches.
    for (uint32_t channel = 0u; channel < 2u; ++channel)
    {
        for (uint32_t bad = 0u; bad < 2u; ++bad)
        {
            const uint32_t vif_base = channel == 0u ? 0x3800u : 0x3c00u;
            const uint32_t dma_base = channel == 0u ? 0x8000u : 0x9000u;
            ee::c_ee_strict_firmware_pilot pilot(make_firmware_pilot_rom({
                0x40806000u, 0x3c04a000u,
                bad != 0u ? 0x3c021400u : 0x3c020100u,
                0x34420101u, 0xac820000u, 0x3c028700u, 0x34421234u,
                0xac820004u, 0x3c05b000u, 0x34a6e000u,
                0x34020001u, 0xacc20000u,
                0x3c020000u | (1u << channel), 0xacc20010u,
                0x34a7f010u, 0x34020000u | (1u << (4u + channel)), 0xace20000u,
                0x34a70000u | dma_base, 0xace00010u,
                0x34020001u, 0xace20020u, 0x34020101u, 0xace20000u,
                0x34a70000u | vif_base, 0x8ce80040u, 0x8ce90030u,
                0x8cea0000u, 0x34020008u, 0xace20010u,
                0x34020001u, 0xace20010u, 0x8ceb0040u, 0x8cec0030u}));
            for (uint32_t i = 0u; i < 22u; ++i)
            {
                if (pilot.step(1u).m_boundary != boundary::RETIRED)
                {
                    return false;
                }
            }
            const auto step = pilot.step(1u);
            const uint32_t mask = (1u << channel) << 16u;
            if (bad != 0u)
            {
                if (step.m_boundary != boundary::HOST_REFUSAL || pilot.cycle() != 22u ||
                    pilot.dmac_started() != 0u || pilot.dmac_status() != mask ||
                    pilot.intc_status() != 0u || step.m_dma_completed)
                {
                    return false;
                }
                continue;
            }
            if (step.m_boundary != boundary::RETIRED || !step.m_dma_completed ||
                pilot.dmac_status() != (mask | (1u << channel)) ||
                pilot.intc_status() != (1u << (4u + channel)) ||
                (pilot.cpu().m_cop0[ee::COP0_Cause] & 0xc00u) != 0xc00u)
            {
                return false;
            }
            for (uint32_t i = 23u; i < 33u; ++i)
            {
                if (pilot.step(1u).m_boundary != boundary::RETIRED)
                {
                    return false;
                }
            }
            if (pilot.cpu().m_gpr[8].m_lo != 0x101u || pilot.cpu().m_gpr[9].m_lo != 0x1234u ||
                (pilot.cpu().m_gpr[10].m_lo & 0x840u) != 0x840u ||
                pilot.cpu().m_gpr[11].m_lo != 0u || pilot.cpu().m_gpr[12].m_lo != 0u)
            {
                return false;
            }
        }
    }
    // SQ directly to VIF1 FIFO with a complete all-NOP quadword.
    {
        ee::c_ee_strict_firmware_pilot pilot(make_firmware_pilot_rom({
            0x40806000u, 0x3c04b000u, 0x34845000u, 0x7c800000u}));
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            if (pilot.step(1u).m_boundary != boundary::RETIRED)
            {
                return false;
            }
        }
        if (pilot.intc_status() != 0u || pilot.dmac_status() != 0u)
        {
            return false;
        }
    }
    return true;
}

bool test_strict_firmware_sif_transport()
{
    using boundary = ee::e_ee_firmware_boundary;
    constexpr uint32_t base = 0xbfc00000u;
    // EE guest constructs a SIF1 destination tag and payload in RAM,
    // enables DMA, arms SIF0 then starts SIF1 with no IOP receiver yet.
    ee::c_ee_strict_firmware_pilot pilot(make_firmware_pilot_rom({
        0x40806000u, 0x3c04a000u, 0x3c024000u, 0x34420100u,
        0xac820000u, 0x34020010u, 0xac820004u,
        0x34021234u, 0xac820010u, 0x3c05b000u, 0x34a6e000u,
        0x34020001u, 0xacc20000u, 0x3c020060u, 0xacc20010u,
        0x34a7c000u, 0xace00020u, 0x34020184u, 0xace20000u,
        0x34a7c400u, 0xace00010u, 0x34020002u, 0xace20020u,
        0x34020101u, 0xace20000u,
        0x34020040u, 0xacc20010u, // ack EE sender only
        0x3c020041u, 0x34420801u, 0x40826000u, 0u}));
    // All tags, payload transformation and DMA starts on the IOP
    // are guest instructions. No host writes to either RAM.
    const auto iop_rom = make_firmware_pilot_rom({
        0x3c08bf80u, 0x35081538u, 0x3c090100u, 0x35290700u,
        0xad090000u, 0x3c0f0100u,
        0x8d0e0000u, 0u, 0x01cf7024u, 0x15c0fffcu, 0u,
        0x24080100u, 0x8d090000u, 0u, 0x39295aa5u, 0xad090100u,
        0x24080300u, 0x3c098000u, 0x35290200u, 0xad090000u,
        0x24090004u, 0xad090004u, 0x3c099000u, 0x35290001u, 0xad090008u,
        0x24090040u, 0xad09000cu, 0xad000010u, 0xad000014u,
        0x3c08bf80u, 0x3508152cu, 0x24090300u, 0xad090000u,
        0x2508fffcu, 0x3c090100u, 0x35290701u, 0xad090000u,
        0x1000ffffu, 0u});
    if (!pilot.attach_iop(iop_rom) || pilot.attach_iop(iop_rom) ||
        pilot.advance_iop(0u).has_value())
    {
        return false;
    }
    for (uint32_t i = 0u; i < 25u; ++i)
    {
        const auto step = pilot.step(1u);
        if (step.m_boundary != boundary::RETIRED || step.m_interrupt_vector != 0u)
        {
            return false;
        }
    }
    // DMA6 can finish into the real FIFO without DMA10. DMA5
    // stays armed, no payload delivered and no invented reply.
    if (pilot.sif1_delivered() != 0u || pilot.sif0_delivered() != 0u ||
        pilot.sif1_fifo_words() != 8u || pilot.dmac_started() != 0x20u ||
        pilot.dmac_status() != 0x00600040u || pilot.sif_faulted())
    {
        return false;
    }
    for (uint32_t i = 25u; i < 30u; ++i)
    {
        const auto step = pilot.step(1u);
        if (step.m_boundary != boundary::RETIRED || step.m_interrupt_vector != 0u)
        {
            return false;
        }
    }
    const auto run = pilot.advance_iop(128u);
    if (!run.has_value() || run->m_stop != iop::e_kernel_stop::budget_exhausted ||
        run->m_executed == 0u || pilot.sif_faulted() || pilot.cycle() != 30u ||
        pilot.sif0_delivered() != 16u || pilot.sif1_delivered() != 16u ||
        pilot.sif1_fifo_words() != 0u || pilot.dmac_started() != 0u ||
        pilot.dmac_status() != 0x00600020u ||
        ee::ee_read_instruction_word(pilot.ram().data() + 0x40u) != (0x1234u ^ 0x5aa5u))
    {
        return false;
    }
    // External guest progress asserted IP3 at the committed EE boundary.
    // Take it before executing the next EE word; no invented EE slots.
    const auto irq = pilot.step(1u);
    return irq.m_boundary == boundary::INTERRUPT_ENTRY &&
        irq.m_interrupt_vector == 0xbfc00400u && pilot.retired() == 30u &&
        pilot.cycle() == 30u && pilot.cpu().m_cop0[ee::COP0_EPC] == base + 120u &&
        pilot.cpu().m_cop0[ee::COP0_Cause] == 0x800u;
}

bool test_strict_firmware_scanout()
{
    using boundary = ee::e_ee_firmware_boundary;
    constexpr uint64_t first = 4920115u;
    constexpr uint64_t end = first + first * 44u / 525u;
    constexpr uint32_t base = 0xbfc00000u;
    static_assert((first & 1u) == 1u && (end & 1u) == 1u);
    // Nine ordinary slots then an atomic two-slot branch loop.
    // First NTSC start is odd, at the completed branch boundary.
    auto rom = make_firmware_pilot_rom({
        0x40806000u, 0x3c04b000u, 0x3485f010u, 0x34020004u,
        0xaca20000u, 0x3c020041u, 0x34420401u, 0x40826000u,
        0x3c06b200u, 0x1000ffffu, 0u});
    const uint32_t handler[] = {
        0x8cc31000u, 0x34020008u, 0xacc21000u, // read CSR, W1C VSINT
        0x3485f000u, 0x34020004u, 0xaca20000u, // acknowledge INTC 2
        0u, 0x42000018u};
    size_t offset = 0x400u;
    for (const uint32_t word : handler)
    {
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            rom[offset++] = static_cast<uint8_t>(word >> (i * 8u));
        }
    }
    ee::c_ee_strict_firmware_pilot pilot(std::move(rom));
    if (pilot.configure_scanout(1u, 0u) || !pilot.configure_scanout(2u, 1u) ||
        pilot.configure_scanout(3u, 0u) || pilot.next_scanout_event() != first ||
        pilot.gs_csr() != 0u || pilot.intc_status() != 0u)
    {
        return false;
    }
    for (uint32_t i = 0u; i < 9u; ++i)
    {
        if (pilot.step(1u).m_boundary != boundary::RETIRED)
        {
            return false;
        }
    }
    while (pilot.cycle() < first - 2u)
    {
        const auto step = pilot.step(2u);
        if (step.m_boundary != boundary::RETIRED || step.m_vblank_started ||
            step.m_vblank_ended || step.m_interrupt_vector != 0u)
        {
            return false;
        }
    }
    if (pilot.gs_csr() != 0u || pilot.intc_status() != 0u ||
        pilot.fields_started() != 0u || pilot.step(1u).m_boundary != boundary::BUDGET)
    {
        return false;
    }
    const auto start = pilot.step(2u);
    if (start.m_boundary != boundary::RETIRED || !start.m_vblank_started ||
        start.m_vblank_ended || start.m_interrupt_vector != base + 0x400u ||
        pilot.cycle() != first || pilot.retired() != first || pilot.fields_started() != 1u ||
        pilot.fields_ended() != 0u || pilot.next_scanout_event() != end ||
        pilot.gs_csr() != 0x2008u || pilot.intc_status() != 4u ||
        pilot.cpu().m_cop0[ee::COP0_EPC] != base + 36u)
    {
        return false;
    }
    for (uint32_t i = 0u; i < 8u; ++i)
    {
        if (pilot.step(1u).m_boundary != boundary::RETIRED)
        {
            return false;
        }
        if (i == 2u && (pilot.gs_csr() != 0x2000u || pilot.intc_status() != 4u))
        {
            return false; // CSR ack is independent from INTC ack
        }
    }
    if (pilot.cpu().m_gpr[3].m_lo != 0x2008u || pilot.intc_status() != 0u ||
        pilot.cpu().m_pc != base + 36u)
    {
        return false;
    }
    while (pilot.cycle() < end - 2u)
    {
        const auto step = pilot.step(2u);
        if (step.m_boundary != boundary::RETIRED || step.m_vblank_started ||
            step.m_vblank_ended || step.m_interrupt_vector != 0u)
        {
            return false;
        }
    }
    const auto finish = pilot.step(2u);
    if (finish.m_boundary != boundary::RETIRED || finish.m_vblank_started ||
        !finish.m_vblank_ended || finish.m_interrupt_vector != 0u ||
        pilot.cycle() != end || pilot.fields_started() != 1u ||
        pilot.fields_ended() != 1u || pilot.intc_status() != 8u ||
        pilot.gs_csr() != 0x2000u || (pilot.cpu().m_cop0[ee::COP0_Cause] & 0x400u) != 0u)
    {
        return false;
    }
    // PAL has an independent reset cadence. No field at configuration.
    ee::c_ee_strict_firmware_pilot pal(make_firmware_pilot_rom({0x40806000u}));
    return pal.configure_scanout(3u, 0u) && pal.next_scanout_event() == 5898240u &&
        pal.fields_started() == 0u && pal.gs_csr() == 0u && pal.intc_status() == 0u;
}
} // namespace
