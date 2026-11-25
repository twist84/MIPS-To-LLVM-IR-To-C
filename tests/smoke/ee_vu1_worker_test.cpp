#include "ee_vu1_worker.h"

#include <cstdio>
#include <cstring>
#include <memory>

namespace
{
void put32(uint8_t* bytes, uint32_t value)
{
    for (uint32_t lane = 0u; lane < 4u; ++lane)
    {
        bytes[lane] = static_cast<uint8_t>(value >> (lane * 8u));
    }
}

bool check(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "VU1 worker: %s\n", message);
    }
    return condition;
}

std::unique_ptr<ee::s_ee_vu1_job> fixture()
{
    auto job = std::make_unique<ee::s_ee_vu1_job>();
    auto& code = job->m_resident->m_code;
    auto& data = job->m_resident->m_data;
    const uint32_t words[] = {
        (0x08u << 25u) | (1u << 16u) | 2u, 0x000002ffu,
        0x80000efcu, 0x400002ffu,
        0x8000033cu, 0x000002ffu
    };
    for (size_t index = 0u; index < sizeof(words) / sizeof(words[0]); ++index)
    {
        put32(code.data() + index * 4u, words[index]);
    }
    // Real packed GIFtag + payload at VI1=2, with a staged prefix.
    put32(data.data() + 32u, 0x8001u);
    put32(data.data() + 36u, 0x10000000u);
    put32(data.data() + 40u, 0xeu);
    put32(data.data() + 48u, 6u);
    job->m_path1.resize(16u, 0x5au);
    return job;
}
} // namespace

int main()
{
    // Snapshots must not alias; explicit handoff must swap the exact
    // allocation, while copy assignment preserves the committed address.
    ee::c_ee_vu1_resident_slot committed;
    committed->m_data[0] = 42u;
    ee::c_ee_vu1_resident_slot staged = committed;
    ee::c_ee_vu1_resident_slot spare;
    auto* const committed_address = &*committed;
    auto* const staged_address = &*staged;
    auto* const spare_address = &*spare;
    staged.swap(spare);
    spare->m_data[0] = 99u;
    if (!check(&*staged == spare_address && &*spare == staged_address &&
            committed->m_data[0] == 42u, "resident ownership swap aliases committed state"))
    {
        return 1;
    }
    staged.swap(spare);
    committed = staged;
    if (!check(&*committed == committed_address && committed->m_data[0] == 99u &&
            &*staged != &*committed, "resident snapshot replaced committed storage"))
    {
        return 1;
    }

    ee::c_ee_vu1_worker worker;
    auto job = fixture();
    auto reference = *job;
    uint64_t submitted_ns = 0u;
    auto& resident = *reference.m_resident;
    if (!check(ee::c_ee_vu1::run(
            resident.m_state, resident.m_code, resident.m_data, 0u, 0u, 0u,
            reference.m_path1, &reference.m_issue),
            "synchronous fixture failed") ||
        !check(!worker.busy() && !worker.ready() && !worker.try_take() && !worker.wait_take(),
            "empty worker returned stale result") ||
        !check(worker.submit(job, &submitted_ns) && submitted_ns != 0u && !job && worker.busy(),
            "worker did not take exclusive ownership"))
    {
        return 1;
    }

    // Even if the worker has already finished, the outstanding slot
    // remains occupied until its result is collected. No sleep or
    // timing-dependent race is needed to test backpressure.
    auto next = fixture();
    if (!check(!worker.submit(next) && next != nullptr,
            "full slot accepted another job"))
    {
        return 1;
    }
    auto result = worker.try_take();
    if (!result)
    {
        result = worker.wait_take();
    }
    if (!check(result && result->m_accepted && !result->m_host_failed &&
            result->m_submitted_ns == submitted_ns &&
            result->m_started_ns >= submitted_ns &&
            result->m_finished_ns >= result->m_started_ns &&
            worker.finished_ns() == result->m_finished_ns &&
            worker.started_ns() == result->m_started_ns &&
            worker.execution_interval() ==
                std::make_pair(result->m_started_ns, result->m_finished_ns) &&
            result->m_resident->m_state.m_vi == resident.m_state.m_vi &&
            result->m_resident->m_state.m_vf == resident.m_state.m_vf &&
            result->m_resident->m_state.m_tpc == resident.m_state.m_tpc &&
            result->m_resident->m_data == resident.m_data &&
            result->m_resident->m_code == resident.m_code &&
            result->m_issue.m_steps == reference.m_issue.m_steps &&
            result->m_issue.m_cycles == reference.m_issue.m_cycles &&
            result->m_path1 == reference.m_path1 &&
            result->m_path1.size() == 48u && !worker.busy() && !worker.ready(),
            "worker result differs from synchronous XGKICK"))
    {
        return 1;
    }

    // Reject an invalid program, then reuse the same persistent thread.
    next->m_address = 1u;
    if (!check(worker.submit(next), "second job rejected by queue"))
    {
        return 1;
    }
    result = worker.wait_take();
    if (!check(result && !result->m_accepted && !result->m_host_failed,
            "invalid execution was fabricated as success"))
    {
        return 1;
    }
    next = fixture();
    if (!check(worker.submit(next), "third job rejected by queue"))
    {
        return 1;
    }
    worker.discard();
    if (!check(!worker.busy() && !worker.ready() && !worker.try_take(),
            "discard exposed an old generation"))
    {
        return 1;
    }
    next = fixture();
    next->m_path1.push_back(0u);
    if (!check(!worker.submit(next) && next != nullptr,
            "unaligned PATH1 prefix accepted"))
    {
        return 1;
    }
    next = fixture();
    if (!check(worker.submit(next), "worker failed to reuse slot after discard"))
    {
        return 1;
    }
    // Destructor must join safely with an outstanding owned request.
    std::puts("EE VU1 persistent worker: PASS");
    return 0;
}
