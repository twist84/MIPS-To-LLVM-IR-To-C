#include "iop_cdvd.h"

#include "iop_cpu.h"
#include "iop_hw.h"

#include <limits>
#include <cstdio>

namespace iop
{
c_iso_sector_source::c_iso_sector_source(const std::filesystem::path& path)
    : m_input(path, std::ios::binary | std::ios::ate)
{
    if (!m_input)
    {
        return;
    }
    const std::streampos size = m_input.tellg();
    if (size <= 0 || size % k_sector_size != 0)
    {
        m_input.close();
        return;
    }
    m_sector_count = static_cast<uint64_t>(size) / k_sector_size;
}

bool c_iso_sector_source::valid() const
{
    return m_input.is_open() && m_sector_count != 0u;
}

uint64_t c_iso_sector_source::sector_count() const
{
    return m_sector_count;
}

bool c_iso_sector_source::read_sectors(
    uint32_t lba, uint32_t count, std::vector<uint8_t>& data
)
{
    const uint64_t end = static_cast<uint64_t>(lba) + count;
    if (!valid() || count == 0u || end > m_sector_count ||
        count > (std::numeric_limits<uint32_t>::max)() / k_sector_size)
    {
        return false;
    }
    data.resize(static_cast<size_t>(count) * k_sector_size);
    m_input.clear();
    m_input.seekg(static_cast<std::streamoff>(lba) * k_sector_size);
    return static_cast<bool>(m_input.read(
        reinterpret_cast<char*>(data.data()),
        static_cast<std::streamsize>(data.size())
    ));
}

c_cdvd_dma_transport::c_cdvd_dma_transport(
    c_cpu& cpu, c_hw& hardware, c_sector_source& source
) : m_cpu(cpu), m_hardware(hardware), m_source(source)
{
    m_hardware.bind_cdvd_start(this, start_callback);
}

c_cdvd_dma_transport::~c_cdvd_dma_transport()
{
    m_hardware.bind_cdvd_start(nullptr, nullptr);
}

bool c_cdvd_dma_transport::decode_read(
    const c_hw& hardware, s_cdvd_read_request* request
)
{
    if (request == nullptr || !hardware.cdvd_ncmd_pending() ||
        hardware.cdvd_ncmd_command() != 0x06u ||
        hardware.cdvd_ncmd_parameter_count() != 11u)
    {
        return false;
    }
    uint8_t bytes[11]{};
    for (uint32_t index = 0u; index < 11u; ++index)
    {
        if (!hardware.cdvd_ncmd_parameter(index, &bytes[index]))
        {
            return false;
        }
    }
    request->m_lba = static_cast<uint32_t>(bytes[0u]) |
        (static_cast<uint32_t>(bytes[1u]) << 8u) |
        (static_cast<uint32_t>(bytes[2u]) << 16u) |
        (static_cast<uint32_t>(bytes[3u]) << 24u);
    request->m_sector_count = static_cast<uint32_t>(bytes[4u]) |
        (static_cast<uint32_t>(bytes[5u]) << 8u) |
        (static_cast<uint32_t>(bytes[6u]) << 16u) |
        (static_cast<uint32_t>(bytes[7u]) << 24u);
    request->m_retry_count = bytes[8u];
    request->m_spindle_control = bytes[9u];
    request->m_data_mode = bytes[10u];
    return request->m_sector_count != 0u && request->m_data_mode == 0u;
}

void c_cdvd_dma_transport::start_callback(void* user)
{
    static_cast<c_cdvd_dma_transport*>(user)->try_transfer();
}

bool c_cdvd_dma_transport::try_transfer()
{
    uint32_t madr = 0u;
    uint32_t bcr = 0u;
    uint32_t chcr = 0u;
    s_cdvd_read_request request{};
    if (!decode_read(m_hardware, &request) ||
        !m_hardware.cdrom_dma_register(0u, &madr) ||
        !m_hardware.cdrom_dma_register(1u, &bcr) ||
        !m_hardware.cdrom_dma_register(2u, &chcr) ||
        (chcr & 0x01000000u) == 0u || (chcr & 1u) != 0u ||
        ((chcr >> 9u) & 3u) != 1u)
    {
        return false;
    }
    const uint32_t words = (bcr & 0xffffu) * (bcr >> 16u);
    const uint64_t dma_bytes = static_cast<uint64_t>(words) * 4u;
    const uint64_t read_bytes =
        static_cast<uint64_t>(request.m_sector_count) *
        c_iso_sector_source::k_sector_size;
    if (words == 0u || dma_bytes != read_bytes ||
        read_bytes > c_cpu::k_ram_size)
    {
        return false;
    }
    std::vector<uint8_t> data;
    if (!m_source.read_sectors(
            request.m_lba, request.m_sector_count, data) ||
        data.size() != read_bytes ||
        !m_cpu.dma_write(madr, data.data(), static_cast<uint32_t>(data.size())) ||
        !m_hardware.complete_cdvd_read(0x01u))
    {
        return false;
    }
    m_last_request = request;
    m_bytes_transferred = static_cast<uint32_t>(data.size());
    std::printf(
        "CDVD READ DMA3 complete: lba=%u sectors=%u mode=0x%02x "
        "madr=0x%08x bcr=0x%08x chcr=0x%08x bytes=%u "
        "prefix=%02x%02x%02x%02x%02x%02x%02x\n",
        request.m_lba, request.m_sector_count, request.m_data_mode,
        madr, bcr, chcr, m_bytes_transferred,
        data[0u], data[1u], data[2u], data[3u],
        data[4u], data[5u], data[6u]
    );
    return true;
}

uint32_t c_cdvd_dma_transport::bytes_transferred() const
{
    return m_bytes_transferred;
}

const s_cdvd_read_request& c_cdvd_dma_transport::last_request() const
{
    return m_last_request;
}
} // namespace iop
