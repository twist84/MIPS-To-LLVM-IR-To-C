#include "iop_kernel.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace
{
int fail(const char* message)
{
    std::fprintf(stderr, "IOP kernel: %s\n", message);
    return 1;
}

void put16(std::vector<uint8_t>& bytes, uint32_t at, uint16_t value)
{
    bytes[at] = static_cast<uint8_t>(value);
    bytes[at + 1u] = static_cast<uint8_t>(value >> 8u);
}

void put32(std::vector<uint8_t>& bytes, uint32_t at, uint32_t value)
{
    for (uint32_t index = 0u; index < 4u; ++index)
    {
        bytes[at + index] = static_cast<uint8_t>(
            value >> (index * 8u)
        );
    }
}

std::vector<uint8_t> make_module(bool with_import)
{
    std::vector<uint8_t> bytes(0x220u, 0u);
    bytes[0] = 0x7fu;
    bytes[1] = 'E';
    bytes[2] = 'L';
    bytes[3] = 'F';
    bytes[4] = 1u;
    bytes[5] = 1u;
    bytes[6] = 1u;
    put16(bytes, 16u, 0xff80u);
    put16(bytes, 18u, 8u);
    put32(bytes, 28u, 0x34u);
    put16(bytes, 40u, 52u);
    put16(bytes, 42u, 32u);
    put16(bytes, 44u, 2u);
    put32(bytes, 0x34u, 1u);
    put32(bytes, 0x38u, 0x100u);
    put32(bytes, 0x44u, 0x60u);
    put32(bytes, 0x48u, 0x80u);
    put32(bytes, 0x54u, 0x70000080u);
    put32(bytes, 0x58u, 0x180u);
    put32(bytes, 0x184u, 0u);  // entry offset
    put32(bytes, 0x188u, 0x50u); // gp offset
    put32(bytes, 0x18cu, 0x40u);
    put32(bytes, 0x190u, 0x20u);
    put32(bytes, 0x194u, 0x20u);
    const char name[] = "MODTEST";
    for (uint32_t index = 0u; index < sizeof(name); ++index)
    {
        bytes[0x19au + index] =
            static_cast<uint8_t>(name[index]);
    }

    put32(bytes, 0x100u, 0x24020007u); // addiu v0,zero,7
    put32(bytes, 0x104u, 0x0000000cu); // syscall 7
    put32(bytes, 0x108u, 0x24420005u); // addiu v0,v0,5
    put32(bytes, 0x10cu, 0x0000000du); // break

    if (with_import)
    {
        put32(bytes, 0x120u, 0x41e00000u);
        const char library[] = "LIBRARY";
        for (uint32_t index = 0u; index < 7u; ++index)
        {
            bytes[0x12cu + index] =
                static_cast<uint8_t>(library[index]);
        }
        put32(bytes, 0x134u, 0x03e00008u);
        put32(bytes, 0x138u, 0x24000007u);
    }
    return bytes;
}

bool put_program(
    iop::c_cpu& cpu, uint32_t base,
    const uint32_t* instructions, uint32_t count
)
{
    bool success = true;
    for (uint32_t index = 0u; index < count; ++index)
    {
        success = success && cpu.write32(
            base + index * 4u, instructions[index]
        );
    }
    return success;
}
} // namespace

int main()
{
    // The lookahead is advisory: querying it must not execute a
    // callback, retire a guest instruction or advance a device.
    {
        iop::c_kernel idle;
        auto& hw = idle.hardware();
        constexpr uint64_t rollover = uint64_t{1u} << 32u;
        if (idle.idle_event_cycles_remaining() != rollover ||
            !hw.write(iop::c_hw::k_timer5_mode, 4u, 0x2058u) ||
            !hw.write(iop::c_hw::k_timer5_target, 4u, 10u) ||
            idle.idle_event_cycles_remaining() != 80u)
        {
            return fail("IOP lookahead timer divisor/compare");
        }
        (void)idle.run(7u);
        if (idle.idle_event_cycles_remaining() != 73u ||
            idle.cycles() != 7u)
        {
            return fail("IOP lookahead timer fractional phase");
        }
        uint32_t callbacks = 0u;
        if (!idle.schedule_hle_event(3u, [&]() { ++callbacks; }) ||
            idle.idle_event_cycles_remaining() != 3u ||
            callbacks != 0u)
        {
            return fail("IOP lookahead HLE event");
        }
        (void)idle.run(3u);
        if (callbacks != 0u || idle.idle_event_cycles_remaining() != 0u)
        {
            return fail("IOP lookahead due HLE event");
        }
        (void)idle.run(1u);
        if (callbacks != 1u || idle.idle_event_cycles_remaining() != 69u)
        {
            return fail("IOP lookahead after HLE event");
        }
        const uint32_t thread = idle.create_thread(0x1800u, 0u, 8u, 0x400u);
        if (thread == 0u || idle.idle_event_cycles_remaining() != 69u ||
            !idle.start_thread(thread) ||
            idle.idle_event_cycles_remaining() != 0u ||
            !idle.stop_thread(thread) ||
            idle.idle_event_cycles_remaining() != 69u)
        {
            return fail("IOP lookahead ready/dormant transition");
        }
        if (!hw.write(iop::c_hw::k_timer5_mode, 4u, 0u) ||
            !hw.write(iop::c_spu2::k_base + 0x19au, 2u, 0u) ||
            idle.idle_event_cycles_remaining() != iop::c_spu2::k_cycles_per_sample)
        {
            return fail("IOP lookahead SPU2 sample boundary");
        }
        (void)idle.run(1u);
        if (idle.idle_event_cycles_remaining() !=
                iop::c_spu2::k_cycles_per_sample - 1u)
        {
            return fail("IOP lookahead SPU2 sample phase");
        }
    }

    // Before a known event, combining complete idle service turns must
    // leave device clocks, software events and guest-visible state equal.
    // The event itself is serviced with the original cadence.
    {
        auto baseline = std::make_unique<iop::c_kernel>();
        auto combined = std::make_unique<iop::c_kernel>();
        uint32_t baseline_callbacks = 0u;
        uint32_t combined_callbacks = 0u;
        const auto configure = [](iop::c_kernel& kernel, uint32_t& callbacks)
        {
            auto& hw = kernel.hardware();
            return hw.write(iop::c_hw::k_timer5_mode, 4u, 0x2058u) &&
                hw.write(iop::c_hw::k_timer5_target, 4u, 10000u) &&
                hw.write(iop::c_spu2::k_base + 0x19au, 2u, 0u) &&
                kernel.schedule_hle_event(600u, [&callbacks]() { ++callbacks; });
        };
        if (!configure(*baseline, baseline_callbacks) ||
            !configure(*combined, combined_callbacks))
        {
            return fail("IOP combined idle configuration");
        }
        (void)baseline->run(1u, 128u);
        (void)combined->run(1u, 128u);
        const auto equal = [&]()
        {
            auto& left = baseline->hardware();
            auto& right = combined->hardware();
            uint32_t left_count = 0u;
            uint32_t right_count = 0u;
            return left.read(iop::c_hw::k_timer5_count, 4u, &left_count) &&
                right.read(iop::c_hw::k_timer5_count, 4u, &right_count) &&
                left_count == right_count &&
                baseline->cycles() == combined->cycles() &&
                baseline->idle_event_cycles_remaining() ==
                    combined->idle_event_cycles_remaining() &&
                left.pending_interrupts() == right.pending_interrupts() &&
                left.spu2().clock_cycles() == right.spu2().clock_cycles() &&
                left.spu2().elapsed_samples() == right.spu2().elapsed_samples() &&
                baseline_callbacks == combined_callbacks;
        };
        if (combined->idle_event_cycles_remaining() != 599u)
        {
            return fail("IOP combined idle initial event");
        }
        const auto first = baseline->run(256u, 128u);
        const auto second = baseline->run(256u, 128u);
        const auto batch = combined->run(512u, 128u);
        if (first.m_executed != 0u || second.m_executed != 0u ||
            batch.m_executed != 0u || batch.m_stop != second.m_stop ||
            !equal() || baseline_callbacks != 0u)
        {
            return fail("IOP combined idle before HLE and SPU2 events");
        }
        (void)baseline->run(87u, 128u);
        (void)combined->run(87u, 128u);
        if (!equal() || baseline_callbacks != 0u ||
            combined->idle_event_cycles_remaining() != 0u)
        {
            return fail("IOP combined idle reached exact HLE boundary");
        }
        (void)baseline->run(168u, 128u);
        (void)combined->run(168u, 128u);
        if (!equal() || baseline_callbacks != 1u ||
            combined->hardware().spu2().elapsed_samples() != 1u)
        {
            return fail("IOP combined idle subsequent event delivery");
        }
    }

    // A real IOP thread stack is allocated from upper RAM downward,
    // in 256-byte multiples. Guest modules and IOPHEAP use lower RAM.
    // Releasing a dormant thread permits high-gap reuse without
    // overwriting the low heap or any still-owned stack.
    iop::c_kernel stack_kernel;
    const uint32_t first_stack =
        stack_kernel.create_thread(0x1800u, 0u, 8u, 0x410u);
    const uint32_t second_stack =
        stack_kernel.create_thread(0x1810u, 0u, 8u, 0x400u);
    const iop::s_kernel_thread* first_thread =
        stack_kernel.thread(first_stack);
    const iop::s_kernel_thread* second_thread =
        stack_kernel.thread(second_stack);
    if (first_stack == 0u || second_stack == 0u ||
        first_thread == nullptr || second_thread == nullptr ||
        first_thread->m_stack_size != 0x500u ||
        first_thread->m_stack_base !=
            iop::c_cpu::k_ram_size - 0x500u ||
        second_thread->m_stack_base !=
            iop::c_cpu::k_ram_size - 0x900u ||
        stack_kernel.allocate_heap(0x300u) !=
            iop::c_kernel::k_user_base ||
        !stack_kernel.delete_thread(first_stack))
    {
        return fail("IOP upper-RAM thread stack/low heap parity");
    }
    const uint32_t reused_stack =
        stack_kernel.create_thread(0x1820u, 0u, 8u, 0x400u);
    const iop::s_kernel_thread* reused_thread =
        stack_kernel.thread(reused_stack);
    if (reused_stack == 0u || reused_thread == nullptr ||
        reused_thread->m_stack_base !=
            iop::c_cpu::k_ram_size - 0x400u ||
        stack_kernel.thread(second_stack)->m_stack_base !=
            iop::c_cpu::k_ram_size - 0x900u ||
        stack_kernel.create_thread(0x1830u, 0u, 8u, 0xffffffffu) != 0u)
    {
        return fail("IOP high allocation gap reuse/overflow rejection");
    }

    iop::c_kernel kernel;
    auto module = make_module(true);

    const iop::s_module_load_result unresolved =
        kernel.load_module(module);
    uint32_t word = 0u;
    if (unresolved.m_status !=
            iop::e_irx_load_result::unresolved_import ||
        unresolved.m_id != 0u ||
        !kernel.cpu().read32(0x20000u, &word) ||
        word != 0u)
    {
        return fail("unresolved import must roll back guest RAM/allocation");
    }

    if (!kernel.register_library("LIBRARY", 7u, 0x00020000u) ||
        kernel.register_library("LIBRARY", 7u, 0x00020000u) ||
        kernel.register_library("", 5u, 0x00020000u))
    {
        return fail("import registry validation or duplicate detection");
    }

    const iop::s_module_load_result loaded =
        kernel.load_module(module);
    const iop::s_kernel_module* image =
        kernel.module(loaded.m_id);
    if (loaded.m_status != iop::e_irx_load_result::success ||
        loaded.m_id != 1u || image == nullptr ||
        image->m_irx.m_base != 0x20000u ||
        image->m_irx.m_size != 0x80u ||
        image->m_irx.m_entry != 0x20000u ||
        image->m_irx.m_gp != 0x20050u)
    {
        return fail("module allocator or IRX header registration");
    }
    if (!kernel.cpu().read32(0x20034u, &word) ||
        word != 0x08008000u)
    {
        return fail("registered IOP library import not linked");
    }

    const uint32_t boot_thread =
        kernel.create_module_thread(loaded.m_id, 1u, 0x400u);
    const iop::s_kernel_thread* initial =
        kernel.thread(boot_thread);
    if (boot_thread != 1u || initial == nullptr ||
        initial->m_cpu.m_pc != 0x20000u ||
        initial->m_cpu.m_gpr[28] != 0x20050u ||
        initial->m_cpu.m_gpr[29] == 0u ||
        initial->m_status != iop::e_thread_status::dormant ||
        kernel.threads().size() != 1u ||
        kernel.threads().at(boot_thread).m_cpu.m_pc != 0x20000u ||
        !kernel.start_thread(boot_thread) ||
        kernel.start_thread(boot_thread))
    {
        return fail("module entry, GP, stack or dormant->ready transition");
    }

    bool executed = false;
    kernel.bind_syscall(
        [&](iop::c_kernel& active, uint32_t service) -> bool
        {
            if (service == 7u)
            {
                executed =
                    active.current_thread_id() == boot_thread &&
                    active.cpu().state().m_pc == 0x20004u;
                active.cpu().state().m_gpr[2] = 12u;
                return active.stop_thread(boot_thread);
            }
            return false;
        }
    );

    const iop::s_kernel_run_result completed =
        kernel.run(8u, 2u);
    const iop::s_kernel_thread* after =
        kernel.thread(boot_thread);
    if (!executed ||
        completed.m_stop != iop::e_kernel_stop::idle ||
        completed.m_executed != 2u ||
        completed.m_thread_id != boot_thread ||
        after == nullptr ||
        after->m_status != iop::e_thread_status::dormant ||
        after->m_cpu.m_pc != 0x20008u ||
        after->m_cpu.m_gpr[2] != 12u ||
        kernel.cycles() != 8u)
    {
        return fail("module thread syscall callback or explicit stop");
    }

    // Module _start must return through a real MIPS JR $ra + delay
    // slot to a reserved BREAK trampoline, become dormant, and preserve
    // its guest return value. A thread that falls through RAM at PC=0
    // must never be mistaken for a completed module initialization.
    constexpr uint32_t k_module_result = 0x1234u;
    const uint32_t module_return[] =
    {
        0x03e00008u, // jr ra
        0x24021234u  // delay slot: addiu v0,zero,0x1234
    };
    if (!put_program(kernel.cpu(), image->m_irx.m_entry,
                     module_return, 2u) ||
        !kernel.start_thread_with_arguments(
            boot_thread, 2u, 0x00020020u))
    {
        return fail("cannot restart original module entrypoint");
    }
    const iop::s_kernel_thread* restarted =
        kernel.thread(boot_thread);
    if (restarted == nullptr ||
        !restarted->m_module_entry ||
        restarted->m_module_id != loaded.m_id ||
        restarted->m_module_entry_returned ||
        restarted->m_cpu.m_gpr[31] == 0u ||
        restarted->m_cpu.m_gpr[4] != 2u ||
        restarted->m_cpu.m_gpr[5] != 0x00020020u)
    {
        return fail("module start lost return trampoline or argc/argv");
    }
    const uint32_t module_return_pc =
        restarted->m_cpu.m_gpr[31];
    const iop::s_kernel_run_result returned =
        kernel.run(8u, 4u);
    const iop::s_kernel_thread* retired =
        kernel.thread(boot_thread);
    if (returned.m_stop != iop::e_kernel_stop::idle ||
        returned.m_executed != 2u ||
        returned.m_thread_id != boot_thread ||
        retired == nullptr ||
        retired->m_status != iop::e_thread_status::dormant ||
        retired->m_cpu.m_pc != module_return_pc ||
        retired->m_cpu.m_gpr[2] != k_module_result ||
        !retired->m_module_entry_returned ||
        retired->m_module_entry_result != k_module_result ||
        retired->m_cpu.m_gpr[31] != module_return_pc)
    {
        return fail("module _start returned without genuine trampoline retirement");
    }
    // No stale module thread may consume further scheduler quanta.
    if (kernel.run(16u, 4u).m_executed != 0u)
    {
        return fail("dormant module initializer continued executing IOP RAM");
    }

    // The reference IOP kernel installs a distinct return trampoline
    // for every started guest thread, not only module initializers.
    // A guest worker that returns through $ra becomes dormant instead
    // of wandering through low mirrored RAM at PC zero.
    const uint32_t worker_return[] =
    {
        0x03e00008u, // jr ra
        0x24020056u  // delay slot: addiu v0,zero,0x56
    };
    if (!put_program(kernel.cpu(), 0x1800u, worker_return, 2u))
    {
        return fail("cannot install returning IOP guest worker");
    }
    const uint32_t worker =
        kernel.create_thread(0x1800u, 0u, 4u, 0x400u);
    if (worker == 0u || !kernel.start_thread(worker))
    {
        return fail("cannot start returning IOP guest worker");
    }
    const uint32_t worker_ra =
        kernel.thread(worker)->m_cpu.m_gpr[31];
    if (worker_ra == 0u || worker_ra == module_return_pc)
    {
        return fail("ordinary IOP guest worker inherited invalid RA");
    }
    const iop::s_kernel_run_result worker_returned =
        kernel.run(8u, 4u);
    const iop::s_kernel_thread* dormant_worker =
        kernel.thread(worker);
    if (worker_returned.m_stop != iop::e_kernel_stop::idle ||
        worker_returned.m_executed != 2u ||
        worker_returned.m_thread_id != worker ||
        dormant_worker == nullptr ||
        dormant_worker->m_status != iop::e_thread_status::dormant ||
        dormant_worker->m_cpu.m_pc != worker_ra ||
        dormant_worker->m_cpu.m_gpr[2] != 0x56u)
    {
        return fail("ordinary worker did not retire on MIPS $ra return");
    }

    // Two equal-priority R3000 threads must each execute exactly one
    // three-instruction loop per 3-cycle scheduler quantum.
    const uint32_t loop[] =
    {
        0x25080001u, // addiu t0,t0,1
        0x1000fffeu, // beq zero,zero,-2 (back to addiu)
        0x00000000u  // delay slot
    };
    if (!put_program(kernel.cpu(), 0x1000u, loop, 3u) ||
        !put_program(kernel.cpu(), 0x1100u, loop, 3u))
    {
        return fail("cannot install R3000 thread fixture");
    }
    const uint32_t first =
        kernel.create_thread(0x1000u, 0u, 4u, 0x400u);
    const uint32_t second =
        kernel.create_thread(0x1100u, 0u, 4u, 0x400u);
    if (!first || !second ||
        !kernel.start_thread(first) ||
        !kernel.start_thread(second))
    {
        return fail("cannot create independently scheduled threads");
    }

    iop::s_kernel_run_result result = kernel.run(6u, 3u);
    if (result.m_stop !=
            iop::e_kernel_stop::budget_exhausted ||
        result.m_executed != 6u ||
        result.m_thread_id != second ||
        kernel.thread(first)->m_cpu.m_gpr[8] != 1u ||
        kernel.thread(second)->m_cpu.m_gpr[8] != 1u)
    {
        return fail("round-robin scheduling of equal-priority threads");
    }

    const uint32_t high_program[] =
    {
        0x2402000bu, // addiu v0,zero,11
        0x0000000cu  // syscall
    };
    if (!put_program(kernel.cpu(), 0x1200u, high_program, 2u))
    {
        return fail("cannot install high-priority thread");
    }
    const uint32_t high =
        kernel.create_thread(0x1200u, 0u, 0u, 0x400u);
    if (!high || !kernel.start_thread(high))
    {
        return fail("cannot start high-priority thread");
    }

    kernel.bind_syscall(
        [&](iop::c_kernel& active, uint32_t service) -> bool
        {
            if (service == 11u)
            {
                return active.stop_thread(high);
            }
            return false;
        }
    );
    result = kernel.run(2u, 2u);
    if (result.m_executed != 2u ||
        result.m_thread_id != high ||
        kernel.thread(high)->m_status !=
            iop::e_thread_status::dormant ||
        kernel.thread(first)->m_cpu.m_gpr[8] != 1u ||
        kernel.thread(second)->m_cpu.m_gpr[8] != 1u)
    {
        return fail("higher-priority ready thread did not preempt");
    }

    result = kernel.run(6u, 3u);
    if (result.m_executed != 6u ||
        kernel.thread(first)->m_cpu.m_gpr[8] != 2u ||
        kernel.thread(second)->m_cpu.m_gpr[8] != 2u)
    {
        return fail("suspended high-priority thread consumed CPU");
    }

    // An unhandled syscall must stop at the offending guest word
    // and must not return an invented value to the guest thread.
    const uint32_t unknown_program[] =
    {
        0x24020063u, // addiu v0,zero,99
        0x0000000cu  // syscall
    };
    if (!put_program(kernel.cpu(), 0x1300u, unknown_program, 2u))
    {
        return fail("cannot install unknown-service fixture");
    }
    const uint32_t unknown =
        kernel.create_thread(0x1300u, 0u, 0u, 0x400u);
    if (!unknown || !kernel.start_thread(unknown))
    {
        return fail("cannot start unknown-service thread");
    }
    result = kernel.run(8u, 4u);
    if (result.m_stop !=
            iop::e_kernel_stop::unhandled_syscall ||
        result.m_thread_id != unknown ||
        result.m_guest_pc != 0x1304u ||
        result.m_opcode != 0x0000000cu ||
        kernel.thread(unknown)->m_status !=
            iop::e_thread_status::faulted ||
        kernel.thread(unknown)->m_cpu.m_pc != 0x1304u)
    {
        return fail("unknown IOP syscall did not stop at guest EPC");
    }

    // Reset must invalidate old module IDs, thread state, registered
    // libraries and RAM. No reset can synthesize a SIF ready flag.
    kernel.reset();
    if (kernel.module(loaded.m_id) != nullptr ||
        kernel.thread(boot_thread) != nullptr ||
        !kernel.threads().empty() ||
        kernel.cycles() != 0u ||
        !kernel.cpu().read32(0x20000u, &word) || word != 0u)
    {
        return fail("kernel reset did not clear isolated IOP state");
    }

    // Two timed sleeps installed in descending deadline order, with
    // an intervening HLE event. The wake hint must never skip an
    // earlier inserted deadline or move the HLE event.
    const uint32_t sleeper_program[] =
    {
        0x2402000bu, // addiu v0,zero,11
        0x0000000cu, // syscall
        0x25080001u, // addiu t0,t0,1
        0x1000fffeu, // beq zero,zero,-2
        0x00000000u
    };
    if (!put_program(kernel.cpu(), 0x1000u, sleeper_program, 5u) ||
        !put_program(kernel.cpu(), 0x1100u, sleeper_program, 5u))
    {
        return fail("cannot install timed sleep fixture");
    }
    const uint32_t late = kernel.create_thread(0x1000u, 0u, 4u, 0x400u);
    const uint32_t early = kernel.create_thread(0x1100u, 0u, 4u, 0x400u);
    if (!late || !early || !kernel.start_thread(late) ||
        !kernel.start_thread(early))
    {
        return fail("cannot start timed sleep fixture");
    }
    kernel.bind_syscall(
        [&](iop::c_kernel& active, uint32_t service) -> bool
        {
            return service == 11u && active.sleep_current(
                active.current_thread_id() == late ? 20u : 8u);
        });
    result = kernel.run(4u, 2u);
    if (result.m_executed != 4u ||
        kernel.thread(late)->m_status != iop::e_thread_status::blocked ||
        kernel.thread(early)->m_status != iop::e_thread_status::blocked)
    {
        return fail("timed sleep fixture did not block");
    }
    const uint64_t late_wake = kernel.thread(late)->m_wake_cycle;
    const uint64_t early_wake = kernel.thread(early)->m_wake_cycle;
    if (kernel.idle_event_cycles_remaining() != early_wake - kernel.cycles())
    {
        return fail("IOP lookahead did not select earliest timed thread");
    }
    const uint64_t event_cycle = kernel.cycles() + 2u;
    uint64_t observed_event_cycle = 0u;
    if (event_cycle >= early_wake || early_wake >= late_wake ||
        !kernel.schedule_hle_event(2u, [&]()
        {
            observed_event_cycle = kernel.cycles();
        }))
    {
        return fail("cannot schedule intervening HLE event");
    }
    result = kernel.run(early_wake - kernel.cycles(), 1u);
    if (result.m_executed != 0u || kernel.cycles() != early_wake ||
        observed_event_cycle != event_cycle ||
        kernel.thread(early)->m_status != iop::e_thread_status::blocked)
    {
        return fail("idle advance did not preserve event/deadline boundaries");
    }
    result = kernel.run(1u, 1u);
    if (result.m_executed != 1u || result.m_thread_id != early ||
        kernel.thread(early)->m_cpu.m_gpr[8] != 1u ||
        kernel.thread(late)->m_status != iop::e_thread_status::blocked)
    {
        return fail("earliest timed thread did not resume on deadline");
    }
    // Cancellation leaves a conservative hint until the next scan,
    // but must not create a phantom idle deadline.
    if (!kernel.stop_thread(early) || !kernel.wake_thread(late) ||
        !kernel.stop_thread(late))
    {
        return fail("cannot cancel timed sleep fixture");
    }
    result = kernel.run(late_wake - kernel.cycles() + 1u, 1u);
    if (result.m_stop != iop::e_kernel_stop::idle ||
        result.m_executed != 0u || kernel.cycles() != late_wake + 1u)
    {
        return fail("cancelled sleep changed idle timing");
    }
    kernel.reset();
    if (kernel.cycles() != 0u || !kernel.threads().empty())
    {
        return fail("timed sleep reset did not clear guest state");
    }

    std::printf("IOP kernel: PASS\n");
    return 0;
}
