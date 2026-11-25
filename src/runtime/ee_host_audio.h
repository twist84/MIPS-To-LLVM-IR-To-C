#pragma once

#include <memory>

namespace iop
{
class c_hw;
}

namespace ee
{
// Host-only lifetime. Declare after the IOP kernel so its sink is detached
// before guest hardware is destroyed, including on early returns.
class c_ee_host_audio final
{
public:
    c_ee_host_audio();
    ~c_ee_host_audio();
    c_ee_host_audio(const c_ee_host_audio&) = delete;
    c_ee_host_audio& operator=(const c_ee_host_audio&) = delete;

    void start(iop::c_hw* hardware, bool profile = false);
    void pause();
    void resume();
    void shutdown();
    void report() const;

private:
    struct s_impl;
    std::unique_ptr<s_impl> m_impl;
    const char* m_status = "disabled";
};
}
