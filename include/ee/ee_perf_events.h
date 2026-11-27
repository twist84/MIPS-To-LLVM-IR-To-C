#pragma once

#include <cstdint>

namespace ee
{

// EE Core User's Manual v6.0, Table 7-1 and section 7.2.3.
// Event identities are independent of the numeric PCR selector.
enum class e_ee_perf_event : uint8_t
{
    PROCESSOR_CYCLE,
    SINGLE_ISSUE,
    DUAL_ISSUE,
    LOW_ORDER_BRANCH_ISSUED,
    BRANCH_ISSUED,
    BRANCH_MISPREDICTED,
    BTAC_MISS,
    TLB_MISS,
    ITLB_MISS,
    DTLB_MISS,
    ICACHE_MISS,
    DCACHE_MISS,
    DTLB_ACCESS,
    WBB_SINGLE_UNAVAILABLE,
    NONBLOCKING_LOAD,
    WBB_BURST_UNAVAILABLE,
    WBB_SINGLE_REQUEST,
    WBB_BURST_ALMOST_FULL,
    WBB_BURST_REQUEST,
    WBB_BURST_FULL,
    ADDRESS_BUS_BUSY,
    DATA_BUS_BUSY,
    INSTRUCTION_COMPLETED,
    NON_BDS_COMPLETED,
    COP2_COMPLETED,
    COP1_COMPLETED,
    LOAD_COMPLETED,
    STORE_COMPLETED,
    COUNT,
    NO_EVENT,
    RESERVED
};

inline constexpr e_ee_perf_event ee_perf_event_for_selector(
    uint32_t counter, uint32_t selector)
{
    using e = e_ee_perf_event;
    constexpr e events[2][16] =
    {
        {e::RESERVED, e::PROCESSOR_CYCLE, e::SINGLE_ISSUE,
         e::BRANCH_ISSUED, e::BTAC_MISS, e::ITLB_MISS, e::ICACHE_MISS,
         e::DTLB_ACCESS, e::NONBLOCKING_LOAD, e::WBB_SINGLE_REQUEST,
         e::WBB_BURST_REQUEST, e::ADDRESS_BUS_BUSY, e::INSTRUCTION_COMPLETED,
         e::NON_BDS_COMPLETED, e::COP2_COMPLETED, e::LOAD_COMPLETED},
        {e::LOW_ORDER_BRANCH_ISSUED, e::PROCESSOR_CYCLE, e::DUAL_ISSUE,
         e::BRANCH_MISPREDICTED, e::TLB_MISS, e::DTLB_MISS, e::DCACHE_MISS,
         e::WBB_SINGLE_UNAVAILABLE, e::WBB_BURST_UNAVAILABLE,
         e::WBB_BURST_ALMOST_FULL, e::WBB_BURST_FULL, e::DATA_BUS_BUSY,
         e::INSTRUCTION_COMPLETED, e::NON_BDS_COMPLETED, e::COP1_COMPLETED,
         e::STORE_COMPLETED}
    };
    if (counter >= 2u || selector > 16u)
    {
        return e::RESERVED;
    }
    if (selector == 16u)
    {
        return e::NO_EVENT;
    }
    return events[counter][selector];
}

inline constexpr uint32_t k_ee_perf_event_count =
    static_cast<uint32_t>(e_ee_perf_event::COUNT);
static_assert(k_ee_perf_event_count < 32u);
inline constexpr uint32_t k_ee_perf_event_mask =
    (uint32_t{1} << k_ee_perf_event_count) - 1u;
inline constexpr uint32_t k_ee_perf_cycle_event_mask =
    uint32_t{1} << static_cast<uint32_t>(e_ee_perf_event::PROCESSOR_CYCLE);

// A producer supplies event counts for one constant-PCCR/Status interval.
// An explicit available zero is distinct from an absent event measurement.
// This structure does not establish that a producer is hardware-accurate.
struct s_ee_perf_event_interval
{
    uint32_t m_available = 0u;
    uint64_t m_delta[k_ee_perf_event_count]{};

    constexpr bool set(e_ee_perf_event event, uint64_t delta)
    {
        const uint32_t index = static_cast<uint32_t>(event);
        if (index >= k_ee_perf_event_count)
        {
            return false;
        }
        m_available |= uint32_t{1} << index;
        m_delta[index] = delta;
        return true;
    }
};

enum class e_ee_perf_interval_result : uint8_t
{
    OK,
    UNAVAILABLE_SOURCE,
    MISSING_EVENT,
    RESERVED_EVENT,
    COUNTER_OVERFLOW
};

// Per-counter evidence describes a boundary in event-count space, not
// a timestamp. Different event identities cannot be ordered from an
// aggregate interval. A zero delta never creates a new crossing.
struct s_ee_perf_counter_plan
{
    e_ee_perf_event m_event = e_ee_perf_event::NO_EVENT;
    e_ee_perf_interval_result m_result = e_ee_perf_interval_result::OK;
    uint32_t m_next = 0u;
    uint64_t m_events_to_overflow = 0u;
};

struct s_ee_perf_interval_plan
{
    e_ee_perf_interval_result m_result = e_ee_perf_interval_result::OK;
    s_ee_perf_counter_plan m_counters[2]{};
};

// Preserve the existing first-failure result, but inspect both counters
// independently so a producer can identify missing evidence and every
// potential overflow. This function never mutates architectural state.
inline constexpr s_ee_perf_interval_plan ee_perf_plan_interval(
    uint32_t control, uint32_t status,
    const s_ee_perf_event_interval& interval, const uint32_t (&counters)[2])
{
    using e = e_ee_perf_event;
    using result = e_ee_perf_interval_result;
    s_ee_perf_interval_plan plan{};
    plan.m_counters[0].m_next = counters[0];
    plan.m_counters[1].m_next = counters[1];
    if ((control & 0x80000000u) != 0u && (status & 4u) == 0u)
    {
        const uint32_t ksu = (status >> 3u) & 3u;
        const uint32_t mode = (status & 2u) != 0u ? 2u :
            (ksu < 3u ? (1u << (ksu + 2u)) : 0u);
        for (uint32_t index = 0u; index < 2u; ++index)
        {
            const uint32_t shift = index * 10u;
            if (((control >> shift) & mode) == 0u)
            {
                continue;
            }
            auto& counter = plan.m_counters[index];
            counter.m_event = ee_perf_event_for_selector(
                index, (control >> (5u + shift)) & 31u);
            if (counter.m_event == e::RESERVED)
            {
                counter.m_result = result::RESERVED_EVENT;
            }
            else if (counter.m_event != e::NO_EVENT)
            {
                const uint32_t event_index = static_cast<uint32_t>(counter.m_event);
                if ((interval.m_available & (uint32_t{1} << event_index)) == 0u)
                {
                    counter.m_result = result::MISSING_EVENT;
                }
                else
                {
                    const uint64_t delta = interval.m_delta[event_index];
                    if (delta != 0u)
                    {
                        // Preserve the existing fail-closed policy when the
                        // incoming counter already has its high bit set.
                        const uint64_t boundary = counters[index] >= 0x80000000u
                            ? 1u : 0x80000000ull - counters[index];
                        if (delta >= boundary)
                        {
                            counter.m_result = result::COUNTER_OVERFLOW;
                            counter.m_events_to_overflow = boundary;
                        }
                        else
                        {
                            counter.m_next += static_cast<uint32_t>(delta);
                        }
                    }
                }
            }
            if (plan.m_result == result::OK && counter.m_result != result::OK)
            {
                plan.m_result = counter.m_result;
            }
        }
    }
    return plan;
}

// Ordering is evidence in event-count space only. Even a shared event
// ordinal cannot establish PC/BD state, exception priority or a timestamp.
enum class e_ee_perf_overflow_order : uint8_t
{
    NONE,
    PCR0_FIRST,
    PCR1_FIRST,
    SAME_EVENT_ORDINAL,
    INDETERMINATE
};

inline constexpr e_ee_perf_overflow_order ee_perf_classify_overflow_order(
    const s_ee_perf_interval_plan& plan)
{
    using order = e_ee_perf_overflow_order;
    using result = e_ee_perf_interval_result;
    const auto& pcr0 = plan.m_counters[0];
    const auto& pcr1 = plan.m_counters[1];
    const bool overflow0 = pcr0.m_result == result::COUNTER_OVERFLOW;
    const bool overflow1 = pcr1.m_result == result::COUNTER_OVERFLOW;
    order value = order::INDETERMINATE;

    if ((pcr0.m_result == result::OK || overflow0) &&
        (pcr1.m_result == result::OK || overflow1))
    {
        if (!overflow0 && !overflow1)
        {
            value = order::NONE;
        }
        else if (overflow0 && !overflow1)
        {
            value = order::PCR0_FIRST;
        }
        else if (!overflow0 && overflow1)
        {
            value = order::PCR1_FIRST;
        }
        else if (pcr0.m_event == pcr1.m_event)
        {
            if (pcr0.m_events_to_overflow < pcr1.m_events_to_overflow)
            {
                value = order::PCR0_FIRST;
            }
            else if (pcr1.m_events_to_overflow < pcr0.m_events_to_overflow)
            {
                value = order::PCR1_FIRST;
            }
            else
            {
                value = order::SAME_EVENT_ORDINAL;
            }
        }
    }
    return value;
}

// Read-only projection for a producer-supplied event prefix ending at
// the first crossing. Counts alone cannot prove that this prefix ends
// at the execution boundary; the producer must establish that separately.
// Restrict this candidate to exact first crossings from clear OVFL bits.
// Do not truncate an aggregate interval or infer the other PCR's delta.
enum class e_ee_perf_boundary_result : uint8_t
{
    NO_CROSSING,
    EXISTING_OVERFLOW,
    INCOMPLETE_EVIDENCE,
    OVERSHOT_BOUNDARY,
    AMBIGUOUS_ORDER,
    EXACT_CROSSING
};

struct s_ee_perf_boundary_projection
{
    e_ee_perf_boundary_result m_result = e_ee_perf_boundary_result::NO_CROSSING;
    uint32_t m_crossing_mask = 0u;
    uint32_t m_next[2]{};
};

inline constexpr s_ee_perf_boundary_projection ee_perf_project_crossing_prefix(
    uint32_t control, uint32_t status,
    const s_ee_perf_event_interval& prefix, const uint32_t (&counters)[2])
{
    using boundary = e_ee_perf_boundary_result;
    using result = e_ee_perf_interval_result;
    using order = e_ee_perf_overflow_order;
    constexpr uint32_t high_bit = 0x80000000u;
    s_ee_perf_boundary_projection projection{};
    projection.m_next[0] = counters[0];
    projection.m_next[1] = counters[1];

    if (((counters[0] | counters[1]) & high_bit) != 0u)
    {
        projection.m_result = boundary::EXISTING_OVERFLOW;
    }
    else
    {
        const auto plan = ee_perf_plan_interval(control, status, prefix, counters);
        const auto ordering = ee_perf_classify_overflow_order(plan);
        uint32_t mask = 0u;
        bool incomplete = false;
        bool overshot = false;
        for (uint32_t index = 0u; index < 2u; ++index)
        {
            const auto& counter = plan.m_counters[index];
            if (counter.m_result == result::COUNTER_OVERFLOW)
            {
                mask |= 1u << index;
                const uint32_t event_index = static_cast<uint32_t>(counter.m_event);
                overshot |= prefix.m_delta[event_index] != counter.m_events_to_overflow;
            }
            else if (counter.m_result != result::OK)
            {
                incomplete = true;
            }
        }
        if (incomplete)
        {
            projection.m_result = boundary::INCOMPLETE_EVIDENCE;
        }
        else if (overshot)
        {
            projection.m_result = boundary::OVERSHOT_BOUNDARY;
        }
        else if (mask != 0u && ordering == order::INDETERMINATE)
        {
            projection.m_result = boundary::AMBIGUOUS_ORDER;
        }
        else if (mask != 0u)
        {
            projection.m_result = boundary::EXACT_CROSSING;
            projection.m_crossing_mask = mask;
            for (uint32_t index = 0u; index < 2u; ++index)
            {
                projection.m_next[index] = (mask & (1u << index)) != 0u
                    ? high_bit : plan.m_counters[index].m_next;
            }
        }
    }
    return projection;
}

// Offline recognition candidate, using the architectural PCR flag bits,
// not aggregate event deltas. Section 7.2.4's printed ERL predicate
// conflicts with the explicit inhibition/defer rules in 4.1.3/7.2.6.
// Use the latter interpretation here; hardware validation is outstanding.
// Inputs describe one already established observation boundary, not a
// mechanism for generating, latching or acknowledging Reset/NMI.
enum class e_ee_perf_recognition : uint8_t
{
    NO_PENDING_OVERFLOW,
    COUNTING_DISABLED,
    DEFERRED_ERL,
    SUPERSEDED_RESET,
    DEFERRED_NMI,
    ELIGIBLE
};

struct s_ee_perf_recognition_plan
{
    uint32_t m_overflow_mask = 0u;
    e_ee_perf_recognition m_result = e_ee_perf_recognition::NO_PENDING_OVERFLOW;
};

inline constexpr s_ee_perf_recognition_plan ee_perf_plan_recognition(
    uint32_t control, uint32_t status, const uint32_t (&counters)[2],
    bool reset_pending, bool nmi_pending)
{
    using result = e_ee_perf_recognition;
    constexpr uint32_t high_bit = 0x80000000u;
    constexpr uint32_t erl = 1u << 2u;
    s_ee_perf_recognition_plan plan{};
    plan.m_overflow_mask = ((counters[0] & high_bit) != 0u ? 1u : 0u) |
                           ((counters[1] & high_bit) != 0u ? 2u : 0u);
    if (plan.m_overflow_mask != 0u)
    {
        if (reset_pending)
        {
            plan.m_result = result::SUPERSEDED_RESET;
        }
        else if ((control & high_bit) == 0u)
        {
            plan.m_result = result::COUNTING_DISABLED;
        }
        else if ((status & erl) != 0u)
        {
            plan.m_result = result::DEFERRED_ERL;
        }
        else if (nmi_pending)
        {
            plan.m_result = result::DEFERRED_NMI;
        }
        else
        {
            plan.m_result = result::ELIGIBLE;
        }
    }
    return plan;
}

// EE Core User's Manual v6.0, sections 4.1.3 and 4.2.3.
// Project register changes for an already recognized counter exception.
// The caller must supply the cancelled instruction's PC and delay-slot
// state; neither an event ordinal nor a producer's current PC is sufficient.
// This does not recognize, prioritize, cancel or deliver an exception.
struct s_ee_perf_exception_entry
{
    uint32_t m_status;
    uint32_t m_cause;
    uint32_t m_error_epc;
    uint32_t m_vector;
};

inline constexpr s_ee_perf_exception_entry ee_perf_project_exception_entry(
    uint32_t status, uint32_t cause, uint32_t cancelled_pc, bool in_delay_slot)
{
    constexpr uint32_t erl = 1u << 2u;
    constexpr uint32_t dev = 1u << 23u;
    constexpr uint32_t bd2 = 1u << 30u;
    constexpr uint32_t exc2_mask = 7u << 16u;
    constexpr uint32_t counter_exc2 = 2u << 16u;

    return
    {
        status | erl,
        (cause & ~(bd2 | exc2_mask)) | counter_exc2 |
            (in_delay_slot ? bd2 : 0u),
        in_delay_slot ? cancelled_pc - 4u : cancelled_pc,
        (status & dev) != 0u ? 0xbfc00280u : 0x80000080u
    };
}

// The producer must identify the cancelled instruction and establish
// that the architectural state is at its restart boundary. A counter
// crossing, an issued instruction or the current dispatcher PC is not
// that evidence. This contract supports ordinary aligned instructions
// and ordinary branch delay slots only; unknown boundaries fail closed.
enum class e_ee_perf_restart_kind : uint8_t
{
    UNAVAILABLE,
    ORDINARY,
    DELAY_SLOT
};

struct s_ee_perf_restart_boundary
{
    e_ee_perf_restart_kind m_kind = e_ee_perf_restart_kind::UNAVAILABLE;
    uint32_t m_cancelled_pc = 0u;
    uint32_t m_branch_pc = 0u;
    bool m_architectural_state_restored = false;
};

enum class e_ee_perf_entry_result : uint8_t
{
    NOT_ELIGIBLE,
    UNAVAILABLE_BOUNDARY,
    INCONSISTENT_BOUNDARY,
    READY
};

struct s_ee_perf_entry_plan
{
    e_ee_perf_entry_result m_result = e_ee_perf_entry_result::NOT_ELIGIBLE;
    s_ee_perf_recognition_plan m_recognition{};
    s_ee_perf_exception_entry m_entry{};
};

// Read-only composition of pending flag recognition and entry projection.
// The input state must already correspond to the restored boundary.
// This helper checks the supplied structural evidence; it cannot prove
// cancellation, rollback or the validity of an external timing source.
inline constexpr s_ee_perf_entry_plan ee_perf_plan_exception_at_boundary(
    uint32_t control, uint32_t status, uint32_t cause, uint32_t error_epc,
    const uint32_t (&counters)[2], bool reset_pending, bool nmi_pending,
    const s_ee_perf_restart_boundary& boundary)
{
    using kind = e_ee_perf_restart_kind;
    using result = e_ee_perf_entry_result;
    s_ee_perf_entry_plan plan{};
    plan.m_recognition = ee_perf_plan_recognition(
        control, status, counters, reset_pending, nmi_pending);
    plan.m_entry = {status, cause, error_epc, 0u};

    if (plan.m_recognition.m_result == e_ee_perf_recognition::ELIGIBLE)
    {
        if (boundary.m_kind == kind::UNAVAILABLE ||
            !boundary.m_architectural_state_restored)
        {
            plan.m_result = result::UNAVAILABLE_BOUNDARY;
        }
        else if ((boundary.m_cancelled_pc & 3u) != 0u ||
            (boundary.m_kind == kind::ORDINARY && boundary.m_branch_pc != 0u) ||
            (boundary.m_kind == kind::DELAY_SLOT &&
             ((boundary.m_branch_pc & 3u) != 0u ||
              boundary.m_branch_pc + 4u != boundary.m_cancelled_pc)) ||
            (boundary.m_kind != kind::ORDINARY && boundary.m_kind != kind::DELAY_SLOT))
        {
            plan.m_result = result::INCONSISTENT_BOUNDARY;
        }
        else
        {
            plan.m_entry = ee_perf_project_exception_entry(
                status, cause, boundary.m_cancelled_pc,
                boundary.m_kind == kind::DELAY_SLOT);
            plan.m_result = result::READY;
        }
    }
    return plan;
}

// An independently supplied crossing prefix and restored restart boundary
// must refer to the same constant-PCCR/Status interval. Snapshot comparison
// detects changed register values, not an intervening write/restore or a
// producer generation change. Never retain a plan for later commitment.
struct s_ee_perf_crossing_request
{
    uint32_t m_control = 0u;
    uint32_t m_status = 0u;
    uint32_t m_before[2]{};
    s_ee_perf_event_interval m_prefix{};
    s_ee_perf_restart_boundary m_boundary{};
    bool m_reset_pending = false;
    bool m_nmi_pending = false;
};

enum class e_ee_perf_crossing_entry_result : uint8_t
{
    UNAVAILABLE_SOURCE,
    STALE_SNAPSHOT,
    UNAUTHORIZED_EVENTS,
    PREFIX_REJECTED,
    ENTRY_REJECTED,
    READY,
    UNSAFE_LIVE_STATE,
    COMMITTED
};

struct s_ee_perf_crossing_entry_plan
{
    e_ee_perf_crossing_entry_result m_result =
        e_ee_perf_crossing_entry_result::UNAVAILABLE_SOURCE;
    s_ee_perf_boundary_projection m_crossing{};
    s_ee_perf_entry_plan m_entry{};
};

// Compose the projections without mutating counters or CPU state.
// The runtime preflight additionally verifies source and authorization.
// Only READY contains a complete proposed entry; even then no delivery
// has occurred and precise producer evidence remains a prerequisite.
inline constexpr s_ee_perf_crossing_entry_plan ee_perf_plan_crossing_entry(
    const s_ee_perf_crossing_request& request,
    uint32_t control, uint32_t status, uint32_t cause, uint32_t error_epc,
    const uint32_t (&counters)[2])
{
    using result = e_ee_perf_crossing_entry_result;
    s_ee_perf_crossing_entry_plan plan{};
    if (request.m_control != control || request.m_status != status ||
        request.m_before[0] != counters[0] ||
        request.m_before[1] != counters[1])
    {
        plan.m_result = result::STALE_SNAPSHOT;
    }
    else
    {
        plan.m_crossing = ee_perf_project_crossing_prefix(
            control, status, request.m_prefix, counters);
        if (plan.m_crossing.m_result != e_ee_perf_boundary_result::EXACT_CROSSING)
        {
            plan.m_result = result::PREFIX_REJECTED;
        }
        else
        {
            plan.m_entry = ee_perf_plan_exception_at_boundary(
                control, status, cause, error_epc, plan.m_crossing.m_next,
                request.m_reset_pending, request.m_nmi_pending,
                request.m_boundary);
            plan.m_result = plan.m_entry.m_result == e_ee_perf_entry_result::READY
                ? result::READY : result::ENTRY_REJECTED;
        }
    }
    return plan;
}

// Isolated delivery model; not installed as a runtime producer.
// No overflow is silently consumed: the Level 2 exception path must
// handle the boundary before any counter increment can be committed.
inline constexpr e_ee_perf_interval_result ee_perf_apply_interval(
    uint32_t control, uint32_t status,
    const s_ee_perf_event_interval& interval, uint32_t (&counters)[2])
{
    const auto plan = ee_perf_plan_interval(control, status, interval, counters);
    if (plan.m_result == e_ee_perf_interval_result::OK)
    {
        counters[0] = plan.m_counters[0].m_next;
        counters[1] = plan.m_counters[1].m_next;
    }
    return plan.m_result;
}

} // namespace ee
