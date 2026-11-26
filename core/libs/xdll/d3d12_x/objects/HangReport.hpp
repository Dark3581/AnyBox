#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace abx::d3d12x {

class TitleAddressSpace;
class VulkanDevice;

class HangReport {
public:
    static std::unique_ptr<HangReport> Start(const VulkanDevice& device, const TitleAddressSpace* space);
    ~HangReport();
    HangReport(const HangReport&) = delete;
    HangReport& operator=(const HangReport&) = delete;
    static void NotePresent();

private:
    HangReport(const VulkanDevice& device, const TitleAddressSpace* space, std::uint64_t seconds) : device_(device), space_(space), seconds_(seconds) {}
    void Run();
    void Report(std::uint64_t idleMilliseconds, std::uint64_t presents, bool secondLook);

    const VulkanDevice& device_;
    const TitleAddressSpace* space_;
    std::uint64_t seconds_;
    std::mutex lock_;
    std::condition_variable wake_;
    bool stop_ = false;
    std::thread thread_;
};

}
