#include "xdll/chad/chad.hpp"

#include <cstdint>

namespace {

struct DspConnection {
    const void* first = nullptr;
    std::uint32_t second = 0;
    std::uint32_t third = 0;
    std::uint32_t streamLimit = 0;
    std::uint32_t fifth = 0;
};

}

ABX_API HRESULT XDspConnectWithMaximumStreamLimit(const void* first, std::uint32_t second, std::uint32_t third, std::uint32_t streamLimit, std::uint32_t fifth,
                                                  void** connection) {
    if (connection == nullptr) return E_POINTER;
    *connection = new DspConnection{first, second, third, streamLimit, fifth};
    AbxLog("chad", "XDspConnectWithMaximumStreamLimit(%p, 0x%x, 0x%x, stream limit %u, 0x%x): connection %p; no DSP processes its commands", first, second, third, streamLimit,
           fifth, *connection);
    return S_OK;
}
