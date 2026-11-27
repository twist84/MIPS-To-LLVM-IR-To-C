#include "ee_strict_reset.h"
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
    std::puts("EE ROM0 fetch probe: PASS (no instructions retired during fetch probe)");
    auto cpu = ee::ee_make_strict_reset_prefix_state();
    const auto step = ee::ee_execute_strict_reset_prefix(cpu, rom);
    if (step.m_result != ee::e_ee_reset_prefix_result::COMMITTED)
    {
        std::fprintf(stderr, "EE reset prefix refused: result=%u opcode=%08x\n",
            static_cast<unsigned>(step.m_result), step.m_opcode);
        return 5;
    }
    if (cpu.m_pc != 0xbfc00004u || cpu.m_cop0[ee::COP0_Status] != 0u)
    {
        return 6;
    }
    std::printf("EE reset prefix: retired=1 pc=%08x status=%08x opcode=%08x\n",
        cpu.m_pc, cpu.m_cop0[ee::COP0_Status], step.m_opcode);
    // Twelve actual retirements include the initial Status write and
    // the selected EE path, skipping the unexecuted IOP setup.
    constexpr uint32_t budget = 12u;
    uint32_t retired = step.m_retired;
    while (retired < budget)
    {
        const uint32_t pc = cpu.m_pc;
        const auto action = ee::ee_execute_strict_rom_register_step(cpu, rom, budget - retired);
        if (action.m_result != ee::e_ee_reset_prefix_result::COMMITTED ||
            action.m_retired == 0u || action.m_retired > budget - retired)
        {
            std::fprintf(stderr, "EE reset register stop: pc=%08x opcode=%08x result=%u retired=%u\n",
                pc, action.m_opcode, static_cast<unsigned>(action.m_result), retired);
            return 8;
        }
        retired += action.m_retired;
        std::printf("EE reset register step: pc=%08x opcode=%08x count=%u next=%08x total=%u\n",
            pc, action.m_opcode, action.m_retired, cpu.m_pc, retired);
    }
    if (retired != budget || cpu.m_pc != 0xbfc00044u ||
        cpu.m_gpr[8].m_lo != 0x2e00u || cpu.m_gpr[9].m_lo != 0x2e00u ||
        cpu.m_gpr[24].m_lo != 0xffffffffa0010000ull ||
        cpu.m_gpr[15].m_lo != 0xffffffffbfc0012cull)
    {
        std::fprintf(stderr, "EE processor-selection state mismatch\n");
        return 9;
    }
    std::printf("EE reset selection: retired=%u pc=%08x prid=%08x t8=%016llx t7=%016llx\n",
        retired, cpu.m_pc, cpu.m_cop0[ee::COP0_PRId],
        static_cast<unsigned long long>(cpu.m_gpr[24].m_lo),
        static_cast<unsigned long long>(cpu.m_gpr[15].m_lo));
    // Check the first actual ROM data read and the resulting guest
    // branch independently of the interpreter's own decode.
    constexpr uint32_t read_checkpoint = 20u;
    while (retired < read_checkpoint)
    {
        const auto action = ee::ee_execute_strict_rom_register_step(cpu, rom,
            read_checkpoint - retired);
        if (action.m_result != ee::e_ee_reset_prefix_result::COMMITTED ||
            action.m_retired == 0u || action.m_retired > read_checkpoint - retired)
        {
            std::fprintf(stderr, "EE reset first ROM read refused: pc=%08x opcode=%08x result=%u retired=%u\n",
                cpu.m_pc, action.m_opcode, static_cast<unsigned>(action.m_result), retired);
            return 10;
        }
        retired += action.m_retired;
    }
    const uint32_t first_byte = bytes[0x1000u];
    if (cpu.m_gpr[8].m_lo != 0xffffffffbfc01000ull ||
        cpu.m_gpr[9].m_lo != 0xffffffffbfc00000ull ||
        cpu.m_gpr[10].m_lo != first_byte || cpu.m_gpr[14].m_lo != 0x20u ||
        cpu.m_pc != (first_byte == 0u ? 0xbfc000bcu : 0xbfc00064u))
    {
        std::fprintf(stderr, "EE first ROM data read state mismatch\n");
        return 11;
    }
    std::printf("EE reset ROM read: retired=%u address=bfc01000 byte=%02x next=%08x\n",
        retired, first_byte, cpu.m_pc);

    // The pinned image's first directory entry differs from the
    // sought name. Require execution of one complete mismatch/advance
    // iteration and independently reconstruct its little-endian size.
    // The interpreter reads the original image, not this expectation.
    constexpr uint32_t directory_checkpoint = 38u;
    while (retired < directory_checkpoint)
    {
        const auto action = ee::ee_execute_strict_rom_register_step(cpu, rom,
            directory_checkpoint - retired);
        if (action.m_result != ee::e_ee_reset_prefix_result::COMMITTED ||
            action.m_retired == 0u || action.m_retired > directory_checkpoint - retired)
        {
            std::fprintf(stderr, "EE reset directory scan refused: pc=%08x opcode=%08x result=%u retired=%u\n",
                cpu.m_pc, action.m_opcode, static_cast<unsigned>(action.m_result), retired);
            return 13;
        }
        retired += action.m_retired;
    }
    const uint32_t file_size = static_cast<uint32_t>(bytes[0x100cu]) |
        (static_cast<uint32_t>(bytes[0x100du]) << 8u) |
        (static_cast<uint32_t>(bytes[0x100eu]) << 16u) |
        (static_cast<uint32_t>(bytes[0x100fu]) << 24u);
    const uint32_t padded_size = (file_size + 15u) & ~15u;
    const uint32_t next_file = 0xbfc00000u + padded_size;
    const uint64_t expected_file = (next_file & 0x80000000u) != 0u
        ? 0xffffffff00000000ull | next_file : next_file;
    if (first_byte != 0x52u || bytes[0x12cu] != 0x45u ||
        cpu.m_pc != 0xbfc00054u ||
        cpu.m_gpr[8].m_lo != 0xffffffffbfc01010ull ||
        cpu.m_gpr[9].m_lo != expected_file ||
        cpu.m_gpr[10].m_lo != padded_size ||
        cpu.m_gpr[14].m_lo != 31u)
    {
        std::fprintf(stderr, "EE first directory iteration mismatch: pc=%08x size=%08x padded=%08x\n",
            cpu.m_pc, file_size, padded_size);
        return 14;
    }
    std::printf("EE reset directory advance: retired=%u size=%08x padded=%08x next_entry=%016llx next_file=%016llx pc=%08x\n",
        retired, file_size, padded_size,
        static_cast<unsigned long long>(cpu.m_gpr[8].m_lo),
        static_cast<unsigned long long>(cpu.m_gpr[9].m_lo), cpu.m_pc);

    // Independently validate the pinned image's matched directory
    // record and all observed range checks through the first copy byte.
    constexpr uint32_t match_checkpoint = 183u;
    while (retired < match_checkpoint)
    {
        const auto action = ee::ee_execute_strict_rom_register_step(cpu, rom,
            match_checkpoint - retired);
        if (action.m_result != ee::e_ee_reset_prefix_result::COMMITTED ||
            action.m_retired == 0u || action.m_retired > match_checkpoint - retired)
        {
            std::fprintf(stderr, "EE reset directory match refused: pc=%08x opcode=%08x result=%u retired=%u\n",
                cpu.m_pc, action.m_opcode, static_cast<unsigned>(action.m_result), retired);
            return 15;
        }
        retired += action.m_retired;
    }
    constexpr uint32_t source_offset = 0x1090u;
    constexpr uint32_t image_size = 0xa3a8u;
    constexpr uint32_t destination = 0x10000u;
    const uint32_t directory_size = ee::ee_read_instruction_word(bytes.data() + 0x104cu);
    if (cpu.m_pc != 0xbfc000d0u ||
        cpu.m_gpr[8].m_lo != 0xffffffffbfc01040ull ||
        cpu.m_gpr[9].m_lo != 0xffffffffbfc01090ull ||
        cpu.m_gpr[10].m_lo != image_size || directory_size != image_size ||
        cpu.m_gpr[11].m_lo != 0xffffffffa0010000ull)
    {
        std::fprintf(stderr, "EE matched directory entry mismatch\n");
        return 16;
    }
    constexpr uint32_t copy_read_checkpoint = 199u;
    while (retired < copy_read_checkpoint)
    {
        const auto action = ee::ee_execute_strict_rom_register_step(cpu, rom,
            copy_read_checkpoint - retired);
        if (action.m_result != ee::e_ee_reset_prefix_result::COMMITTED ||
            action.m_retired == 0u || action.m_retired > copy_read_checkpoint - retired)
        {
            std::fprintf(stderr, "EE reset range validation refused: pc=%08x opcode=%08x result=%u retired=%u\n",
                cpu.m_pc, action.m_opcode, static_cast<unsigned>(action.m_result), retired);
            return 17;
        }
        retired += action.m_retired;
    }
    if (cpu.m_pc != 0xbfc00110u ||
        cpu.m_gpr[12].m_lo != bytes[source_offset] ||
        cpu.m_gpr[13].m_lo != 0xffffffffbfc0b438ull)
    {
        std::fprintf(stderr, "EE first image byte state mismatch\n");
        return 18;
    }
    std::printf("EE reset image read: retired=%u source=bfc01090 size=%08x byte=%02x destination=a0010000 pc=%08x\n",
        retired, image_size, static_cast<unsigned>(bytes[source_offset]), cpu.m_pc);
    // Main RAM is diagnostic caller-owned backing, not live runtime
    // state. Initialize to a sentinel to distinguish guest stores from
    // unchanged host storage; never pre-copy the firmware.
    std::vector<uint8_t> ram_bytes(0x02000000u, 0xa5u);
    const ee::s_ee_reset_ram_view ram{ram_bytes.data(), ram_bytes.size()};

    // Stop exactly at the authentic JR handoff before exploring RAM.
    // The pinned image must retain all previously observed copy results.
    constexpr uint32_t handoff_checkpoint = 251576u;
    struct s_trace
    {
        uint32_t m_pc;
        uint32_t m_opcode;
        uint32_t m_count;
        uint32_t m_next;
        uint32_t m_total;
    };
    constexpr uint32_t tail_capacity = 32u;
    s_trace tail[tail_capacity]{};
    uint32_t trace_count = 0u;
    ee::s_ee_reset_prefix_step stop{ee::e_ee_reset_prefix_result::BUDGET_EXHAUSTED};
    for (; retired < handoff_checkpoint;)
    {
        const uint32_t pc = cpu.m_pc;
        const auto action = ee::ee_execute_strict_rom_register_step(cpu, rom,
            handoff_checkpoint - retired, ram);
        if (action.m_result != ee::e_ee_reset_prefix_result::COMMITTED)
        {
            stop = action;
            break;
        }
        if (action.m_retired == 0u || action.m_retired > handoff_checkpoint - retired)
        {
            return 12;
        }
        retired += action.m_retired;
        tail[trace_count % tail_capacity] = {pc, action.m_opcode, action.m_retired, cpu.m_pc, retired};
        ++trace_count;
    }
    const uint32_t first_trace = trace_count > tail_capacity ? trace_count - tail_capacity : 0u;
    for (uint32_t i = first_trace; i < trace_count; ++i)
    {
        const auto& item = tail[i % tail_capacity];
        std::printf("EE reset trace: pc=%08x opcode=%08x count=%u next=%08x total=%u\n",
            item.m_pc, item.m_opcode, item.m_count, item.m_next, item.m_total);
    }
    std::printf("EE reset copy checkpoint: retired=%u pc=%08x result=%u detail_pc=%08x opcode=%08x data_address=%08x\n",
        retired, cpu.m_pc, static_cast<unsigned>(stop.m_result), stop.m_detail_pc,
        stop.m_opcode, stop.m_data_address);
    for (uint32_t i = 0u; i < 32u; ++i)
    {
        std::printf("EE reset GPR: r%02u=%016llx\n", i,
            static_cast<unsigned long long>(i == 0u ? 0u : cpu.m_gpr[i].m_lo));
    }
    // Count content equality, not assumed stores: an unchanged
    // sentinel can coincidentally equal a ROM byte. A complete match
    // verifies contents, not transfer count or execution of RAM code.
    uint32_t matching = 0u;
    uint32_t matching_prefix = 0u;
    for (uint32_t i = 0u; i < image_size; ++i)
    {
        if (ram_bytes[destination + i] == bytes[source_offset + i])
        {
            ++matching;
            if (matching_prefix == i)
            {
                ++matching_prefix;
            }
        }
    }
    std::printf("EE reset RAM image: matching=%u/%u matching_prefix=%u guard_before=%02x guard_after=%02x\n",
        matching, image_size, matching_prefix,
        static_cast<unsigned>(ram_bytes[destination - 1u]),
        static_cast<unsigned>(ram_bytes[destination + image_size]));
    if (retired != handoff_checkpoint || cpu.m_pc != 0xa0010000u ||
        matching != image_size || matching_prefix != image_size ||
        ram_bytes[destination - 1u] != 0xa5u ||
        ram_bytes[destination + image_size] != 0xa5u || trace_count == 0u)
    {
        std::fprintf(stderr, "EELOAD handoff or image regression\n");
        return 19;
    }
    const auto& handoff = tail[(trace_count - 1u) % tail_capacity];
    if (handoff.m_pc != 0xbfc00124u || handoff.m_opcode != 0x03000008u ||
        handoff.m_count != 2u || handoff.m_next != cpu.m_pc ||
        handoff.m_total != handoff_checkpoint)
    {
        std::fprintf(stderr, "EELOAD JR retirement regression\n");
        return 20;
    }
    const auto next = ee::ee_plan_instruction_fetch(tlb, cpu.m_pc,
        cpu.m_cop0[ee::COP0_Status], cpu.m_cop0[ee::COP0_Config],
        cpu.m_cop0[ee::COP0_Context], cpu.m_cop0[ee::COP0_EntryHi]);
    const uint8_t* code = ee::ee_fetch_strict_reset_word(next, rom, ram);
    if (code != ram_bytes.data() + destination ||
        next.m_access.m_physical_address != destination)
    {
        std::fprintf(stderr, "EELOAD live RAM fetch regression\n");
        return 21;
    }
    const uint32_t word = ee::ee_read_instruction_word(code);
    const auto ins = ee::decode(cpu.m_pc, word);
    std::printf("EELOAD handoff: retired=%u pc=%08x physical=%08x opcode=%08x decoded=%s valid=%u (RAM-backed fetch, not retirement)\n",
        retired, cpu.m_pc, next.m_access.m_physical_address, word, ins.mnemonic,
        static_cast<unsigned>(ins.valid));
    // Capture a bounded lookahead for the next implementation batch;
    // this lookahead fetches and decodes without executing any word.
    for (uint32_t i = 1u; i < 16u; ++i)
    {
        const uint32_t pc = cpu.m_pc + i * 4u;
        const auto plan = ee::ee_plan_instruction_fetch(tlb, pc,
            cpu.m_cop0[ee::COP0_Status], cpu.m_cop0[ee::COP0_Config],
            cpu.m_cop0[ee::COP0_Context], cpu.m_cop0[ee::COP0_EntryHi]);
        const uint8_t* look = ee::ee_fetch_strict_reset_word(plan, rom, ram);
        if (look == nullptr)
        {
            break;
        }
        const uint32_t opcode = ee::ee_read_instruction_word(look);
        const auto decoded = ee::decode(pc, opcode);
        std::printf("EE reset lookahead: pc=%08x opcode=%08x decoded=%s\n",
            pc, opcode, decoded.mnemonic);
    }
    // The pinned image sets CU0/CU1/CU2 without changing privilege or
    // enabling interrupts, then clears a RAM range via guest SB.
    // Require both milestones, without skipping the guest loop.
    constexpr uint32_t status_checkpoint = handoff_checkpoint + 2u;
    for (; retired < status_checkpoint;)
    {
        const auto action = ee::ee_execute_strict_rom_register_step(cpu, rom,
            status_checkpoint - retired, ram);
        if (action.m_result != ee::e_ee_reset_prefix_result::COMMITTED ||
            action.m_retired == 0u || action.m_retired > status_checkpoint - retired)
        {
            std::fprintf(stderr, "EELOAD Status refused: pc=%08x opcode=%08x result=%u retired=%u\n",
                cpu.m_pc, action.m_opcode, static_cast<unsigned>(action.m_result), retired);
            return 22;
        }
        retired += action.m_retired;
    }
    if (cpu.m_pc != 0xa0010008u || cpu.m_cop0[ee::COP0_Status] != 0x70000000u ||
        cpu.m_gpr[8].m_lo != 0x70000000u)
    {
        std::fprintf(stderr, "EELOAD Status checkpoint mismatch\n");
        return 23;
    }
    std::printf("EELOAD Status: retired=%u pc=%08x status=%08x\n",
        retired, cpu.m_pc, cpu.m_cop0[ee::COP0_Status]);

    // ADDIU sign-extends a3b0 (-5c50) and dd90 (-2270).
    // Compute the physical endpoints independently of the executor.
    constexpr uint32_t clear_begin = 0x00020000u - 0x5c50u;
    constexpr uint32_t clear_end = 0x00030000u - 0x2270u;
    static_assert(clear_begin == 0x0001a3b0u);
    static_assert(clear_end == 0x0002dd90u);
    static_assert(clear_begin > destination + image_size);
    constexpr uint32_t clear_size = clear_end - clear_begin;
    // Ten initial instructions, five per byte, then the final taken
    // BEQ and its slot. Check the whole region before and after.
    constexpr uint32_t clear_checkpoint = handoff_checkpoint + 10u + 5u * clear_size + 2u;
    static_assert(clear_size == 80352u);
    static_assert(clear_checkpoint == 653348u);
    for (uint32_t i = clear_begin - 1u; i <= clear_end; ++i)
    {
        if (ram_bytes[i] != 0xa5u)
        {
            std::fprintf(stderr, "EELOAD clear initial sentinel mismatch: physical=%08x\n", i);
            return 24;
        }
    }
    for (; retired < clear_checkpoint;)
    {
        const auto action = ee::ee_execute_strict_rom_register_step(cpu, rom,
            clear_checkpoint - retired, ram);
        if (action.m_result != ee::e_ee_reset_prefix_result::COMMITTED ||
            action.m_retired == 0u || action.m_retired > clear_checkpoint - retired)
        {
            std::fprintf(stderr, "EELOAD clear refused: pc=%08x opcode=%08x result=%u retired=%u detail_pc=%08x address=%08x\n",
                cpu.m_pc, action.m_opcode, static_cast<unsigned>(action.m_result), retired,
                action.m_detail_pc, action.m_data_address);
            return 25;
        }
        retired += action.m_retired;
    }
    uint32_t cleared = 0u;
    for (uint32_t i = clear_begin; i < clear_end; ++i)
    {
        if (ram_bytes[i] == 0u)
        {
            ++cleared;
        }
    }
    if (cpu.m_pc != 0xa001003cu || cpu.m_cop0[ee::COP0_Status] != 0x70000000u ||
        cpu.m_gpr[8].m_lo != 0xffffffffa002dd90ull ||
        cpu.m_gpr[9].m_lo != 0xffffffffa002dd90ull ||
        cpu.m_gpr[29].m_lo != 0xffffffffa007ffe0ull ||
        cleared != clear_size || ram_bytes[clear_begin - 1u] != 0xa5u ||
        ram_bytes[clear_end] != 0xa5u)
    {
        std::fprintf(stderr, "EELOAD clear checkpoint mismatch: retired=%u pc=%08x status=%08x cleared=%u/%u t0=%016llx t1=%016llx sp=%016llx guard_before=%02x guard_after=%02x\n",
            retired, cpu.m_pc, cpu.m_cop0[ee::COP0_Status], cleared, clear_size,
            static_cast<unsigned long long>(cpu.m_gpr[8].m_lo),
            static_cast<unsigned long long>(cpu.m_gpr[9].m_lo),
            static_cast<unsigned long long>(cpu.m_gpr[29].m_lo),
            static_cast<unsigned>(ram_bytes[clear_begin - 1u]),
            static_cast<unsigned>(ram_bytes[clear_end]));
        return 26;
    }
    for (uint32_t i = 0u; i < image_size; ++i)
    {
        if (ram_bytes[destination + i] != bytes[source_offset + i])
        {
            std::fprintf(stderr, "EELOAD image changed during clear: offset=%08x\n", i);
            return 27;
        }
    }
    if (ram_bytes[destination - 1u] != 0xa5u || ram_bytes[destination + image_size] != 0xa5u)
    {
        return 28;
    }
    std::printf("EELOAD clear: retired=%u pc=%08x range=%08x-%08x cleared=%u/%u guard_before=%02x guard_after=%02x sp=%016llx\n",
        retired, cpu.m_pc, clear_begin, clear_end, cleared, clear_size,
        static_cast<unsigned>(ram_bytes[clear_begin - 1u]), static_cast<unsigned>(ram_bytes[clear_end]),
        static_cast<unsigned long long>(cpu.m_gpr[29].m_lo));

    // Inspect the next real blocker after the enforced clear loop.
    for (uint32_t i = 0u; i < 16u; ++i)
    {
        const uint32_t pc = cpu.m_pc + i * 4u;
        const auto plan = ee::ee_plan_instruction_fetch(tlb, pc,
            cpu.m_cop0[ee::COP0_Status], cpu.m_cop0[ee::COP0_Config],
            cpu.m_cop0[ee::COP0_Context], cpu.m_cop0[ee::COP0_EntryHi]);
        const uint8_t* look = ee::ee_fetch_strict_reset_word(plan, rom, ram);
        if (look == nullptr)
        {
            break;
        }
        const uint32_t opcode = ee::ee_read_instruction_word(look);
        std::printf("EELOAD next lookahead: pc=%08x opcode=%08x decoded=%s\n",
            pc, opcode, ee::decode(pc, opcode).mnemonic);
    }
    // Retire the real JAL and its NOP before fetching the callee.
    constexpr uint32_t call_checkpoint = clear_checkpoint + 2u;
    const auto call = ee::ee_execute_strict_rom_register_step(cpu, rom, 2u, ram);
    if (call.m_result != ee::e_ee_reset_prefix_result::COMMITTED ||
        call.m_opcode != 0x0c005877u || call.m_retired != 2u ||
        cpu.m_pc != 0xa00161dcu || cpu.m_gpr[31].m_lo != 0x00000000a0010044ull ||
        cpu.m_gpr[29].m_lo != 0xffffffffa007ffe0ull)
    {
        std::fprintf(stderr, "EELOAD call refused or mismatched: pc=%08x opcode=%08x result=%u count=%u ra=%016llx\n",
            cpu.m_pc, call.m_opcode, static_cast<unsigned>(call.m_result), call.m_retired,
            static_cast<unsigned long long>(cpu.m_gpr[31].m_lo));
        return 30;
    }
    retired += call.m_retired;
    const auto callee = ee::ee_plan_instruction_fetch(tlb, cpu.m_pc,
        cpu.m_cop0[ee::COP0_Status], cpu.m_cop0[ee::COP0_Config],
        cpu.m_cop0[ee::COP0_Context], cpu.m_cop0[ee::COP0_EntryHi]);
    if (retired != call_checkpoint || callee.m_access.m_physical_address != 0x000161dcu ||
        ee::ee_fetch_strict_reset_word(callee, rom, ram) != ram_bytes.data() + 0x161dcu)
    {
        std::fprintf(stderr, "EELOAD callee fetch regression\n");
        return 31;
    }
    std::printf("EELOAD call: retired=%u pc=%08x physical=%08x ra=%016llx\n",
        retired, cpu.m_pc, callee.m_access.m_physical_address,
        static_cast<unsigned long long>(cpu.m_gpr[31].m_lo));

    // Capture the actual callee entry before continuing execution.
    for (uint32_t i = 0u; i < 16u; ++i)
    {
        const uint32_t pc = cpu.m_pc + i * 4u;
        const auto plan = ee::ee_plan_instruction_fetch(tlb, pc,
            cpu.m_cop0[ee::COP0_Status], cpu.m_cop0[ee::COP0_Config],
            cpu.m_cop0[ee::COP0_Context], cpu.m_cop0[ee::COP0_EntryHi]);
        const uint8_t* look = ee::ee_fetch_strict_reset_word(plan, rom, ram);
        if (look == nullptr)
        {
            break;
        }
        const uint32_t opcode = ee::ee_read_instruction_word(look);
        std::printf("EELOAD callee lookahead: pc=%08x opcode=%08x decoded=%s\n",
            pc, opcode, ee::decode(pc, opcode).mnemonic);
    }
    // Require the actual callee prologue and nested call. Stack RAM
    // has never been preloaded: both saved words must originate in SW.
    constexpr uint32_t stack_frame = 0x0007ffa8u;
    constexpr uint32_t saved_s0 = stack_frame + 48u;
    constexpr uint32_t saved_ra = stack_frame + 52u;
    constexpr uint32_t nested_checkpoint = call_checkpoint + 6u;
    static_assert(saved_s0 == 0x0007ffd8u && saved_ra == 0x0007ffdcu);
    static_assert(nested_checkpoint == 653356u);
    for (uint32_t i = stack_frame - 1u; i <= saved_ra + 4u; ++i)
    {
        if (ram_bytes[i] != 0xa5u)
        {
            std::fprintf(stderr, "EELOAD stack initial sentinel mismatch: physical=%08x\n", i);
            return 32;
        }
    }
    for (; retired < nested_checkpoint;)
    {
        const uint32_t pc = cpu.m_pc;
        const auto action = ee::ee_execute_strict_rom_register_step(cpu, rom,
            nested_checkpoint - retired, ram);
        if (action.m_result != ee::e_ee_reset_prefix_result::COMMITTED ||
            action.m_retired == 0u || action.m_retired > nested_checkpoint - retired)
        {
            std::fprintf(stderr, "EELOAD prologue refused: pc=%08x opcode=%08x result=%u detail_pc=%08x address=%08x retired=%u\n",
                pc, action.m_opcode, static_cast<unsigned>(action.m_result),
                action.m_detail_pc, action.m_data_address, retired);
            return 33;
        }
        retired += action.m_retired;
    }
    const uint32_t saved_s0_word = ee::ee_read_instruction_word(ram_bytes.data() + saved_s0);
    const uint32_t saved_ra_word = ee::ee_read_instruction_word(ram_bytes.data() + saved_ra);
    if (cpu.m_pc != 0xa0010528u || cpu.m_gpr[29].m_lo != 0xffffffffa007ffa8ull ||
        cpu.m_gpr[31].m_lo != 0x00000000a00161f4ull ||
        cpu.m_gpr[4].m_lo != 0xffffffffa00197e6ull ||
        saved_s0_word != 0u || saved_ra_word != 0xa0010044u ||
        ram_bytes[saved_ra + 4u] != 0xa5u)
    {
        std::fprintf(stderr, "EELOAD prologue mismatch: pc=%08x sp=%016llx ra=%016llx a0=%016llx saved_s0=%08x saved_ra=%08x\n",
            cpu.m_pc, static_cast<unsigned long long>(cpu.m_gpr[29].m_lo),
            static_cast<unsigned long long>(cpu.m_gpr[31].m_lo),
            static_cast<unsigned long long>(cpu.m_gpr[4].m_lo), saved_s0_word, saved_ra_word);
        return 34;
    }
    for (uint32_t i = stack_frame - 1u; i < saved_s0; ++i)
    {
        if (ram_bytes[i] != 0xa5u)
        {
            std::fprintf(stderr, "EELOAD stack unexpected write: physical=%08x\n", i);
            return 35;
        }
    }
    std::printf("EELOAD prologue: retired=%u pc=%08x sp=%016llx ra=%016llx a0=%016llx saved_s0=%08x saved_ra=%08x\n",
        retired, cpu.m_pc, static_cast<unsigned long long>(cpu.m_gpr[29].m_lo),
        static_cast<unsigned long long>(cpu.m_gpr[31].m_lo),
        static_cast<unsigned long long>(cpu.m_gpr[4].m_lo), saved_s0_word, saved_ra_word);
    // The first nested function emits a NUL-terminated string through
    // EE SIO_TXFIFO. Derive expected bytes from the original ROM image,
    // independently of the guest's live RAM reads and SIO writes.
    constexpr uint32_t message_physical = 0x000197e6u;
    constexpr uint32_t message_offset = message_physical - destination;
    static_assert(message_offset < image_size);
    constexpr uint32_t tx_capacity = 1024u;
    uint8_t tx_bytes[tx_capacity]{};
    ee::s_ee_reset_sio_tx sio{tx_bytes, tx_capacity, 0u};
    uint32_t message_size = 0u;
    for (; message_offset + message_size < image_size && message_size < tx_capacity; ++message_size)
    {
        if (bytes[source_offset + message_offset + message_size] == 0u)
        {
            break;
        }
    }
    if (message_size == 0u || message_size == tx_capacity ||
        message_offset + message_size >= image_size ||
        bytes[source_offset + message_offset] != 0x5bu)
    {
        std::fprintf(stderr, "EELOAD expected console string invalid\n");
        return 36;
    }
    // First LBU/BEQ+slot/ADDIU/LUI/ORI = six retirements.
    // Each byte then takes SB/ADDIU/LBU/BNE+slot, followed by JR+slot.
    const uint32_t console_checkpoint = nested_checkpoint + 8u + 5u * message_size;
    for (; retired < console_checkpoint;)
    {
        const uint32_t pc = cpu.m_pc;
        const auto action = ee::ee_execute_strict_rom_register_step(cpu, rom,
            console_checkpoint - retired, ram, &sio);
        if (action.m_result != ee::e_ee_reset_prefix_result::COMMITTED ||
            action.m_retired == 0u || action.m_retired > console_checkpoint - retired)
        {
            std::fprintf(stderr, "EELOAD console refused: pc=%08x opcode=%08x result=%u detail_pc=%08x address=%08x retired=%u bytes=%zu\n",
                pc, action.m_opcode, static_cast<unsigned>(action.m_result),
                action.m_detail_pc, action.m_data_address, retired, sio.m_count);
            return 37;
        }
        retired += action.m_retired;
    }
    if (cpu.m_pc != 0xa00161f4u || sio.m_count != message_size ||
        cpu.m_gpr[2].m_lo != 0u || cpu.m_gpr[29].m_lo != 0xffffffffa007ffa8ull)
    {
        std::fprintf(stderr, "EELOAD console checkpoint mismatch: retired=%u pc=%08x bytes=%zu/%u\n",
            retired, cpu.m_pc, sio.m_count, message_size);
        return 38;
    }
    for (uint32_t i = 0u; i < message_size; ++i)
    {
        if (tx_bytes[i] != bytes[source_offset + message_offset + i])
        {
            std::fprintf(stderr, "EELOAD console byte mismatch: offset=%u\n", i);
            return 39;
        }
    }
    std::printf("EELOAD console: retired=%u pc=%08x bytes=%zu hex=", retired, cpu.m_pc, sio.m_count);
    for (uint32_t i = 0u; i < message_size; ++i)
    {
        std::printf("%02x", static_cast<unsigned>(tx_bytes[i]));
    }
    std::putchar('\n');
    std::printf("EELOAD console text: ");
    for (uint32_t i = 0u; i < message_size; ++i)
    {
        const uint8_t byte = tx_bytes[i];
        std::putchar(byte >= 32u && byte <= 126u ? byte : '.');
    }
    std::putchar('\n');

    // Require the real Config read/modify/write and SYNC.P sequence.
    // Fixed cache-size fields must survive, and cached accesses still
    // refuse because this probe has no cache backing or timing model.
    constexpr uint32_t expected_config = 0x00073443u;
    const uint32_t config_checkpoint = console_checkpoint + 9u;
    if (cpu.m_cop0[ee::COP0_Config] != 0x440u)
    {
        std::fprintf(stderr, "EELOAD Config reset mismatch\n");
        return 40;
    }
    for (; retired < config_checkpoint;)
    {
        const auto action = ee::ee_execute_strict_rom_register_step(cpu, rom,
            config_checkpoint - retired, ram, &sio);
        if (action.m_result != ee::e_ee_reset_prefix_result::COMMITTED ||
            action.m_retired == 0u || action.m_retired > config_checkpoint - retired)
        {
            std::fprintf(stderr, "EELOAD Config refused: pc=%08x opcode=%08x result=%u retired=%u\n",
                cpu.m_pc, action.m_opcode, static_cast<unsigned>(action.m_result), retired);
            return 41;
        }
        retired += action.m_retired;
    }
    if (cpu.m_pc != 0xa0016218u || cpu.m_cop0[ee::COP0_Config] != expected_config ||
        cpu.m_gpr[3].m_lo != 0x440u || cpu.m_gpr[2].m_lo != expected_config ||
        cpu.m_cop0[ee::COP0_Status] != 0x70000000u || sio.m_count != message_size)
    {
        std::fprintf(stderr, "EELOAD Config checkpoint mismatch: retired=%u pc=%08x config=%08x v0=%016llx v1=%016llx\n",
            retired, cpu.m_pc, cpu.m_cop0[ee::COP0_Config],
            static_cast<unsigned long long>(cpu.m_gpr[2].m_lo),
            static_cast<unsigned long long>(cpu.m_gpr[3].m_lo));
        return 42;
    }
    std::printf("EELOAD Config: retired=%u pc=%08x config=%08x old=%016llx\n",
        retired, cpu.m_pc, cpu.m_cop0[ee::COP0_Config],
        static_cast<unsigned long long>(cpu.m_gpr[3].m_lo));

    // The next routine constructs handler instruction words, then
    // writes through a firmware table of low-RAM offsets. Derive the
    // first destination from the pinned ROM, not from the executor.
    constexpr uint32_t vector_table_physical = 0x0001a1a4u;
    constexpr uint32_t vector_table_offset = vector_table_physical - destination;
    static_assert(vector_table_offset + 4u <= image_size);
    const uint32_t vector_offset = ee::ee_read_instruction_word(
        bytes.data() + source_offset + vector_table_offset);
    constexpr uint32_t first_vector_word = 0x3c1aa001u;
    constexpr uint32_t second_vector_word = 0x375a9160u;
    if ((vector_offset & 3u) != 0u || vector_offset > destination - 4u)
    {
        std::fprintf(stderr, "EELOAD unexpected first vector table offset: %08x\n", vector_offset);
        return 43;
    }
    for (uint32_t i = 0u; i < destination; ++i)
    {
        if (ram_bytes[i] != 0xa5u)
        {
            std::fprintf(stderr, "EELOAD low RAM initial sentinel mismatch: physical=%08x\n", i);
            return 44;
        }
    }
    // Seven retirements through a0016704, then fifteen instructions
    // through the first SW at a0016740 (inclusive).
    const uint32_t vector_checkpoint = config_checkpoint + 22u;
    for (; retired < vector_checkpoint;)
    {
        const auto action = ee::ee_execute_strict_rom_register_step(cpu, rom,
            vector_checkpoint - retired, ram, &sio);
        if (action.m_result != ee::e_ee_reset_prefix_result::COMMITTED ||
            action.m_retired == 0u || action.m_retired > vector_checkpoint - retired)
        {
            std::fprintf(stderr, "EELOAD vector generation refused: retired=%u pc=%08x opcode=%08x result=%u detail_pc=%08x address=%08x\n",
                retired, cpu.m_pc, action.m_opcode, static_cast<unsigned>(action.m_result),
                action.m_detail_pc, action.m_data_address);
            return 45;
        }
        retired += action.m_retired;
    }
    if (cpu.m_pc != 0xa0016744u || cpu.m_gpr[2].m_lo != second_vector_word ||
        cpu.m_gpr[3].m_lo != first_vector_word ||
        cpu.m_gpr[11].m_lo != (0xffffffffa0000000ull + vector_offset) ||
        ee::ee_read_instruction_word(ram_bytes.data() + vector_offset) != first_vector_word ||
        cpu.m_cop0[ee::COP0_Config] != expected_config || sio.m_count != message_size)
    {
        std::fprintf(stderr, "EELOAD first vector word mismatch: retired=%u pc=%08x offset=%08x word=%08x v0=%016llx v1=%016llx t3=%016llx\n",
            retired, cpu.m_pc, vector_offset, ee::ee_read_instruction_word(ram_bytes.data() + vector_offset),
            static_cast<unsigned long long>(cpu.m_gpr[2].m_lo),
            static_cast<unsigned long long>(cpu.m_gpr[3].m_lo),
            static_cast<unsigned long long>(cpu.m_gpr[11].m_lo));
        return 46;
    }
    for (uint32_t i = 0u; i < destination; ++i)
    {
        if ((i < vector_offset || i >= vector_offset + 4u) && ram_bytes[i] != 0xa5u)
        {
            std::fprintf(stderr, "EELOAD first vector unexpected write: physical=%08x\n", i);
            return 47;
        }
    }
    std::printf("EELOAD first vector word: retired=%u pc=%08x physical=%08x word=%08x next_word=%08x\n",
        retired, cpu.m_pc, vector_offset, first_vector_word, second_vector_word);

    // The authentic routine returned after writing four complete
    // LUI/ORI/JR/NOP stubs. Validate each destination using the ROM
    // table, including every untouched byte of low RAM.
    constexpr uint32_t vector_count = 4u;
    constexpr uint32_t vector_words[] = {0x3c1aa001u, 0x375a9160u, 0x03400008u, 0u};
    constexpr uint32_t vector_bytes = sizeof(vector_words);
    static_assert(vector_table_offset + vector_count * 4u <= image_size);
    uint32_t vector_offsets[vector_count]{};
    for (uint32_t i = 0u; i < vector_count; ++i)
    {
        vector_offsets[i] = ee::ee_read_instruction_word(
            bytes.data() + source_offset + vector_table_offset + i * 4u);
        if ((vector_offsets[i] & 3u) != 0u || vector_offsets[i] > destination - vector_bytes)
        {
            std::fprintf(stderr, "EELOAD invalid vector offset: index=%u offset=%08x\n", i, vector_offsets[i]);
            return 48;
        }
        for (uint32_t j = 0u; j < i; ++j)
        {
            if (vector_offsets[i] < vector_offsets[j] + vector_bytes &&
                vector_offsets[j] < vector_offsets[i] + vector_bytes)
            {
                std::fprintf(stderr, "EELOAD overlapping vector offsets\n");
                return 49;
            }
        }
    }
    const uint32_t vectors_done = vector_checkpoint + 44u;
    for (; retired < vectors_done;)
    {
        const auto action = ee::ee_execute_strict_rom_register_step(cpu, rom,
            vectors_done - retired, ram, &sio);
        if (action.m_result != ee::e_ee_reset_prefix_result::COMMITTED ||
            action.m_retired == 0u || action.m_retired > vectors_done - retired)
        {
            std::fprintf(stderr, "EELOAD vectors refused: retired=%u pc=%08x opcode=%08x result=%u\n",
                retired, cpu.m_pc, action.m_opcode, static_cast<unsigned>(action.m_result));
            return 50;
        }
        retired += action.m_retired;
    }
    if (cpu.m_pc != 0xa0016220u || cpu.m_gpr[31].m_lo != 0x00000000a0016220ull ||
        cpu.m_cop0[ee::COP0_Config] != expected_config || sio.m_count != message_size)
    {
        std::fprintf(stderr, "EELOAD vector return mismatch: retired=%u pc=%08x\n", retired, cpu.m_pc);
        return 51;
    }
    for (uint32_t i = 0u; i < destination; ++i)
    {
        uint8_t expected = 0xa5u;
        for (uint32_t j = 0u; j < vector_count; ++j)
        {
            if (i >= vector_offsets[j] && i < vector_offsets[j] + vector_bytes)
            {
                const uint32_t offset = i - vector_offsets[j];
                expected = static_cast<uint8_t>(vector_words[offset / 4u] >> ((offset % 4u) * 8u));
                break;
            }
        }
        if (ram_bytes[i] != expected)
        {
            std::fprintf(stderr, "EELOAD vector byte mismatch: physical=%08x expected=%02x actual=%02x\n",
                i, static_cast<unsigned>(expected), static_cast<unsigned>(ram_bytes[i]));
            return 52;
        }
    }
    for (uint32_t i = 0u; i < vector_count; ++i)
    {
        std::printf("EELOAD vector installed: physical=%08x words=%08x,%08x,%08x,%08x\n",
            vector_offsets[i], vector_words[0], vector_words[1], vector_words[2], vector_words[3]);
    }
    std::printf("EELOAD vectors complete: retired=%u pc=%08x count=%u\n",
        retired, cpu.m_pc, vector_count);

    // The next call sequence writes Wired=0, serializes and returns.
    // Verify the guest write and return; do not manufacture Random
    // progression or clear entries from a host shortcut.
    const uint32_t wired_entry = vectors_done + 15u;
    const uint32_t wired_done = wired_entry + 4u;
    for (; retired < wired_entry;)
    {
        const auto action = ee::ee_execute_strict_rom_register_step(cpu, rom,
            wired_entry - retired, ram, &sio);
        if (action.m_result != ee::e_ee_reset_prefix_result::COMMITTED ||
            action.m_retired == 0u || action.m_retired > wired_entry - retired)
        {
            std::fprintf(stderr, "EELOAD Wired call refused: retired=%u pc=%08x opcode=%08x result=%u\n",
                retired, cpu.m_pc, action.m_opcode, static_cast<unsigned>(action.m_result));
            return 53;
        }
        retired += action.m_retired;
    }
    if (cpu.m_pc != 0xa00197acu || cpu.m_gpr[4].m_lo != 0u ||
        cpu.m_gpr[31].m_lo != 0x00000000a001635cull)
    {
        std::fprintf(stderr, "EELOAD Wired call entry mismatch\n");
        return 54;
    }
    const auto wired = ee::ee_execute_strict_rom_register_step(cpu, rom, 1u, ram, &sio);
    if (wired.m_result != ee::e_ee_reset_prefix_result::COMMITTED ||
        wired.m_opcode != 0x40843000u || wired.m_retired != 1u ||
        cpu.m_cop0[ee::COP0_Wired] != 0u || cpu.m_cop0[ee::COP0_Random] != 47u ||
        cpu.m_pc != 0xa00197b0u)
    {
        std::fprintf(stderr, "EELOAD Wired write mismatch\n");
        return 55;
    }
    retired += wired.m_retired;
    for (; retired < wired_done;)
    {
        const auto action = ee::ee_execute_strict_rom_register_step(cpu, rom,
            wired_done - retired, ram, &sio);
        if (action.m_result != ee::e_ee_reset_prefix_result::COMMITTED ||
            action.m_retired == 0u || action.m_retired > wired_done - retired)
        {
            std::fprintf(stderr, "EELOAD Wired return refused: pc=%08x opcode=%08x\n",
                cpu.m_pc, action.m_opcode);
            return 56;
        }
        retired += action.m_retired;
    }
    if (cpu.m_pc != 0xa001635cu || cpu.m_cop0[ee::COP0_Wired] != 0u ||
        cpu.m_cop0[ee::COP0_Random] != 47u || cpu.m_gpr[29].m_lo != 0xffffffffa007ff60ull)
    {
        std::fprintf(stderr, "EELOAD Wired return mismatch\n");
        return 57;
    }
    std::printf("EELOAD Wired: retired=%u pc=%08x wired=%u random=%u\n",
        retired, cpu.m_pc, cpu.m_cop0[ee::COP0_Wired], cpu.m_cop0[ee::COP0_Random]);

    const uint32_t exploration_limit = wired_done + 4096u;
    stop = {ee::e_ee_reset_prefix_result::BUDGET_EXHAUSTED};
    for (; retired < exploration_limit;)
    {
        const uint32_t pc = cpu.m_pc;
        const auto action = ee::ee_execute_strict_rom_register_step(cpu, rom,
            exploration_limit - retired, ram, &sio);
        if (action.m_result != ee::e_ee_reset_prefix_result::COMMITTED)
        {
            stop = action;
            break;
        }
        if (action.m_retired == 0u || action.m_retired > exploration_limit - retired)
        {
            return 29;
        }
        retired += action.m_retired;
        std::printf("EELOAD step: pc=%08x opcode=%08x count=%u next=%08x total=%u\n",
            pc, action.m_opcode, action.m_retired, cpu.m_pc, retired);
    }
    std::printf("EELOAD exploration stop: retired=%u pc=%08x result=%u detail_pc=%08x opcode=%08x data_address=%08x\n",
        retired, cpu.m_pc, static_cast<unsigned>(stop.m_result), stop.m_detail_pc,
        stop.m_opcode, stop.m_data_address);
    for (uint32_t i = 0u; i < 16u; ++i)
    {
        const uint32_t pc = cpu.m_pc + i * 4u;
        const auto plan = ee::ee_plan_instruction_fetch(tlb, pc,
            cpu.m_cop0[ee::COP0_Status], cpu.m_cop0[ee::COP0_Config],
            cpu.m_cop0[ee::COP0_Context], cpu.m_cop0[ee::COP0_EntryHi]);
        const uint8_t* look = ee::ee_fetch_strict_reset_word(plan, rom, ram);
        if (look == nullptr)
        {
            break;
        }
        const uint32_t opcode = ee::ee_read_instruction_word(look);
        std::printf("EELOAD stop lookahead: pc=%08x opcode=%08x decoded=%s\n",
            pc, opcode, ee::decode(pc, opcode).mnemonic);
    }
    for (uint32_t i = 0u; i < 32u; ++i)
    {
        std::printf("EELOAD stop GPR: r%02u=%016llx\n", i,
            static_cast<unsigned long long>(i == 0u ? 0u : cpu.m_gpr[i].m_lo));
    }
    return 0;
}
