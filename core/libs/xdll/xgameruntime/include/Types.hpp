#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <ctime>

struct XTaskQueueObject;
using XTaskQueueHandle = XTaskQueueObject*;

enum class XTaskQueueDispatchMode : std::uint32_t {
    Manual,
    ThreadPool,
    SerializedThreadPool,
    Immediate,
};

enum class XTaskQueuePort : std::uint32_t {
    Work,
    Completion,
};

struct XTaskQueuePortObject;
using XTaskQueuePortHandle = XTaskQueuePortObject*;

struct XTaskQueueRegistrationToken {
    std::uint64_t token;
};

using XTaskQueueCallback = void(void* context, bool canceled);
using XTaskQueueMonitorCallback = void(void* context, XTaskQueueHandle queue, XTaskQueuePort port);
using XTaskQueueTerminatedCallback = void(void* context);

inline constexpr std::uint32_t XTaskQueueNoWait = 0;
inline constexpr std::uint32_t XTaskQueueInfinite = 0xffffffffu;

struct XAsyncBlock;

using XAsyncCompletionRoutine = void(XAsyncBlock* asyncBlock);
using XAsyncWork = HRESULT(XAsyncBlock* asyncBlock);

struct XAsyncBlock {
    XTaskQueueHandle queue;
    void* context;
    XAsyncCompletionRoutine* callback;
    unsigned char internal[sizeof(void*) * 4];
};

enum class XAsyncOp : std::uint32_t {
    Begin,
    DoWork,
    GetResult,
    Cancel,
    Cleanup,
};

struct XAsyncProviderData {
    XAsyncBlock* async;
    std::size_t bufferSize;
    void* buffer;
    void* context;
};

using XAsyncProvider = HRESULT(XAsyncOp op, const XAsyncProviderData* data);

enum class XSystemDeviceType : std::uint32_t {
    Unknown = 0x00,
    Pc = 0x01,
    XboxOne = 0x02,
    XboxOneS = 0x03,
    XboxOneX = 0x04,
    XboxOneXDevkit = 0x05,
    XboxScarlettLockhart = 0x06,
    XboxScarlettAnaconda = 0x07,
    XboxScarlettDevkit = 0x08,
};

enum class XGameRuntimeGameConfigSource : std::uint32_t {
    Default,
    Inline,
    File,
};

struct XGameRuntimeOptions {
    XGameRuntimeGameConfigSource gameConfigSource;
    const char* gameConfig;
};

enum class XGameRuntimeFeature : std::uint32_t {
    XAccessibility = 0,
    XAppCapture = 1,
    XAsync = 2,
    XAsyncProvider = 3,
    XDisplay = 4,
    XGame = 5,
    XGameInvite = 6,
    XGameSave = 7,
    XGameUI = 8,
    XLauncher = 9,
    XNetworking = 10,
    XPackage = 11,
    XPersistentLocalStorage = 12,
    XSpeechSynthesizer = 13,
    XStore = 14,
    XSystem = 15,
    XTaskQueue = 16,
    XThread = 17,
    XUser = 18,
    XError = 19,
    XGameEvent = 20,
    XGameStreaming = 21,
};

inline constexpr HRESULT E_GAMEPACKAGE_APP_NOT_PACKAGED = static_cast<HRESULT>(0x89245200u);

struct XPackageMount;
using XPackageMountHandle = XPackageMount*;

struct XPersistentLocalStorageSpaceInfo {
    std::uint64_t availableFreeBytes;
    std::uint64_t totalFreeBytes;
    std::uint64_t usedBytes;
    std::uint64_t totalBytes;
};

struct XUser;
using XUserHandle = XUser*;

struct XGameSaveProvider;
using XGameSaveProviderHandle = XGameSaveProvider*;
struct XGameSaveContainer;
using XGameSaveContainerHandle = XGameSaveContainer*;
struct XGameSaveUpdate;
using XGameSaveUpdateHandle = XGameSaveUpdate*;

struct XGameSaveContainerInfo {
    const char* name;
    const char* displayName;
    std::uint32_t blobCount;
    std::uint64_t totalSize;
    std::time_t lastModifiedTime;
    bool needsSync;
};

struct XGameSaveBlobInfo {
    const char* name;
    std::uint32_t size;
};

struct XGameSaveBlob {
    XGameSaveBlobInfo info;
    std::uint8_t* data;
};

using XGameSaveContainerInfoCallback = bool(const XGameSaveContainerInfo* info, void* context);
using XGameSaveBlobInfoCallback = bool(const XGameSaveBlobInfo* info, void* context);

inline constexpr std::size_t XSystemConsoleIdBytes = 39;
inline constexpr std::size_t XSystemXboxLiveSandboxIdMaxBytes = 16;
