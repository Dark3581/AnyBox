#include "xdll/chad/chad.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>

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

namespace {

constexpr std::size_t kStreamStateBytes = 4096;
constexpr std::size_t kStreamBusyOffset = 0xc;

struct DspStream {
    void* connection = nullptr;
    void* state = nullptr;
};

std::mutex g_streamLock;
std::set<DspStream*> g_streams;

}

ABX_API HRESULT XDspActivate(void* connection, const void* second, void** state, void** stream) {
    if (state == nullptr || stream == nullptr) {
        AbxUnsupported(__func__, "a null state or stream output, which the measured call sites always give");
    }
    void* const memory = VirtualAlloc(nullptr, kStreamStateBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (memory == nullptr) return E_OUTOFMEMORY;
    const std::uint32_t busy = 1;
    std::memcpy(static_cast<std::uint8_t*>(memory) + kStreamBusyOffset, &busy, sizeof(busy));
    auto* created = new DspStream{connection, memory};
    {
        const std::lock_guard guard(g_streamLock);
        g_streams.insert(created);
    }
    *state = memory;
    *stream = created;
    static std::atomic<std::uint64_t> activated{0};
    const std::uint64_t count = ++activated;
    if (count == 1 || (count & (count - 1)) == 0) {
        AbxLog("chad", "XDspActivate(%p, %p): the title gets a stream state block (%zu zeroed bytes) whose dword at +0x%zx stays 1, which its mixer reads as a block the DSP has not "
                       "finished and renders as silence (title 7952987A v1.0.2.1 reads it at exe+0x4e5ad4d and falls to exe+0x4e5afec); no DSP runs the stream (%llu so far)",
               connection, second, kStreamStateBytes, kStreamBusyOffset, static_cast<unsigned long long>(count));
    }
    return S_OK;
}

ABX_API HRESULT XDspDeactivate(void* stream) {
    auto* ours = static_cast<DspStream*>(stream);
    {
        const std::lock_guard guard(g_streamLock);
        if (g_streams.erase(ours) == 0) {
            AbxUnsupported(__func__, "a stream that XDspActivate did not hand out (the measured caller, title 7952987A v1.0.2.1 exe+0x1165fd7, passes the stream handle alone)");
        }
    }
    VirtualFree(ours->state, 0, MEM_RELEASE);
    delete ours;
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) AbxLog("chad", "XDspDeactivate(stream): the stream's state block is released (the title clears its own pointers to it right after)");
    return S_OK;
}

ABX_API HRESULT XDspSubmitCommand(void* connection, const void* command) {
    static bool reported = false;
    if (!reported) {
        reported = true;
        AbxLog("chad", "XDspSubmitCommand(%p, %p): accepted and dropped; no DSP runs commands (later submits are not logged)", connection, command);
    }
    return S_OK;
}
