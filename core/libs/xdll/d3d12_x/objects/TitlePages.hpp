#pragma once

#include "TitleAddressSpace.hpp"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace abx::d3d12x {

class TitlePages {
public:
    static std::unique_ptr<TitlePages> Start(TitleAddressSpace& space);
    ~TitlePages();
    TitlePages(const TitlePages&) = delete;
    TitlePages& operator=(const TitlePages&) = delete;

private:
    explicit TitlePages(TitleAddressSpace& space) : space_(space) {}
    void Run();

    TitleAddressSpace& space_;
    std::mutex lock_;
    std::condition_variable wake_;
    bool stop_ = false;
    std::thread thread_;
};

}
