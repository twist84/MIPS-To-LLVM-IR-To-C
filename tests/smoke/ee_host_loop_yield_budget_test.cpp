#include "ee_host_loop_yield_budget.h"

#include <cstdint>
#include <cstdio>

namespace
{
constexpr bool fail(const char* message)
{
    if !consteval
    {
        std::fprintf(stderr, "EE host loop yield budget: %s\n", message);
    }
    return false;
}
 
consteval bool test_watchdog_budget()
{
    // The old implementation exhausted sixteen *lifetime* resumes,
    // even when VBlank, GIF DMA and IOP RPC made real progress.
    ee::c_host_loop_yield_budget budget{};
    for (uint32_t i = 0u; i < 16u; ++i)
    {
        if (!budget.try_resume(16u, 0u, 0u, 0u))
        {
            return fail("native loop resumed fewer than 16 times");
        }
    }
    if (budget.try_resume(16u, 0u, 0u, 0u) ||
        budget.without_progress() != 16u ||
        budget.total_resumes() != 16u)
    {
        return fail("unchanging native loop exceeded budget");
    }

    // A new GS field has *actually* been observed, so its subsequent
    // guest poll may take a fresh bounded set of native safepoints.
    if (!budget.try_resume(16u, 1u, 0u, 0u) ||
        budget.without_progress() != 1u ||
        budget.total_resumes() != 17u ||
        budget.progress_resets() != 1u)
    {
        return fail("real GS field did not reopen watchdog window");
    }
    for (uint32_t i = 1u; i < 16u; ++i)
    {
        if (!budget.try_resume(16u, 1u, 0u, 0u))
        {
            return fail("GS field resumed too few native watchdogs");
        }
    }
    if (budget.try_resume(16u, 1u, 0u, 0u))
    {
        return fail("GS field fabricated repeated progress");
    }

    // Only authentic IOP RPC commits or GIF register writes may also
    // reopen the bounded resume window. Arbitrary repeated polling or
    // an ever-growing EE instruction count must never reset it.
    if (!budget.try_resume(16u, 1u, 1u, 0u) ||
        budget.without_progress() != 1u ||
        budget.progress_resets() != 2u)
    {
        return fail("IOP RPC completion did not reopen watchdog window");
    }
    if (!budget.try_resume(16u, 1u, 1u, 1u) ||
        budget.without_progress() != 1u ||
        budget.progress_resets() != 3u)
    {
        return fail("genuine GIF write did not reopen watchdog window");
    }
    if (budget.try_resume(0u, 2u, 2u, 2u) ||
        budget.total_resumes() != 34u)
    {
        return fail("explicit zero limit did not preserve fail-closed stop");
    }

    // A fresh run cannot inherit a previous session's resume budget.
    ee::c_host_loop_yield_budget fresh{};
    if (!fresh.try_resume(1u, 0u, 0u, 0u) ||
        fresh.try_resume(1u, 0u, 0u, 0u))
    {
        return fail("fresh watchdog state was not bounded");
    }

    return true;
}

static_assert(test_watchdog_budget(), "EE host loop yield budget regression");
} // namespace

int main()
{
    std::puts("EE host loop yield budget: compile-time PASS");
    return 0;
}
