#include "iop_sif1_bridge.h"
#include "iop_sifcmd_bridge.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <vector>

namespace
{
int fail(const char* message)
{
    std::fprintf(stderr, "IOP guest SIFCMD: %s\n", message);
    return 1;
}

bool write_call(
    iop::c_cpu& cpu, uint32_t at, uint32_t target
)
{
    // jal target; nop; break. The R3000 must execute the actual
    // installed HLE import stub and its syscall before breaking.
    const uint32_t jal = 0x0c000000u |
        ((target >> 2u) & 0x03ffffffu);
    return cpu.write32(at, jal) &&
           cpu.write32(at + 4u, 0u) &&
           cpu.write32(at + 8u, 0x0000000du);
}

void descriptor(
    ee::ee_memory& memory,
    uint32_t address, uint32_t src, uint32_t dst, uint32_t size,
    uint32_t attributes = 0u
)
{
    memory.write32(address, src);
    memory.write32(address + 4u, dst);
    memory.write32(address + 8u, size);
    memory.write32(address + 12u, attributes);
}

void command(
    ee::ee_memory& memory, uint32_t address,
    uint32_t cid, uint32_t first, uint32_t second
)
{
    memory.write32(address + 0u, 24u);
    memory.write32(address + 4u, 0u);
    memory.write32(address + 8u, cid);
    memory.write32(address + 12u, 0u);
    memory.write32(address + 16u, first);
    memory.write32(address + 20u, second);
    memory.write32(address + 24u, 0u);
    memory.write32(address + 28u, 0u);
}

void command20(
    ee::ee_memory& memory, uint32_t address,
    uint32_t cid, uint32_t receive_address
)
{
    memory.write32(address + 0u, 20u);
    memory.write32(address + 4u, 0u);
    memory.write32(address + 8u, cid);
    memory.write32(address + 12u, 0u);
    memory.write32(address + 16u, receive_address);
    memory.write32(address + 20u, 0u);
    memory.write32(address + 24u, 0u);
    memory.write32(address + 28u, 0u);
}
} // namespace

int main()
{
    constexpr uint32_t k_list = 0x11000u;
    constexpr uint32_t k_source = 0x12000u;
    constexpr uint32_t k_data_source = 0x13000u;
    constexpr uint32_t k_destination = 0x14000u;
    constexpr uint32_t k_entry = 0x1c000u;

    ee::ee_memory ee_ram(32u * 1024u * 1024u);
    ee::s_ee_cpu_state ee_cpu{};
    ee::c_ee_sif sif;
    ee::c_ee_dmac dmac;
    sif.reset();
    dmac.bind_cpu(&ee_cpu);
    dmac.bind_memory(&ee_ram);
    dmac.reset();

    iop::c_kernel kernel;
    iop::c_sifcmd_bridge commands(kernel, sif);
    iop::c_sif1_bridge transport(
        ee_ram, kernel.cpu(), dmac, &commands
    );
    if (!commands.install() ||
        !write_call(
            kernel.cpu(), k_entry,
            iop::c_sifcmd_bridge::k_init_cmd_stub))
    {
        return fail("could not install guest SIFCMD import stub");
    }

    uint32_t register_value = 0xffffffffu;
    command(
        ee_ram, k_source, 0x80000001u, 3u, 0x12345678u
    );
    descriptor(
        ee_ram, k_list, k_source,
        iop::c_sifcmd_bridge::k_command_buffer, 24u
    );

    // Merely installing imports is not equivalent to executing IOP
    // initialization. Never make SUBADDR/SMFLAG ready on install.
    if (!sif.get_reg(2u, &register_value) ||
        register_value != 0u ||
        commands.initialized() ||
        transport.set_dma(k_list, 1u, &register_value) ||
        dmac.status() != 0u)
    {
        return fail("unexecuted guest sifcmd accepted a command");
    }

    const uint32_t thread_id = kernel.create_thread(
        k_entry, 0u, 1u, 0x400u
    );
    if (thread_id == 0u || !kernel.start_thread(thread_id))
    {
        return fail("could not start IOP guest initializer");
    }

    kernel.bind_syscall(
        [&](iop::c_kernel& active, uint32_t service)
        {
            return commands.handle_syscall(active, service);
        }
    );

    const iop::s_kernel_run_result init_result =
        kernel.run(64u, 64u);
    uint32_t smflag = 0xffffffffu;
    if (init_result.m_stop != iop::e_kernel_stop::breakpoint ||
        init_result.m_guest_pc != k_entry + 8u ||
        !commands.initialized() ||
        !sif.get_reg(2u, &register_value) ||
        register_value != iop::c_sifcmd_bridge::k_command_buffer ||
        !sif.get_reg(4u, &smflag) || smflag != 0u)
    {
        return fail("R3000 sifcmd init did not publish only SUBADDR");
    }

    // The production extracted-disc HLE kernel may explicitly start
    // already-installed SIFCMD services without guest firmware. It is a
    // distinct mode: install() and guest-import initialization above
    // must not silently publish the initial boot flags.
    {
        ee::c_ee_sif hle_sif;
        hle_sif.reset();
        iop::c_kernel hle_kernel;
        iop::c_sifcmd_bridge hle_commands(hle_kernel, hle_sif);
        uint32_t subaddr = 0xffffffffu;
        uint32_t flags = 0xffffffffu;
        if (hle_commands.initialize_hle_bootstrap() ||
            !hle_commands.install() ||
            !hle_sif.get_reg(4u, &flags) || flags != 0u ||
            !hle_commands.initialize_hle_bootstrap() ||
            hle_commands.initialize_hle_bootstrap() ||
            !hle_commands.initialized() ||
            !hle_sif.get_reg(2u, &subaddr) ||
            subaddr != iop::c_sifcmd_bridge::k_command_buffer ||
            !hle_sif.get_reg(4u, &flags) || flags != 0x60000u)
        {
            return fail("explicit HLE-only SIFCMD bootstrap state");
        }
        // The IOP-owned SMFLAG still follows real EE acknowledgment
        // semantics; it is not a stuck/high fake return value.
        if (!hle_sif.set_reg(4u, 0x20000u) ||
            !hle_sif.get_reg(4u, &flags) || flags != 0x40000u)
        {
            return fail("HLE SMFLAG EE acknowledgment semantics");
        }
    }

    uint32_t dma_id = 0u;
    if (!transport.set_dma(k_list, 1u, &dma_id) ||
        dma_id != 1u ||
        commands.sreg(3u) != 0x12345678u ||
        (dmac.status() & (1u << 6u)) == 0u)
    {
        return fail("SIF1 SREG system command not delivered");
    }
    uint32_t iop_word = 0u;
    if (!kernel.cpu().read32(
            iop::c_sifcmd_bridge::k_command_buffer + 8u,
            &iop_word) ||
        iop_word != 0x80000001u)
    {
        return fail("IOP did not receive original SIFCMD packet");
    }

    dmac.acknowledge(6u);
    command(
        ee_ram, k_source, 0x80000002u, 0x00180000u, 0u
    );
    if (!transport.set_dma(k_list, 1u, &dma_id) ||
        dma_id != 2u ||
        commands.ee_receive_address() != 0x00180000u ||
        !sif.get_reg(4u, &smflag) || smflag != 0u)
    {
        return fail("EE command receive address or SMFLAG ownership");
    }

    // Real EE sifcmd uses a terminal 20-byte 0x44 descriptor for
    // CHANGE_SADDR and INIT_CMD. Receiving it updates only the real
    // registered EE command buffer, never SMFLAG or an RPC reply.
    dmac.acknowledge(6u);
    command20(ee_ram, k_source, 0x80000000u, 0x00181000u);
    descriptor(
        ee_ram, k_list, k_source,
        iop::c_sifcmd_bridge::k_command_buffer, 20u, 0x44u
    );
    if (!transport.set_dma(k_list, 1u, &dma_id) ||
        dma_id != 3u ||
        commands.ee_receive_address() != 0x00181000u ||
        !kernel.cpu().read32(
            iop::c_sifcmd_bridge::k_command_buffer + 16u, &iop_word) ||
        iop_word != 0x00181000u ||
        (dmac.status() & (1u << 6u)) == 0u ||
        !sif.get_reg(4u, &smflag) || smflag != 0u)
    {
        return fail("terminal CHANGE_SADDR command was not delivered");
    }

    dmac.acknowledge(6u);
    // Older PS2SDK publishes the uncached EE command buffer alias.
    command20(ee_ram, k_source, 0x80000002u, 0x20182000u);
    if (!transport.set_dma(k_list, 1u, &dma_id) ||
        dma_id != 4u ||
        commands.ee_receive_address() != 0x00182000u ||
        !kernel.cpu().read32(
            iop::c_sifcmd_bridge::k_command_buffer + 8u, &iop_word) ||
        iop_word != 0x80000002u ||
        !kernel.cpu().read32(
            iop::c_sifcmd_bridge::k_command_buffer + 16u, &iop_word) ||
        iop_word != 0x20182000u ||
        (dmac.status() & (1u << 6u)) == 0u)
    {
        return fail("terminal 20-byte INIT_CMD uncached address not delivered");
    }

    // Validate both receive CIDs without modifying command state.
    // Only normalize the EE uncached RDRAM mirror, not an arbitrary
    // segment whose masked address happens to be inside RAM.
    for (uint32_t cid : {0x80000000u, 0x80000002u})
    {
        std::vector<uint32_t> words{20u, 0u, cid, 0u, 0x20118f40u};
        iop::c_sifcmd_bridge::s_command prepared{};
        if (!commands.prepare_command(words, 20u, &prepared) ||
            prepared.m_value != 0x00118f40u ||
            commands.ee_receive_address() != 0x00182000u)
        {
            return fail("uncached command receiver normalization");
        }
        for (uint32_t invalid : {0u, 0x20000000u, 0x20118f41u,
                                 0x22000000u, 0x40118f40u, 0x70118f40u})
        {
            words[4] = invalid;
            if (commands.prepare_command(words, 20u, &prepared))
            {
                return fail("invalid receive alias accepted");
            }
        }
    }

    // A terminal descriptor does not authorize an empty command or
    // an arbitrary DMA endpoint. Reject these without IOP mutations,
    // DMAC completion, or a newly issued transaction ID.
    dmac.acknowledge(6u);
    ee_ram.write32(k_source, 0u);
    iop::s_sif1_dma_diagnostic command_rejection{};
    if (transport.set_dma(k_list, 1u, &dma_id, &command_rejection) ||
        command_rejection.m_failure !=
            iop::e_sif1_dma_failure::unsupported_sifcmd_packet ||
        command_rejection.m_packet_size != 0u ||
        command_rejection.m_packet_dest != 0u ||
        command_rejection.m_packet_cid != 0x80000002u ||
        command_rejection.m_packet_opt != 0u ||
        command_rejection.m_packet_arg != 0x20182000u ||
        commands.ee_receive_address() != 0x00182000u ||
        dmac.status() != 0u)
    {
        return fail("empty INIT_CMD was wrongly completed");
    }

    descriptor(
        ee_ram, k_list, k_data_source, k_destination, 16u, 0x44u
    );
    if (transport.set_dma(k_list, 1u, &dma_id, &command_rejection) ||
        command_rejection.m_failure !=
            iop::e_sif1_dma_failure::invalid_size_or_attributes ||
        dmac.status() != 0u)
    {
        return fail("terminal SIFCMD flags were accepted on ordinary DMA");
    }

    // A 16-byte opt=1 INIT_CMD needs an IOP->EE reply transport.
    // This fixture has no RPC/SIF0 reply backend; reject before
    // changing command state, RAM, or DMAC.
    dmac.acknowledge(6u);
    ee_ram.write32(k_source, 16u);
    ee_ram.write32(k_source + 4u, 0u);
    ee_ram.write32(k_source + 8u, 0x80000002u);
    ee_ram.write32(k_source + 12u, 1u);
    descriptor(
        ee_ram, k_list, k_source,
        iop::c_sifcmd_bridge::k_command_buffer, 16u, 0x44u
    );
    if (transport.set_dma(k_list, 1u, &dma_id) ||
        commands.ee_receive_address() != 0x00182000u ||
        dmac.status() != 0u)
    {
        return fail("INIT_CMD reply accepted without SIF0 backend");
    }

    // An unsupported RPC CID may not be mistaken for a completed
    // operation or reply. Check that earlier transfers in the same
    // descriptor list are not partially committed on rejection.
    dmac.acknowledge(6u);
    kernel.cpu().write32(k_destination, 0xdeadc0deu);
    ee_ram.write32(k_data_source, 0xabcdef98u);
    descriptor(
        ee_ram, k_list, k_data_source, k_destination, 16u
    );
    descriptor(
        ee_ram, k_list + 16u, k_source,
        iop::c_sifcmd_bridge::k_command_buffer, 24u
    );
    command(
        ee_ram, k_source, 0x80000009u, 0x80000006u, 0u
    );
    iop::s_sif1_dma_diagnostic rejection{};
    if (transport.set_dma(k_list, 2u, &dma_id, &rejection) ||
        rejection.m_failure !=
            iop::e_sif1_dma_failure::unsupported_rpc_packet ||
        rejection.m_descriptor_index != 1u ||
        rejection.m_source != k_source ||
        rejection.m_destination !=
            iop::c_sifcmd_bridge::k_command_buffer ||
        rejection.m_size != 24u ||
        rejection.m_attributes != 0u ||
        rejection.m_packet_size != 24u ||
        rejection.m_packet_cid != 0x80000009u ||
        !kernel.cpu().read32(k_destination, &iop_word) ||
        iop_word != 0xdeadc0deu ||
        commands.ee_receive_address() != 0x00182000u ||
        dmac.status() != 0u)
    {
        return fail("unsupported RPC command caused a partial DMA");
    }
    if (!kernel.cpu().read32(
            iop::c_sifcmd_bridge::k_command_buffer + 8u,
            &iop_word) ||
        iop_word != 0x80000002u)
    {
        return fail("unsupported command overwrote prior packet");
    }

    // An IOP reset packet is a special operation, not a physical
    // write to RAM zero and never an implicitly completed DMA.
    descriptor(ee_ram, k_list, k_data_source, 0u, 16u);
    if (transport.set_dma(k_list, 1u, &dma_id, &rejection) ||
        rejection.m_failure !=
            iop::e_sif1_dma_failure::unsupported_iop_reboot ||
        rejection.m_destination != 0u ||
        rejection.m_source != k_data_source ||
        dmac.status() != 0u ||
        !kernel.cpu().read32(k_destination, &iop_word) ||
        iop_word != 0xdeadc0deu)
    {
        return fail("IOP reboot DMA was silently accepted or mutated RAM");
    }

    // Even an ordinary transfer may not overlap the reserved command
    // buffer without passing the command validation protocol.
    descriptor(
        ee_ram, k_list, k_data_source,
        iop::c_sifcmd_bridge::k_command_buffer - 16u, 32u
    );
    if (transport.set_dma(k_list, 1u, &dma_id, &rejection) ||
        rejection.m_failure !=
            iop::e_sif1_dma_failure::command_buffer_overlap)
    {
        return fail("unaligned command-region crossing accepted");
    }

    // Let another guest R3000 thread call the imported SetSreg service.
    constexpr uint32_t k_set_entry = k_entry + 0x20u;
    if (!write_call(
            kernel.cpu(), k_set_entry,
            iop::c_sifcmd_bridge::k_set_sreg_stub))
    {
        return fail("could not author R3000 SetSreg call");
    }
    const uint32_t setter = kernel.create_thread(
        k_set_entry, 0u, 1u, 0x400u, 5u, 0xf00dbeefu
    );
    if (setter == 0u || !kernel.start_thread(setter))
    {
        return fail("could not start guest SREG writer");
    }
    const iop::s_kernel_run_result setter_result =
        kernel.run(64u, 64u);
    if (setter_result.m_stop != iop::e_kernel_stop::breakpoint ||
        commands.sreg(5u) != 0xf00dbeefu ||
        !sif.get_reg(4u, &smflag) || smflag != 0u)
    {
        return fail("guest SetSreg was not executed correctly");
    }

    // Retail SDBZ issues SIF_CMD_RESET_CMD after shutting down EE
    // SIF0. The 104-byte command carries arglen, mode and up to 80
    // command-line bytes without a required NUL terminator. A valid
    // SIF1 transfer is not itself proof of successful IOP reboot.
    const std::string reboot_line =
        "rom0:UDNL cdrom0:\\IOPRP300.IMG;1";
    const auto write_reboot = [&](uint32_t arglen, uint32_t mode)
    {
        for (uint32_t offset = 0u; offset < 112u; offset += 4u)
        {
            ee_ram.write32(k_source + offset, 0u);
        }
        ee_ram.write32(k_source, 104u);
        ee_ram.write32(k_source + 8u, 0x80000003u);
        ee_ram.write32(k_source + 16u, arglen);
        ee_ram.write32(k_source + 20u, mode);
        for (uint32_t index = 0u; index < reboot_line.size(); ++index)
        {
            ee_ram.write8(
                k_source + 24u + index,
                static_cast<uint8_t>(reboot_line[index])
            );
        }
    };
    write_reboot(static_cast<uint32_t>(reboot_line.size()), 0u);
    descriptor(
        ee_ram, k_list, k_source,
        iop::c_sifcmd_bridge::k_command_buffer, 104u, 0x44u
    );
    dmac.acknowledge(6u);
    std::string accepted_reboot;
    if (!transport.set_dma(k_list, 1u, &dma_id) ||
        (dmac.status() & (1u << 6u)) == 0u ||
        !commands.take_reboot_request(&accepted_reboot) ||
        accepted_reboot != reboot_line ||
        commands.take_reboot_request(&accepted_reboot) ||
        !kernel.cpu().read32(
            iop::c_sifcmd_bridge::k_command_buffer + 8u,
            &iop_word) ||
        iop_word != 0x80000003u)
    {
        return fail("validated IOP reboot was not staged through SIF1");
    }

    // Bad packet lengths, modes and embedded NULs must fail before
    // changing IOP RAM or completing DMA, including 0x44 terminal flags.
    dmac.acknowledge(6u);
    write_reboot(81u, 0u);
    if (transport.set_dma(k_list, 1u, &dma_id, &rejection) ||
        rejection.m_failure !=
            iop::e_sif1_dma_failure::unsupported_sifcmd_packet ||
        dmac.status() != 0u ||
        commands.take_reboot_request(&accepted_reboot))
    {
        return fail("oversized IOP reset argument accepted");
    }
    write_reboot(static_cast<uint32_t>(reboot_line.size()), 1u);
    if (transport.set_dma(k_list, 1u, &dma_id, &rejection) ||
        rejection.m_failure !=
            iop::e_sif1_dma_failure::unsupported_sifcmd_packet ||
        dmac.status() != 0u)
    {
        return fail("unsupported IOP reset mode accepted");
    }
    write_reboot(static_cast<uint32_t>(reboot_line.size()), 0u);
    ee_ram.write8(k_source + 24u + 4u, 0u);
    if (transport.set_dma(k_list, 1u, &dma_id, &rejection) ||
        rejection.m_failure !=
            iop::e_sif1_dma_failure::unsupported_sifcmd_packet ||
        dmac.status() != 0u ||
        commands.take_reboot_request(&accepted_reboot))
    {
        return fail("malformed IOP reset arguments accepted");
    }

    // The host HLE reset coordinator must discard the IOP-owned flags
    // and old kernel state before it can install services again. In
    // particular, receiving the reboot DMA alone never publishes BOOTEND.
    sif.publish_iop_smflag(0x60000u);
    sif.begin_iop_reboot();
    kernel.reset();
    commands.reset_for_reboot();
    if (!sif.get_reg(4u, &smflag) || smflag != 0u ||
        commands.initialized() ||
        commands.sreg(5u) != 0u ||
        !commands.install() ||
        !commands.initialize_hle_bootstrap() ||
        !sif.get_reg(4u, &smflag) || smflag != 0x60000u)
    {
        return fail("IOP HLE reset/reinstall lifecycle was inconsistent");
    }

    std::printf("IOP guest SIFCMD: PASS\n");
    return 0;
}
