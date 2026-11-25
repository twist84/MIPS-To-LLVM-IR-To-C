#include "ee_kernel_thread_status.h"

#include <cstdio>
#include <cstdint>
#include <initializer_list>

namespace
{
bool check(
    const char* label,
    ee_kernel_thread::s_status_transition actual,
    uint32_t expected_status,
    bool expected_changed)
{
    bool passed =
        actual.m_status == expected_status &&
        actual.m_changed == expected_changed;
    if (!passed)
    {
        std::fprintf(stderr,
            "%s: status=%u changed=%u expected=%u changed=%u\n",
            label, actual.m_status, actual.m_changed ? 1u : 0u,
            expected_status, expected_changed ? 1u : 0u);
    }
    return passed;
}
}

int main()
{
    using namespace ee_kernel_thread;
    bool passed = true;

    // Exact retail SDBZ blocker: StartThread produced READY status 2,
    // then the main thread called SuspendThread(3).
    passed = check("suspend ready", suspend(k_ready), k_suspend, true) && passed;
    passed = check("suspend run", suspend(k_run), k_suspend, true) && passed;
    passed = check("suspend wait", suspend(k_wait), k_wait_suspend, true) && passed;
    passed = check("suspend dormant", suspend(k_dormant), k_dormant, false) && passed;
    passed = check("suspend suspended", suspend(k_suspend), k_suspend, false) && passed;
    passed = check("suspend wait-suspended",
                   suspend(k_wait_suspend), k_wait_suspend, false) && passed;

    passed = check("resume ready", resume(k_suspend, false), k_ready, true) && passed;
    passed = check("resume current running",
                   resume(k_suspend, true), k_run, true) && passed;
    passed = check("resume wait-suspended",
                   resume(k_wait_suspend, false), k_wait, true) && passed;
    passed = check("resume dormant", resume(k_dormant, false),
                   k_dormant, false) && passed;
    passed = check("resume non-suspended ready", resume(k_ready, false),
                   k_ready, false) && passed;
    passed = check("resume non-suspended wait", resume(k_wait, false),
                   k_wait, false) && passed;

    passed = check("release wait", release_wait(k_wait),
                   k_ready, true) && passed;
    passed = check("release suspended wait", release_wait(k_wait_suspend),
                   k_suspend, true) && passed;
    passed = check("release ready", release_wait(k_ready),
                   k_ready, false) && passed;
    passed = check("release running", release_wait(k_run),
                   k_run, false) && passed;
    passed = check("release dormant", release_wait(k_dormant),
                   k_dormant, false) && passed;
    passed = check("release suspended", release_wait(k_suspend),
                   k_suspend, false) && passed;

    for (uint32_t status : {k_run, k_ready, k_wait, k_suspend, k_wait_suspend, k_dormant, 0u})
    {
        const bool deletable = status == k_dormant;
        const bool terminable = status == k_ready || status == k_wait ||
            status == k_suspend || status == k_wait_suspend;
        if (can_delete(status, false) != deletable ||
            can_terminate(status, false) != terminable ||
            can_delete(status, true) || can_terminate(status, true))
        {
            std::fprintf(stderr, "invalid lifecycle transition for status %u\n", status);
            passed = false;
        }
    }

    if (passed)
    {
        std::puts("ee_kernel_thread_status: PASS");
    }
    return passed ? 0 : 1;
}
