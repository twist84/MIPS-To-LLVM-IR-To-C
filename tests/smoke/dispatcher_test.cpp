#include "ee_dispatcher.h"
#include "ee_dispatch_index.h"

#include <cstdio>
#include <initializer_list>
#include <string>

namespace
{
int g_pass = 0;
int g_fail = 0;

void check(bool ok, const char* name)
{
    if (ok)
    {
        ++g_pass;
        std::printf("PASS: %s\n", name);
    }
    else
    {
        ++g_fail;
        std::printf("FAIL: %s\n", name);
    }
}

uint32_t caller_entry(ee::s_ee_cpu_state* cpu, ee::s_ee_runtime*)
{
    if (cpu->m_pc == 0x00100000u)
    {
        cpu->m_gpr[31].m_lo = 0x00100008u;
        cpu->m_pc = 0x00102000u;
        return static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
    }

    if (cpu->m_pc == 0x00100008u)
    {
        cpu->m_gpr[2].m_lo = 0x55u;
        cpu->m_pc = 0xDEAD0000u;
        return static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
    }

    return static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_UNSUPPORTED);
}

uint32_t callee_entry(ee::s_ee_cpu_state* cpu, ee::s_ee_runtime*)
{
    cpu->m_pc = static_cast<uint32_t>(cpu->m_gpr[31].m_lo);
    return static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_RETURN);
}

uint32_t jalr_caller_entry(ee::s_ee_cpu_state* cpu, ee::s_ee_runtime*)
{
    if (cpu->m_pc == 0x00200000u)
    {
        // Model the JALR's target capture, encoded rd link, and delay slot:
        // the captured target must survive rs being overwritten by the slot.
        const uint64_t captured_target = cpu->m_gpr[8].m_lo;
        cpu->m_gpr[31].m_lo = 0x00200008u;
        cpu->m_gpr[8].m_lo = cpu->m_gpr[31].m_lo;
        cpu->m_pc = static_cast<uint32_t>(captured_target);
        return static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
    }

    if (cpu->m_pc == 0x00200008u)
    {
        cpu->m_gpr[17].m_lo = 0x66u;
        cpu->m_pc = 0xBADC0DEu;
        return static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
    }

    return static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_UNSUPPORTED);
}

uint32_t external_return_entry(ee::s_ee_cpu_state* cpu, ee::s_ee_runtime*)
{
    cpu->m_pc = 0xF0000000u;
    return static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_RETURN);
}

uint32_t null_return_entry(ee::s_ee_cpu_state* cpu, ee::s_ee_runtime*)
{
    cpu->m_pc = 0u;
    return static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_RETURN);
}

uint32_t zero_initial_pc_entry(ee::s_ee_cpu_state* cpu, ee::s_ee_runtime*)
{
    // Generated entry switches treat PC zero as the function's initial entry.
    if (cpu->m_pc == 0 || cpu->m_pc == 0x00400000u)
    {
        cpu->m_gpr[3].m_lo = 0x33u;
        cpu->m_pc = 0xDEAD0000u;
        return static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
    }

    return static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_UNSUPPORTED);
}

uint32_t host_yield_entry(ee::s_ee_cpu_state* cpu, ee::s_ee_runtime*)
{
    // Model one host-scheduled, fully committed guest loop slice. The
    // dispatcher must not consume another entry or turn this into a guest
    // interrupt: the outer host explicitly decides when to re-enter.
    cpu->m_gpr[8].m_lo += 1u;
    cpu->m_pc = 0x00310000u;
    return static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_HOST_YIELD);
}

uint32_t dispatch_loop_entry(ee::s_ee_cpu_state* cpu, ee::s_ee_runtime*)
{
    // Deliberately return to this same translated block forever.
    cpu->m_pc = 0x00300000u;
    return static_cast<uint32_t>(ee::e_ee_exit_reason::EE_EXIT_DISPATCH);
}

ee::s_recompiled_fn make_function(
    const char* name,
    uint32_t entry_pc,
    std::initializer_list<uint32_t> block_pcs)
{
    ee::s_recompiled_fn result{};
    result.name = name;
    result.vaddr = entry_pc;
    result.block_pcs.assign(block_pcs.begin(), block_pcs.end());
    result.block_count = static_cast<uint32_t>(result.block_pcs.size());
    return result;
}

bool is_exited(const ee::s_ee_dispatch_result& result, uint32_t reason)
{
    return result.status == ee::e_ee_dispatch_status::EXITED &&
           result.exit_reason == reason;
}

} // namespace

int main()
{
    using ee::e_ee_dispatch_status;
    using ee::e_ee_exit_reason;

    // A direct call dispatches to the callee. Its JR $ra returns to a caller
    // continuation block, which the same dispatcher invokes before returning
    // to the outer host.
    {
        ee::s_recompiled_elf compiled{};
        compiled.functions.push_back(
            make_function("caller", 0x00100000u, {0x00100000u, 0x00100008u}));
        compiled.functions.push_back(
            make_function("callee", 0x00102000u, {0x00102000u}));
        const ee::t_ee_translated_entry entries[] =
        {
            caller_entry,
            callee_entry
        };
        const ee::s_ee_dispatch_table table{&compiled, entries, 2u};

        ee::s_ee_cpu_state cpu{};
        cpu.m_pc = 0x00100000u;
        cpu.m_exit_reason = static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH);

        const ee::s_ee_dispatch_result result =
            ee::ee_dispatch_run(&cpu, nullptr, table, 16u);

        check(is_exited(result, static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH)) &&
              result.dispatch_count == 3u &&
              result.guest_pc == 0xDEAD0000u &&
              cpu.m_gpr[2].m_lo == 0x55u,
              "dispatch callee return into caller continuation");
    }

    // JALR's rs is overwritten by the delay slot, but the captured target
    // still selects the separately registered callee. The callee then returns
    // to the caller's second block.
    {
        ee::s_recompiled_elf compiled{};
        compiled.functions.push_back(
            make_function("jalr_caller", 0x00200000u,
                          {0x00200000u, 0x00200008u}));
        compiled.functions.push_back(
            make_function("jalr_callee", 0x00202000u, {0x00202000u}));
        const ee::t_ee_translated_entry entries[] =
        {
            jalr_caller_entry,
            callee_entry
        };
        const ee::s_ee_dispatch_table table{&compiled, entries, 2u};

        ee::s_ee_cpu_state cpu{};
        cpu.m_pc = 0x00200000u;
        cpu.m_gpr[8].m_lo = 0x00202000u;

        const ee::s_ee_dispatch_result result =
            ee::ee_dispatch_run(&cpu, nullptr, table, 16u);

        check(is_exited(result, static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH)) &&
              result.dispatch_count == 3u &&
              result.guest_pc == 0xBADC0DEu &&
              cpu.m_gpr[8].m_lo == 0x00200008u &&
              cpu.m_gpr[17].m_lo == 0x66u,
              "dispatch dynamic JALR target and resume caller");
    }

    // A translated JR $ra can return outside the compiled set. The dispatcher
    // must propagate EE_EXIT_RETURN to the outer host instead of treating the
    // unmapped address as an error or invoking another function.
    {
        ee::s_recompiled_elf compiled{};
        compiled.functions.push_back(
            make_function("external_return", 0x00280000u, {0x00280000u}));
        const ee::t_ee_translated_entry entries[] = {external_return_entry};
        const ee::s_ee_dispatch_table table{&compiled, entries, 1u};

        ee::s_ee_cpu_state cpu{};
        cpu.m_pc = 0x00280000u;

        const ee::s_ee_dispatch_result result =
            ee::ee_dispatch_run(&cpu, nullptr, table, 4u);

        check(is_exited(result, static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_RETURN)) &&
              result.dispatch_count == 1u &&
              result.guest_pc == 0xF0000000u,
              "propagate return to external guest address");
    }

    // After an actual translated function returns to null, PC zero must
    // not be resolved as the start-of-run entry sentinel.
    {
        ee::s_recompiled_elf compiled{};
        compiled.entry_point = 0x00400000u;
        compiled.functions.push_back(
            make_function("entry", 0x00400000u, {0x00400000u}));
        const ee::t_ee_translated_entry entries[] = {null_return_entry};
        const ee::s_ee_dispatch_table table{&compiled, entries, 1u};

        ee::s_ee_cpu_state cpu{};
        cpu.m_pc = 0x00400000u;
        const ee::s_ee_dispatch_result result =
            ee::ee_dispatch_run(&cpu, nullptr, table, 8u);

        check(is_exited(result, static_cast<uint32_t>(
                  e_ee_exit_reason::EE_EXIT_RETURN)) &&
              result.dispatch_count == 1u &&
              result.guest_pc == 0u,
              "null return PC exits rather than restarting ELF entry");
    }

    // PC zero is a supported generated-function entry sentinel and must
    // resolve to the ELF entry's translated function, not appear unmapped.
    {
        ee::s_recompiled_elf compiled{};
        compiled.entry_point = 0x00400000u;
        compiled.functions.push_back(
            make_function("entry", 0x00400000u, {0x00400000u}));
        const ee::t_ee_translated_entry entries[] = {zero_initial_pc_entry};
        const ee::s_ee_dispatch_table table{&compiled, entries, 1u};

        ee::s_ee_cpu_state cpu{};
        cpu.m_pc = 0u;
        cpu.m_exit_reason = static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH);

        const ee::s_ee_dispatch_result result =
            ee::ee_dispatch_run(&cpu, nullptr, table, 4u);

        check(is_exited(result, static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH)) &&
              result.dispatch_count == 1u &&
              result.guest_pc == 0xDEAD0000u &&
              cpu.m_gpr[3].m_lo == 0x33u,
              "map zero initial PC to the ELF entry function");
    }

    // An unknown initial PC is returned untouched to the outer dispatcher.
    {
        ee::s_recompiled_elf compiled{};
        compiled.functions.push_back(
            make_function("known", 0x00300000u, {0x00300000u}));
        const ee::t_ee_translated_entry entries[] = {dispatch_loop_entry};
        const ee::s_ee_dispatch_table table{&compiled, entries, 1u};

        ee::s_ee_cpu_state cpu{};
        cpu.m_pc = 0xDEAD0000u;
        cpu.m_exit_reason = static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH);

        const ee::s_ee_dispatch_result result =
            ee::ee_dispatch_run(&cpu, nullptr, table, 4u);

        check(is_exited(result, static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH)) &&
              result.dispatch_count == 0u &&
              result.guest_pc == 0xDEAD0000u,
              "return unknown initial PC to outer dispatcher");
    }

    // The dispatch budget prevents a malformed CFG or a guest loop from
    // monopolising the host thread indefinitely.
    {
        ee::s_recompiled_elf compiled{};
        compiled.functions.push_back(
            make_function("loop", 0x00300000u, {0x00300000u}));
        const ee::t_ee_translated_entry entries[] = {dispatch_loop_entry};
        const ee::s_ee_dispatch_table table{&compiled, entries, 1u};

        ee::s_ee_cpu_state cpu{};
        cpu.m_pc = 0x00300000u;

        const ee::s_ee_dispatch_result result =
            ee::ee_dispatch_run(&cpu, nullptr, table, 5u);

        check(result.status == e_ee_dispatch_status::DISPATCH_LIMIT &&
              result.exit_reason == static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH) &&
              result.dispatch_count == 5u &&
              result.guest_pc == 0x00300000u,
              "stop dispatcher at configured budget");
    }

    // A host-cooperative native loop yield is an EXITED status carrying a
    // distinct host-only reason, not a guest exception or a completed frame.
    // The host can re-enter the same PC with the exact committed state.
    {
        ee::s_recompiled_elf compiled{};
        compiled.functions.push_back(
            make_function("host_yield", 0x00310000u, {0x00310000u}));
        const ee::t_ee_translated_entry entries[] = {host_yield_entry};
        const ee::s_ee_dispatch_table table{&compiled, entries, 1u};

        ee::s_ee_cpu_state cpu{};
        cpu.m_pc = 0x00310000u;
        cpu.m_gpr[8].m_lo = 41u;
        cpu.m_gpr[8].m_hi = 0x0123456789abcdefull;
        cpu.m_cop0[13] = 0u; // No hardware IRQ or guest exception.

        const auto first = ee::ee_dispatch_run(&cpu, nullptr, table, 50u);
        const bool first_valid =
            is_exited(first, static_cast<uint32_t>(
                e_ee_exit_reason::EE_EXIT_HOST_YIELD)) &&
            first.dispatch_count == 1u &&
            first.guest_pc == 0x00310000u &&
            cpu.m_pc == 0x00310000u && cpu.m_gpr[8].m_lo == 42u &&
            cpu.m_gpr[8].m_hi == 0x0123456789abcdefull &&
            cpu.m_cop0[13] == 0u;

        const auto resumed = ee::ee_dispatch_run(&cpu, nullptr, table, 50u);
        check(first_valid &&
              is_exited(resumed, static_cast<uint32_t>(
                  e_ee_exit_reason::EE_EXIT_HOST_YIELD)) &&
              resumed.dispatch_count == 1u &&
              resumed.guest_pc == 0x00310000u &&
              cpu.m_gpr[8].m_lo == 43u &&
              cpu.m_gpr[8].m_hi == 0x0123456789abcdefull &&
              cpu.m_cop0[13] == 0u,
              "propagate exact host yield and resume same guest entry");
    }

    // Metadata and native entry arrays must remain index-aligned.
    {
        ee::s_recompiled_elf compiled{};
        compiled.functions.push_back(
            make_function("mismatch", 0x00400000u, {0x00400000u}));
        const ee::t_ee_translated_entry entries[] = {dispatch_loop_entry};
        const ee::s_ee_dispatch_table table{&compiled, entries, 0u};

        ee::s_ee_cpu_state cpu{};
        cpu.m_pc = 0x00400000u;

        const ee::s_ee_dispatch_result result =
            ee::ee_dispatch_run(&cpu, nullptr, table, 4u);

        check(result.status == e_ee_dispatch_status::INVALID_ARGUMENT &&
              result.dispatch_count == 0u,
              "reject mismatched dispatcher metadata and code arrays");
    }

    // The O(log n) dispatch index must pick exactly the function the linear
    // lookup picks: first owner in table order for overlapping block PCs,
    // and the PC-zero sentinel (ELF entry, or an explicit zero block PC).
    {
        ee::s_recompiled_elf compiled{};
        compiled.entry_point = 0x00200000u;
        compiled.functions.push_back(
            make_function("a", 0x00100000u, {0x00100010u, 0x00100000u, 0x00100008u}));
        compiled.functions.push_back(
            make_function("overlap", 0x00100008u, {0x00100008u, 0x00100020u}));
        compiled.functions.push_back(
            make_function("entry", 0x00200000u, {0x00200000u, 0x00200004u}));
        compiled.functions.push_back(
            make_function("late_zero", 0x00300000u, {0x00000000u, 0x00300000u}));
        const ee::s_ee_dispatch_index index = ee::ee_build_dispatch_index(compiled);

        bool same = true;
        for (uint32_t pc : {0x00000000u, 0x00100000u, 0x00100008u, 0x00100010u,
                            0x00100020u, 0x00100004u, 0x00200000u, 0x00200004u,
                            0x00300000u, 0x00300004u, 0xFFFFFFFCu})
        {
            same = same && ee::ee_find_dispatch_owner(compiled, index, pc) ==
                ee::find_recompiled_function(compiled, pc);
        }
        check(same, "dispatch index matches linear owner lookup");
        check(ee::ee_find_dispatch_owner(compiled, index, 0x00100008u) ==
                  &compiled.functions[0],
              "dispatch index keeps first owner of an overlapping block PC");
        check(ee::ee_find_dispatch_owner(compiled, index, 0u) ==
                  &compiled.functions[2],
              "dispatch index maps PC zero to the first entry/zero owner");

        // An indexed table dispatches the same way as an unindexed one.
        ee::s_recompiled_elf calls{};
        calls.functions.push_back(
            make_function("caller", 0x00100000u, {0x00100000u, 0x00100008u}));
        calls.functions.push_back(
            make_function("callee", 0x00102000u, {0x00102000u}));
        const ee::t_ee_translated_entry entries[] = {caller_entry, callee_entry};
        const ee::s_ee_dispatch_index call_index = ee::ee_build_dispatch_index(calls);
        const ee::s_ee_dispatch_table table{&calls, entries, 2u};
        ee::s_ee_cpu_state cpu{};
        cpu.m_pc = 0x00100000u;
        const ee::s_ee_dispatch_result result =
            ee::ee_dispatch_run_indexed(&cpu, nullptr, table, call_index, 8u);
        check(result.status == e_ee_dispatch_status::EXITED &&
              result.dispatch_count == 3u && cpu.m_gpr[2].m_lo == 0x55u &&
              result.guest_pc == 0xDEAD0000u,
              "indexed dispatch table runs caller, callee and continuation");
    }

    // A dense window must be exact for overlaps, holes, unaligned
    // metadata and the zero sentinel, even adjacent to UINT32_MAX.
    {
        ee::s_recompiled_elf compiled{};
        compiled.entry_point = 0xfffff000u;
        compiled.functions.push_back(make_function("first", 0xfffff000u,
            {0xfffff000u, 0xfffff004u, 0xfffff009u, 0xfffff010u,
             0xfffff014u, 0xfffff018u, 0xfffff01cu}));
        compiled.functions.push_back(make_function("overlap", 0xfffff004u,
            {0xfffff004u, 0xfffff008u, 0xfffff00cu, 0xfffff020u}));
        // Fill a compact range up to the last representable aligned PC.
        for (uint32_t offset = 0x100u; offset < 0x1000u; offset += 16u)
        {
            compiled.functions[1].block_pcs.push_back(0xfffff000u + offset);
        }
        compiled.functions[1].block_pcs.push_back(0xfffffffcu);
        const auto index = ee::ee_build_dispatch_index(compiled);
        bool same = index.m_dense_base == 0xfffff000u &&
            index.m_dense_owners.size() == 1024u;
        for (uint32_t offset = 0u; offset < 0x1000u; ++offset)
        {
            const uint32_t pc = 0xfffff000u + offset;
            same = same && ee::ee_find_dispatch_owner(compiled, index, pc) ==
                ee::find_recompiled_function(compiled, pc);
        }
        for (uint32_t pc : {0u, 4u, 0xffffeffcu})
        {
            same = same && ee::ee_find_dispatch_owner(compiled, index, pc) ==
                ee::find_recompiled_function(compiled, pc);
        }
        check(same, "dense dispatch index matches reference including holes and high PCs");

        compiled.functions.push_back(make_function("far", 0x1000u,
            {0x1000u}));
        const auto sparse = ee::ee_build_dispatch_index(compiled);
        check(sparse.m_dense_owners.empty() &&
            ee::ee_find_dispatch_owner(compiled, sparse, 0x1000u) ==
                &compiled.functions[2] &&
            ee::ee_find_dispatch_owner(compiled, sparse, 0xfffffffcu) ==
                &compiled.functions[1] &&
            ee::ee_find_dispatch_owner(compiled, sparse, 0xfffff002u) == nullptr,
            "sparse address span retains bounded sorted dispatch lookup");
    }

    // The HLE runner normally executes exactly one translated function
    // per boundary. Compare indexed and linear behavior across terminal
    // exits, the budget, sparse/dense holes, zero and continuation PCs.
    {
        ee::s_recompiled_elf compiled{};
        compiled.entry_point = 0x00100000u;
        compiled.functions.push_back(make_function("caller", 0x00100000u,
            {0x00100000u, 0x00100008u}));
        compiled.functions.push_back(make_function("callee", 0x00100010u,
            {0x00100010u}));
        compiled.functions.push_back(make_function("null", 0x00100020u,
            {0x00100020u}));
        compiled.functions.push_back(make_function("yield", 0x00100030u,
            {0x00100030u}));
        compiled.functions.push_back(make_function("external", 0x00100040u,
            {0x00100040u}));
        compiled.functions.push_back(make_function("missing_native", 0x00100050u,
            {0x00100050u}));
        const ee::t_ee_translated_entry entries[] =
        {
            caller_entry, callee_entry, null_return_entry, host_yield_entry,
            external_return_entry, nullptr
        };
        const ee::s_ee_dispatch_table table{&compiled, entries, 6u};
        const auto index = ee::ee_build_dispatch_index(compiled);
        bool same = !index.m_dense_owners.empty();
        for (uint32_t pc : {0u, 0x00100000u, 0x00100004u, 0x00100008u,
                            0x00100010u, 0x00100020u, 0x00100030u,
                            0x00100040u, 0x00100050u, 0xdead0000u})
        {
            ee::s_ee_cpu_state linear{};
            linear.m_pc = pc;
            linear.m_exit_reason = static_cast<uint32_t>(e_ee_exit_reason::EE_EXIT_DISPATCH);
            linear.m_gpr[31].m_lo = 0x00100008u;
            ee::s_ee_cpu_state indexed = linear;
            const auto a = ee::ee_dispatch_run(&linear, nullptr, table, 1u);
            const auto b = ee::ee_dispatch_run_indexed(&indexed, nullptr, table, index, 1u);
            same = same && a.status == b.status &&
                a.dispatch_count == b.dispatch_count &&
                a.guest_pc == b.guest_pc && a.exit_reason == b.exit_reason &&
                linear.m_pc == indexed.m_pc &&
                linear.m_exit_reason == indexed.m_exit_reason &&
                linear.m_gpr[2].m_lo == indexed.m_gpr[2].m_lo &&
                linear.m_gpr[8].m_lo == indexed.m_gpr[8].m_lo &&
                linear.m_gpr[31].m_lo == indexed.m_gpr[31].m_lo;
        }
        ee::s_ee_cpu_state cpu{};
        cpu.m_pc = 0x00100000u;
        const auto result = ee::ee_dispatch_run_indexed(&cpu, nullptr, table, index, 1u);
        check(same && result.dispatch_count == 1u &&
              result.status == e_ee_dispatch_status::DISPATCH_LIMIT &&
              result.guest_pc == 0x00102000u,
              "single-slice dispatch preserves budget, terminal exits and exact ownership");
    }

    std::printf("dispatcher_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
