#include "xdll/xapu/xapu.hpp"

#include <cstdint>

namespace {

constexpr std::size_t kSharedBytes = 64ull << 20;

struct Connection {
    void* shared = nullptr;
};

}

ABX_API HRESULT XApuConnect(const void* configuration, void** shared, void** connection) {
    if (connection == nullptr || shared == nullptr) return E_POINTER;
    auto* created = new Connection();
    created->shared = VirtualAlloc(nullptr, kSharedBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (created->shared == nullptr) {
        delete created;
        return E_OUTOFMEMORY;
    }
    *connection = created;
    *shared = created->shared;
    AbxLog("xapu", "XApuConnect(configuration %p): shared memory %p of %zu MiB (an AnyBox choice; the title's size is not measured); no audio processor reads it", configuration,
           created->shared, kSharedBytes >> 20);
    return S_OK;
}

ABX_API std::uint32_t XApuGetResultCount(void* connection) {
    static bool reported = false;
    if (!reported) {
        reported = true;
        AbxLog("xapu", "XApuGetResultCount(%p): 0, since no audio processor executes the enqueued commands", connection);
    }
    return 0;
}
