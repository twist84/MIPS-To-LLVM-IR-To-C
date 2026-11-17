#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace iop
{
class c_cpu;
class c_hw;

class c_sector_source
{
public:
    virtual ~c_sector_source() = default;
    virtual uint64_t sector_count() const = 0;
    virtual bool read_sectors(
        uint32_t lba, uint32_t count, std::vector<uint8_t>& data
    ) = 0;
};

class c_iso_sector_source final : public c_sector_source
{
public:
    static constexpr uint32_t k_sector_size = 2048u;

    explicit c_iso_sector_source(const std::filesystem::path& path);
    bool valid() const;
    uint64_t sector_count() const override;
    bool read_sectors(
        uint32_t lba, uint32_t count, std::vector<uint8_t>& data
    ) override;

private:
    std::ifstream m_input;
    uint64_t m_sector_count = 0u;
};

struct s_cdvd_read_request
{
    uint32_t m_lba = 0u;
    uint32_t m_sector_count = 0u;
    uint8_t m_retry_count = 0u;
    uint8_t m_spindle_control = 0u;
    uint8_t m_data_mode = 0u;
};

class c_cdvd_dma_transport
{
public:
    c_cdvd_dma_transport(c_cpu& cpu, c_hw& hardware,
                         c_sector_source& source);
    ~c_cdvd_dma_transport();
    static bool decode_read(
        const c_hw& hardware, s_cdvd_read_request* request
    );
    bool try_transfer();
    uint32_t bytes_transferred() const;
    const s_cdvd_read_request& last_request() const;

private:
    static void start_callback(void* user);

    c_cpu& m_cpu;
    c_hw& m_hardware;
    c_sector_source& m_source;
    s_cdvd_read_request m_last_request{};
    uint32_t m_bytes_transferred = 0u;
};
} // namespace iop
