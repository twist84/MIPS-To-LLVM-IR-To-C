// Differential test: the same R3000 program runs on the IOP interpreter and
// through ahead-of-time LLVM IR (iop_ir_lifter -> clang -> shared library),
// with small instruction budgets so translated code exits and resumes in
// the middle of loops and delay slots. Final GPR/HI/LO/PC, run result and
// RAM must match exactly.
//
// usage: iop_aot_test WORK_DIR   (needs clang on PATH; skips otherwise)

#include "iop_aot.h"
#include "iop_cpu.h"
#include "iop_hw.h"
#include "iop_ir_lifter.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
constexpr uint32_t k_base = 0x00010000u;
constexpr uint32_t k_data = 0x00020000u;
constexpr uint32_t k_mmio_pc = k_base + 0x200u;

uint32_t r_type(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sa, uint32_t funct)
{
    return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct;
}

uint32_t i_type(uint32_t op, uint32_t rs, uint32_t rt, uint32_t imm)
{
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xffffu);
}

uint32_t j_type(uint32_t op, uint32_t target)
{
    return (op << 26) | ((target >> 2) & 0x03ffffffu);
}

enum : uint32_t
{
    zero = 0, at = 1, v0 = 2, v1 = 3, a0 = 4, a1 = 5, a2 = 6, a3 = 7,
    t0 = 8, t1 = 9, t2 = 10, t3 = 11, t4 = 12, t5 = 13, t6 = 14, t7 = 15,
    s0 = 16, s1 = 17, s2 = 18, s3 = 19, s4 = 20, s5 = 21, s6 = 22, s7 = 23,
    t8 = 24, t9 = 25, sp = 29, ra = 31
};

std::vector<uint32_t> program()
{
    std::vector<uint32_t> p;
    const auto pc = [&p]() { return k_base + static_cast<uint32_t>(p.size()) * 4u; };
    p.push_back(i_type(0x09, sp, sp, 0xfff0));          // addiu sp,sp,-16
    p.push_back(i_type(0x0f, 0, t0, k_data >> 16));      // lui t0,data
    p.push_back(i_type(0x09, zero, t1, 10));            // addiu t1,zero,10
    p.push_back(i_type(0x09, zero, t2, 0));             // addiu t2,zero,0
    const uint32_t loop = pc();
    p.push_back(i_type(0x2b, t0, t1, 0));               // sw t1,0(t0)
    p.push_back(i_type(0x23, t0, t3, 0));               // lw t3,0(t0)
    p.push_back(r_type(t2, t3, t2, 0, 0x21));           // addu t2,t2,t3
    p.push_back(i_type(0x09, t1, t1, 0xffff));          // addiu t1,t1,-1
    p.push_back(i_type(0x05, t1, zero,
                       (loop - (pc() + 4u)) >> 2));     // bne t1,zero,loop
    p.push_back(i_type(0x09, t0, t0, 4));               // addiu t0,t0,4 (delay)
    const size_t call_site = p.size();
    p.push_back(0u);                                    // jal func (patched)
    p.push_back(i_type(0x09, zero, a0, 7));             // addiu a0,zero,7 (delay)
    p.push_back(r_type(t2, a0, 0, 0, 0x18));            // mult t2,a0
    p.push_back(r_type(0, 0, s0, 0, 0x12));             // mflo s0
    p.push_back(r_type(0, 0, s1, 0, 0x10));             // mfhi s1
    p.push_back(i_type(0x09, zero, a1, 0xfffd));        // addiu a1,zero,-3
    p.push_back(r_type(t2, a1, 0, 0, 0x1a));            // div t2,a1
    p.push_back(r_type(0, 0, s2, 0, 0x12));             // mflo s2
    p.push_back(r_type(0, 0, s3, 0, 0x10));             // mfhi s3
    p.push_back(r_type(t2, zero, 0, 0, 0x1b));          // divu t2,zero
    p.push_back(r_type(0, 0, s4, 0, 0x12));             // mflo s4
    p.push_back(i_type(0x09, zero, t4, 0x80ff));        // addiu t4,zero,0xffff80ff
    p.push_back(i_type(0x29, t0, t4, 0));               // sh t4,0(t0)
    p.push_back(i_type(0x20, t0, t5, 1));               // lb t5,1(t0)
    p.push_back(i_type(0x21, t0, t6, 0));               // lh t6,0(t0)
    p.push_back(i_type(0x24, t0, t7, 1));               // lbu t7,1(t0)
    p.push_back(i_type(0x25, t0, s5, 0));               // lhu s5,0(t0)
    p.push_back(r_type(t5, t7, s6, 0, 0x2a));           // slt s6,t5,t7
    p.push_back(r_type(t5, t7, s7, 0, 0x2b));           // sltu s7,t5,t7
    p.push_back(r_type(0, t6, t8, 4, 0x03));            // sra t8,t6,4
    p.push_back(r_type(a0, t6, t9, 0, 0x06));           // srlv t9,t6,a0
    p.push_back(r_type(t8, t9, v1, 0, 0x27));           // nor v1,t8,t9
    p.push_back(i_type(0x0a, t5, a2, 0xff00));          // slti a2,t5,-256
    p.push_back(i_type(0x0b, t5, a3, 0xff00));          // sltiu a3,t5,0xffffff00
    p.push_back(i_type(0x06, t5, zero, 2));             // blez t5,+2
    p.push_back(i_type(0x0d, zero, at, 0x1234));        // ori at,zero,0x1234 (delay)
    p.push_back(i_type(0x09, zero, at, 0x5555));        // skipped
    p.push_back(i_type(0x2b, t0, at, 8));               // sw at,8(t0)
    p.push_back(i_type(0x28, t0, s0, 12));              // sb s0,12(t0)
    p.push_back(i_type(0x0f, 0, t3, 0x7fff));           // lui t3,0x7fff
    p.push_back(i_type(0x0d, t3, t3, 0xffff));          // ori t3,t3,0xffff
    p.push_back(i_type(0x08, t3, t3, 1));               // addi t3,t3,1 -> overflow
    const uint32_t func = pc();
    p.push_back(r_type(a0, a0, v0, 0, 0x21));           // addu v0,a0,a0
    p.push_back(r_type(ra, 0, 0, 0, 0x08));             // jr ra
    p.push_back(i_type(0x09, v0, v0, 1));               // addiu v0,v0,1 (delay)
    p[call_site] = j_type(0x03, func);
    p.resize((k_mmio_pc - k_base) / 4u, 0u);
    // Native RAM/ALU work followed by device accesses, including a
    // device read in a translated branch's delay slot.
    p.push_back(i_type(0x0f, zero, t0, 0x1f80));
    p.push_back(i_type(0x0d, t0, t0, 0x14a0));
    p.push_back(i_type(0x09, zero, t1, 7));
    p.push_back(i_type(0x2b, t0, t1, 0)); // timer5 COUNT = 7
    p.push_back(i_type(0x23, t0, t2, 0)); // reads COUNT after retirement
    p.push_back(i_type(0x04, zero, zero, 2));
    p.push_back(i_type(0x23, t0, t3, 0)); // MMIO in delay slot
    p.push_back(i_type(0x09, zero, t3, 0x5555)); // skipped
    p.push_back(i_type(0x23, t0, t4, 4)); // timer5 MODE
    p.push_back(i_type(0x23, t0, t5, 0));
    p.push_back(0x0000000cu); // syscall
    return p;
}

struct s_outcome
{
    iop::s_cpu_state m_state{};
    iop::e_step_result m_result = iop::e_step_result::completed;
    std::vector<uint8_t> m_data;
    uint64_t m_instructions = 0u;
};

s_outcome run(iop::c_iop_aot* aot)
{
    iop::c_cpu cpu;
    cpu.reset(k_base);
    const std::vector<uint32_t> code = program();
    for (size_t i = 0; i < code.size(); ++i)
    {
        cpu.write32(k_base + static_cast<uint32_t>(i) * 4u, code[i]);
    }
    cpu.state().m_gpr[sp] = 0x00030000u;
    if (aot != nullptr)
    {
        aot->refresh(cpu);
        cpu.set_aot(aot);
    }
    s_outcome outcome;
    for (int slice = 0; slice < 10000; ++slice)
    {
        const iop::s_run_result result = cpu.run(5u);
        outcome.m_instructions += result.m_instructions;
        if (result.m_result != iop::e_step_result::budget_exhausted)
        {
            outcome.m_result = result.m_result;
            break;
        }
    }
    outcome.m_state = cpu.state();
    outcome.m_data.resize(64);
    cpu.dma_read(k_data, outcome.m_data.data(), 64);
    return outcome;
}
// Final-state comparison alone misses an overrun that is later
// compensated by a shorter slice. Compare every scheduler boundary.
bool compare_slices(iop::c_iop_aot& aot, uint32_t budget)
{
    iop::c_cpu reference;
    iop::c_cpu translated;
    const auto code = program();
    for (auto* cpu : {&reference, &translated})
    {
        cpu->reset(k_base);
        for (size_t i = 0u; i < code.size(); ++i)
        {
            if (!cpu->write32(k_base + static_cast<uint32_t>(i) * 4u, code[i]))
            {
                return false;
            }
        }
        cpu->state().m_gpr[sp] = 0x00030000u;
    }
    aot.refresh(translated);
    translated.set_aot(&aot);
    const uint64_t native_before = aot.instructions();
    for (uint32_t slice = 0u; slice < 10000u; ++slice)
    {
        const auto a = reference.run(budget);
        const auto b = translated.run(budget);
        const auto& x = reference.state();
        const auto& y = translated.state();
        uint8_t ram_a[64]{}, ram_b[64]{};
        if (a.m_result != b.m_result || a.m_instructions != b.m_instructions ||
            b.m_instructions > budget || a.m_pc != b.m_pc ||
            a.m_opcode != b.m_opcode || reference.cycles() != translated.cycles() ||
            x.m_pc != y.m_pc || x.m_next_pc != y.m_next_pc ||
            x.m_in_delay_slot != y.m_in_delay_slot ||
            (x.m_in_delay_slot && x.m_branch_pc != y.m_branch_pc) ||
            x.m_hi != y.m_hi || x.m_lo != y.m_lo ||
            std::memcmp(x.m_gpr, y.m_gpr, sizeof(x.m_gpr)) != 0 ||
            !reference.dma_read(k_data, ram_a, sizeof(ram_a)) ||
            !translated.dma_read(k_data, ram_b, sizeof(ram_b)) ||
            std::memcmp(ram_a, ram_b, sizeof(ram_a)) != 0)
        {
            std::fprintf(stderr, "IOP slice mismatch: budget=%u slice=%u "
                "interp=%u native=%u pc=%08x/%08x next=%08x/%08x\n",
                budget, slice, a.m_instructions, b.m_instructions,
                x.m_pc, y.m_pc, x.m_next_pc, y.m_next_pc);
            return false;
        }
        if (a.m_result != iop::e_step_result::budget_exhausted)
        {
            return a.m_result == iop::e_step_result::overflow &&
                aot.instructions() > native_before;
        }
    }
    return false;
}
bool compare_hardware_slices(iop::c_iop_aot& aot, uint32_t budget)
{
    iop::c_hw reference_hw, translated_hw;
    iop::c_cpu reference, translated;
    reference.bind_hardware(&reference_hw);
    translated.bind_hardware(&translated_hw);
    const auto code = program();
    for (auto* cpu : {&reference, &translated})
    {
        cpu->reset(k_mmio_pc);
        for (size_t i = 0u; i < code.size(); ++i)
        {
            if (!cpu->write32(k_base + static_cast<uint32_t>(i) * 4u, code[i]))
            {
                return false;
            }
        }
        // A real timer compare interrupts the native instruction window.
        if (!cpu->write32(iop::c_hw::k_timer5_target, 2u) ||
            !cpu->write32(iop::c_hw::k_timer5_mode, 0x58u))
        {
            return false;
        }
    }
    aot.refresh(translated);
    translated.set_aot(&aot);
    const uint64_t native_before = aot.instructions();
    for (uint32_t slice = 0u; slice < 32u; ++slice)
    {
        const auto a = reference.run(budget);
        const auto b = translated.run(budget);
        const auto& x = reference.state();
        const auto& y = translated.state();
        const auto h1 = reference_hw.scheduler_checkpoint();
        const auto h2 = translated_hw.scheduler_checkpoint();
        if (a.m_result != b.m_result || a.m_instructions != b.m_instructions ||
            a.m_pc != b.m_pc || a.m_opcode != b.m_opcode ||
            reference.cycles() != translated.cycles() ||
            x.m_pc != y.m_pc || x.m_next_pc != y.m_next_pc ||
            x.m_in_delay_slot != y.m_in_delay_slot ||
            x.m_branch_pc != y.m_branch_pc ||
            std::memcmp(x.m_gpr, y.m_gpr, sizeof(x.m_gpr)) != 0 ||
            h1.m_timer_count != h2.m_timer_count ||
            h1.m_timer_mode != h2.m_timer_mode ||
            h1.m_timer_remainder != h2.m_timer_remainder ||
            h1.m_timer_armed != h2.m_timer_armed ||
            h1.m_irq_stat != h2.m_irq_stat)
        {
            std::fprintf(stderr, "IOP device slice mismatch: budget=%u slice=%u\n",
                budget, slice);
            return false;
        }
        if (a.m_result != iop::e_step_result::budget_exhausted)
        {
            return a.m_result == iop::e_step_result::syscall &&
                aot.instructions() > native_before;
        }
    }
    return false;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: %s WORK_DIR\n", argv[0]);
        return 2;
    }
    const std::filesystem::path work = argv[1];
    std::filesystem::create_directories(work);

    const std::vector<uint32_t> code = program();
    iop::s_iop_ir_module module;
    module.m_name = "aot_test";
    module.m_base = k_base;
    for (uint32_t word : code)
    {
        for (int b = 0; b < 4; ++b)
        {
            module.m_text.push_back(static_cast<uint8_t>(word >> (8 * b)));
        }
    }
    const uint64_t hash = iop::iop_aot_hash(module.m_text.data(), module.m_text.size());
    {
        std::ofstream(work / "mod.ll") << iop::iop_lift_module_ir(module, "iop_aot_fn_0");
#if defined(_WIN32)
        const bool windows = true;
        const std::string library = (work / "iop-aot-test.dll").string();
#else
        const bool windows = false;
        const std::string library = (work / "iop-aot-test.so").string();
#endif
        std::ofstream(work / "table.ll") << iop::iop_table_module_ir(
            {{module.m_name, k_base, static_cast<uint32_t>(module.m_text.size()),
              hash, "iop_aot_fn_0"}}, windows);
        std::string command = "clang -O2 -shared -fuse-ld=lld -Wno-override-module -o \"" +
            library + "\" \"" + (work / "mod.ll").string() + "\" \"" +
            (work / "table.ll").string() + "\"";
#if defined(_WIN32)
        command += " -nostdlib -Wl,/noentry";
#else
        command += " -fPIC";
#endif
        if (std::system(command.c_str()) != 0)
        {
            std::printf("SKIP: clang could not build the translated library\n");
            return 0;
        }
        iop::c_iop_aot aot;
        std::string error;
        if (!aot.open(library, &error))
        {
            std::printf("FAIL: %s\n", error.c_str());
            return 1;
        }
        const s_outcome reference = run(nullptr);
        const s_outcome translated = run(&aot);
        int failures = 0;
        const auto check = [&failures](bool ok, const char* what)
        {
            std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
            failures += ok ? 0 : 1;
        };
        check(reference.m_result == iop::e_step_result::overflow,
              "reference stops at the ADDI overflow");
        check(translated.m_result == reference.m_result, "same run result");
        check(std::memcmp(translated.m_state.m_gpr, reference.m_state.m_gpr,
                          sizeof(reference.m_state.m_gpr)) == 0, "same GPRs");
        check(translated.m_state.m_hi == reference.m_state.m_hi &&
              translated.m_state.m_lo == reference.m_state.m_lo, "same HI/LO");
        check(translated.m_state.m_pc == reference.m_state.m_pc &&
              translated.m_state.m_next_pc == reference.m_state.m_next_pc,
              "same resume PC");
        check(translated.m_data == reference.m_data, "same RAM");
        check(translated.m_instructions == reference.m_instructions,
              "same retired instruction count");
        check(aot.instructions() > 0u, "translated code executed");
        for (uint32_t budget = 1u; budget <= 17u; ++budget)
        {
            check(compare_slices(aot, budget), "exact per-slice native budget and state");
            check(compare_hardware_slices(aot, budget), "exact MMIO and timer retirement");
        }
        // Split the existing synthetic program at the direct-call target
        // and inside the attached-hardware sequence. Deliberately leave
        // one ordinary PC unowned to exercise interpreter fallback.
        std::vector<std::vector<uint32_t>> regions(4);
        const uint32_t callee = ((code[10] & 0x03ffffffu) << 2);
        for (uint32_t pc = k_base; pc < k_base + module.m_text.size(); pc += 4u)
        {
            if (pc == k_base + 4u || (pc >= callee + 12u && pc < k_mmio_pc))
            {
                continue;
            }
            const size_t region = pc < callee ? 0u : pc < k_mmio_pc ? 1u :
                pc < k_mmio_pc + 12u ? 2u : 3u;
            regions[region].push_back(pc);
        }
        std::vector<std::string> split_code;
        std::string router;
        const bool split_ok = iop::iop_lift_regions_ir(module, regions,
            "iop_split_entry", &split_code, &router);
        check(split_ok && split_code.size() == regions.size(), "split IR generation");
        if (!split_ok)
        {
            return 1;
        }
        auto invalid = regions;
        invalid[1].insert(invalid[1].begin(), k_base);
        std::vector<std::string> untouched{"sentinel"};
        std::string untouched_router = "sentinel";
        check(!iop::iop_lift_regions_ir(module, invalid, "invalid", &untouched,
            &untouched_router) && untouched == std::vector<std::string>{"sentinel"} &&
            untouched_router == "sentinel", "reject overlapping region ownership");
#if defined(_WIN32)
        const std::string split_library = (work / "iop-aot-split.dll").string();
#else
        const std::string split_library = (work / "iop-aot-split.so").string();
#endif
        std::ofstream(work / "split-router.ll") << router;
        std::ofstream(work / "split-table.ll") << iop::iop_table_module_ir(
            {{module.m_name, k_base, static_cast<uint32_t>(module.m_text.size()),
              hash, "iop_split_entry"}}, windows);
        std::string split_command = "clang -O2 -shared -fuse-ld=lld -Wno-override-module -o \"" +
            split_library + "\" \"" + (work / "split-router.ll").string() +
            "\" \"" + (work / "split-table.ll").string() + "\"";
        for (size_t i = 0u; i < split_code.size(); ++i)
        {
            const auto path = work / ("split-" + std::to_string(i) + ".ll");
            std::ofstream(path) << split_code[i];
            split_command += " \"" + path.string() + "\"";
        }
#if defined(_WIN32)
        split_command += " -nostdlib -Wl,/noentry";
#else
        split_command += " -fPIC";
#endif
        if (std::system(split_command.c_str()) != 0)
        {
            std::printf("FAIL: split library failed to compile\n");
            return 1;
        }
        iop::c_iop_aot split;
        if (!split.open(split_library, &error))
        {
            std::printf("FAIL: %s\n", error.c_str());
            return 1;
        }
        iop::c_cpu activation;
        activation.reset(k_base);
        for (size_t i = 0u; i < code.size(); ++i)
        {
            activation.write32(k_base + static_cast<uint32_t>(i) * 4u, code[i]);
        }
        split.refresh(activation);
        const auto* descriptor = split.find(k_base);
        check(descriptor != nullptr && split.module_count() == 1u,
            "single split descriptor activates original linked image");
        if (descriptor == nullptr)
        {
            return 1;
        }
        s_iop_aot_context ctx{};
        check(descriptor->m_entry(&ctx, k_base) == IOP_AOT_EXIT &&
            ctx.m_executed == 0u && ctx.m_pc == k_base &&
            ctx.m_next_pc == k_base + 4u && ctx.m_in_delay == 0u,
            "zero budget exits without native retirement");
        ctx.m_budget = 1u;
        check(descriptor->m_entry(&ctx, k_base + 4u) == IOP_AOT_INTERPRET &&
            ctx.m_executed == 0u && ctx.m_pc == k_base + 4u &&
            ctx.m_next_pc == k_base + 8u && ctx.m_in_delay == 0u,
            "unowned PC falls back without retirement");
        for (uint32_t budget = 1u; budget <= 17u; ++budget)
        {
            check(compare_slices(split, budget), "split exact per-slice budget and state");
            check(compare_hardware_slices(split, budget), "split exact MMIO and timer retirement");
        }
        for (int r = 0; r < 32; ++r)
        {
            if (translated.m_state.m_gpr[r] != reference.m_state.m_gpr[r])
            {
                std::printf("  r%d: interp=0x%08x aot=0x%08x\n", r,
                            reference.m_state.m_gpr[r], translated.m_state.m_gpr[r]);
            }
        }
        std::printf("iop_aot_test: %d failure(s), %llu translated instructions\n",
                    failures, static_cast<unsigned long long>(aot.instructions()));
        return failures == 0 ? 0 : 1;
    }
}
