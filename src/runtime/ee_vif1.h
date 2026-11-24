// Adapted from ps2-web-recomp:SDBZ by Samir Nuri (2026), MIT license.
// Upstream license: src/runtime/ps2_webgpu/gs/LICENSE.upstream
#pragma once

#include "ee_gif.h"
#include "ee_vu1.h"
#include "ee_vu1_worker.h"
#include "ee_runtime.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ee
{
// VIF1 state and packet processing adapted from ps2-web-recomp:SDBZ,
// runtime/src/vif.cpp (MIT license, copyright 2026 Samir Nuri).
// LLVM executes the EE guest; VIF1 remains a host-owned PS2 device.
// Unsupported VU execution or VIF stalls never constitute DMA completion.
class c_ee_vif1
{
public:
    static constexpr uint32_t k_register_base = 0x10003c00u;
    static constexpr uint32_t k_fifo_base = 0x10005000u;
    static constexpr uint32_t k_vu1_code_base = 0x11008000u;
    static constexpr uint32_t k_vu1_data_base = 0x1100c000u;
    static constexpr size_t k_vu1_memory_size = 16384u;

    enum class e_reject : uint32_t
    {
        none,
        invalid_packet,
        unsupported_command,
        unsupported_vu_execution,
        irq_requires_resume,
        missing_path2,
        path2_rejected,
        missing_path1,
        path1_rejected,
        mixed_gif_paths,
        pending_limit,
        mscalf_path3_busy
    };

    struct s_diagnostic
    {
        e_reject m_reason = e_reject::none;
        uint32_t m_code = 0u;
        uint32_t m_pending_words = 0u;
        uint32_t m_commands = 0u;
        uint32_t m_mpg_uploads = 0u;
        uint32_t m_unpack_commands = 0u;
        uint32_t m_direct_qwc = 0u;
        uint32_t m_vu_pc = 0u;
        uint32_t m_vu_lower = 0u;
        uint32_t m_vu_upper = 0u;
        // Lifetime records survive later successful VIF packets. This
        // distinguishes an unreached VU1 from a rejected VU1 command.
        uint64_t m_rejected_batches = 0u;
        e_reject m_first_rejection = e_reject::none;
        uint32_t m_first_rejected_code = 0u;
        // Parsed attempts include retries of a rejected DMA packet.
        uint64_t m_vu_start_attempts = 0u;
        uint64_t m_vu_start_rejections = 0u;
        uint32_t m_last_vu_start_code = 0u;
        uint32_t m_last_rejected_vu_start_code = 0u;
        uint32_t m_last_rejected_vu_pc = 0u;
        uint32_t m_last_rejected_vu_lower = 0u;
        uint32_t m_last_rejected_vu_upper = 0u;
        uint32_t m_last_rejected_vu_steps = 0u;
        s_ee_vu1_issue m_last_rejected_vu_issue{};
        uint32_t m_last_rejected_staged_mpg = 0u;
        uint32_t m_last_rejected_staged_unpack = 0u;
        std::array<uint64_t, 128u> m_committed_opcodes{};
    };

    void bind_path2(c_ee_gif_path3* path2);
    void bind_path1(c_ee_gif_path3* path1);
    void bind_path3(const c_ee_gif_path3* path3);
    void reset();
    bool consume(const uint8_t* data, uint32_t qwc);
    const s_diagnostic& diagnostic() const;

    // Host-side opt-in staged transfer. No guest completion is implied by
    // pending; the caller must serialize conflicting device operations.
    enum class e_deferred_result
    {
        idle,
        pending,
        completed,
        rejected,
        busy
    };
    bool enable_deferred(std::unique_ptr<c_ee_vu1_aot> backend = {},
        bool detailed_profile = false);
    void native_execution_begin();
    void native_execution_end();
    void write_aot_json(FILE* output) const;
    void write_worker_json(FILE* output) const;
    e_deferred_result begin_deferred(const uint8_t* data, uint32_t qwc);
    e_deferred_result poll_deferred();
    e_deferred_result wait_deferred();
    bool deferred_busy() const;
    void cancel_deferred();

    uint32_t read_register(uint32_t offset) const;
    bool write_register(uint32_t offset, uint32_t value);
    const uint8_t* vu1_data() const;
    const uint8_t* vu1_code() const;

    static bool dma_transfer(void* user, uint32_t channel,
                             const uint8_t* data, uint32_t qwc);
    static bool mmio_read(void* user, uint32_t address,
                          uint32_t width, s_ee_u128* value);
    static bool mmio_write(void* user, uint32_t address,
                           uint32_t width, const s_ee_u128* value);

private:
    // VU1 residency has one owner: the staged or committed machine.
    // VIF command reset preserves this entire value; a full reset clears it.
    // Keeping the memories and execution context together permits an
    // explicit ownership handoff without lending MMIO pointers to a worker.
    struct s_machine
    {
        uint32_t m_stat = 0u;
        uint32_t m_err = 0u;
        uint32_t m_mark = 0u;
        uint32_t m_cycle = 0u;
        uint32_t m_mode = 0u;
        uint32_t m_num = 0u;
        uint32_t m_mask = 0u;
        uint32_t m_code = 0u;
        uint32_t m_itops = 0u;
        uint32_t m_base = 0u;
        uint32_t m_ofst = 0u;
        uint32_t m_tops = 0u;
        uint32_t m_itop = 0u;
        uint32_t m_top = 0u;
        uint32_t m_row[4]{};
        uint32_t m_col[4]{};
        bool m_dbf = false;
        std::vector<uint32_t> m_pending;
        s_ee_vu1_resident m_vu1{};
        uint32_t m_commands = 0u;
        uint32_t m_mpg_uploads = 0u;
        uint32_t m_unpack_commands = 0u;
        uint32_t m_direct_qwc = 0u;
        std::array<uint64_t, 128u> m_committed_opcodes{};
    };

    struct s_path_range
    {
        uint32_t m_path;
        size_t m_offset;
        size_t m_size;
    };

    // Staging and publication are separate. Only the controller accesses
    // this object; a worker receives a detached resident value and PATH1.
    struct s_transaction
    {
        explicit s_transaction(c_ee_vif1& owner);
        ~s_transaction();
        s_transaction(const s_transaction&) = delete;
        s_transaction& operator=(const s_transaction&) = delete;

        c_ee_vif1& m_owner;
        s_machine m_next{};
        std::vector<uint8_t> m_path1;
        std::vector<uint8_t> m_path2;
        std::vector<s_path_range> m_ranges;
        size_t m_index = 0u;
        size_t m_before1 = 0u;
        size_t m_before2 = 0u;
        uint32_t m_waiting_words = 0u;
        s_ee_vu1_issue m_issue{};
        e_reject m_rejection = e_reject::none;
        uint32_t m_rejected_code = 0u;
        bool m_committed = false;
    };

    void reject_packet(e_reject reason, uint32_t code);
    bool prepare(s_transaction& tx, const uint8_t* data, uint32_t qwc);
    bool advance(s_transaction& tx, c_ee_vu1_worker* worker);
    bool record_command(s_transaction& tx, uint32_t code, uint32_t words);
    bool publish(s_transaction& tx);
    e_deferred_result resume_deferred(bool wait);
    static void complete_vu_start(s_machine& machine);
    void record_native_overlap(uint64_t end);
    std::unique_ptr<s_ee_vu1_job> take_worker_job(bool wait);
    void record_worker_job(const s_ee_vu1_job& job, bool discarded);

    static uint32_t command_data_words(const s_machine& machine,
                                       uint32_t code);
    static void unpack(s_machine& machine, uint32_t code,
                       const uint32_t* words);
    static bool execute(s_machine& machine, uint32_t code,
                        const uint32_t* words,
                        std::vector<uint8_t>& path2_bytes,
                        std::vector<uint8_t>& path1_bytes,
                        s_ee_vu1_issue* vu_issue,
                        e_reject* reject);

    s_machine m_machine{};
    // Host-only transaction storage. Never part of committed VIF state.
    std::vector<uint32_t> m_pending_scratch;
    std::vector<uint8_t> m_path1_scratch;
    std::vector<uint8_t> m_path2_scratch;
    s_diagnostic m_diagnostic{};
    c_ee_gif_path3* m_path2 = nullptr;
    c_ee_gif_path3* m_path1 = nullptr;
    const c_ee_gif_path3* m_path3 = nullptr;
    // Host timing only. Neither timestamps nor overlap advance guest cycles.
    struct s_worker_profile
    {
        uint64_t m_submitted = 0u;
        uint64_t m_collected = 0u;
        uint64_t m_rejected = 0u;
        uint64_t m_discarded = 0u;
        uint64_t m_host_failures = 0u;
        uint64_t m_pairs = 0u;
        uint64_t m_execution_ns = 0u;
        uint64_t m_queue_ns = 0u;
        uint64_t m_compiled_ns = 0u;
        uint64_t m_compiled_pairs = 0u;
        uint64_t m_xgkick_ns = 0u;
        uint64_t m_wait_ns = 0u;
        uint64_t m_wait_calls = 0u;
        uint64_t m_poll_calls = 0u;
        uint64_t m_empty_polls = 0u;
        uint64_t m_native_overlap_ns = 0u;
        uint64_t m_native_overlap_intervals = 0u;
        uint64_t m_native_pending_boundaries = 0u;
    } m_worker_profile{};
    bool m_native_executing = false;
    uint64_t m_native_interval_start = 0u;
    std::unique_ptr<s_ee_vu1_job> m_spare_job;
    std::unique_ptr<c_ee_vu1_worker> m_worker;
    std::unique_ptr<s_transaction> m_deferred;
};
} // namespace ee
