#pragma once

#include "iop_cpu.h"
#include "iop_hw.h"
#include "irx_loader.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace iop
{
// Deliberately separate IOP kernel threads from EE guest threads.
// A syscall requires a real kernel handler; unknown requests stop execution.
enum class e_thread_status
{
    dormant,
    ready,
    running,
    blocked,
    faulted
};

// Optional host-only attribution of a sampled HLE kernel invocation.
// Phases are disjoint but not exhaustive; CPU time includes retirement
// and attached device work. Never use timings for guest scheduling.
enum class e_kernel_phase : uint32_t
{
    wake_irq,
    ready,
    idle_deadline,
    idle_advance,
    guest_context,
    guest_cpu,
    count
};

struct s_kernel_phase_sample
{
    static constexpr uint32_t k_count =
        static_cast<uint32_t>(e_kernel_phase::count);
    uint64_t m_calls[k_count]{};
    uint64_t m_ns[k_count]{};
};

enum class e_kernel_stop
{
    budget_exhausted,
    idle,
    unhandled_syscall,
    unsupported_instruction,
    memory_fault,
    arithmetic_overflow,
    breakpoint
};

struct s_kernel_run_result
{
    e_kernel_stop m_stop = e_kernel_stop::budget_exhausted;
    uint64_t m_executed = 0u;
    uint32_t m_thread_id = 0u;
    uint32_t m_guest_pc = 0u;
    uint32_t m_opcode = 0u;
};


enum class e_guest_callback_source : uint32_t
{
    unknown = 0u,
    vblank_start = 1u,
    vblank_end = 2u,
    cdvdman_read = 3u
};

// Origin data is host-only and never exposed to guest IOP state.
struct s_guest_callback_origin
{
    e_guest_callback_source m_source = e_guest_callback_source::unknown;
    uint32_t m_registration_thread_id = 0u;
    uint32_t m_registration_pc = 0u;
    uint32_t m_registration_ra = 0u;
    uint32_t m_registration_gp = 0u;
    uint32_t m_registration_a0 = 0u;
    uint32_t m_registration_a1 = 0u;
    uint32_t m_registration_a2 = 0u;
    uint32_t m_registration_a3 = 0u;
    uint32_t m_registration_recent_count = 0u;
    uint32_t m_registration_recent_pc[8]{};
    uint32_t m_registration_recent_opcode[8]{};
};

struct s_kernel_thread
{
    uint32_t m_id = 0u;
    uint32_t m_priority = 0u;
    uint32_t m_initial_priority = 0u;
    // Original IOP CreateThread descriptor; ReferThreadStatus exposes
    // these exact guest values rather than fabricated zeros.
    uint32_t m_attr = 0u;
    uint32_t m_option = 0u;
    uint32_t m_entry = 0u;
    uint32_t m_gp = 0u;
    // Host-only origin. When created inside a guest syscall, m_creator_pc
    // is the executing R3000's resume/continuation PC, not a fabricated
    // guest callsite. Zero creator means host-originated module/IRQ/callback.
    uint32_t m_creator_thread_id = 0u;
    uint32_t m_creator_pc = 0u;
    uint64_t m_created_cycle = 0u;
    uint32_t m_wakeup_count = 0u;
    bool m_delete_on_exit = false;
    bool m_hle_guest_callback = false;
    s_guest_callback_origin m_callback_origin{};
    bool m_interrupt_callback = false;
    uint32_t m_interrupt_line = 0u;
    // INTRMAN suspension is part of this guest thread's context.
    // A blocked thread must not globally mask another thread's IRQs.
    bool m_interrupts_suspended = false;
    // Module _start has a guest return trampoline; returning must
    // retire its init thread instead of executing zero-filled RAM.
    bool m_module_entry = false;
    uint32_t m_module_id = 0u;
    // Only the reserved guest return BREAK records a completed _start.
    // Stopping or faulting a startup thread cannot complete LOADFILE.
    bool m_module_entry_returned = false;
    uint32_t m_module_entry_result = 0u;
    uint32_t m_wait_type = 0u;
    uint32_t m_stack_base = 0u;
    uint32_t m_stack_size = 0u;
    uint64_t m_wake_cycle = 0u;
    uint32_t m_alarm_id = 0u;
    // Monotonic, host-only IOP scheduling diagnostics. These do not
    // affect original guest scheduling or register state and are
    // printed only at bounded outstanding-RPC snapshots.
    uint64_t m_guest_instructions = 0u;
    uint64_t m_guest_slices = 0u;
    uint64_t m_handled_syscalls = 0u;
    uint64_t m_self_restarts = 0u;
    uint32_t m_last_self_restart_ra = 0u;
    uint32_t m_last_self_restart_arg = 0u;
    uint32_t m_last_syscall_number = 0u;
    uint32_t m_last_syscall_pc = 0u;
    uint32_t m_last_syscall_ra = 0u;
    uint32_t m_last_syscall_a0 = 0u;
    e_thread_status m_status = e_thread_status::dormant;
    s_cpu_state m_cpu{};
};

// A scheduled IOP timer runs the original guest callback, never a
// synthetic successful completion. Timing uses the current uncalibrated
// IOP guest-instruction clock until the EE/IOP scheduler is synchronized.
struct s_kernel_alarm
{
    uint32_t m_id = 0u;
    uint32_t m_thread_id = 0u;
    uint32_t m_callback = 0u;
    uint32_t m_argument = 0u;
    uint32_t m_gp = 0u;
    uint64_t m_deadline = 0u;
    bool m_cancelled = false;
};

struct s_kernel_module
{
    uint32_t m_id = 0u;
    s_irx_module m_irx{};
};

struct s_module_load_result
{
    e_irx_load_result m_status = e_irx_load_result::invalid_module;
    uint32_t m_id = 0u;
    s_irx_link_diagnostic m_link_diagnostic{};
};

// Called after the R3000 encounters a syscall. The callback may update
// the CPU registers and call sleep_current()/stop_current(). Returning
// false leaves the syscall unhandled and stops at the original guest PC.
// No default kernel HLE, SIF service, or synthetic ready bit exists.
struct s_kernel_interrupt_target
{
    uint32_t m_handler = 0u;
    uint32_t m_argument = 0u;
};

using t_kernel_irq_lookup = std::function<bool(
    uint32_t line, s_kernel_interrupt_target* target
)>;
using t_kernel_irq_complete = std::function<void(
    uint32_t line, uint32_t guest_result
)>;

class c_kernel;
using t_kernel_syscall = std::function<bool(
    c_kernel& kernel, uint32_t syscall_number
)>;

class c_kernel
{
public:
    static constexpr uint32_t k_user_base = 0x00020000u;
    // ps2-web-recomp/SDBZ runtime/src/iop/kernel.cpp: start_module().
    static constexpr uint32_t k_default_module_priority = 8u;

    c_kernel();
    void reset();

    // The authentic R3000 bootstrap runs directly from the caller's
    // ROM0 image at BFC00000, not as a fabricated kernel thread.
    // This must occur before any IRX imports or modules are installed.
    bool start_rom0_boot(const std::vector<uint8_t>& image);
    // Attach the real physical IOP bus for HLE-first IRX execution.
    bool attach_hle_hardware();
    bool rom0_boot_active() const;
    c_hw& hardware();
    const c_hw& hardware() const;

    c_cpu& cpu();
    const c_cpu& cpu() const;
    uint64_t cycles() const;
    uint32_t current_thread_id() const;
    bool in_interrupt_context() const;
    bool interrupts_suspended() const;
    void set_interrupts_suspended(bool suspended);

    // Actual IOP library exports supply concrete executable targets.
    bool lookup_library(
        const std::string& name, uint16_t ordinal,
        uint32_t* entry
    ) const;
    bool register_library(
        const std::string& name, uint16_t ordinal, uint32_t entry
    );
    // Register/release only a verified export table belonging to a
    // successfully loaded guest IRX. Never remove HLE-owned imports.
    // All ordinals are validated before the library registry changes.
    bool set_module_library_registration(uint32_t table, bool registered);
    // Called after a module is relocated, linked and registered (before
    // its start thread runs). Used for IRX AOT capture and activation.
    void set_module_loaded_callback(
        std::function<void(const s_irx_module&)> callback)
    {
        m_module_loaded = std::move(callback);
    }
    s_module_load_result load_module(
        const std::vector<uint8_t>& bytes,
        const std::string& source_name = {}
    );
    const s_kernel_module* module(uint32_t id) const;
    const std::map<uint32_t, s_kernel_module>& modules() const;

    // HLE IOPHEAP shares the same first-fit IOP RAM address space
    // as linked IRX modules and guest thread stacks. Only addresses
    // allocated by this API can be freed through IOPHEAP.
    uint32_t allocate_heap(uint32_t size);
    bool free_heap(uint32_t address);
    // Enforce ownership for guest-requested file copies into IOP RAM.
    bool owns_heap_span(uint32_t address, uint32_t size) const;
    uint32_t heap_max_free() const;
    uint32_t heap_total_free() const;
    bool query_heap_block(uint32_t address, uint32_t* base,
                          uint32_t* size) const;

    // Create an initially dormant R3000 thread. Priority 0 is highest.
    // A kernel will eventually publish proper thread APIs to IRX imports.
    uint32_t create_thread(
        uint32_t entry, uint32_t gp, uint32_t priority,
        uint32_t stack_size = 0x1000u,
        uint32_t a0 = 0u, uint32_t a1 = 0u,
        uint32_t attr = 0u, uint32_t option = 0u
    );
    uint32_t create_module_thread(
        uint32_t module_id, uint32_t priority,
        uint32_t stack_size = 0x4000u
    );
    bool start_thread(uint32_t id);
    bool start_thread_with_argument(uint32_t id, uint32_t argument);
    bool start_thread_with_arguments(uint32_t id,
                                     uint32_t a0, uint32_t a1);
    // Start a linked IRX with the real _start(argc, argv) guest ABI.
    // argv[0] is the module path; further NUL-separated LOADFILE
    // arguments reside in the thread-owned IOP stack allocation.
    bool start_module_thread_with_arguments(
        uint32_t thread_id,
        const std::string& module_path,
        const std::vector<std::string>& arguments
    );
    bool wake_thread(uint32_t id);
    bool wake_thread_with_result(uint32_t id, uint32_t result);
    bool stop_thread(uint32_t id);
    bool delete_thread(uint32_t id);
    bool exit_current_thread(bool delete_on_exit = false);
    bool change_thread_priority(uint32_t id, uint32_t priority);
    bool sleep_thread();
    bool wakeup_thread(uint32_t id);
    bool cancel_wakeup_thread(uint32_t id, uint32_t* previous);
    bool sleep_current(uint64_t delay_cycles);
    bool block_current(uint32_t wait_type = 0u);
    const s_kernel_thread* thread(uint32_t id) const;
    // Diagnostic read-only view of real guest thread contexts; never
    // changes scheduler order, interrupts or IOP virtual time.
    const std::map<uint32_t, s_kernel_thread>& threads() const;
    // Only scheduled guest work permits an HLE transport-triggered
    // IOP execution turn; absent modules cannot advance IOP time.
    bool has_guest_work() const;
    // Relative IOP cycles before any currently modeled autonomous event.
    // Zero for runnable work or a scheduling barrier. Externally driven
    // EE/SIF operations must be checked separately by the caller.
    uint64_t idle_event_cycles_remaining() const;

    // Host-only fast service for a strictly pre-event idle interval.
    // A false return leaves guest time and architectural state untouched.
    // The caller still owns EE/IOP RPC ordering and cycle debt accounting.
    bool try_advance_quiet_cycles(uint32_t cycles);

    // Timers use the same bounded virtual-clock domain as sleep_current.
    // Callback return values of zero disarm, otherwise they reschedule by
    // the returned system-clock tick count. This is not a calibrated IRQ.
    bool set_alarm(uint64_t ticks, uint32_t callback,
                   uint32_t argument, uint32_t gp);
    bool cancel_alarm(uint32_t callback, uint32_t argument);
    // Deterministic HLE tasks in IOP guest-instruction time, not host time.
    // Scheduling a task does not imply a hardware IRQ or DMA completion.
    bool can_schedule_hle_event(uint64_t ticks) const;
    bool schedule_hle_event(uint64_t ticks, std::function<void()> callback);
    // Execute the original R3000 guest function as a bounded one-shot
    // callback with a real MIPS return trampoline, not a host fake.
    s_guest_callback_origin make_guest_callback_origin(
        e_guest_callback_source source) const;
    bool start_guest_callback(uint32_t callback, uint32_t gp,
        uint32_t argument, const s_guest_callback_origin& origin = {});

    void bind_syscall(t_kernel_syscall handler);
    // INTRMAN registrations resolve actual pending device IRQs to guest
    // addresses. No IRQ handler executes unless the source is asserted.
    void bind_interrupts(t_kernel_irq_lookup lookup,
                         t_kernel_irq_complete complete);
    // Explicit IOP cycle budget. The EE must eventually provide this from
    // guest execution timing, never by counting native dispatches.
    s_kernel_run_result run(
        uint64_t cycle_budget, uint32_t quantum = 128u,
        s_kernel_phase_sample* sample = nullptr
    );

private:
    uint32_t allocate(uint32_t size);
    // The IOP reference uses upper-RAM, 256-byte-aligned stacks;
    // module images and heap allocations retain low-RAM first-fit.
    uint32_t allocate_thread_stack(uint32_t size);
    bool estimate_module_size(
        const std::vector<uint8_t>& bytes, uint32_t* size
    ) const;
    uint32_t next_ready(uint64_t& next_wake) const;
    void rebuild_ready_cache() const;
    void wake_due();
    bool start_guest_interrupt(uint32_t line,
                               const s_kernel_interrupt_target& target);

    c_cpu m_cpu{};
    c_hw m_hardware{};
    std::map<uint32_t, uint32_t> m_allocations;
    std::map<uint32_t, uint32_t> m_heap_allocations;
    std::map<std::string, std::map<uint16_t, uint32_t>> m_libraries;
    std::map<uint32_t, s_kernel_module> m_modules;
    std::function<void(const s_irx_module&)> m_module_loaded;
    std::map<uint32_t, s_kernel_thread> m_threads;
    // Exact highest-priority ready IDs in original map order, and the
    // earliest future timed wake. Invalidate on durable thread state
    // changes; a normal READY -> RUNNING -> READY slice is unchanged.
    mutable bool m_ready_cache_dirty = true;
    mutable std::vector<uint32_t> m_ready_cache;
    mutable uint64_t m_ready_cache_wake = std::numeric_limits<uint64_t>::max();
    std::map<uint32_t, s_kernel_alarm> m_alarms;
    std::multimap<uint64_t, std::function<void()>> m_hle_events;
    t_kernel_syscall m_syscall;
    t_kernel_irq_lookup m_irq_lookup;
    t_kernel_irq_complete m_irq_complete;
    uint32_t m_interrupt_thread_id = 0u;
    uint32_t m_critical_thread_id = 0u;
    bool m_interrupts_suspended = false;
    uint64_t m_cycles = 0u;
    // Conservative lower bound on timed thread wakes. Cancellation may
    // leave it early, but scheduling a sleep must never leave it late.
    uint64_t m_next_thread_wake = std::numeric_limits<uint64_t>::max();
    // Conservative earliest alarm deadline. Cancellation can leave an
    // earlier hint, but never a hint later than a live alarm.
    uint64_t m_next_alarm_wake = std::numeric_limits<uint64_t>::max();
    bool m_rom0_boot_active = false;
    uint32_t m_next_module_id = 1u;
    uint32_t m_next_thread_id = 1u;
    uint32_t m_next_alarm_id = 1u;
    uint32_t m_last_scheduled_id = 0u;
    uint32_t m_current_thread_id = 0u;
    // A genuine StartThread(0) may restart its own running guest
    // thread. Apply the new R3000 context after the syscall handler,
    // not the old post-syscall continuation.
    bool m_restart_current_thread = false;
};
} // namespace iop
