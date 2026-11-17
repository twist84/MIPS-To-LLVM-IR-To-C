#include "iop_cdvd.h"
#include "iop_cpu.h"
#include "iop_hw.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace
{
int fail(const char* message)
{
    std::fprintf(stderr, "IOP CDVD: %s\n", message);
    return 1;
}

struct s_temp_file
{
    std::filesystem::path m_path;
    ~s_temp_file()
    {
        std::error_code error;
        std::filesystem::remove(m_path, error);
    }
};

bool submit_read(iop::c_hw& hardware, uint32_t lba, uint32_t count,
                 uint8_t mode = 0u, uint8_t command = 0x06u)
{
    const uint8_t params[11]{
        static_cast<uint8_t>(lba), static_cast<uint8_t>(lba >> 8u),
        static_cast<uint8_t>(lba >> 16u), static_cast<uint8_t>(lba >> 24u),
        static_cast<uint8_t>(count), static_cast<uint8_t>(count >> 8u),
        static_cast<uint8_t>(count >> 16u), static_cast<uint8_t>(count >> 24u),
        0x10u, 0x85u, mode
    };
    for (uint8_t value : params)
    {
        if (!hardware.write(iop::c_hw::k_cdvd_ncmd_status, 1u, value))
        {
            return false;
        }
    }
    return hardware.write(
        iop::c_hw::k_cdvd_ncmd_command, 1u, command
    );
}

bool arm_dma(iop::c_hw& hardware, uint32_t address, uint32_t chcr)
{
    return hardware.write(iop::c_hw::k_cdrom_dma_base, 4u, address) &&
        hardware.write(
            iop::c_hw::k_cdrom_dma_base + 4u, 4u, 0x00010200u
        ) && hardware.write(iop::c_hw::k_cdrom_dma_chcr, 4u, chcr);
}
} // namespace

int main()
{
    const auto token = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    s_temp_file fixture{
        std::filesystem::temp_directory_path() /
            ("iop-cdvd-" + std::to_string(token) + ".iso")
    };
    std::vector<uint8_t> image(18u * 2048u, 0u);
    uint8_t* pvd = image.data() + 16u * 2048u;
    pvd[0u] = 1u;
    pvd[1u] = 'C'; pvd[2u] = 'D'; pvd[3u] = '0';
    pvd[4u] = '0'; pvd[5u] = '1'; pvd[6u] = 1u;
    {
        std::ofstream output(fixture.m_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(image.data()),
                     static_cast<std::streamsize>(image.size()));
    }

    iop::c_iso_sector_source source(fixture.m_path);
    std::vector<uint8_t> sector;
    if (!source.valid() || source.sector_count() != 18u ||
        !source.read_sectors(16u, 1u, sector) || sector.size() != 2048u ||
        sector[0u] != 1u || sector[1u] != 'C' || sector[5u] != '1' ||
        source.read_sectors(18u, 1u, sector) ||
        source.read_sectors(17u, 2u, sector))
    {
        return fail("ISO reads or bounds are incorrect");
    }

    iop::c_cpu cpu;
    iop::c_hw hardware;
    cpu.bind_hardware(&hardware);
    iop::c_cdvd_dma_transport transport(cpu, hardware, source);
    if (!hardware.write(iop::c_hw::k_dma_icr, 4u, 0x00880000u) ||
        !arm_dma(hardware, 0x0004af00u, 0x41000200u) ||
        !submit_read(hardware, 16u, 1u))
    {
        return fail("valid READ/DMA request was rejected");
    }
    uint32_t word = 0u;
    uint32_t chcr = 0u;
    uint32_t cdvd_stat = 0u;
    uint32_t i_stat = 0u;
    if (transport.bytes_transferred() != 2048u ||
        transport.last_request().m_lba != 16u ||
        !cpu.read32(0x0004af00u, &word) || word != 0x30444301u ||
        !hardware.read(iop::c_hw::k_cdrom_dma_chcr, 4u, &chcr) ||
        (chcr & 0x01000000u) != 0u ||
        !hardware.read(iop::c_hw::k_i_stat, 4u, &i_stat) ||
        (i_stat & ((1u << 2u) | (1u << 3u))) !=
            ((1u << 2u) | (1u << 3u)) ||
        !hardware.read(iop::c_hw::k_cdvd_i_stat, 1u, &cdvd_stat) ||
        cdvd_stat != 1u || hardware.cdvd_ncmd_pending())
    {
        return fail("sector DMA completion or guest-visible bytes are wrong");
    }

    iop::c_hw no_media;
    if (!arm_dma(no_media, 0x1000u, 0x41000200u) ||
        !submit_read(no_media, 16u, 1u) ||
        !no_media.read(iop::c_hw::k_cdrom_dma_chcr, 4u, &chcr) ||
        (chcr & 0x01000000u) == 0u || !no_media.cdvd_ncmd_pending() ||
        no_media.pending_interrupts() != 0u)
    {
        return fail("missing media fabricated completion");
    }

    iop::c_cpu wrong_direction_cpu;
    iop::c_hw wrong_direction_hw;
    wrong_direction_cpu.bind_hardware(&wrong_direction_hw);
    iop::c_cdvd_dma_transport wrong_direction_transport(
        wrong_direction_cpu, wrong_direction_hw, source
    );
    if (!arm_dma(wrong_direction_hw, 0x2000u, 0x41000201u) ||
        !submit_read(wrong_direction_hw, 16u, 1u) ||
        !wrong_direction_hw.read(
            iop::c_hw::k_cdrom_dma_chcr, 4u, &chcr
        ) || (chcr & 0x01000000u) == 0u ||
        !wrong_direction_hw.cdvd_ncmd_pending() ||
        wrong_direction_transport.bytes_transferred() != 0u ||
        wrong_direction_hw.pending_interrupts() != 0u)
    {
        return fail("wrong DMA direction completed a disc-to-RAM transfer");
    }

    iop::c_cpu unsupported_cpu;
    iop::c_hw unsupported_hw;
    unsupported_cpu.bind_hardware(&unsupported_hw);
    iop::c_cdvd_dma_transport unsupported_transport(
        unsupported_cpu, unsupported_hw, source
    );
    if (!arm_dma(unsupported_hw, 0x3000u, 0x41000200u) ||
        !submit_read(unsupported_hw, 16u, 1u, 1u) ||
        unsupported_transport.bytes_transferred() != 0u ||
        !unsupported_hw.cdvd_ncmd_pending() ||
        unsupported_hw.pending_interrupts() != 0u)
    {
        return fail("unsupported CDVD data mode fabricated completion");
    }

    iop::c_cpu wrong_command_cpu;
    iop::c_hw wrong_command_hw;
    wrong_command_cpu.bind_hardware(&wrong_command_hw);
    iop::c_cdvd_dma_transport wrong_command_transport(
        wrong_command_cpu, wrong_command_hw, source
    );
    if (!arm_dma(wrong_command_hw, 0x4000u, 0x41000200u) ||
        !submit_read(wrong_command_hw, 16u, 1u, 0u, 0x07u) ||
        wrong_command_transport.bytes_transferred() != 0u ||
        !wrong_command_hw.cdvd_ncmd_pending() ||
        wrong_command_hw.pending_interrupts() != 0u)
    {
        return fail("unsupported CDVD command fabricated completion");
    }

    hardware.reset();
    if (hardware.cdvd_ncmd_pending() ||
        !hardware.read(iop::c_hw::k_cdrom_dma_chcr, 4u, &chcr) || chcr != 0u)
    {
        return fail("reset retained pending CDVD state");
    }
    std::printf("IOP CDVD sector DMA: PASS\n");
    return 0;
}
