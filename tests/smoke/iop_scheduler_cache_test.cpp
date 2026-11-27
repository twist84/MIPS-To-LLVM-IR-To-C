#include "iop_kernel.h"

#include <cstdio>

namespace
{
bool check(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "IOP scheduler cache: %s\n", message);
    }
    return condition;
}
}

int main()
{
    iop::c_kernel kernel{};
    // An endless guest branch/NOP loop; each one-instruction slice
    // returns READY without changing priority or the runnable set.
    if (!kernel.cpu().write32(0x4000u, 0x1000ffffu) ||
        !kernel.cpu().write32(0x4004u, 0u) ||
        !kernel.cpu().write32(0x4100u, 0x0000000cu) ||
        !kernel.cpu().write32(0x4104u, 0x1000ffffu) ||
        !kernel.cpu().write32(0x4108u, 0u))
    {
        return 1;
    }
    const uint32_t first = kernel.create_thread(0x4000u, 0u, 20u, 256u);
    const uint32_t second = kernel.create_thread(0x4000u, 0u, 20u, 256u);
    const uint32_t low = kernel.create_thread(0x4000u, 0u, 40u, 256u);
    const uint32_t sleeper = kernel.create_thread(0x4100u, 0u, 10u, 256u);
    const auto run = [&](uint32_t expected)
    {
        const auto result = kernel.run(1u, 1u);
        return result.m_thread_id == expected &&
            result.m_stop == iop::e_kernel_stop::budget_exhausted;
    };
    if (!check(first && second && low && sleeper &&
            kernel.start_thread(first) && kernel.start_thread(second) &&
            kernel.start_thread(low), "thread fixture setup"))
    {
        return 1;
    }
    for (uint32_t i = 0u; i < 100u; ++i)
    {
        if (!check(run(first) && run(second), "equal priority round robin"))
        {
            return 1;
        }
    }
    if (!check(kernel.change_thread_priority(low, 5u) && run(low) &&
            run(low), "priority promotion") ||
        !check(kernel.stop_thread(low) && run(first) &&
            kernel.delete_thread(low) && run(second), "stop/delete invalidation"))
    {
        return 1;
    }

    kernel.bind_syscall([](iop::c_kernel& k, uint32_t)
    {
        return k.sleep_current(8u);
    });
    if (!check(kernel.start_thread(sleeper) && run(sleeper) &&
            kernel.thread(sleeper)->m_status == iop::e_thread_status::blocked,
            "sleep removes highest priority thread"))
    {
        return 1;
    }
    const uint64_t wake = kernel.thread(sleeper)->m_wake_cycle;
    while (kernel.cycles() < wake)
    {
        const auto result = kernel.run(1u, 1u);
        if (!check(result.m_thread_id == first || result.m_thread_id == second,
                "sleeper scheduled before deadline"))
        {
            return 1;
        }
    }
    if (!check(run(sleeper), "timed wake invalidation") ||
        !check(kernel.stop_thread(sleeper) && kernel.stop_thread(first) &&
            kernel.stop_thread(second) &&
            kernel.run(10u, 1u).m_stop == iop::e_kernel_stop::idle,
            "empty ready set"))
    {
        return 1;
    }
    if (!check(kernel.start_thread(first) && run(first), "restart invalidation"))
    {
        return 1;
    }
    kernel.reset();
    if (!check(kernel.run(10u, 1u).m_stop == iop::e_kernel_stop::idle,
            "reset retained cached IDs"))
    {
        return 1;
    }
    // Sampled and ordinary runs must preserve the stop reason and clock.
    iop::c_kernel control{};
    kernel.reset();
    iop::s_kernel_phase_sample idle_sample{};
    const auto idle_control = control.run(10u, 1u);
    const auto idle_profiled = kernel.run(10u, 1u, &idle_sample);
    const auto calls = [](const iop::s_kernel_phase_sample& sample,
                          iop::e_kernel_phase phase)
    {
        return sample.m_calls[static_cast<uint32_t>(phase)];
    };
    if (!check(idle_control.m_stop == idle_profiled.m_stop &&
            idle_control.m_executed == idle_profiled.m_executed &&
            control.cycles() == kernel.cycles() &&
            calls(idle_sample, iop::e_kernel_phase::wake_irq) == 1u &&
            calls(idle_sample, iop::e_kernel_phase::ready) == 1u &&
            calls(idle_sample, iop::e_kernel_phase::idle_deadline) == 1u &&
            calls(idle_sample, iop::e_kernel_phase::idle_advance) == 1u &&
            calls(idle_sample, iop::e_kernel_phase::guest_cpu) == 0u,
            "sampled idle service equivalence"))
    {
        return 1;
    }
    for (auto* item : {&control, &kernel})
    {
        if (!item->cpu().write32(0x4000u, 0x1000ffffu) ||
            !item->cpu().write32(0x4004u, 0u))
        {
            return 1;
        }
        const auto id = item->create_thread(0x4000u, 0u, 20u, 256u);
        if (!id || !item->start_thread(id))
        {
            return 1;
        }
    }
    iop::s_kernel_phase_sample guest_sample{};
    const auto guest_control = control.run(8u, 1u);
    const auto guest_profiled = kernel.run(8u, 1u, &guest_sample);
    if (!check(guest_control.m_stop == guest_profiled.m_stop &&
            guest_control.m_executed == guest_profiled.m_executed &&
            guest_control.m_thread_id == guest_profiled.m_thread_id &&
            control.cycles() == kernel.cycles() &&
            calls(guest_sample, iop::e_kernel_phase::guest_context) != 0u &&
            calls(guest_sample, iop::e_kernel_phase::guest_cpu) ==
                calls(guest_sample, iop::e_kernel_phase::guest_context) &&
            calls(guest_sample, iop::e_kernel_phase::idle_advance) == 0u,
            "sampled guest service equivalence"))
    {
        return 1;
    }
    std::puts("IOP scheduler cache: PASS");
    return 0;
}
