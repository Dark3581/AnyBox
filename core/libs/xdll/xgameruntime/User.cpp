#include "xdll/xgameruntime/xgameruntime.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>

struct XUser {
    std::atomic<ULONG> references{1};
    std::uint64_t localId = 0;
};

namespace {

XUser* LocalUser() {
    static XUser* user = [] {
        auto* created = new XUser();
        created->localId = 1;
        AbxLog("xgameruntime", "XUserAddAsync: AnyBox signs in one offline local user (local id 1); it has no online identity");
        return created;
    }();
    return user;
}

HRESULT AddProvider(XAsyncOp op, const XAsyncProviderData* data) {
    switch (op) {
    case XAsyncOp::Begin:
        return XAsyncSchedule(data->async, 0);
    case XAsyncOp::DoWork:
        XAsyncComplete(data->async, S_OK, sizeof(XUserHandle));
        return S_OK;
    case XAsyncOp::GetResult: {
        if (data->buffer == nullptr || data->bufferSize < sizeof(XUserHandle)) return E_NOT_SUFFICIENT_BUFFER;
        XUser* user = LocalUser();
        ++user->references;
        std::memcpy(data->buffer, &user, sizeof(user));
        return S_OK;
    }
    default:
        return S_OK;
    }
}

}

ABX_API HRESULT XUserAddAsync(std::uint32_t options, XAsyncBlock* async) {
    if (async == nullptr) return E_INVALIDARG;
    AbxTrace("XUserAddAsync(options 0x%x)", options);
    return XAsyncBegin(async, nullptr, reinterpret_cast<const void*>(&XUserAddAsync), __func__, AddProvider);
}

ABX_API HRESULT XUserAddResult(XAsyncBlock* async, XUserHandle* user) {
    if (user == nullptr) return E_POINTER;
    *user = nullptr;
    return XAsyncGetResult(async, reinterpret_cast<const void*>(&XUserAddAsync), sizeof(*user), user, nullptr);
}

ABX_API HRESULT AbxUserGetIdentifier(XUserHandle user, std::uint64_t* value) {
    if (user == nullptr || value == nullptr) return E_INVALIDARG;
    *value = user->localId;
    return S_OK;
}

ABX_API HRESULT AbxUserDuplicateHandle(XUserHandle user, XUserHandle* duplicated) {
    if (user == nullptr || duplicated == nullptr) return E_INVALIDARG;
    ++user->references;
    *duplicated = user;
    return S_OK;
}

ABX_API void AbxUserCloseHandle(XUserHandle user) {
    if (user != nullptr && user->references > 1) --user->references;
}

ABX_API HRESULT XUserGetGamertag(XUserHandle user, std::uint32_t component, std::size_t gamertagSize, char* gamertag, std::size_t* gamertagUsed) {
    static constexpr char kGamertag[] = "Player";
    if (user == nullptr || gamertag == nullptr) return E_INVALIDARG;
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) {
        AbxLog("xgameruntime", "XUserGetGamertag(component %u): the offline user is called \"%s\" (an AnyBox choice)", component, kGamertag);
    }
    if (gamertagSize < sizeof(kGamertag)) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    std::memcpy(gamertag, kGamertag, sizeof(kGamertag));
    if (gamertagUsed != nullptr) *gamertagUsed = sizeof(kGamertag);
    return S_OK;
}

ABX_API HRESULT XUserGetAgeGroup(XUserHandle user, std::uint32_t* ageGroup) {
    constexpr std::uint32_t kAdult = 3;
    if (user == nullptr || ageGroup == nullptr) return E_INVALIDARG;
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true)) AbxLog("xgameruntime", "XUserGetAgeGroup: the offline user is an adult (3, an AnyBox choice)");
    *ageGroup = kAdult;
    return S_OK;
}

ABX_API HRESULT XUserGetIsGuest(XUserHandle user, bool* isGuest) {
    if (user == nullptr || isGuest == nullptr) return E_INVALIDARG;
    *isGuest = false;
    return S_OK;
}

ABX_API HRESULT XUserCheckPrivilege(XUserHandle user, std::uint32_t options, std::uint32_t privilege, bool* hasPrivilege, std::uint32_t* reasons) {
    if (user == nullptr || hasPrivilege == nullptr) return E_INVALIDARG;
    static std::mutex lock;
    static std::set<std::uint32_t> reported;
    {
        const std::lock_guard guard(lock);
        if (reported.insert(privilege).second) {
            AbxLog("xgameruntime", "XUserCheckPrivilege(options 0x%x, privilege %u): granted to the offline user (an AnyBox choice; online features then fail on connectivity)", options,
                   privilege);
        }
    }
    *hasPrivilege = true;
    if (reasons != nullptr) *reasons = 0;
    return S_OK;
}
