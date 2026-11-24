#include "iop_sif1_bridge.h"
#include "iop_sifcmd_bridge.h"
#include "iop_sif_rpc_bridge.h"
#include "iop_folder_sector_source.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
int fail(const char* reason)
{
    std::fprintf(stderr, "IOP LOADFILE RPC: %s\n", reason);
    return 1;
}

void write16(std::vector<uint8_t>& bytes, uint32_t offset, uint16_t value)
{
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1u] = static_cast<uint8_t>(value >> 8u);
}

void write32(std::vector<uint8_t>& bytes, uint32_t offset, uint32_t value)
{
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8u));
    }
}

bool create_irx(const std::filesystem::path& location)
{
    std::vector<uint8_t> bytes(0x220u, 0u);
    bytes[0] = 0x7fu;
    bytes[1] = 'E';
    bytes[2] = 'L';
    bytes[3] = 'F';
    bytes[4] = 1u;
    bytes[5] = 1u;
    bytes[6] = 1u;
    write16(bytes, 16u, 0xff80u);
    write16(bytes, 18u, 8u);
    write32(bytes, 28u, 0x34u);
    write16(bytes, 40u, 52u);
    write16(bytes, 42u, 32u);
    write16(bytes, 44u, 2u);
    write32(bytes, 0x34u, 1u);
    write32(bytes, 0x38u, 0x100u);
    write32(bytes, 0x44u, 0x60u);
    write32(bytes, 0x48u, 0x80u);
    write32(bytes, 0x54u, 0x70000080u);
    write32(bytes, 0x58u, 0x180u);
    write32(bytes, 0x184u, 0u);
    write32(bytes, 0x188u, 0x50u);
    write32(bytes, 0x18cu, 0x40u);
    write32(bytes, 0x190u, 0x20u);
    write32(bytes, 0x194u, 0x20u);
    const char name[] = "RPCTEST";
    for (uint32_t i = 0u; i < sizeof(name); ++i)
    {
        bytes[0x19au + i] = static_cast<uint8_t>(name[i]);
    }
    write32(bytes, 0x100u, 0x24020007u); // guest _start returns 7
    write32(bytes, 0x104u, 0x03e00008u); // jr ra
    write32(bytes, 0x108u, 0u);          // delay slot
    std::ofstream out(location, std::ios::binary);
    out.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size())
    );
    return out.good();
}

void descriptor(
    ee::ee_memory& memory,
    uint32_t at, uint32_t src, uint32_t dst, uint32_t size
)
{
    memory.write32(at + 0u, src);
    memory.write32(at + 4u, dst);
    memory.write32(at + 8u, size);
    memory.write32(at + 12u, 0u);
}

void packet(
    ee::ee_memory& memory, uint32_t at,
    const std::vector<uint32_t>& words
)
{
    for (uint32_t index = 0u; index < words.size(); ++index)
    {
        memory.write32(at + index * 4u, words[index]);
    }
}

bool init_sifcmd_guest(iop::c_kernel& kernel)
{
    constexpr uint32_t start = 0x0001c000u;
    const uint32_t jump = 0x0c000000u |
        (iop::c_sifcmd_bridge::k_init_cmd_stub >> 2u);
    if (!kernel.cpu().write32(start, jump) ||
        !kernel.cpu().write32(start + 4u, 0u) ||
        !kernel.cpu().write32(start + 8u, 0x0000000du))
    {
        return false;
    }
    const uint32_t thread_id = kernel.create_thread(
        start, 0u, 1u, 0x400u
    );
    if (thread_id == 0u || !kernel.start_thread(thread_id))
    {
        return false;
    }
    const iop::s_kernel_run_result run = kernel.run(64u, 64u);
    return run.m_stop == iop::e_kernel_stop::breakpoint &&
           run.m_guest_pc == start + 8u;
}

struct s_temp_dir
{
    std::filesystem::path m_path;
    ~s_temp_dir()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }
};



bool test_arkd_deferred_bind(const std::filesystem::path& disc_root)
{
    constexpr uint32_t k_list = 0x10000u;
    constexpr uint32_t k_packet = 0x12000u;
    constexpr uint32_t k_receive = 0x00180000u;
    constexpr uint32_t k_queue = 0x00027000u;
    constexpr uint32_t k_descriptor = 0x00027100u;
    constexpr uint32_t k_handler = 0x00027200u;
    constexpr uint32_t k_stack = 0x00027300u;
    constexpr uint32_t k_server = 0x500u;

    ee::ee_memory memory(32u * 1024u * 1024u);
    ee::s_ee_cpu_state ee_cpu{};
    ee::c_ee_sif sif;
    ee::c_ee_dmac dmac;
    sif.reset();
    dmac.bind_cpu(&ee_cpu);
    dmac.bind_memory(&memory);
    dmac.reset();
    iop::c_kernel kernel;
    iop::c_sifcmd_bridge commands(kernel, sif, &memory);
    iop::c_disc_loader loader(kernel, disc_root);
    iop::c_folder_sector_source sectors(disc_root);
    iop::c_sif_rpc_bridge rpc(
        loader, kernel, commands, memory, dmac, &sectors);
    iop::c_sif1_bridge sif1(
        memory, kernel.cpu(), dmac, &commands, &rpc);
    kernel.bind_syscall(
        [&](iop::c_kernel& active, uint32_t service)
        {
            return commands.handle_syscall(active, service);
        });
    if (!sectors.valid() || !commands.install() ||
        !init_sifcmd_guest(kernel))
    {
        return false;
    }
    kernel.cpu().state().m_gpr[4] = 0u;
    if (!commands.handle_syscall(
            kernel, iop::c_sifcmd_bridge::k_service_base + 14u) ||
        kernel.cpu().state().m_gpr[2] != 0u)
    {
        return false;
    }

    uint32_t dma_id = 0u;
    packet(memory, k_packet, {
        24u, 0u, 0x80000002u, 0u, k_receive, 0u, 0u, 0u
    });
    descriptor(memory, k_list, k_packet,
               iop::c_sifcmd_bridge::k_command_buffer, 24u);
    if (!sif1.set_dma(k_list, 1u, &dma_id))
    {
        return false;
    }
    dmac.acknowledge(6u);

    // Replay packet: 64-byte EE BIND for the guest ARKD server 0x500.
    const std::vector<uint32_t> bind = {
        64u, 0u, iop::c_sif_rpc_bridge::k_bind_cid, 0u,
        5u, 0x20561900u, 15u, 0x005a9330u,
        k_server, 0x200u, 0x00564b40u, 8u,
        1u, 0x2cacacaau, 0u, 0u
    };
    memory.write32(k_receive + 16u, 0xcafef00du);
    packet(memory, k_packet, bind);
    descriptor(memory, k_list, k_packet,
               iop::c_sifcmd_bridge::k_command_buffer, 64u);
    memory.write32(k_list + 12u, 0x44u);
    if (!sif1.set_dma(k_list, 1u, &dma_id) ||
        rpc.pending_binds() != 1u ||
        rpc.pending_replies() != 0u ||
        rpc.has_waiting_bind_guest_work() ||
        memory.read32(k_receive + 16u) != 0xcafef00du ||
        (dmac.status() & (1u << 5u)) != 0u)
    {
        return false;
    }

    // An unrelated unknown server still fails atomically.
    dmac.acknowledge(6u);
    memory.write32(k_packet + 32u, 0x8000beefu);
    iop::s_sif1_dma_diagnostic rejected{};
    if (sif1.set_dma(k_list, 1u, &dma_id, &rejected) ||
        rejected.m_rpc_rejection != iop::e_rpc_rejection::unknown_server ||
        rpc.pending_binds() != 1u ||
        rpc.pending_replies() != 0u ||
        memory.read32(k_receive + 16u) != 0xcafef00du)
    {
        return false;
    }

    if (!kernel.cpu().write32(k_handler, 0x03e00008u) ||
        !kernel.cpu().write32(k_stack + 24u, k_queue))
    {
        return false;
    }
    const uint32_t thread_id = kernel.create_thread(
        k_handler, 0u, 1u, 0x400u);
    // An allocated but dormant R3000 thread is not permission to
    // advance IOP time or answer the waiting EE client.
    if (thread_id == 0u || rpc.has_waiting_bind_guest_work())
    {
        return false;
    }
    if (!kernel.start_thread(thread_id) ||
        !rpc.has_waiting_bind_guest_work())
    {
        return false;
    }
    const iop::s_kernel_run_result guest_turn = kernel.run(1u, 1u);
    if (guest_turn.m_stop != iop::e_kernel_stop::budget_exhausted ||
        guest_turn.m_executed != 1u ||
        !rpc.has_waiting_bind_guest_work() ||
        rpc.pending_binds() != 1u ||
        rpc.pending_replies() != 0u ||
        (dmac.status() & (1u << 5u)) != 0u)
    {
        // A real guest instruction must be permitted to execute without
        // prematurely manufacturing an RPC registration or EE IRQ.
        return false;
    }

    kernel.cpu().state().m_gpr[4] = k_queue;
    kernel.cpu().state().m_gpr[5] = thread_id;
    if (!commands.handle_syscall(
            kernel, iop::c_sifcmd_bridge::k_service_base + 19u))
    {
        return false;
    }
    if (rpc.pending_binds() != 1u)
    {
        return false;
    }

    kernel.cpu().state().m_gpr[4] = k_descriptor;
    kernel.cpu().state().m_gpr[5] = k_server;
    kernel.cpu().state().m_gpr[6] = k_handler;
    kernel.cpu().state().m_gpr[7] = k_handler + 0x40u;
    kernel.cpu().state().m_gpr[29] = k_stack;
    if (!commands.handle_syscall(
            kernel, iop::c_sifcmd_bridge::k_service_base + 17u) ||
        kernel.cpu().state().m_gpr[2] != 0u ||
        !commands.has_guest_rpc_server(k_server) ||
        rpc.pending_binds() != 0u ||
        rpc.pending_replies() != 1u ||
        rpc.has_waiting_bind_guest_work() ||
        memory.read32(k_receive + 16u) != 0xcafef00du)
    {
        return false;
    }

    const ee::s_ee_u128 armed{0x184u, 0u};
    if (!ee::c_ee_dmac::mmio_write(
            &dmac, ee::c_ee_dmac::k_sif0_base,
            4u, &armed) ||
        !dmac.raise(5u) || rpc.pump() ||
        rpc.pending_replies() != 1u ||
        memory.read32(k_receive + 16u) != 0xcafef00du)
    {
        return false;
    }
    // Kernel HLE acknowledges DMA5 before running the EE callback.
    // The acknowledgement alone must leave the receiver untouched.
    rpc.set_ee_receive_busy(true);
    dmac.acknowledge(5u);
    if (rpc.pump() || rpc.pending_replies() != 1u ||
        memory.read32(k_receive + 16u) != 0xcafef00du)
    {
        return false;
    }
    rpc.set_ee_receive_busy(false);
    if (!rpc.pump() ||
        rpc.pending_replies() != 0u ||
        memory.read32(k_receive + 8u) !=
            iop::c_sif_rpc_bridge::k_reply_cid ||
        memory.read32(k_receive + 16u) != 5u ||
        memory.read32(k_receive + 28u) != 0x005a9330u ||
        memory.read32(k_receive + 32u) !=
            iop::c_sif_rpc_bridge::k_bind_cid ||
        memory.read32(k_receive + 36u) !=
            (k_server ^ iop::c_sif_rpc_bridge::k_server_xor) ||
        (dmac.status() & (1u << 5u)) == 0u)
    {
        return false;
    }

    // A pending bind to 0x501 must not survive an IOP reboot.
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    memory.write32(k_packet + 32u, 0x501u);
    if (!sif1.set_dma(k_list, 1u, &dma_id) ||
        rpc.pending_binds() != 1u)
    {
        return false;
    }
    rpc.reset_for_reboot();
    return rpc.pending_binds() == 0u &&
        rpc.pending_replies() == 0u &&
        !rpc.has_waiting_bind_guest_work();
}

bool test_guest_rpc_lifecycle(const std::filesystem::path& disc_root)
{
    // A registered guest IOP server cannot satisfy a bind before
    // sceSifInitRpc/SetRpcQueue/RegisterRpc have run. An EE RPC call
    // may only complete after the actual R3000 handler returns.
    constexpr uint32_t k_list = 0x10000u;
    constexpr uint32_t k_packet = 0x12000u;
    constexpr uint32_t k_payload = 0x14000u;
    constexpr uint32_t k_receive = 0x00180000u;
    constexpr uint32_t k_result = 0x00190000u;
    constexpr uint32_t k_queue = 0x00024000u;
    constexpr uint32_t k_descriptor = 0x00024100u;
    constexpr uint32_t k_function = 0x00024200u;
    constexpr uint32_t k_buffer = 0x00024300u;
    constexpr uint32_t k_loop = 0x00024500u;
    constexpr uint32_t k_stack = 0x00025000u;
    constexpr uint32_t k_server = 0x8000badau;

    ee::ee_memory memory(32u * 1024u * 1024u);
    ee::s_ee_cpu_state ee_cpu{};
    ee::c_ee_sif sif;
    ee::c_ee_dmac dmac;
    sif.reset();
    dmac.bind_cpu(&ee_cpu);
    dmac.bind_memory(&memory);
    dmac.reset();
    iop::c_kernel kernel;
    iop::c_sifcmd_bridge commands(kernel, sif, &memory);
    iop::c_disc_loader loader(kernel, disc_root);
    iop::c_folder_sector_source sectors(disc_root);
    iop::c_sif_rpc_bridge rpc(
        loader, kernel, commands, memory, dmac, &sectors);
    iop::c_sif1_bridge sif1(
        memory, kernel.cpu(), dmac, &commands, &rpc);
    if (!sectors.valid() || !commands.install())
    {
        return false;
    }
    kernel.bind_syscall(
        [&](iop::c_kernel& active, uint32_t service)
        {
            return commands.handle_syscall(active, service);
        });

    uint32_t init_rpc_stub = 0u;
    uint32_t loop_stub = 0u;
    for (uint16_t ordinal : {14u, 17u, 18u, 19u,
                             22u, 23u, 24u, 25u})
    {
        uint32_t entry = 0u;
        uint32_t syscall_instruction = 0u;
        if (!kernel.lookup_library("sifcmd", ordinal, &entry) ||
            !kernel.cpu().read32(entry + 8u, &syscall_instruction) ||
            syscall_instruction != 0x0000000cu)
        {
            return false;
        }
        if (ordinal == 14u)
        {
            init_rpc_stub = entry;
        }
        if (ordinal == 22u)
        {
            loop_stub = entry;
        }
    }
    // Both newly supported SIFCMD send variants must be backed by
    // executable import stubs; #13 remains unimplemented.
    for (uint16_t ordinal : {12u, 28u})
    {
        uint32_t entry = 0u;
        uint32_t opcode = 0u;
        if (!kernel.lookup_library("sifcmd", ordinal, &entry) ||
            !kernel.cpu().read32(entry + 8u, &opcode) ||
            opcode != 0x0000000cu)
        {
            return false;
        }
    }
    uint32_t unimplemented = 0u;
    if (kernel.lookup_library("sifcmd", 13u, &unimplemented))
    {
        return false;
    }

    iop::c_sif_rpc_bridge::s_request preview{};
    iop::e_rpc_rejection rejection = iop::e_rpc_rejection::none;
    const std::vector<uint32_t> bind = {
        36u, 0u, iop::c_sif_rpc_bridge::k_bind_cid, 0u,
        1u, 0x11000u, 0x1234u, 0x15000u, k_server
    };
    if (!init_sifcmd_guest(kernel) ||
        rpc.prepare_command(bind, 36u, 0u, 0u,
                            &preview, &rejection) ||
        rejection != iop::e_rpc_rejection::unknown_server)
    {
        return false;
    }

    // R3000 JAL -> exported #14 -> syscall -> original caller.
    constexpr uint32_t k_init_rpc_entry = 0x0001c080u;
    if (!kernel.cpu().write32(
            k_init_rpc_entry,
            0x0c000000u |
                ((init_rpc_stub >> 2u) & 0x03ffffffu)) ||
        !kernel.cpu().write32(k_init_rpc_entry + 4u, 0u) ||
        !kernel.cpu().write32(
            k_init_rpc_entry + 8u, 0x0000000du))
    {
        return false;
    }
    const uint32_t init_id = kernel.create_thread(
        k_init_rpc_entry, 0u, 1u, 0x400u);
    if (init_id == 0u || !kernel.start_thread(init_id))
    {
        return false;
    }
    const iop::s_kernel_run_result initialized = kernel.run(64u);
    if (initialized.m_stop != iop::e_kernel_stop::breakpoint ||
        initialized.m_guest_pc != k_init_rpc_entry + 8u)
    {
        return false;
    }
    // Actual guest callback increments the incoming argument word and
    // returns its buffer pointer in v0 to the SIFCMD finish trampoline.
    const uint32_t handler[] = {
        0x2409012cu, // addiu t1, zero, 300
        0x2529ffffu, // decrement guest counter
        0x1520fffeu, // bne t1, zero, back to decrement
        0u,          // delay slot
        0x8ca80000u, // lw t0, 0(a1)
        0x25080001u, // addiu t0, t0, 1
        0xaca80000u, // sw t0, 0(a1)
        0x3c020002u, // lui v0, 2
        0x34424300u, // ori v0, v0, 0x4300
        0x03e00008u, // jr ra
        0x00000000u
    };
    for (uint32_t i = 0u; i < sizeof(handler) / sizeof(handler[0]); ++i)
    {
        if (!kernel.cpu().write32(
                k_function + i * 4u, handler[i]))
        {
            return false;
        }
    }
    if (!kernel.cpu().write32(
            k_loop,
            0x0c000000u | ((loop_stub >> 2u) & 0x03ffffffu)) ||
        !kernel.cpu().write32(k_loop + 4u, 0u) ||
        !kernel.cpu().write32(k_loop + 8u, 0x0000000du))
    {
        return false;
    }
    const uint32_t loop_id = kernel.create_thread(
        k_loop, 0u, 1u, 0x400u, k_queue);
    if (loop_id == 0u)
    {
        return false;
    }

    kernel.cpu().state().m_gpr[4] = k_queue;
    kernel.cpu().state().m_gpr[5] = loop_id;
    if (!commands.handle_syscall(
            kernel, iop::c_sifcmd_bridge::k_service_base + 19u) ||
        kernel.cpu().state().m_gpr[2] != 0u)
    {
        return false;
    }
    if (!kernel.cpu().write32(k_stack + 24u, k_queue))
    {
        return false;
    }
    kernel.cpu().state().m_gpr[4] = k_descriptor;
    kernel.cpu().state().m_gpr[5] = k_server;
    kernel.cpu().state().m_gpr[6] = k_function;
    kernel.cpu().state().m_gpr[7] = k_buffer;
    kernel.cpu().state().m_gpr[29] = k_stack;
    if (!commands.handle_syscall(
            kernel, iop::c_sifcmd_bridge::k_service_base + 17u) ||
        kernel.cpu().state().m_gpr[2] != 0u ||
        !commands.has_guest_rpc_server(k_server) ||
        !kernel.start_thread(loop_id))
    {
        return false;
    }

    const iop::s_kernel_run_result idle = kernel.run(128u);
    const iop::s_kernel_thread* loop_thread =
        kernel.thread(loop_id);
    if (idle.m_stop != iop::e_kernel_stop::idle ||
        loop_thread == nullptr ||
        loop_thread->m_status != iop::e_thread_status::blocked ||
        rpc.pending_replies() != 0u)
    {
        return false;
    }

    // EE establishes the receive buffer using an actual SIF1
    // SIFCMD packet, then binds to the newly registered guest server.
    uint32_t dma_id = 0u;
    packet(memory, k_packet, {
        24u, 0u, 0x80000002u, 0u, k_receive, 0u, 0u, 0u
    });
    descriptor(memory, k_list, k_packet,
               iop::c_sifcmd_bridge::k_command_buffer, 24u);
    if (!sif1.set_dma(k_list, 1u, &dma_id))
    {
        return false;
    }
    dmac.acknowledge(6u);
    packet(memory, k_packet, bind);
    descriptor(memory, k_list, k_packet,
               iop::c_sifcmd_bridge::k_command_buffer, 36u);
    if (!sif1.set_dma(k_list, 1u, &dma_id) ||
        rpc.pending_replies() != 1u)
    {
        return false;
    }
    const ee::s_ee_u128 armed{0x184u, 0u};
    if (!ee::c_ee_dmac::mmio_write(
            &dmac, ee::c_ee_dmac::k_sif0_base,
            4u, &armed) ||
        !rpc.pump() ||
        rpc.pending_replies() != 0u ||
        memory.read32(k_receive + 8u) !=
            iop::c_sif_rpc_bridge::k_reply_cid ||
        memory.read32(k_receive + 32u) !=
            iop::c_sif_rpc_bridge::k_bind_cid)
    {
        return false;
    }

    // A real 4-byte payload and 56-byte RPC call. Merely committing
    // the SIF1 request MUST NOT write EE output or deliver a SIF0 reply.
    memory.write32(k_payload, 41u);
    memory.write32(k_result, 0xcafebabeu);
    packet(memory, k_packet, {
        56u, 0u, iop::c_sif_rpc_bridge::k_call_cid, 0u,
        2u, 0x11000u, 0x1234u, 0x15000u,
        0x24u, 4u, k_result, 4u, 1u,
        k_server ^ iop::c_sif_rpc_bridge::k_server_xor
    });
    descriptor(memory, k_list, k_payload,
               iop::c_sif_rpc_bridge::k_rpc_data_destination, 4u);
    descriptor(memory, k_list + 16u, k_packet,
               iop::c_sifcmd_bridge::k_command_buffer, 56u);
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    if (!sif1.set_dma(k_list, 2u, &dma_id) ||
        rpc.pending_guest_calls() != 1u ||
        memory.read32(k_result) != 0xcafebabeu ||
        rpc.pending_replies() != 0u ||
        (dmac.status() & (1u << 5u)) != 0u)
    {
        return false;
    }

    // Runner-equivalent bounded turns: an EE WaitSema may have no
    // runnable EE peer while the original IOP RPC handler is still
    // executing. Neither one slice nor a committed SIF1 request may
    // create the EE reply or signal DMA5 prematurely.
    const iop::s_kernel_run_result first = kernel.run(128u, 64u);
    if (first.m_stop != iop::e_kernel_stop::budget_exhausted ||
        rpc.pending_guest_calls() != 1u ||
        memory.read32(k_result) != 0xcafebabeu ||
        (dmac.status() & (1u << 5u)) != 0u)
    {
        return false;
    }
    iop::s_kernel_run_result finished = first;
    for (uint32_t turn = 0u;
         turn < 16u && rpc.pending_guest_calls() != 0u; ++turn)
    {
        if (!kernel.has_guest_work())
        {
            return false;
        }
        finished = kernel.run(128u, 64u);
        if (finished.m_stop != iop::e_kernel_stop::budget_exhausted &&
            finished.m_stop != iop::e_kernel_stop::idle)
        {
            return false;
        }
        (void)rpc.pump();
    }
    uint32_t guest_value = 0u;
    loop_thread = kernel.thread(loop_id);
    if ((finished.m_stop != iop::e_kernel_stop::idle &&
         finished.m_stop != iop::e_kernel_stop::budget_exhausted) ||
        rpc.pending_guest_calls() != 0u ||
        loop_thread == nullptr ||
        loop_thread->m_status != iop::e_thread_status::blocked ||
        !kernel.cpu().read32(k_buffer, &guest_value) ||
        guest_value != 42u ||
        memory.read32(k_result) != 42u ||
        (dmac.status() & (1u << 5u)) == 0u ||
        memory.read32(k_receive + 8u) !=
            iop::c_sif_rpc_bridge::k_reply_cid ||
        memory.read32(k_receive + 32u) !=
            iop::c_sif_rpc_bridge::k_call_cid)
    {
        return false;
    }

    // No outstanding requests may remain after executing the handler.
    kernel.cpu().state().m_gpr[4] = k_descriptor;
    if (!commands.handle_syscall(
            kernel, iop::c_sifcmd_bridge::k_service_base + 24u) ||
        kernel.cpu().state().m_gpr[2] != 0u)
    {
        return false;
    }
    kernel.cpu().state().m_gpr[4] = k_queue;
    if (!commands.handle_syscall(
            kernel, iop::c_sifcmd_bridge::k_service_base + 25u) ||
        kernel.cpu().state().m_gpr[2] != 0u ||
        commands.has_guest_rpc_server(k_server))
    {
        return false;
    }
    return true;
}


bool test_iop_to_ee_rpc(const std::filesystem::path& disc_root)
{
    // IOP-originated BIND/CALL must traverse an armed EE SIF0 DMAC,
    // and only a matching EE SIF1 Rend may wake the guest caller.
    const auto fail_stage = [](int line) -> bool
    {
        std::fprintf(stderr,
                     "IOP reverse SIFRPC stage failed at line %d\n",
                     line);
        return false;
    };
    constexpr uint32_t list = 0x10000u;
    constexpr uint32_t packet_source = 0x12000u;
    constexpr uint32_t data_source = 0x14000u;
    constexpr uint32_t ee_receive = 0x00180000u;
    constexpr uint32_t ee_buffer = 0x00190000u;
    constexpr uint32_t client = 0x00028100u;
    constexpr uint32_t iop_payload = 0x00028200u;
    constexpr uint32_t iop_result = 0x00028300u;
    constexpr uint32_t server_id = 0x80005555u;
    constexpr uint32_t ee_server = 0x00188000u;

    ee::ee_memory memory(32u * 1024u * 1024u);
    ee::s_ee_cpu_state ee_cpu{};
    ee::c_ee_sif sif;
    ee::c_ee_dmac dmac;
    sif.reset();
    dmac.bind_cpu(&ee_cpu);
    dmac.bind_memory(&memory);
    dmac.reset();
    iop::c_kernel kernel;
    iop::c_sifcmd_bridge cmd(kernel, sif, &memory);
    iop::c_disc_loader loader(kernel, disc_root);
    iop::c_folder_sector_source sectors(disc_root);
    iop::c_sif_rpc_bridge rpc(
        loader, kernel, cmd, memory, dmac, &sectors);
    iop::c_sif1_bridge sif1(
        memory, kernel.cpu(), dmac, &cmd, &rpc);
    if (!sectors.valid() || !cmd.install())
    {
        return fail_stage(__LINE__);
    }
    kernel.bind_syscall(
        [&](iop::c_kernel& active, uint32_t service)
        {
            return cmd.handle_syscall(active, service);
        });
    if (!init_sifcmd_guest(kernel))
    {
        return fail_stage(__LINE__);
    }

    uint32_t init_stub = 0u;
    uint32_t bind_stub = 0u;
    uint32_t call_stub = 0u;
    for (uint16_t ordinal : {14u, 15u, 16u})
    {
        uint32_t entry = 0u;
        uint32_t instruction = 0u;
        if (!kernel.lookup_library("sifcmd", ordinal, &entry) ||
            !kernel.cpu().read32(entry + 8u, &instruction) ||
            instruction != 0x0000000cu)
        {
            return fail_stage(__LINE__);
        }
        if (ordinal == 14u)
        {
            init_stub = entry;
        }
        if (ordinal == 15u)
        {
            bind_stub = entry;
        }
        if (ordinal == 16u)
        {
            call_stub = entry;
        }
    }
    // Execute sceSifInitRpc through an actual R3000 guest JAL.
    constexpr uint32_t init_pc = 0x00024000u;
    const auto make_call = [&](uint32_t pc, uint32_t dest,
                               bool stack_frame)
    {
        const uint32_t instruction =
            0x0c000000u | ((dest >> 2u) & 0x03ffffffu);
        const uint32_t shift = stack_frame ? 4u : 0u;
        return (!stack_frame ||
                kernel.cpu().write32(pc, 0x27bdffc0u)) &&
            kernel.cpu().write32(pc + shift, instruction) &&
            kernel.cpu().write32(pc + shift + 4u, 0u) &&
            kernel.cpu().write32(
                pc + shift + 8u, 0x0000000du);
    };
    if (!make_call(init_pc, init_stub, false))
    {
        return fail_stage(__LINE__);
    }
    const uint32_t init_thread = kernel.create_thread(
        init_pc, 0u, 1u, 0x400u);
    if (init_thread == 0u || !kernel.start_thread(init_thread) ||
        kernel.run(64u).m_stop != iop::e_kernel_stop::breakpoint)
    {
        return fail_stage(__LINE__);
    }

    // EE publishes a genuine SIF0 receive buffer via SIF1.
    packet(memory, packet_source, {
        24u, 0u, 0x80000002u, 0u, ee_receive, 0u
    });
    descriptor(memory, list, packet_source,
               iop::c_sifcmd_bridge::k_command_buffer, 24u);
    uint32_t dma_id = 0u;
    if (!sif1.set_dma(list, 1u, &dma_id) ||
        cmd.ee_receive_address() != ee_receive)
    {
        return fail_stage(__LINE__);
    }
    dmac.acknowledge(6u);

    // Guest sceSifBindRpc blocks. The unarmed EE SIF0 receiver must
    // not be modified merely because a BIND request was enqueued.
    constexpr uint32_t bind_pc = 0x00024040u;
    if (!make_call(bind_pc, bind_stub, false))
    {
        return fail_stage(__LINE__);
    }
    const uint32_t bind_thread = kernel.create_thread(
        bind_pc, 0u, 1u, 0x400u, client, server_id);
    if (bind_thread == 0u || !kernel.start_thread(bind_thread) ||
        kernel.run(64u).m_stop != iop::e_kernel_stop::idle ||
        kernel.thread(bind_thread)->m_status !=
            iop::e_thread_status::blocked ||
        rpc.pending_replies() != 1u ||
        memory.read32(ee_receive + 8u) != 0u)
    {
        return fail_stage(__LINE__);
    }
    const ee::s_ee_u128 armed{0x184u, 0u};
    if (!ee::c_ee_dmac::mmio_write(
            &dmac, ee::c_ee_dmac::k_sif0_base, 4u, &armed) ||
        !rpc.pump() ||
        memory.read32(ee_receive + 8u) !=
            iop::c_sif_rpc_bridge::k_bind_cid ||
        memory.read32(ee_receive + 32u) != server_id ||
        (dmac.status() & (1u << 5u)) == 0u)
    {
        return fail_stage(__LINE__);
    }

    uint32_t packet_id = 0u;
    if (!kernel.cpu().read32(client + 4u, &packet_id) ||
        packet_id == 0u)
    {
        return fail_stage(__LINE__);
    }
    const std::vector<uint32_t> bind_reply = {
        48u, 0u, iop::c_sif_rpc_bridge::k_reply_cid, 0u,
        packet_id, client, packet_id, client,
        iop::c_sif_rpc_bridge::k_bind_cid,
        ee_server, ee_buffer, 0u
    };
    packet(memory, packet_source, bind_reply);
    memory.write32(packet_source + 16u, packet_id + 1u);
    descriptor(memory, list, packet_source,
               iop::c_sifcmd_bridge::k_command_buffer, 48u);
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    if (sif1.set_dma(list, 1u, &dma_id) ||
        dmac.status() != 0u ||
        kernel.thread(bind_thread)->m_status !=
            iop::e_thread_status::blocked)
    {
        return fail_stage(__LINE__);
    }
    packet(memory, packet_source, bind_reply);
    if (!sif1.set_dma(list, 1u, &dma_id) ||
        kernel.thread(bind_thread)->m_status !=
            iop::e_thread_status::ready ||
        kernel.run(64u).m_stop != iop::e_kernel_stop::breakpoint)
    {
        return fail_stage(__LINE__);
    }
    uint32_t bound_server = 0u;
    uint32_t bound_buffer = 0u;
    uint32_t status = 0xffffffffu;
    if (!kernel.cpu().read32(client + 36u, &bound_server) ||
        !kernel.cpu().read32(client + 20u, &bound_buffer) ||
        !kernel.cpu().read32(client, &status) ||
        bound_server != ee_server || bound_buffer != ee_buffer ||
        status != 0u)
    {
        return fail_stage(__LINE__);
    }

    // Real R3000 sceSifCallRpc, with five stack-passed arguments,
    // copies a guest payload into the EE buffer when SIF0 is armed.
    // Its calling thread remains blocked until a *matched* EE Rend.
    constexpr uint32_t call_pc = 0x00024100u;
    // The caller's fifth and later parameters live at sp+16.
    // Explicitly populate a3 with the IOP source buffer as a
    // guest instruction, rather than modifying a saved CPU snapshot.
    if (!make_call(call_pc, call_stub, true) ||
        !kernel.cpu().write32(call_pc, 0x3c070002u) ||
        !kernel.cpu().write32(call_pc + 4u, 0x34e78200u) ||
        !kernel.cpu().write32(call_pc + 8u, 0x27bdffc0u) ||
        !kernel.cpu().write32(
            call_pc + 12u,
            0x0c000000u | ((call_stub >> 2u) & 0x03ffffffu)) ||
        !kernel.cpu().write32(call_pc + 16u, 0u) ||
        !kernel.cpu().write32(call_pc + 20u, 0x0000000du) ||
        !kernel.cpu().write32(iop_payload, 41u) ||
        !kernel.cpu().write32(iop_result, 0xdeadbeefu))
    {
        return fail_stage(__LINE__);
    }
    // This is EE memory: SIF1 reads its DMA source from the EE,
    // not from the IOP CPU's guest RAM.
    memory.write32(data_source, 42u);
    const uint32_t call_thread = kernel.create_thread(
        call_pc, 0u, 1u, 0x400u, client, 3u);
    const iop::s_kernel_thread* caller = kernel.thread(call_thread);
    if (call_thread == 0u || caller == nullptr)
    {
        return fail_stage(__LINE__);
    }
    const uint32_t frame_sp = caller->m_cpu.m_gpr[29] - 64u;
    if (!kernel.cpu().write32(frame_sp + 16u, 4u) ||
        !kernel.cpu().write32(frame_sp + 20u, iop_result) ||
        !kernel.cpu().write32(frame_sp + 24u, 4u) ||
        !kernel.cpu().write32(frame_sp + 28u, 0u) ||
        !kernel.cpu().write32(frame_sp + 32u, 0u) ||
        !kernel.start_thread(call_thread))
    {
        return fail_stage(__LINE__);
    }
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    if (kernel.run(64u).m_stop != iop::e_kernel_stop::idle ||
        kernel.thread(call_thread)->m_status !=
            iop::e_thread_status::blocked ||
        !kernel.cpu().read32(client, &status) || status != 1u ||
        memory.read32(ee_receive + 8u) !=
            iop::c_sif_rpc_bridge::k_call_cid ||
        memory.read32(ee_receive + 32u) != 3u ||
        memory.read32(ee_buffer) != 41u ||
        (dmac.status() & (1u << 5u)) == 0u)
    {
        return fail_stage(__LINE__);
    }
    uint32_t call_id = 0u;
    if (!kernel.cpu().read32(client + 4u, &call_id) ||
        call_id == packet_id)
    {
        return fail_stage(__LINE__);
    }
    const std::vector<uint32_t> call_reply = {
        48u, 0u, iop::c_sif_rpc_bridge::k_reply_cid, 0u,
        call_id, client, call_id, client,
        iop::c_sif_rpc_bridge::k_call_cid,
        0u, 0u, 0u
    };
    // Atomic staging: malformed Rend must not partially commit the
    // earlier data DMA in its same SIF1 descriptor list.
    packet(memory, packet_source, call_reply);
    memory.write32(packet_source + 16u, call_id + 1u);
    descriptor(memory, list, data_source, iop_result, 16u);
    descriptor(memory, list + 16u, packet_source,
               iop::c_sifcmd_bridge::k_command_buffer, 48u);
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    if (sif1.set_dma(list, 2u, &dma_id) ||
        !kernel.cpu().read32(iop_result, &status) ||
        status != 0xdeadbeefu ||
        kernel.thread(call_thread)->m_status !=
            iop::e_thread_status::blocked ||
        dmac.status() != 0u)
    {
        return fail_stage(__LINE__);
    }
    packet(memory, packet_source, call_reply);
    const bool completed_dma = sif1.set_dma(list, 2u, &dma_id);
    uint32_t copied_result = 0u;
    uint32_t finished_status = 0xffffffffu;
    const bool copied = kernel.cpu().read32(
        iop_result, &copied_result);
    const iop::e_thread_status prior_thread_status =
        kernel.thread(call_thread)->m_status;
    const iop::s_kernel_run_result resumed =
        kernel.run(64u);
    const bool read_status = kernel.cpu().read32(
        client, &finished_status);
    if (!completed_dma || !copied || copied_result != 42u ||
        prior_thread_status != iop::e_thread_status::ready ||
        resumed.m_stop != iop::e_kernel_stop::breakpoint ||
        !read_status || finished_status != 0u)
    {
        std::fprintf(stderr,
            "reverse RPC completion: dma=%u copied=%u value=%u "
            "before=%u stop=%u pc=%08x status_read=%u status=%08x\n",
            completed_dma ? 1u : 0u, copied ? 1u : 0u,
            copied_result,
            static_cast<uint32_t>(prior_thread_status),
            static_cast<uint32_t>(resumed.m_stop),
            resumed.m_guest_pc,
            read_status ? 1u : 0u, finished_status);
        return fail_stage(__LINE__);
    }
    // A previously completed Rend may not be replayed as a second
    // callback or a new IOP DMA completion.
    dmac.acknowledge(6u);
    if (sif1.set_dma(list + 16u, 1u, &dma_id) ||
        (dmac.status() & (1u << 6u)) != 0u)
    {
        return fail_stage(__LINE__);
    }
    return true;
}

bool test_guest_sifcmd_send(const std::filesystem::path& disc_root)
{
    const auto fail_at = [](int line) -> bool
    {
        std::fprintf(stderr,
                     "IOP guest SIFCMD send failed at line %d\n", line);
        return false;
    };
    // The original R3000 ABI puts extra destination/size in the
    // caller's stack. No command may appear on the EE until its
    // SIF0 receiver is actually armed.
    ee::ee_memory memory(32u * 1024u * 1024u);
    ee::s_ee_cpu_state ee_cpu{};
    ee::c_ee_sif sif;
    ee::c_ee_dmac dmac;
    sif.reset();
    dmac.bind_cpu(&ee_cpu);
    dmac.bind_memory(&memory);
    dmac.reset();

    iop::c_kernel kernel;
    iop::c_sifcmd_bridge cmd(kernel, sif, &memory);
    iop::c_disc_loader loader(kernel, disc_root);
    iop::c_folder_sector_source sectors(disc_root);
    iop::c_sif_rpc_bridge rpc(
        loader, kernel, cmd, memory, dmac, &sectors);
    iop::c_sif1_bridge sif1(
        memory, kernel.cpu(), dmac, &cmd, &rpc);
    if (!sectors.valid() || !cmd.install() ||
        !cmd.initialize_hle_bootstrap())
    {
        return fail_at(__LINE__);
    }
    kernel.bind_syscall([&](iop::c_kernel& active, uint32_t service)
    {
        return cmd.handle_syscall(active, service);
    });
    constexpr uint32_t k_list = 0x10000u;
    constexpr uint32_t k_handshake = 0x12000u;
    constexpr uint32_t k_receive = 0x00180000u;
    constexpr uint32_t k_extra_destination = 0x00190000u;
    constexpr uint32_t k_packet = 0x27000u;
    constexpr uint32_t k_extra = 0x28000u;
    constexpr uint32_t k_program = 0x2a000u;
    packet(memory, k_handshake,
           {20u, 0u, 0x80000002u, 0u, k_receive});
    descriptor(memory, k_list, k_handshake,
               iop::c_sifcmd_bridge::k_command_buffer, 20u);
    uint32_t dma_id = 0u;
    if (!sif1.set_dma(k_list, 1u, &dma_id) ||
        cmd.ee_receive_address() != k_receive)
    {
        return fail_at(__LINE__);
    }
    dmac.acknowledge(6u);

    const uint32_t original[] = {
        0xaaaaaaaau, 0x12345678u, 0x12345678u,
        0u, 0x11223344u, 0x55667788u
    };
    for (uint32_t i = 0u; i < 6u; ++i)
    {
        if (!kernel.cpu().write32(k_packet + i * 4u, original[i]))
        {
            return fail_at(__LINE__);
        }
    }
    if (!kernel.cpu().write32(k_extra, 0xdeadbeefu))
    {
        return fail_at(__LINE__);
    }
    for (uint32_t ordinal : {12u, 28u})
    {
        const uint32_t guest_program = k_program + ordinal * 0x40u;
        uint32_t stub = 0u;
        if (!kernel.lookup_library(
                "sifcmd", static_cast<uint16_t>(ordinal), &stub))
        {
            return fail_at(__LINE__);
        }
        const uint32_t instructions[] = {
            0x27bdffc0u, // addiu sp,sp,-64: own an O32 call frame
            0x34060018u, // ori a2, zero, 24-byte packet
            0x3c070002u, // lui a3, 2
            0x34e78000u, // ori a3, a3, 0x8000
            0x0c000000u | ((stub >> 2u) & 0x03ffffffu),
            0u,          // delay slot
            0x0000000du // break after returning from the import
        };
        for (uint32_t i = 0u; i < 7u; ++i)
        {
            if (!kernel.cpu().write32(
                    guest_program + 4u * i, instructions[i]))
            {
                return fail_at(__LINE__);
            }
        }
        const uint32_t id = kernel.create_thread(
            guest_program, 0u, 1u, 0x400u,
            0x80000200u + ordinal, k_packet);
        if (id == 0u)
        {
            return fail_at(__LINE__);
        }
        const iop::s_kernel_thread* thread = kernel.thread(id);
        // The guest prologue subtracts 64 before passing stack args.
        // Writing at initial sp+16 would be past the owned stack's
        // upper boundary when high-RAM stacks reach 0x00200000.
        const uint32_t sp = thread->m_cpu.m_gpr[29] - 64u;
        if (sp < thread->m_stack_base ||
            !kernel.cpu().write32(sp + 16u, k_extra_destination) ||
            !kernel.cpu().write32(sp + 20u, 4u) ||
            !kernel.start_thread(id))
        {
            return fail_at(__LINE__);
        }
        const iop::s_kernel_run_result ran = kernel.run(64u);
        thread = kernel.thread(id);
        if (ran.m_stop != iop::e_kernel_stop::breakpoint ||
            ran.m_guest_pc != guest_program + 24u ||
            thread == nullptr || thread->m_cpu.m_gpr[2] != 1u ||
            rpc.pending_replies() != 1u ||
            memory.read32(k_receive + 8u) ==
                (0x80000200u + ordinal) ||
            memory.read32(k_extra_destination) == 0xdeadbeefu)
        {
            std::fprintf(stderr,
                "SIFCMD send #%u: stop=%u pc=%08x expected=%08x v0=%08x "
                "queued=%zu cid=%08x extra=%08x\n",
                ordinal, static_cast<uint32_t>(ran.m_stop),
                ran.m_guest_pc, guest_program + 24u,
                thread != nullptr ? thread->m_cpu.m_gpr[2] : 0xffffffffu,
                rpc.pending_replies(),
                memory.read32(k_receive + 8u),
                memory.read32(k_extra_destination));
            return fail_at(__LINE__);
        }
        // The DMA commits only after the EE explicitly arms SIF0.
        const ee::s_ee_u128 armed{0x184u, 0u};
        if (!ee::c_ee_dmac::mmio_write(
                &dmac, ee::c_ee_dmac::k_sif0_base,
                4u, &armed) ||
            !rpc.pump() || rpc.pending_replies() != 0u ||
            memory.read32(k_receive) != (24u | (4u << 8u)) ||
            memory.read32(k_receive + 4u) != k_extra_destination ||
            memory.read32(k_receive + 8u) !=
                (0x80000200u + ordinal) ||
            memory.read32(k_receive + 16u) != 0x11223344u ||
            memory.read32(k_extra_destination) != 0xdeadbeefu ||
            (dmac.status() & (1u << 5u)) == 0u)
        {
            return fail_at(__LINE__);
        }
        dmac.acknowledge(5u);
        // Acknowledging the previous SIF0 IRQ alone does not clear
        // the receiver's CHCR. Explicitly disarm it so the next
        // guest command is verified queued, not auto-delivered into
        // an intentionally still-armed EE destination.
        const ee::s_ee_u128 disarmed{0x84u, 0u};
        if (!ee::c_ee_dmac::mmio_write(
                &dmac, ee::c_ee_dmac::k_sif0_base,
                4u, &disarmed))
        {
            return fail_at(__LINE__);
        }
        memory.write32(k_receive + 8u, 0u);
        memory.write32(k_extra_destination, 0u);
    }
    // Malformed guest command payloads must not enqueue, alter the EE
    // receiver or report a successful SIF packet.
    kernel.cpu().state().m_gpr[4] = 0x80000901u;
    kernel.cpu().state().m_gpr[5] = 0x001ffffcu;
    kernel.cpu().state().m_gpr[6] = 24u;
    kernel.cpu().state().m_gpr[7] = k_extra;
    if (!cmd.handle_syscall(
            kernel, iop::c_sifcmd_bridge::k_service_base + 12u) ||
        kernel.cpu().state().m_gpr[2] != 0u ||
        rpc.pending_replies() != 0u)
    {
        return fail_at(__LINE__);
    }
    return true;
}

} // namespace

int main()
{
    constexpr uint32_t list = 0x10000u;
    constexpr uint32_t bind_source = 0x12000u;
    constexpr uint32_t call_source = 0x13000u;
    constexpr uint32_t args_source = 0x14000u;
    constexpr uint32_t ee_receive = 0x00180000u;
    constexpr uint32_t rpc_result = 0x00190000u;

    s_temp_dir folder{
        std::filesystem::temp_directory_path() /
        ("sif-rpc-" + std::to_string(
            std::chrono::steady_clock::now()
                .time_since_epoch().count()))
    };
    std::filesystem::create_directories(folder.m_path);
    if (!create_irx(folder.m_path / "PADMAN.IRX"))
    {
        return fail("cannot write synthetic disc module");
    }
    {
        std::vector<uint8_t> image(0x100u, 0u);
        const std::string marker = "PsIIloadfile3040";
        std::copy(marker.begin(), marker.end(), image.begin() + 0x40u);
        std::ofstream out(folder.m_path / "IOPRP300.IMG", std::ios::binary);
        out.write(reinterpret_cast<const char*>(image.data()),
                  static_cast<std::streamsize>(image.size()));
        if (!out.good())
        {
            return fail("cannot create reboot image metadata fixture");
        }
    }

    ee::ee_memory memory(32u * 1024u * 1024u);
    ee::s_ee_cpu_state ee_cpu{};
    ee::c_ee_sif sif;
    ee::c_ee_dmac dmac;
    sif.reset();
    dmac.bind_cpu(&ee_cpu);
    dmac.bind_memory(&memory);
    dmac.reset();

    iop::c_kernel kernel;
    iop::c_sifcmd_bridge commands(kernel, sif);
    iop::c_disc_loader loader(kernel, folder.m_path);
    iop::c_folder_sector_source sectors(folder.m_path);
    if (!sectors.valid())
    {
        return fail("synthetic extracted disc could not mount");
    }
    iop::c_sif_rpc_bridge rpc(
        loader, kernel, commands, memory, dmac, &sectors
    );
    iop::c_sif1_bridge sif1(
        memory, kernel.cpu(), dmac, &commands, &rpc
    );

    if (!commands.install())
    {
        return fail("SIFCMD import registration failed");
    }
    kernel.bind_syscall(
        [&](iop::c_kernel& active, uint32_t syscall)
        {
            return commands.handle_syscall(active, syscall);
        }
    );

    // An installed HLE bridge and a static on-disc IRX are not
    // scheduled guest work and must not advance the IOP clock.
    if (kernel.has_guest_work())
    {
        return fail("IOP scheduler reported work with no runnable module");
    }
    // The EE cannot dispatch any LOADFILE command merely because
    // a module exists on the host disc.
    const std::vector<uint32_t> bind = {
        36u, 0u, iop::c_sif_rpc_bridge::k_bind_cid, 0u,
        1u, 0x11000u, 0x1234u, 0x15000u,
        iop::c_sif_rpc_bridge::k_loadfile_server,
        0u, 0u, 0u
    };
    packet(memory, bind_source, bind);
    descriptor(
        memory, list, bind_source,
        iop::c_sifcmd_bridge::k_command_buffer, 36u
    );
    uint32_t dma_id = 0u;
    if (sif1.set_dma(list, 1u, &dma_id) ||
        rpc.pending_replies() != 0u ||
        rpc.load_requests() != 0u)
    {
        return fail("RPC accepted before guest IOP init");
    }

    if (!init_sifcmd_guest(kernel) || !commands.initialized())
    {
        return fail("R3000 sceSifInitCmd not executed");
    }

    // Guest EE publishes an actual command receive buffer via SIF1.
    packet(memory, bind_source, {
        24u, 0u, 0x80000002u, 0u, ee_receive, 0u, 0u, 0u
    });
    descriptor(
        memory, list, bind_source,
        iop::c_sifcmd_bridge::k_command_buffer, 24u
    );
    if (!sif1.set_dma(list, 1u, &dma_id) ||
        commands.ee_receive_address() != ee_receive)
    {
        return fail("SIFCMD EE receive-address handshake failed");
    }

    packet(memory, bind_source, bind);
    descriptor(
        memory, list, bind_source,
        iop::c_sifcmd_bridge::k_command_buffer, 36u
    );
    dmac.acknowledge(6u);
    if (!sif1.set_dma(list, 1u, &dma_id) ||
        rpc.pending_replies() != 1u ||
        (dmac.status() & (1u << 5u)) != 0u)
    {
        return fail("RPC bind should queue until EE SIF0 is armed");
    }

    const ee::s_ee_u128 armed{0x184u, 0u};
    if (!ee::c_ee_dmac::mmio_write(
            &dmac, ee::c_ee_dmac::k_sif0_base,
            4u, &armed) ||
        !rpc.pump() || rpc.pending_replies() != 0u ||
        (dmac.status() & (1u << 5u)) == 0u ||
        memory.read32(ee_receive + 8u) !=
            iop::c_sif_rpc_bridge::k_reply_cid ||
        memory.read32(ee_receive + 32u) !=
            iop::c_sif_rpc_bridge::k_bind_cid ||
        memory.read32(ee_receive + 36u) !=
            (iop::c_sif_rpc_bridge::k_loadfile_server ^
             iop::c_sif_rpc_bridge::k_server_xor) ||
        memory.read32(ee_receive + 40u) !=
            iop::c_sif_rpc_bridge::k_rpc_data_destination)
    {
        return fail("LOADFILE bind reply did not traverse armed SIF0");
    }

    // EE SDK sifrpc also uses a full 64-byte bind command, with
    // identity fields at identical offsets and a terminal 0x44 DMA.
    // Only a registered LOADFILE HLE service may answer this bind.
    const std::vector<uint32_t> bind64 = {
        64u, 0u, iop::c_sif_rpc_bridge::k_bind_cid, 0u,
        5u, 0x11040u, 0x1234u, 0x15000u,
        iop::c_sif_rpc_bridge::k_loadfile_server,
        0u, 0u, 0u, 0u, 0u, 0u, 0u
    };
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    packet(memory, bind_source, bind64);
    descriptor(
        memory, list, bind_source,
        iop::c_sifcmd_bridge::k_command_buffer, 64u
    );
    memory.write32(list + 12u, 0x44u);
    if (!sif1.set_dma(list, 1u, &dma_id) ||
        rpc.pending_replies() != 0u ||
        rpc.load_requests() != 0u ||
        memory.read32(ee_receive + 0u) != 48u ||
        memory.read32(ee_receive + 8u) !=
            iop::c_sif_rpc_bridge::k_reply_cid ||
        memory.read32(ee_receive + 16u) != 5u ||
        memory.read32(ee_receive + 20u) != 0x11040u ||
        memory.read32(ee_receive + 32u) !=
            iop::c_sif_rpc_bridge::k_bind_cid ||
        memory.read32(ee_receive + 36u) !=
            (iop::c_sif_rpc_bridge::k_loadfile_server ^
             iop::c_sif_rpc_bridge::k_server_xor) ||
        (dmac.status() & (1u << 5u)) == 0u)
    {
        return fail("64-byte LOADFILE bind did not reply through SIF0");
    }

    // A syntactically valid 64-byte bind is not proof that an
    // arbitrary RPC server exists. Verify transactional rejection
    // and preserve the last completed EE receive packet.
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    memory.write32(bind_source + 32u, 0x80000004u);
    iop::s_sif1_dma_diagnostic bind_rejection{};
    if (sif1.set_dma(list, 1u, &dma_id, &bind_rejection) ||
        bind_rejection.m_failure !=
            iop::e_sif1_dma_failure::unsupported_rpc_packet ||
        bind_rejection.m_rpc_server != 0x80000004u ||
        bind_rejection.m_rpc_client != 0x15000u ||
        memory.read32(ee_receive + 16u) != 5u ||
        rpc.pending_replies() != 0u ||
        (dmac.status() & ((1u << 5u) | (1u << 6u))) != 0u)
    {
        return fail("unknown 64-byte RPC bind mutated guest state");
    }

    // The next retail SDBZ bind targets CDVDFSV CdInit (0x80000592).
    // It uses the ordinary 64-byte SDK packet and terminal 0x44 DMA.
    // Only the registered HLE service gets an actual SIF0 reply.
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    memory.write32(bind_source + 28u, 0x005688a8u);
    memory.write32(bind_source + 32u,
                   iop::c_sif_rpc_bridge::k_cdvd_init_server);
    if (!sif1.set_dma(list, 1u, &dma_id) ||
        memory.read32(ee_receive + 0u) != 48u ||
        memory.read32(ee_receive + 8u) !=
            iop::c_sif_rpc_bridge::k_reply_cid ||
        memory.read32(ee_receive + 28u) != 0x005688a8u ||
        memory.read32(ee_receive + 32u) !=
            iop::c_sif_rpc_bridge::k_bind_cid ||
        memory.read32(ee_receive + 36u) !=
            (iop::c_sif_rpc_bridge::k_cdvd_init_server ^
             iop::c_sif_rpc_bridge::k_server_xor) ||
        memory.read32(ee_receive + 40u) !=
            iop::c_sif_rpc_bridge::k_rpc_data_destination ||
        rpc.pending_replies() != 0u ||
        rpc.load_requests() != 0u ||
        kernel.module(1u) != nullptr ||
        (dmac.status() & (1u << 5u)) == 0u)
    {
        return fail("64-byte CDVD Init BIND did not reply through SIF0");
    }

    // An unregistered CDVDFSV server remains unsupported. A valid
    // identity alone must not synthesize another server or interrupt.
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    memory.write32(bind_source + 32u, 0x80000594u);
    iop::s_sif1_dma_diagnostic cdvd_rejection{};
    if (sif1.set_dma(list, 1u, &dma_id, &cdvd_rejection) ||
        cdvd_rejection.m_failure !=
            iop::e_sif1_dma_failure::unsupported_rpc_packet ||
        cdvd_rejection.m_rpc_server != 0x80000594u ||
        cdvd_rejection.m_rpc_client != 0x005688a8u ||
        memory.read32(ee_receive + 36u) !=
            (iop::c_sif_rpc_bridge::k_cdvd_init_server ^
             iop::c_sif_rpc_bridge::k_server_xor) ||
        rpc.pending_replies() != 0u ||
        (dmac.status() & ((1u << 5u) | (1u << 6u))) != 0u)
    {
        return fail("unsupported CDVDFSV bind fabricated a response");
    }
    memory.write32(bind_source + 28u, 0x15000u);

    // These fields reproduce the 2026-11-18 12:14 retail SDBZ
    // SIF1 RPC header. Word[4] is record ID 5; word[8] is method 0.
    // The nonzero DEADBEF0 header destination is a valid IOP RPC
    // data address, not evidence of an unsupported packet.
    // Only result[3] changes, as in ps2-web-recomp's CDVDFSV HLE.
    const std::vector<uint32_t> cdvd_call = {
        0x440u, iop::c_sif_rpc_bridge::k_rpc_data_destination,
        iop::c_sif_rpc_bridge::k_call_cid, 0u,
        5u, 0x20561900u, 4u, 0x005688a8u,
        0u, 4u, rpc_result, 16u, 1u,
        iop::c_sif_rpc_bridge::k_cdvd_init_server ^
            iop::c_sif_rpc_bridge::k_server_xor,
        0u, 0u
    };
    packet(memory, call_source, cdvd_call);
    packet(memory, args_source, {0u, 0u, 0u, 0u});
    packet(memory, rpc_result, {
        0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u
    });
    descriptor(memory, list, args_source,
               iop::c_sif_rpc_bridge::k_rpc_data_destination, 4u);
    descriptor(memory, list + 16u, call_source,
               iop::c_sifcmd_bridge::k_command_buffer, 64u);
    memory.write32(list + 28u, 0x44u);
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    if (!sif1.set_dma(list, 2u, &dma_id) ||
        memory.read32(rpc_result) != 0x11111111u ||
        memory.read32(rpc_result + 4u) != 0x22222222u ||
        memory.read32(rpc_result + 8u) != 0x33333333u ||
        memory.read32(rpc_result + 12u) != 0xffu ||
        memory.read32(ee_receive + 16u) != 5u ||
        memory.read32(ee_receive + 20u) != 0x20561900u ||
        memory.read32(ee_receive + 24u) != 4u ||
        memory.read32(ee_receive + 28u) != 0x005688a8u ||
        memory.read32(ee_receive + 32u) !=
            iop::c_sif_rpc_bridge::k_call_cid ||
        rpc.pending_replies() != 0u ||
        rpc.load_requests() != 0u ||
        kernel.module(1u) != nullptr ||
        (dmac.status() & (1u << 5u)) == 0u)
    {
        return fail("retail CDVD Init method 0 did not return through SIF0");
    }

    // A nonzero arbitrary header destination is not an accepted RPC
    // data destination, even though DEADBEF0 is valid.
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    memory.write32(call_source + 4u, 0xdeadbe00u);
    iop::s_sif1_dma_diagnostic invalid_rpc_destination{};
    if (sif1.set_dma(
            list, 2u, &dma_id, &invalid_rpc_destination) ||
        invalid_rpc_destination.m_failure !=
            iop::e_sif1_dma_failure::unsupported_rpc_packet ||
        invalid_rpc_destination.m_rpc_server !=
            iop::c_sif_rpc_bridge::k_cdvd_init_server ||
        rpc.pending_replies() != 0u ||
        memory.read32(rpc_result + 12u) != 0xffu ||
        (dmac.status() & ((1u << 5u) | (1u << 6u))) != 0u)
    {
        return fail("invalid RPC header destination committed DMA");
    }
    memory.write32(
        call_source + 4u,
        iop::c_sif_rpc_bridge::k_rpc_data_destination
    );

    // An unmodeled CdInit method must not write a success result,
    // emit a reply, or acknowledge a DMA that did not complete.
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    memory.write32(call_source + 32u, 6u);
    iop::s_sif1_dma_diagnostic cdvd_call_rejection{};
    if (sif1.set_dma(list, 2u, &dma_id, &cdvd_call_rejection) ||
        cdvd_call_rejection.m_failure !=
            iop::e_sif1_dma_failure::unsupported_rpc_packet ||
        cdvd_call_rejection.m_rpc_server !=
            iop::c_sif_rpc_bridge::k_cdvd_init_server ||
        cdvd_call_rejection.m_rpc_client != 0x005688a8u ||
        cdvd_call_rejection.m_rpc_packet_word_count != 16u ||
        cdvd_call_rejection.m_rpc_packet_words[0] != 0x440u ||
        cdvd_call_rejection.m_rpc_packet_words[8] != 6u ||
        cdvd_call_rejection.m_rpc_packet_words[13] !=
            (iop::c_sif_rpc_bridge::k_cdvd_init_server ^
             iop::c_sif_rpc_bridge::k_server_xor) ||
        cdvd_call_rejection.m_rpc_payload_source != args_source ||
        cdvd_call_rejection.m_rpc_payload_length != 4u ||
        memory.read32(rpc_result + 12u) != 0xffu ||
        rpc.pending_replies() != 0u ||
        (dmac.status() & ((1u << 5u) | (1u << 6u))) != 0u)
    {
        return fail("unsupported CDVD Init call fabricated completion");
    }
    // CDVDFSV CdInit must complete with a short receive buffer,
    // matching the reference HLE, without writing beyond its length.
    // The result[3] field is only written for a sixteen-byte reply.
    memory.write32(call_source + 32u, 0u);
    memory.write32(call_source + 44u, 12u);
    memory.write32(rpc_result + 12u, 0xa55a5aa5u);
    if (!sif1.set_dma(list, 2u, &dma_id, &cdvd_call_rejection) ||
        cdvd_call_rejection.m_failure !=
            iop::e_sif1_dma_failure::none ||
        memory.read32(rpc_result + 12u) != 0xa55a5aa5u ||
        memory.read32(ee_receive + 32u) !=
            iop::c_sif_rpc_bridge::k_call_cid ||
        rpc.pending_replies() != 0u ||
        (dmac.status() & (1u << 5u)) == 0u)
    {
        return fail("CDVD Init short reply did not complete safely");
    }
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);

    // The real SIF packet's advertised send size cannot exceed the
    // staged DMA payload. Preserve its complete header on failure.
    memory.write32(call_source + 44u, 16u);
    memory.write32(call_source + 36u, 20u);
    if (sif1.set_dma(list, 2u, &dma_id, &cdvd_call_rejection) ||
        cdvd_call_rejection.m_failure !=
            iop::e_sif1_dma_failure::unsupported_rpc_packet ||
        cdvd_call_rejection.m_rpc_packet_words[9] != 20u ||
        cdvd_call_rejection.m_rpc_packet_words[11] != 16u ||
        cdvd_call_rejection.m_rpc_payload_length != 4u ||
        memory.read32(rpc_result + 12u) != 0xa55a5aa5u ||
        rpc.pending_replies() != 0u ||
        (dmac.status() & ((1u << 5u) | (1u << 6u))) != 0u)
    {
        return fail("CDVD Init payload length violation committed DMA");
    }
    memory.write32(call_source + 36u, 4u);

    // Captured 13:05 retail LOADFILE version query: one 64-byte
    // terminal SIF1 descriptor; no payload, four-byte receive result.
    const std::vector<uint32_t> version_call = {
        64u, 0u, iop::c_sif_rpc_bridge::k_call_cid, 0u,
        5u, 0x20561900u, 8u, 0x00564d40u,
        0xffu, 0u, rpc_result, 4u, 1u,
        iop::c_sif_rpc_bridge::k_loadfile_server ^
            iop::c_sif_rpc_bridge::k_server_xor,
        0u, 0u
    };
    packet(memory, call_source, version_call);
    descriptor(memory, list, call_source,
               iop::c_sifcmd_bridge::k_command_buffer, 64u);
    memory.write32(list + 12u, 0x44u);
    memory.write32(rpc_result, 0xa5a5a5a5u);
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    if (sif1.set_dma(list, 1u, &dma_id) ||
        memory.read32(rpc_result) != 0xa5a5a5a5u ||
        rpc.pending_replies() != 0u ||
        (dmac.status() & ((1u << 5u) | (1u << 6u))) != 0u)
    {
        return fail("version RPC fabricated metadata before reboot");
    }
    if (!rpc.configure_reboot_image_version(
            "rom0:UDNL cdrom0:\\IOPRP300.IMG;1"))
    {
        return fail("cannot read actual IOPRP LOADFILE version");
    }
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    if (!sif1.set_dma(list, 1u, &dma_id) ||
        rpc.last_observation().m_cid !=
            iop::c_sif_rpc_bridge::k_call_cid ||
        rpc.last_observation().m_server_id !=
            iop::c_sif_rpc_bridge::k_loadfile_server ||
        rpc.last_observation().m_method != 0xffu ||
        rpc.last_observation().m_send_size != 0u ||
        rpc.last_observation().m_recv_size != 4u ||
        rpc.last_observation().m_return_word != 0x30343033u ||
        rpc.last_observation().m_module_started ||
        memory.read32(rpc_result) != 0x30343033u ||
        memory.read32(ee_receive + 16u) != 5u ||
        memory.read32(ee_receive + 28u) != 0x00564d40u ||
        memory.read32(ee_receive + 32u) !=
            iop::c_sif_rpc_bridge::k_call_cid ||
        rpc.pending_replies() != 0u ||
        rpc.load_requests() != 0u ||
        (dmac.status() & (1u << 5u)) == 0u)
    {
        return fail("LOADFILE version query did not complete via SIF0");
    }
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    memory.write32(call_source + 36u, 4u);
    if (sif1.set_dma(list, 1u, &dma_id) ||
        rpc.pending_replies() != 0u ||
        (dmac.status() & ((1u << 5u) | (1u << 6u))) != 0u)
    {
        return fail("version RPC accepted unexpected send payload");
    }
    memory.write32(call_source + 36u, 0u);

    // Restore the single-command descriptor used by the next BIND.
    // The CdInit CALL above used a two-descriptor payload+command DMA.
    descriptor(memory, list, bind_source,
               iop::c_sifcmd_bridge::k_command_buffer, 64u);
    memory.write32(list + 12u, 0x44u);

    // Bind the IOPHEAP server observed in the retail SDBZ bootstrap.
    // The EE must see the actual server identity in the SIF0 reply.
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    memory.write32(bind_source + 32u,
                   iop::c_sif_rpc_bridge::k_iop_heap_server);
    if (!sif1.set_dma(list, 1u, &dma_id) ||
        memory.read32(ee_receive + 0u) != 48u ||
        memory.read32(ee_receive + 8u) !=
            iop::c_sif_rpc_bridge::k_reply_cid ||
        memory.read32(ee_receive + 32u) !=
            iop::c_sif_rpc_bridge::k_bind_cid ||
        memory.read32(ee_receive + 36u) !=
            (iop::c_sif_rpc_bridge::k_iop_heap_server ^
             iop::c_sif_rpc_bridge::k_server_xor) ||
        memory.read32(ee_receive + 40u) !=
            iop::c_sif_rpc_bridge::k_rpc_data_destination ||
        rpc.load_requests() != 0u ||
        (dmac.status() & (1u << 5u)) == 0u)
    {
        return fail("64-byte IOPHEAP BIND did not reply through SIF0");
    }

    // IOPHEAP alloc/free must use the same allocator as linked IOP
    // modules and guest thread stacks, and cannot free those resources.
    const std::vector<uint32_t> heap_call = {
        56u, 0u, iop::c_sif_rpc_bridge::k_call_cid, 0u,
        6u, 0x11060u, 0x1234u, 0x15000u,
        1u, 16u, rpc_result, 4u, 1u,
        iop::c_sif_rpc_bridge::k_iop_heap_server ^
            iop::c_sif_rpc_bridge::k_server_xor
    };
    packet(memory, call_source, heap_call);
    packet(memory, args_source, {0x100u, 0u, 0u, 0u});
    descriptor(memory, list, args_source,
               iop::c_sif_rpc_bridge::k_rpc_data_destination, 16u);
    descriptor(memory, list + 16u, call_source,
               iop::c_sifcmd_bridge::k_command_buffer, 56u);
    memory.write32(list + 28u, 0x44u);
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    if (!sif1.set_dma(list, 2u, &dma_id) ||
        memory.read32(rpc_result) < iop::c_kernel::k_user_base ||
        memory.read32(rpc_result) >= iop::c_cpu::k_ram_size ||
        (memory.read32(rpc_result) & 0x0fu) != 0u ||
        memory.read32(ee_receive + 32u) !=
            iop::c_sif_rpc_bridge::k_call_cid ||
        (dmac.status() & (1u << 5u)) == 0u ||
        rpc.load_requests() != 0u)
    {
        return fail("IOPHEAP method 1 did not allocate IOP RAM");
    }
    const uint32_t heap_address = memory.read32(rpc_result);

    // Method 2 can only free a previously issued IOPHEAP block.
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    memory.write32(call_source + 32u, 2u);
    memory.write32(args_source, heap_address);
    if (!sif1.set_dma(list, 2u, &dma_id) ||
        memory.read32(rpc_result) != 0u ||
        memory.read32(ee_receive + 32u) !=
            iop::c_sif_rpc_bridge::k_call_cid)
    {
        return fail("IOPHEAP method 2 could not free its own block");
    }

    // IOPHEAP method 3 transfers the mounted file into an owned
    // IOPHEAP allocation; no arbitrary IOP RAM write is permitted.
    const uint32_t heap_file_buffer = kernel.allocate_heap(0x400u);
    if (heap_file_buffer == 0u)
    {
        return fail("IOPHEAP file copy destination allocation failed");
    }
    const std::string heap_file_path = "cdrom0:\\PADMAN.IRX;1";
    memory.write32(call_source + 32u, 3u);
    memory.write32(call_source + 36u, 48u);
    memory.write32(args_source, heap_file_buffer);
    for (size_t index = 0u; index <= heap_file_path.size(); ++index)
    {
        memory.write8(args_source + 4u + static_cast<uint32_t>(index),
                      index == heap_file_path.size() ? 0u :
                          static_cast<uint8_t>(heap_file_path[index]));
    }
    descriptor(memory, list, args_source,
               iop::c_sif_rpc_bridge::k_rpc_data_destination, 48u);
    memory.write32(list + 28u, 0x44u);
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    uint32_t heap_file_magic = 0u;
    if (!sif1.set_dma(list, 2u, &dma_id) ||
        memory.read32(rpc_result) != 0u ||
        !kernel.cpu().read32(heap_file_buffer, &heap_file_magic) ||
        heap_file_magic != 0x464c457fu ||
        (dmac.status() & (1u << 5u)) == 0u)
    {
        return fail("IOPHEAP file-to-RAM did not copy real mounted bytes");
    }

    // The same file must not escape its allocated destination.
    memory.write32(args_source, heap_file_buffer + 0x300u);
    memory.write32(rpc_result, 0xccccccccu);
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    iop::s_sif1_dma_diagnostic heap_rejection{};
    if (sif1.set_dma(list, 2u, &dma_id, &heap_rejection) ||
        heap_rejection.m_failure !=
            iop::e_sif1_dma_failure::unsupported_rpc_packet ||
        memory.read32(rpc_result) != 0xccccccccu ||
        rpc.pending_replies() != 0u ||
        (dmac.status() & ((1u << 5u) | (1u << 6u))) != 0u)
    {
        return fail("IOPHEAP out-of-allocation transfer mutated guest RAM");
    }

    const std::string absent_path = "cdrom0:\\NOFILE.IRX;1";
    memory.write32(args_source, heap_file_buffer);
    for (size_t index = 0u; index <= absent_path.size(); ++index)
    {
        memory.write8(args_source + 4u + static_cast<uint32_t>(index),
                      index == absent_path.size() ? 0u :
                          static_cast<uint8_t>(absent_path[index]));
    }
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    if (!sif1.set_dma(list, 2u, &dma_id) ||
        memory.read32(rpc_result) != 0xfffffffeu ||
        !kernel.cpu().read32(heap_file_buffer, &heap_file_magic) ||
        heap_file_magic != 0x464c457fu)
    {
        return fail("IOPHEAP missing file falsely reported success");
    }

    memory.write32(call_source + 36u, 8u);
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    if (sif1.set_dma(list, 2u, &dma_id, &heap_rejection) ||
        heap_rejection.m_failure !=
            iop::e_sif1_dma_failure::unsupported_rpc_packet ||
        rpc.pending_replies() != 0u)
    {
        return fail("IOPHEAP truncated pathname was accepted");
    }
    memory.write32(call_source + 32u, 9u);
    memory.write32(call_source + 36u, 16u);
    descriptor(memory, list, args_source,
               iop::c_sif_rpc_bridge::k_rpc_data_destination, 16u);
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    if (sif1.set_dma(list, 2u, &dma_id, &heap_rejection) ||
        rpc.pending_replies() != 0u)
    {
        return fail("unknown IOPHEAP method was accepted");
    }
    memory.write32(call_source + 32u, 5u);

    // IOPHEAP query methods expose actual IOP memory capacity.
    memory.write32(call_source + 32u, 5u);
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    if (!sif1.set_dma(list, 2u, &dma_id) ||
        memory.read32(rpc_result) != iop::c_cpu::k_ram_size)
    {
        return fail("IOPHEAP method 5 reported incorrect RAM size");
    }
    memory.write32(call_source + 32u, 6u);
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    if (!sif1.set_dma(list, 2u, &dma_id) ||
        memory.read32(rpc_result) == 0u ||
        memory.read32(rpc_result) > iop::c_cpu::k_ram_size)
    {
        return fail("IOPHEAP method 6 reported invalid free maximum");
    }

    const std::string guest_path = "cdrom0:\\PADMAN.IRX;1";
    memory.write32(args_source, 0u);
    memory.write32(args_source + 4u, 0u);
    for (size_t i = 0u; i <= guest_path.size(); ++i)
    {
        memory.write8(
            args_source + 8u + static_cast<uint32_t>(i),
            i == guest_path.size() ? 0u :
                static_cast<uint8_t>(guest_path[i])
        );
    }

    const std::vector<uint32_t> call = {
        56u, 0u, iop::c_sif_rpc_bridge::k_call_cid, 0u,
        2u, 0x11020u, 0x1234u, 0x15000u,
        0u, 260u, rpc_result, 8u, 1u,
        iop::c_sif_rpc_bridge::k_loadfile_server ^
            iop::c_sif_rpc_bridge::k_server_xor,
        0u, 0u
    };
    packet(memory, call_source, call);
    descriptor(
        memory, list, call_source,
        iop::c_sifcmd_bridge::k_command_buffer, 56u
    );
    if (sif1.set_dma(list, 1u, &dma_id) ||
        rpc.load_requests() != 0u)
    {
        return fail("LOADFILE call without parameter DMA accepted");
    }

    dmac.acknowledge(5u);
    descriptor(
        memory, list, args_source,
        iop::c_sif_rpc_bridge::k_rpc_data_destination, 260u
    );
    descriptor(
        memory, list + 16u, call_source,
        iop::c_sifcmd_bridge::k_command_buffer, 56u
    );
    memory.write32(rpc_result, 0xcafebabeu);
    memory.write32(rpc_result + 4u, 0xcafebabeu);
    if (!sif1.set_dma(list, 2u, &dma_id) ||
        rpc.last_observation().m_method != 0u ||
        rpc.last_observation().m_server_id !=
            iop::c_sif_rpc_bridge::k_loadfile_server ||
        rpc.last_observation().m_guest_path != guest_path ||
        rpc.last_observation().m_load_status !=
            iop::e_disc_load_result::success ||
        !rpc.last_observation().m_module_started ||
        !kernel.has_guest_work() ||
        rpc.last_observation().m_module_id != 1u ||
        rpc.last_observation().m_thread_id == 0u ||
        rpc.load_requests() != 1u ||
        rpc.pending_replies() != 0u ||
        rpc.pending_module_starts() != 1u ||
        memory.read32(rpc_result) != 0xcafebabeu ||
        memory.read32(rpc_result + 4u) != 0xcafebabeu ||
        (dmac.status() & (1u << 5u)) != 0u ||
        !kernel.module(1u))
    {
        return fail("LOADFILE RPC did not load and schedule requested IRX");
    }

    // The linked guest entrypoint must actually run on the R3000.
    const iop::s_kernel_run_result first_slice = kernel.run(1u);
    if (first_slice.m_stop != iop::e_kernel_stop::budget_exhausted ||
        first_slice.m_executed != 1u ||
        !kernel.has_guest_work() ||
        kernel.module(1u) == nullptr ||
        rpc.pending_module_starts() != 1u ||
        rpc.pump() ||
        memory.read32(rpc_result) != 0xcafebabeu)
    {
        return fail("LOADFILE guest thread did not consume its first slice");
    }
    const iop::s_kernel_run_result executed = kernel.run(64u);
    const iop::s_kernel_thread* completed_module =
        kernel.thread(rpc.last_observation().m_thread_id);
    const uint32_t free_before_reply = kernel.heap_total_free();
    const uint32_t initializer_stack_size = completed_module != nullptr
        ? completed_module->m_stack_size : 0u;
    if (executed.m_stop != iop::e_kernel_stop::idle ||
        kernel.has_guest_work() ||
        completed_module == nullptr ||
        completed_module->m_status != iop::e_thread_status::dormant ||
        completed_module->m_cpu.m_gpr[2] != 7u ||
        !rpc.pump() ||
        rpc.pending_module_starts() != 0u ||
        memory.read32(rpc_result) != 1u ||
        memory.read32(rpc_result + 4u) != 7u ||
        rpc.last_observation().m_return_word != 1u ||
        (dmac.status() & (1u << 5u)) == 0u)
    {
        return fail("LOADFILE module entrypoint never ran");
    }

    if (kernel.thread(rpc.last_observation().m_thread_id) != nullptr ||
        kernel.module(1u) == nullptr || initializer_stack_size == 0u ||
        kernel.heap_total_free() != free_before_reply + initializer_stack_size)
    {
        return fail("LOADFILE did not retire its initializer after reply");
    }

    // The modeled server responds with an error for a request that
    // cannot be found, without manufacturing another module ID.
    const std::string missing = "cdrom0:\\MISSING.IRX;1";
    for (size_t i = 0u; i <= missing.size(); ++i)
    {
        memory.write8(
            args_source + 8u + static_cast<uint32_t>(i),
            i == missing.size() ? 0u :
                static_cast<uint8_t>(missing[i])
        );
    }
    dmac.acknowledge(5u);
    if (!sif1.set_dma(list, 2u, &dma_id) ||
        rpc.last_observation().m_load_status !=
            iop::e_disc_load_result::missing_file ||
        rpc.last_observation().m_guest_path != missing ||
        rpc.last_observation().m_module_started ||
        rpc.last_observation().m_return_word != 0xffffffffu ||
        rpc.load_requests() != 2u ||
        memory.read32(rpc_result) != 0xffffffffu ||
        memory.read32(rpc_result + 4u) != 0xffffffffu ||
        kernel.module(2u) != nullptr)
    {
        return fail("failed LOADFILE request invented a successful module");
    }

    // Invalid/unknown server IDs fail before any descriptor in the
    // list modifies guest RAM or the IOP module registry.
    memory.write32(call_source + 13u * 4u, 0x12345678u);
    dmac.acknowledge(6u);
    const uint32_t old_word = memory.read32(
        rpc_result
    );
    if (sif1.set_dma(list, 2u, &dma_id) ||
        memory.read32(rpc_result) != old_word ||
        rpc.load_requests() != 2u ||
        (dmac.status() & (1u << 6u)) != 0u)
    {
        return fail("unknown RPC server caused partial effects");
    }

    uint32_t smflag = 0xffffffffu;
    if (!sif.get_reg(4u, &smflag) || smflag != 0u)
    {
        return fail("LOADFILE RPC fabricated IOP SMFLAG readiness");
    }

    // The guest's second initialization command has a 16-byte
    // header (CID INIT_CMD, opt=1), not a 20-byte address payload.
    // The IOP must return SET_SREG[0]=1 through the already-armed
    // EE SIF0 receiver, not directly mutate an EE register.
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    packet(memory, bind_source, {
        16u, 0u, 0x80000002u, 1u
    });
    memory.write32(ee_receive + 20u, 0xdeadbeefu);
    descriptor(
        memory, list, bind_source,
        iop::c_sifcmd_bridge::k_command_buffer, 16u
    );
    memory.write32(list + 12u, 0x44u);
    if (!sif1.set_dma(list, 1u, &dma_id) ||
        rpc.pending_replies() != 0u ||
        memory.read32(ee_receive + 0u) != 24u ||
        memory.read32(ee_receive + 8u) != 0x80000001u ||
        memory.read32(ee_receive + 12u) != 0u ||
        memory.read32(ee_receive + 16u) != 0u ||
        memory.read32(ee_receive + 20u) != 1u ||
        (dmac.status() & (1u << 5u)) == 0u ||
        commands.ee_receive_address() != ee_receive ||
        !sif.get_reg(4u, &smflag) || smflag != 0u)
    {
        return fail("INIT_CMD option 1 did not deliver SREG via SIF0");
    }

    // No other 16-byte option value is a valid initialization
    // request, and it must not raise either DMA completion bit.
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    memory.write32(bind_source + 12u, 2u);
    iop::s_sif1_dma_diagnostic reject{};
    if (sif1.set_dma(list, 1u, &dma_id, &reject) ||
        reject.m_failure !=
            iop::e_sif1_dma_failure::unsupported_sifcmd_packet ||
        rpc.pending_replies() != 0u ||
        (dmac.status() & ((1u << 5u) | (1u << 6u))) != 0u)
    {
        return fail("unsupported INIT_CMD option generated a reply");
    }


    // Exercise the entire implemented CDVDFSV service family through
    // actual SIF1 descriptor lists, SIF0 replies and mounted disc bytes.
    // These tests are independent of the retail SDBZ boot sequence.
    const uint32_t cdvd_servers[] = {
        iop::c_sif_rpc_bridge::k_cdvd_s_server,
        iop::c_sif_rpc_bridge::k_cdvd_n_server,
        iop::c_sif_rpc_bridge::k_cdvd_stream_server,
        iop::c_sif_rpc_bridge::k_cdvd_search_server,
        iop::c_sif_rpc_bridge::k_cdvd_ready_server,
        iop::c_sif_rpc_bridge::k_cdvd_ready_alt_server
    };
    for (uint32_t server : cdvd_servers)
    {
        std::vector<uint32_t> service_bind = bind64;
        service_bind[8] = server;
        packet(memory, bind_source, service_bind);
        descriptor(memory, list, bind_source,
                   iop::c_sifcmd_bridge::k_command_buffer, 64u);
        memory.write32(list + 12u, 0x44u);
        dmac.acknowledge(5u);
        dmac.acknowledge(6u);
        if (!sif1.set_dma(list, 1u, &dma_id) ||
            memory.read32(ee_receive + 32u) !=
                iop::c_sif_rpc_bridge::k_bind_cid ||
            memory.read32(ee_receive + 36u) !=
                (server ^ iop::c_sif_rpc_bridge::k_server_xor) ||
            (dmac.status() & (1u << 5u)) == 0u)
        {
            return fail("CDVDFSV server bind not delivered via SIF0");
        }
    }

    const auto cdvd_rpc_call = [&](uint32_t server, uint32_t method,
                               uint32_t send_size, uint32_t recv_size,
                               iop::s_sif1_dma_diagnostic* diagnostic = nullptr) -> bool
    {
        packet(memory, call_source, {
            64u, 0u, iop::c_sif_rpc_bridge::k_call_cid, 0u,
            8u, 0x110a0u, 0x1234u, 0x005688a8u,
            method, send_size, rpc_result, recv_size, 1u,
            server ^ iop::c_sif_rpc_bridge::k_server_xor, 0u, 0u
        });
        descriptor(memory, list, args_source,
                   iop::c_sif_rpc_bridge::k_rpc_data_destination,
                   send_size);
        descriptor(memory, list + 16u, call_source,
                   iop::c_sifcmd_bridge::k_command_buffer, 64u);
        memory.write32(list + 28u, 0x44u);
        dmac.acknowledge(5u);
        dmac.acknowledge(6u);
        return sif1.set_dma(list, 2u, &dma_id, diagnostic);
    };

    // Retail 2026-11-20 replay: CDVDFSV RTC CALL has header.opt=1.
    // Earlier HLE rejected the exact packet before the mounted-disc
    // CDVD method could run. Exercise the original 64-byte SIF1
    // descriptor and genuine existing CDVD response path.
    std::vector<uint32_t> retail_clock = {
        64u, 0u, iop::c_sif_rpc_bridge::k_call_cid, 1u,
        0x00010005u, 0x20561940u, 0x25u, 0x00464dc0u,
        1u, 0u, rpc_result, 16u, 1u,
        iop::c_sif_rpc_bridge::k_cdvd_s_server ^
            iop::c_sif_rpc_bridge::k_server_xor,
        0u, 0u
    };
    packet(memory, call_source, retail_clock);
    descriptor(memory, list, call_source,
               iop::c_sifcmd_bridge::k_command_buffer, 64u);
    memory.write32(list + 12u, 0x44u);
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    iop::s_sif1_dma_diagnostic rtc_diagnostic{};
    if (!sif1.set_dma(list, 1u, &dma_id, &rtc_diagnostic) ||
        rtc_diagnostic.m_failure != iop::e_sif1_dma_failure::none ||
        rpc.last_observation().m_cid != iop::c_sif_rpc_bridge::k_call_cid ||
        rpc.last_observation().m_server_id !=
            iop::c_sif_rpc_bridge::k_cdvd_s_server ||
        rpc.last_observation().m_method != 1u ||
        memory.read32(rpc_result) != 1u ||
        (dmac.status() & (1u << 5u)) == 0u)
    {
        return fail("retail CDVDFSV RTC RPC opt=1 not committed via SIF1");
    }

    // This exception does not license arbitrary option values or
    // alternate BIND packets. Rejection must remain side-effect-free.
    iop::c_sif_rpc_bridge::s_request opt_request{};
    iop::e_rpc_rejection opt_rejection = iop::e_rpc_rejection::none;
    retail_clock[3] = 2u;
    if (rpc.prepare_command(
            retail_clock, 64u, 0u, 0u,
            &opt_request, &opt_rejection) ||
        opt_rejection != iop::e_rpc_rejection::invalid_envelope)
    {
        return fail("unsupported SIFRPC CALL option was accepted");
    }
    retail_clock[3] = 1u;
    retail_clock[2] = iop::c_sif_rpc_bridge::k_bind_cid;
    if (rpc.prepare_command(
            retail_clock, 64u, 0u, 0u,
            &opt_request, &opt_rejection) ||
        opt_rejection != iop::e_rpc_rejection::invalid_envelope)
    {
        return fail("SIFRPC BIND option 1 bypassed envelope checks");
    }

    packet(memory, args_source, {0u, 0u, 0u, 0u});
    packet(memory, rpc_result, {0x11111111u, 0x22222222u, 0u, 0u});
    if (!cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_s_server,
                   0x03u, 16u, 4u) ||
        memory.read32(rpc_result) != 0x14u ||
        !cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_s_server,
                   0x05u, 16u, 8u) ||
        memory.read32(rpc_result) != 1u ||
        memory.read32(rpc_result + 4u) != 0u ||
        !cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_s_server,
                   0x27u, 16u, 12u) ||
        memory.read32(rpc_result + 8u) != 0u)
    {
        return fail("CDVDFSV S command results were incorrect");
    }
    if (!cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_ready_server,
                   0u, 16u, 4u) ||
        memory.read32(rpc_result) != 2u ||
        !cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_ready_alt_server,
                   0u, 16u, 4u) ||
        memory.read32(rpc_result) != 2u ||
        !cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_n_server,
                   0x04u, 16u, 4u) ||
        memory.read32(rpc_result) != 1u)
    {
        return fail("CDVDFSV media-backed status query failed");
    }

    iop::s_folder_file_info padman{};
    if (!sectors.find_file("cdrom0:\\PADMAN.IRX;1", &padman) ||
        padman.m_size != 0x220u)
    {
        return fail("CDVDFSV test disc file lookup failed");
    }
    packet(memory, args_source, {padman.m_lba, 1u, 0x001a0000u, 0u});
    if (!cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_n_server,
                   0x01u, 16u, 4u) ||
        memory.read32(rpc_result) != 0u ||
        memory.read32(0x001a0000u) != 0x464c457fu)
    {
        return fail("CDVDFSV N sector read did not copy real disc data");
    }
    packet(memory, args_source, {padman.m_lba, 1u, 0x00040000u, 0u});
    uint32_t iop_readback = 0u;
    if (!cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_n_server,
                   0x0du, 16u, 4u) ||
        !kernel.cpu().read32(0x00040000u, &iop_readback) ||
        iop_readback != 0x464c457fu)
    {
        return fail("CDVDFSV IOP sector read did not write actual IOP RAM");
    }

    // ps2sdk ee/rpc/cdvd/src/ncmd.c sceCdRead: N fno 1, SIF_RPC_M_NOWAIT,
    // 24-byte send {lbn, sectors, buf, mode, eeremaindest, eedest} and a
    // NULL/0 receive buffer (retail SDBZ Sofdec stream reads).
    {
        constexpr uint32_t remainder = 0x001a3000u;
        constexpr uint32_t readpos = 0x001a3100u;
        for (uint32_t offset = 0u; offset < 16u; offset += 4u)
        {
            memory.write32(remainder + offset, 0xccccccccu);
        }
        memory.write32(readpos, 0xccccccccu);
        memory.write32(0u, 0x13572468u);
        packet(memory, args_source, {
            padman.m_lba, 1u, 0x201a1000u, 0x00000105u,
            0x20000000u | remainder, 0x20000000u | readpos});
        packet(memory, call_source, {
            64u, 0u, iop::c_sif_rpc_bridge::k_call_cid, 0u,
            5u, 0x20561900u, 0x140u, 0x00464410u,
            1u, 24u, 0u, 0u, 1u,
            iop::c_sif_rpc_bridge::k_cdvd_n_server ^
                iop::c_sif_rpc_bridge::k_server_xor, 0u, 0u
        });
        descriptor(memory, list, args_source,
                   iop::c_sif_rpc_bridge::k_rpc_data_destination, 24u);
        descriptor(memory, list + 16u, call_source,
                   iop::c_sifcmd_bridge::k_command_buffer, 64u);
        memory.write32(list + 28u, 0x44u);
        dmac.acknowledge(5u);
        dmac.acknowledge(6u);
        if (!sif1.set_dma(list, 2u, &dma_id) ||
            memory.read32(0x001a1000u) != 0x464c457fu ||
            memory.read32(remainder) != 0u ||
            memory.read32(remainder + 4u) != 0u ||
            memory.read32(readpos) != 2048u ||
            memory.read32(0u) != 0x13572468u)
        {
            return fail("libcdvd NOWAIT sceCdRead with NULL receive failed");
        }
    }

    const std::string cdvd_path = "cdrom0:\\PADMAN.IRX;1";
    packet(memory, args_source, {0xaabbccddu, 0x11223344u});
    for (size_t index = 0u; index <= cdvd_path.size(); ++index)
    {
        memory.write8(
            args_source + 0x24u + static_cast<uint32_t>(index),
            index == cdvd_path.size() ? 0u :
                static_cast<uint8_t>(cdvd_path[index])
        );
    }
    if (!cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_search_server,
                   0u, 0x80u, 4u) ||
        memory.read32(rpc_result) != 1u ||
        memory.read32(args_source) != padman.m_lba ||
        memory.read32(args_source + 4u) != padman.m_size)
    {
        return fail("CDVDFSV search result did not match mounted ISO map");
    }

    packet(memory, args_source, {padman.m_lba, 1u, 0x001a0800u, 1u});
    if (!cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_n_server,
                   0x09u, 16u, 4u) ||
        !cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_s_server,
                   0x0cu, 16u, 4u) ||
        memory.read32(rpc_result) != 0x12u)
    {
        return fail("CDVDFSV stream-start state did not become visible");
    }
    packet(memory, args_source, {0u, 1u, 0x001a0800u, 2u});
    if (!cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_n_server,
                   0x09u, 16u, 4u) ||
        memory.read32(0x001a0800u) != 0x464c457fu)
    {
        return fail("CDVDFSV stream read did not use mounted sectors");
    }
    packet(memory, args_source, {0u, 0u, 0u, 3u});
    if (!cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_n_server,
                   0x09u, 16u, 4u) ||
        !cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_s_server,
                   0x0cu, 16u, 4u) ||
        memory.read32(rpc_result) != 0x0au)
    {
        return fail("CDVDFSV stream stop did not restore idle status");
    }

    // Reject unknown methods without memory or DMA side effects.
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    packet(memory, args_source, {0u, 0u, 0u, 0u});
    const uint32_t preceding = memory.read32(rpc_result);
    if (cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_n_server,
                  0xffffu, 16u, 4u) ||
        memory.read32(rpc_result) != preceding ||
        rpc.pending_replies() != 0u ||
        (dmac.status() & ((1u << 5u) | (1u << 6u))) != 0u)
    {
        return fail("unknown CDVDFSV RPC invented a completion");
    }

    // Rejection categories are derived only from a genuine staged guest
    // packet. A diagnosis must never acknowledge a SIF0/6 DMA event,
    // alter the EE receive buffer or install an unknown service.
    iop::s_sif1_dma_diagnostic rejection{};
    const uint32_t unchanged_rpc_result = memory.read32(rpc_result);
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    if (cdvd_rpc_call(0x8000beefu, 0u, 16u, 4u, &rejection) ||
        rejection.m_failure !=
            iop::e_sif1_dma_failure::unsupported_rpc_packet ||
        rejection.m_rpc_rejection !=
            iop::e_rpc_rejection::unknown_server ||
        rejection.m_rpc_server != 0x8000beefu ||
        rejection.m_rpc_packet_word_count != 16u ||
        memory.read32(rpc_result) != unchanged_rpc_result ||
        rpc.pending_replies() != 0u ||
        (dmac.status() & ((1u << 5u) | (1u << 6u))) != 0u)
    {
        return fail("unknown SIFRPC server was not diagnosed fail-closed");
    }

    if (cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_s_server,
                      0xffffu, 16u, 4u, &rejection) ||
        rejection.m_rpc_rejection !=
            iop::e_rpc_rejection::unsupported_method ||
        memory.read32(rpc_result) != unchanged_rpc_result ||
        (dmac.status() & ((1u << 5u) | (1u << 6u))) != 0u)
    {
        return fail("unsupported CDVDFSV method was not diagnosed");
    }

    if (cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_n_server,
                      0x01u, 4u, 4u, &rejection) ||
        rejection.m_rpc_rejection !=
            iop::e_rpc_rejection::invalid_arguments ||
        rpc.pending_replies() != 0u)
    {
        return fail("truncated CDVDFSV arguments were not diagnosed");
    }

    if (cdvd_rpc_call(iop::c_sif_rpc_bridge::k_cdvd_stream_server,
                      1u, 16u, 4u, &rejection) ||
        rejection.m_rpc_rejection !=
            iop::e_rpc_rejection::unavailable_backend ||
        rpc.pending_replies() != 0u)
    {
        return fail("unscheduled CDVD stream RPC was not diagnosed");
    }

    // A valid first descriptor cannot partially write IOP RAM if a
    // second descriptor carries an unsupported RPC packet.
    const uint32_t guarded_iop_address = 0x00068000u;
    uint32_t guarded_before = 0u;
    if (!kernel.cpu().read32(guarded_iop_address, &guarded_before))
    {
        return fail("unable to inspect IOP rollback sentinel");
    }
    packet(memory, args_source, {0xdec0de11u, 0x12345678u, 0u, 0u});
    descriptor(memory, list, args_source, guarded_iop_address, 16u);
    // The rejected unknown-server call packet is still in call_source
    // only after explicitly restoring it (the previous calls replaced it).
    packet(memory, call_source, {
        64u, 0u, iop::c_sif_rpc_bridge::k_call_cid, 0u,
        8u, 0x110a0u, 0x1234u, 0x005688a8u,
        0u, 16u, rpc_result, 4u, 1u,
        0x8000beefu ^ iop::c_sif_rpc_bridge::k_server_xor, 0u, 0u
    });
    // The payload descriptor must precede the command; otherwise
    // prepare_command correctly rejects a missing payload before it can
    // classify the intentionally unknown RPC server.
    descriptor(memory, list + 16u, args_source,
               iop::c_sif_rpc_bridge::k_rpc_data_destination, 16u);
    descriptor(memory, list + 32u, call_source,
               iop::c_sifcmd_bridge::k_command_buffer, 64u);
    memory.write32(list + 44u, 0x44u);
    uint32_t guarded_after = 0u;
    if (sif1.set_dma(list, 3u, &dma_id, &rejection) ||
        rejection.m_descriptor_index != 2u ||
        rejection.m_rpc_rejection !=
            iop::e_rpc_rejection::unknown_server ||
        !kernel.cpu().read32(guarded_iop_address, &guarded_after) ||
        guarded_after != guarded_before ||
        memory.read32(rpc_result) != unchanged_rpc_result ||
        rpc.pending_replies() != 0u ||
        (dmac.status() & ((1u << 5u) | (1u << 6u))) != 0u)
    {
        return fail("rejected second RPC partially committed prior IOP DMA");
    }

    // An explicitly successful prepare clears the diagnostic without
    // altering its caller's guest memory or delivering a reply.
    iop::c_sif_rpc_bridge::s_request staged{};
    iop::e_rpc_rejection status = iop::e_rpc_rejection::unknown_server;
    const std::vector<uint32_t> valid_bind = {
        36u, 0u, iop::c_sif_rpc_bridge::k_bind_cid, 0u,
        1u, 0x11000u, 0x1234u, 0x15000u,
        iop::c_sif_rpc_bridge::k_loadfile_server
    };
    if (!rpc.prepare_command(valid_bind, 36u, 0u, 0u, &staged, &status) ||
        status != iop::e_rpc_rejection::none ||
        staged.m_server_id != iop::c_sif_rpc_bridge::k_loadfile_server ||
        rpc.pending_replies() != 0u ||
        memory.read32(rpc_result) != unchanged_rpc_result)
    {
        return fail("accepted RPC preview changed guest state or reason");
    }

    // A bind request for an unknown server is distinct from malformed
    // packets, and neither may overwrite the output staging request.
    std::vector<uint32_t> invalid_bind = valid_bind;
    invalid_bind[8] = 0x8000beefu;
    staged.m_server_id = 0xcafebabeu;
    if (rpc.prepare_command(
            invalid_bind, 36u, 0u, 0u, &staged, &status) ||
        status != iop::e_rpc_rejection::unknown_server ||
        staged.m_server_id != 0xcafebabeu ||
        rpc.pending_replies() != 0u)
    {
        return fail("unsupported bind mutated output or lost server identity");
    }

    invalid_bind = valid_bind;
    invalid_bind[1] = 0x01234560u;
    if (rpc.prepare_command(
            invalid_bind, 36u, 0u, 0u, &staged, &status) ||
        status != iop::e_rpc_rejection::invalid_envelope ||
        staged.m_server_id != 0xcafebabeu ||
        rpc.pending_replies() != 0u)
    {
        return fail("malformed RPC envelope was not distinguished");
    }


    // Retail SDBZ's next LOADFILE call sends 512 bytes with a
    // nonzero argument length (observed after LIBSD.IRX). The
    // PS2SDK arg_len and args[] fields are not reserved padding.
    // First verify transactional validation without mutating IOP RAM.
    const std::string with_args_path = "cdrom0:\\PADMAN.IRX;1";
    const std::vector<uint8_t> module_args = {
        'm', 'c', '0', ':', 0u,
        '-', 'p', 'o', 'r', 't', '=', '0', 0u
    };
    for (uint32_t offset = 0u; offset < 512u; offset += 4u)
    {
        memory.write32(args_source + offset, 0u);
    }
    memory.write32(args_source, static_cast<uint32_t>(module_args.size()));
    for (uint32_t index = 0u; index <= with_args_path.size(); ++index)
    {
        memory.write8(
            args_source + 8u + index,
            index == with_args_path.size() ? 0u :
                static_cast<uint8_t>(with_args_path[index]));
    }
    for (uint32_t index = 0u; index < module_args.size(); ++index)
    {
        memory.write8(args_source + 260u + index, module_args[index]);
    }
    const std::vector<uint32_t> load_with_args = {
        0x00020040u,
        iop::c_sif_rpc_bridge::k_rpc_data_destination,
        iop::c_sif_rpc_bridge::k_call_cid, 0u,
        5u, 0x20561900u, 13u, 0x00564d40u,
        0u, 512u, rpc_result, 8u, 1u,
        iop::c_sif_rpc_bridge::k_loadfile_server ^
            iop::c_sif_rpc_bridge::k_server_xor,
        0u, 0u
    };
    staged.m_guest_path = "unchanged";
    staged.m_module_arguments = {"untouched"};
    if (!rpc.prepare_command(
            load_with_args, 64u, args_source, 512u,
            &staged, &status) ||
        status != iop::e_rpc_rejection::none ||
        staged.m_guest_path != with_args_path ||
        staged.m_module_arguments !=
            std::vector<std::string>({"mc0:", "-port=0"}))
    {
        return fail("LOADFILE nonzero arglen did not decode NUL argv");
    }
    memory.write32(args_source, 253u);
    if (rpc.prepare_command(
            load_with_args, 64u, args_source, 512u,
            &staged, &status) ||
        status != iop::e_rpc_rejection::invalid_arguments ||
        staged.m_guest_path != with_args_path)
    {
        return fail("LOADFILE oversized arglen was accepted");
    }
    memory.write32(args_source, 13u);
    memory.write8(args_source + 260u + 12u, 'X');
    if (rpc.prepare_command(
            load_with_args, 64u, args_source, 512u,
            &staged, &status) ||
        status != iop::e_rpc_rejection::invalid_arguments)
    {
        return fail("LOADFILE unterminated module argument was accepted");
    }
    memory.write8(args_source + 260u + 12u, 0u);

    // Exercise the same retail-form packet through the actual
    // transactional SIF1 descriptor list and LOADFILE backend.
    packet(memory, call_source, load_with_args);
    descriptor(memory, list, args_source,
               iop::c_sif_rpc_bridge::k_rpc_data_destination, 512u);
    descriptor(memory, list + 16u, call_source,
               iop::c_sifcmd_bridge::k_command_buffer, 64u);
    memory.write32(list + 28u, 0x44u);
    memory.write32(rpc_result, 0xffffffffu);
    memory.write32(rpc_result + 4u, 0xffffffffu);
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    if (!sif1.set_dma(list, 2u, &dma_id) ||
        rpc.last_observation().m_guest_path != with_args_path ||
        rpc.last_observation().m_load_status !=
            iop::e_disc_load_result::success ||
        !rpc.last_observation().m_module_started ||
        rpc.last_observation().m_module_id != 2u ||
        rpc.pending_module_starts() != 1u ||
        rpc.pending_replies() != 0u ||
        memory.read32(rpc_result) != 0xffffffffu ||
        memory.read32(rpc_result + 4u) != 0xffffffffu ||
        (dmac.status() & (1u << 5u)) != 0u)
    {
        return fail("LOADFILE argument-bearing SIF1 packet was refused");
    }
    const iop::s_kernel_thread* args_thread =
        kernel.thread(rpc.last_observation().m_thread_id);
    if (args_thread == nullptr ||
        args_thread->m_cpu.m_gpr[4] != 3u)
    {
        return fail("LOADFILE argument-bearing IRX received wrong argc");
    }
    const uint32_t argv_base = args_thread->m_cpu.m_gpr[5];
    const std::vector<std::string> expected_args = {
        with_args_path, "mc0:", "-port=0"
    };
    for (uint32_t index = 0u; index < expected_args.size(); ++index)
    {
        uint32_t guest_pointer = 0u;
        if (!kernel.cpu().read32(
                argv_base + index * 4u, &guest_pointer))
        {
            return fail("LOADFILE guest argv pointer unreadable");
        }
        for (uint32_t byte = 0u;
             byte <= expected_args[index].size(); ++byte)
        {
            uint8_t actual_byte = 0xffu;
            const uint8_t expected_byte =
                byte == expected_args[index].size() ? 0u :
                    static_cast<uint8_t>(expected_args[index][byte]);
            if (!kernel.cpu().read8(
                    guest_pointer + byte, &actual_byte) ||
                actual_byte != expected_byte)
            {
                return fail("LOADFILE did not preserve argument bytes");
            }
        }
    }

    // A terminated initializer is dormant but did not return from _start.
    // Neither the EE receive words nor DMA5 can change in that state.
    const uint32_t args_thread_id =
        rpc.last_observation().m_thread_id;
    if (!kernel.stop_thread(args_thread_id) ||
        rpc.pump() ||
        rpc.pending_module_starts() != 1u ||
        rpc.pending_replies() != 0u ||
        memory.read32(rpc_result) != 0xffffffffu ||
        memory.read32(rpc_result + 4u) != 0xffffffffu ||
        (dmac.status() & (1u << 5u)) != 0u)
    {
        return fail("terminated LOADFILE initializer fabricated completion");
    }

    // Restart the same original IRX and verify that only the real guest
    // return trampoline unlocks the pending LOADFILE reply.
    if (!kernel.start_module_thread_with_arguments(
            args_thread_id, with_args_path,
            {"mc0:", "-port=0"}) ||
        kernel.thread(args_thread_id) == nullptr ||
        kernel.thread(args_thread_id)->m_module_entry_returned ||
        rpc.pump() ||
        (dmac.status() & (1u << 5u)) != 0u)
    {
        return fail("LOADFILE restart fabricated module return");
    }
    const iop::s_kernel_run_result args_completed = kernel.run(64u);
    const iop::s_kernel_thread* args_returned =
        kernel.thread(args_thread_id);
    if (args_completed.m_stop != iop::e_kernel_stop::idle ||
        args_returned == nullptr ||
        !args_returned->m_module_entry_returned ||
        args_returned->m_module_entry_result != 7u ||
        rpc.pending_module_starts() != 1u ||
        !rpc.pump() ||
        rpc.pending_module_starts() != 0u ||
        memory.read32(rpc_result) != 2u ||
        memory.read32(rpc_result + 4u) != 7u ||
        (dmac.status() & (1u << 5u)) == 0u)
    {
        return fail("argument-bearing LOADFILE replied before guest return");
    }

    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    memory.write32(rpc_result, 0xcafebabeu);
    memory.write32(rpc_result + 4u, 0xcafebabeu);
    if (!sif1.set_dma(list, 2u, &dma_id) ||
        rpc.pending_module_starts() != 1u ||
        (dmac.status() & (1u << 5u)) != 0u)
    {
        return fail("LOADFILE cancellation fixture did not queue a start");
    }
    const uint32_t deleted_thread_id = rpc.last_observation().m_thread_id;
    if (!kernel.stop_thread(deleted_thread_id) ||
        !kernel.delete_thread(deleted_thread_id) ||
        !rpc.pump() ||
        rpc.pending_module_starts() != 0u ||
        rpc.pending_replies() != 0u ||
        !rpc.can_queue_replies(64u) ||
        rpc.last_observation().m_load_status !=
            iop::e_disc_load_result::thread_failed ||
        rpc.last_observation().m_return_word != 0xffffffffu ||
        memory.read32(rpc_result) != 0xffffffffu ||
        memory.read32(rpc_result + 4u) != 0xffffffffu ||
        (dmac.status() & (1u << 5u)) == 0u)
    {
        return fail("deleted LOADFILE initializer did not reply with failure");
    }
    dmac.acknowledge(5u);
    if (rpc.pump() || (dmac.status() & (1u << 5u)) != 0u)
    {
        return fail("deleted LOADFILE initializer replied twice");
    }

    // Reset may not retain the previous IOPRP's LOADFILE version.
    rpc.reset_for_reboot();
    packet(memory, call_source, version_call);
    descriptor(memory, list, call_source,
               iop::c_sifcmd_bridge::k_command_buffer, 64u);
    memory.write32(list + 12u, 0x44u);
    dmac.acknowledge(5u);
    dmac.acknowledge(6u);
    if (sif1.set_dma(list, 1u, &dma_id) ||
        rpc.pending_replies() != 0u)
    {
        return fail("reset retained stale LOADFILE version");
    }
    if (!test_arkd_deferred_bind(folder.m_path))
    {
        return fail("ARKD deferred BIND lifecycle failed");
    }
    if (!test_guest_rpc_lifecycle(folder.m_path))
    {
        return fail("real guest IOP SIFRPC lifecycle failed");
    }
    if (!test_iop_to_ee_rpc(folder.m_path))
    {
        return fail("IOP-initiated SIFRPC bind/call lifecycle failed");
    }
    if (!test_guest_sifcmd_send(folder.m_path))
    {
        return fail("guest SIFCMD send did not traverse armed SIF0");
    }

    std::printf("IOP LOADFILE RPC: PASS\n");
    return 0;
}
