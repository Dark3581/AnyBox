#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

struct AsyncBlock {
    void* queue;
    void* context;
    void (*callback)(AsyncBlock* async);
    unsigned char internal[sizeof(void*) * 4];
};

struct AsyncProviderData {
    AsyncBlock* async;
    std::size_t bufferSize;
    void* buffer;
    void* context;
};

using AsyncProvider = HRESULT (*)(std::uint32_t op, const AsyncProviderData* data);
using AsyncWork = HRESULT (*)(AsyncBlock* async);

struct SaveContainerInfo {
    const char* name;
    const char* displayName;
    std::uint32_t blobCount;
    std::uint64_t totalSize;
    std::int64_t lastModifiedTime;
    bool needsSync;
};

struct SaveBlobInfo {
    const char* name;
    std::uint32_t size;
};

struct SaveBlob {
    SaveBlobInfo info;
    std::uint8_t* data;
};

struct RegistrationToken {
    std::uint64_t token;
};

struct StorageSpace {
    std::uint64_t availableFreeBytes;
    std::uint64_t totalFreeBytes;
    std::uint64_t usedBytes;
    std::uint64_t totalBytes;
};

extern "C" {
__declspec(dllimport) HRESULT XAsyncBegin(AsyncBlock* async, void* context, const void* identity, const char* identityName, AsyncProvider provider);
__declspec(dllimport) HRESULT XAsyncSchedule(AsyncBlock* async, std::uint32_t delayMs);
__declspec(dllimport) void XAsyncComplete(AsyncBlock* async, HRESULT result, std::size_t requiredBufferSize);
__declspec(dllimport) void XAsyncCancel(AsyncBlock* async);
__declspec(dllimport) HRESULT XAsyncRun(AsyncBlock* async, AsyncWork work);
__declspec(dllimport) HRESULT XAsyncGetStatus(AsyncBlock* async, bool wait);
__declspec(dllimport) HRESULT XAsyncGetResultSize(AsyncBlock* async, std::size_t* bufferSize);
__declspec(dllimport) HRESULT XAsyncGetResult(AsyncBlock* async, const void* identity, std::size_t bufferSize, void* buffer, std::size_t* bufferUsed);
__declspec(dllimport) HRESULT XGameRuntimeInitialize();
__declspec(dllimport) HRESULT XGameGetXboxTitleId(std::uint32_t* titleId);
__declspec(dllimport) HRESULT XTaskQueueCreate(std::uint32_t work, std::uint32_t completion, void** queue);
__declspec(dllimport) HRESULT XTaskQueueSubmitCallback(void* queue, std::uint32_t port, void* context, void (*callback)(void*, bool));
__declspec(dllimport) HRESULT XTaskQueueSubmitDelayedCallback(void* queue, std::uint32_t port, std::uint32_t delayMs, void* context, void (*callback)(void*, bool));
__declspec(dllimport) bool XTaskQueueDispatch(void* queue, std::uint32_t port, std::uint32_t timeoutMs);
__declspec(dllimport) HRESULT XTaskQueueTerminate(void* queue, bool wait, void* context, void (*callback)(void*));
__declspec(dllimport) void XTaskQueueCloseHandle(void* queue);
__declspec(dllimport) bool XTaskQueueGetCurrentProcessTaskQueue(void** queue);
__declspec(dllimport) void XTaskQueueSetCurrentProcessTaskQueue(void* queue);
__declspec(dllimport) HRESULT XTaskQueueCreateComposite(void* workPort, void* completionPort, void** queue);
__declspec(dllimport) HRESULT XTaskQueueGetPort(void* queue, std::uint32_t port, void** portHandle);
__declspec(dllimport) HRESULT XTaskQueueRegisterMonitor(void* queue, void* context, void (*callback)(void*, void*, std::uint32_t), RegistrationToken* token);
__declspec(dllimport) void XTaskQueueUnregisterMonitor(void* queue, RegistrationToken token);
__declspec(dllimport) HRESULT XTaskQueueRegisterWaiter(void* queue, std::uint32_t port, HANDLE waitHandle, void* context, void (*callback)(void*, bool), RegistrationToken* token);
__declspec(dllimport) void XTaskQueueUnregisterWaiter(void* queue, RegistrationToken token);
__declspec(dllimport) HRESULT XUserAddAsync();
__declspec(dllimport) std::uint32_t XSystemGetDeviceType();
__declspec(dllimport) bool XPackageIsPackagedProcess();
__declspec(dllimport) HRESULT XGameRuntimeInitializeWithOptions(const void* options);
__declspec(dllimport) bool XGameRuntimeIsFeatureAvailable(std::uint32_t feature);
__declspec(dllimport) HRESULT XPackageGetCurrentProcessPackageIdentifier(std::size_t bufferSize, char* buffer);
__declspec(dllimport) HRESULT XThreadSetTimeSensitive(bool isTimeSensitiveThread);
__declspec(dllimport) bool XThreadIsTimeSensitive();
__declspec(dllimport) void XThreadAssertNotTimeSensitive();
__declspec(dllimport) HRESULT XPersistentLocalStorageGetPath(std::size_t pathSize, char* path, std::size_t* pathUsed);
__declspec(dllimport) HRESULT XPersistentLocalStorageGetPathSize(std::size_t* pathSize);
__declspec(dllimport) HRESULT XPersistentLocalStorageGetSpaceInfo(struct StorageSpace* info);
__declspec(dllimport) HRESULT XPersistentLocalStoragePromptUserForSpaceAsync(std::uint64_t requestedBytes, AsyncBlock* async);
__declspec(dllimport) HRESULT XPersistentLocalStoragePromptUserForSpaceResult(AsyncBlock* async);
__declspec(dllimport) HRESULT XGameSaveInitializeProvider(void* user, const char* configurationId, bool syncOnDemand, void** provider);
__declspec(dllimport) HRESULT XGameSaveInitializeProviderAsync(void* user, const char* configurationId, bool syncOnDemand, AsyncBlock* async);
__declspec(dllimport) HRESULT XGameSaveInitializeProviderResult(AsyncBlock* async, void** provider);
__declspec(dllimport) void XGameSaveCloseProvider(void* provider);
__declspec(dllimport) HRESULT XGameSaveGetRemainingQuota(void* provider, std::int64_t* remaining);
__declspec(dllimport) HRESULT XGameSaveGetRemainingQuotaAsync(void* provider, AsyncBlock* async);
__declspec(dllimport) HRESULT XGameSaveGetRemainingQuotaResult(AsyncBlock* async, std::int64_t* remaining);
__declspec(dllimport) HRESULT XGameSaveDeleteContainer(void* provider, const char* name);
__declspec(dllimport) HRESULT XGameSaveDeleteContainerAsync(void* provider, const char* name, AsyncBlock* async);
__declspec(dllimport) HRESULT XGameSaveDeleteContainerResult(AsyncBlock* async);
__declspec(dllimport) HRESULT XGameSaveCreateContainer(void* provider, const char* name, void** container);
__declspec(dllimport) void XGameSaveCloseContainer(void* container);
__declspec(dllimport) HRESULT XGameSaveGetContainerInfo(void* provider, const char* name, void* context, bool (*callback)(const struct SaveContainerInfo*, void*));
__declspec(dllimport) HRESULT XGameSaveEnumerateContainerInfo(void* provider, void* context, bool (*callback)(const struct SaveContainerInfo*, void*));
__declspec(dllimport) HRESULT XGameSaveEnumerateContainerInfoByName(void* provider, const char* prefix, void* context, bool (*callback)(const struct SaveContainerInfo*, void*));
__declspec(dllimport) HRESULT XGameSaveEnumerateBlobInfo(void* container, void* context, bool (*callback)(const struct SaveBlobInfo*, void*));
__declspec(dllimport) HRESULT XGameSaveEnumerateBlobInfoByName(void* container, const char* prefix, void* context, bool (*callback)(const struct SaveBlobInfo*, void*));
__declspec(dllimport) HRESULT XGameSaveReadBlobData(void* container, const char** names, std::uint32_t* count, std::size_t size, struct SaveBlob* blobs);
__declspec(dllimport) HRESULT XGameSaveReadBlobDataAsync(void* container, const char** names, std::uint32_t count, AsyncBlock* async);
__declspec(dllimport) HRESULT XGameSaveReadBlobDataResult(AsyncBlock* async, std::size_t size, struct SaveBlob* blobs, std::uint32_t* count);
__declspec(dllimport) HRESULT XGameSaveCreateUpdate(void* container, const char* displayName, void** update);
__declspec(dllimport) void XGameSaveCloseUpdate(void* update);
__declspec(dllimport) HRESULT XGameSaveSubmitBlobWrite(void* update, const char* name, const std::uint8_t* data, std::size_t size);
__declspec(dllimport) HRESULT XGameSaveSubmitBlobDelete(void* update, const char* name);
__declspec(dllimport) HRESULT XGameSaveSubmitUpdate(void* update);
__declspec(dllimport) HRESULT XGameSaveSubmitUpdateAsync(void* update, AsyncBlock* async);
__declspec(dllimport) HRESULT XGameSaveSubmitUpdateResult(AsyncBlock* async);
__declspec(dllimport) HRESULT XGameSaveFilesGetFolderWithUiAsync(void* user, const char* configurationId, AsyncBlock* async);
__declspec(dllimport) HRESULT XGameSaveFilesGetFolderWithUiResult(AsyncBlock* async, std::size_t folderSize, char* folder);
__declspec(dllimport) HRESULT XGameSaveFilesGetRemainingQuota(void* user, const char* configurationId, std::int64_t* remaining);
__declspec(dllimport) HRESULT XSystemGetConsoleId(std::size_t consoleIdSize, char* consoleId, std::size_t* consoleIdUsed);
__declspec(dllimport) HRESULT XSystemGetXboxLiveSandboxId(std::size_t sandboxIdSize, char* sandboxId, std::size_t* sandboxIdUsed);
}

struct RuntimeOptions {
    std::uint32_t gameConfigSource;
    const char* gameConfig;
};

namespace {

std::atomic<int> g_ran{0};
std::atomic<int> g_canceled{0};
bool g_terminated = false;

void Count(void*, bool canceled) {
    (canceled ? g_canceled : g_ran).fetch_add(1);
}

void Terminated(void*) {
    g_terminated = true;
}

LPTOP_LEVEL_EXCEPTION_FILTER g_titleFilter = nullptr;

LONG WINAPI ChainingFilter(EXCEPTION_POINTERS* pointers) {
    std::fputs("title filter\n", stdout);
    std::fflush(stdout);
    return g_titleFilter != nullptr ? g_titleFilter(pointers) : EXCEPTION_CONTINUE_SEARCH;
}

void* g_readOnlyPage = nullptr;

LONG WINAPI RecoveringFilter(EXCEPTION_POINTERS* pointers) {
    const EXCEPTION_RECORD* record = pointers->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2
        || reinterpret_cast<void*>(record->ExceptionInformation[1]) != g_readOnlyPage) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    DWORD old = 0;
    return VirtualProtect(g_readOnlyPage, 4096, PAGE_READWRITE, &old) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

int Fail(const char* what) {
    std::printf("FAIL %s\n", what);
    return 1;
}

int TaskQueue() {
    if (FAILED(XGameRuntimeInitialize())) return Fail("XGameRuntimeInitialize");
    std::uint32_t titleId = 0;
    if (FAILED(XGameGetXboxTitleId(&titleId)) || titleId != 0x1234ABCDu) return Fail("XGameGetXboxTitleId");
    void* manual = nullptr;
    if (FAILED(XTaskQueueCreate(0, 0, &manual))) return Fail("create manual");
    XTaskQueueSubmitCallback(manual, 0, nullptr, Count);
    XTaskQueueSubmitCallback(manual, 1, nullptr, Count);
    if (!XTaskQueueDispatch(manual, 0, 0) || !XTaskQueueDispatch(manual, 1, 0)) return Fail("manual dispatch");
    if (XTaskQueueDispatch(manual, 0, 0)) return Fail("empty port dispatched");
    XTaskQueueSubmitDelayedCallback(manual, 0, 20, nullptr, Count);
    if (!XTaskQueueDispatch(manual, 0, 5000)) return Fail("delayed dispatch");
    XTaskQueueSubmitCallback(manual, 0, nullptr, Count);
    XTaskQueueSubmitDelayedCallback(manual, 0, 60000, nullptr, Count);
    if (FAILED(XTaskQueueTerminate(manual, true, nullptr, Terminated)) || !g_terminated) return Fail("terminate");
    if (XTaskQueueSubmitCallback(manual, 0, nullptr, Count) != E_ABORT) return Fail("submit after terminate");
    XTaskQueueCloseHandle(manual);
    void* pool = nullptr;
    if (FAILED(XTaskQueueCreate(1, 3, &pool))) return Fail("create pool");
    for (int i = 0; i < 16; ++i) XTaskQueueSubmitCallback(pool, 0, nullptr, Count);
    XTaskQueueSubmitCallback(pool, 1, nullptr, Count);
    g_terminated = false;
    XTaskQueueTerminate(pool, true, nullptr, Terminated);
    XTaskQueueCloseHandle(pool);
    if (g_ran.load() != 20 || g_canceled.load() != 2) {
        std::printf("FAIL counts ran=%d canceled=%d\n", g_ran.load(), g_canceled.load());
        return 1;
    }
    std::printf("task queue ok\n");
    return 0;
}

enum : std::uint32_t { OpBegin, OpDoWork, OpGetResult, OpCancel, OpCleanup };

constexpr HRESULT RunResult = static_cast<HRESULT>(0x12345678);
constexpr HRESULT WorkFailure = static_cast<HRESULT>(0x8009ABCDu);
constexpr HRESULT IllegalMethodCall = static_cast<HRESULT>(0x8000000Eu);
constexpr HRESULT NotSufficientBuffer = static_cast<HRESULT>(0x8007007Au);
constexpr HRESULT NoTaskQueue = static_cast<HRESULT>(0x800701ABu);
constexpr std::uint32_t Payload = 0xC0FFEE42u;
constexpr std::uint32_t ScheduleDelayMs = 200;
constexpr DWORD DispatchTimeoutMs = 10000;
constexpr DWORD WatchdogMs = 20000;
constexpr int SwarmSize = 1000;
constexpr int Repeats = 500;

const char g_identity = 0;
const char g_otherIdentity = 0;
std::atomic<void*> g_workContext{nullptr};

struct Recorder {
    std::uint32_t ops[16] = {};
    int count = 0;
    HRESULT workResult = S_OK;
    std::size_t resultBufferSize = 0;
    std::uint32_t delay = 0;
    int steps = 0;

    void Record(std::uint32_t op) {
        if (count < 16) ops[count] = op;
        ++count;
    }

    bool Is(std::initializer_list<std::uint32_t> expected) const {
        if (count != static_cast<int>(expected.size())) return false;
        int index = 0;
        for (std::uint32_t op : expected) {
            if (ops[index++] != op) return false;
        }
        return true;
    }
};

struct Completion {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT status = E_FAIL;
    DWORD thread = 0;
    std::atomic<bool> returned{false};
    ~Completion() { CloseHandle(event); }
};

struct Counter {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<int> count{0};
    std::atomic<int> failures{0};
    int target = 0;
    ~Counter() { CloseHandle(event); }
};

struct Completer {
    HANDLE request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    AsyncBlock* block = nullptr;
    std::atomic<bool> stop{false};
    ~Completer() { CloseHandle(request); CloseHandle(done); }
};

struct ResultInCallback {
    Recorder recorder;
    HRESULT waited = E_FAIL;
    HRESULT result = E_FAIL;
    std::uint32_t value = 0;
    std::size_t used = 0;
    int opsInCallback = 0;
};

HRESULT ReturnWork(AsyncBlock* async) {
    g_workContext = async->context;
    return RunResult;
}

HRESULT PayloadProvider(std::uint32_t op, const AsyncProviderData* data) {
    auto* recorder = static_cast<Recorder*>(data->context);
    recorder->Record(op);
    switch (op) {
    case OpBegin:
        return XAsyncSchedule(data->async, 0);
    case OpDoWork:
        XAsyncComplete(data->async, S_OK, sizeof(Payload));
        return S_OK;
    case OpGetResult:
        recorder->resultBufferSize = data->bufferSize;
        std::memcpy(data->buffer, &Payload, sizeof(Payload));
        return S_OK;
    default:
        return S_OK;
    }
}

HRESULT CancelProvider(std::uint32_t op, const AsyncProviderData* data) {
    static_cast<Recorder*>(data->context)->Record(op);
    if (op == OpBegin) return XAsyncSchedule(data->async, 0);
    if (op == OpCancel) XAsyncComplete(data->async, E_ABORT, 0);
    return S_OK;
}

HRESULT CompleteInBeginProvider(std::uint32_t op, const AsyncProviderData* data) {
    static_cast<Recorder*>(data->context)->Record(op);
    if (op == OpBegin) XAsyncComplete(data->async, S_OK, 0);
    return S_OK;
}

HRESULT WorkResultProvider(std::uint32_t op, const AsyncProviderData* data) {
    auto* recorder = static_cast<Recorder*>(data->context);
    recorder->Record(op);
    if (op == OpBegin) return XAsyncSchedule(data->async, 0);
    if (op == OpDoWork) return recorder->workResult;
    return S_OK;
}

HRESULT StepProvider(std::uint32_t op, const AsyncProviderData* data) {
    auto* recorder = static_cast<Recorder*>(data->context);
    recorder->Record(op);
    if (op == OpBegin) return XAsyncSchedule(data->async, recorder->delay);
    if (op == OpDoWork) {
        if (recorder->steps > 0) {
            --recorder->steps;
            const HRESULT scheduled = XAsyncSchedule(data->async, 0);
            return SUCCEEDED(scheduled) ? E_PENDING : scheduled;
        }
        XAsyncComplete(data->async, S_OK, 0);
    }
    return S_OK;
}

HRESULT FailBeginProvider(std::uint32_t op, const AsyncProviderData* data) {
    static_cast<Recorder*>(data->context)->Record(op);
    return op == OpBegin ? E_FAIL : S_OK;
}

HRESULT CompleterProvider(std::uint32_t op, const AsyncProviderData* data) {
    if (op == OpCancel) SetEvent(static_cast<Completer*>(data->context)->request);
    return S_OK;
}

HRESULT IdleProvider(std::uint32_t, const AsyncProviderData*) {
    return S_OK;
}

HRESULT ScheduleProvider(std::uint32_t op, const AsyncProviderData* data) {
    return op == OpBegin ? XAsyncSchedule(data->async, 0) : S_OK;
}

void RecordCompletion(AsyncBlock* async) {
    auto* completion = static_cast<Completion*>(async->context);
    completion->status = XAsyncGetStatus(async, false);
    completion->thread = GetCurrentThreadId();
    SetEvent(completion->event);
    completion->returned = true;
}

void ResultCompletion(AsyncBlock* async) {
    auto* state = static_cast<ResultInCallback*>(async->context);
    state->waited = XAsyncGetStatus(async, true);
    state->result = XAsyncGetResult(async, &g_identity, sizeof(state->value), &state->value, &state->used);
    state->opsInCallback = state->recorder.count;
}

void ChainCompletion(AsyncBlock* async) {
    auto* counter = static_cast<Counter*>(async->context);
    if (XAsyncGetStatus(async, false) != RunResult) ++counter->failures;
    if (++counter->count < counter->target) {
        if (XAsyncRun(async, ReturnWork) != S_OK) ++counter->failures;
        return;
    }
    SetEvent(counter->event);
}

void SwarmCompletion(AsyncBlock* async) {
    auto* counter = static_cast<Counter*>(async->context);
    if (XAsyncGetStatus(async, false) != RunResult) ++counter->failures;
    if (++counter->count == counter->target) SetEvent(counter->event);
}

bool Zeroed(const AsyncBlock& block) {
    for (const unsigned char byte : block.internal) {
        if (byte != 0) return false;
    }
    return true;
}

int AsyncRun(void* pool) {
    void* manual = nullptr;
    if (FAILED(XTaskQueueCreate(0, 0, &manual))) return Fail("async manual queue");
    int marker = 0;
    AsyncBlock block{};
    block.queue = manual;
    block.context = &marker;
    if (XAsyncRun(&block, ReturnWork) != S_OK) return Fail("XAsyncRun");
    if (XAsyncGetStatus(&block, false) != E_PENDING) return Fail("run status before dispatch");
    if (!XTaskQueueDispatch(manual, 0, 0)) return Fail("run work dispatch");
    if (g_workContext.load() != &marker) return Fail("run work context");
    if (XAsyncGetStatus(&block, false) != RunResult || XAsyncGetStatus(&block, true) != RunResult) return Fail("run status after dispatch");
    std::size_t size = 1;
    std::size_t used = 1;
    if (XAsyncGetResultSize(&block, &size) != RunResult || size != 0) return Fail("run result size");
    if (XAsyncGetResult(&block, nullptr, 0, nullptr, &used) != RunResult || used != 0) return Fail("run result");
    if (XAsyncGetResult(&block, nullptr, 0, nullptr, &used) != RunResult) return Fail("run result twice");
    if (XTaskQueueDispatch(manual, 1, 0)) return Fail("run without callback queued a completion");

    Completion waited;
    AsyncBlock waitBlock{};
    waitBlock.queue = manual;
    waitBlock.context = &waited;
    waitBlock.callback = RecordCompletion;
    if (XAsyncRun(&waitBlock, ReturnWork) != S_OK || !XTaskQueueDispatch(manual, 0, 0)) return Fail("XAsyncRun to wait on");
    if (XAsyncGetStatus(&waitBlock, false) != RunResult || waited.returned) return Fail("status before the completion callback");
    std::atomic<bool> dispatched{false};
    std::thread dispatcher([&] { dispatched = XTaskQueueDispatch(manual, 1, DispatchTimeoutMs); });
    const HRESULT waitedStatus = XAsyncGetStatus(&waitBlock, true);
    const bool returned = waited.returned.load();
    dispatcher.join();
    if (!dispatched) return Fail("completion dispatch timed out");
    if (waitedStatus != RunResult || !returned || waited.status != RunResult) return Fail("waiting status returned before the completion callback");
    XTaskQueueCloseHandle(manual);

    Completion pooled;
    AsyncBlock poolBlock{};
    poolBlock.queue = pool;
    poolBlock.context = &pooled;
    poolBlock.callback = RecordCompletion;
    if (XAsyncRun(&poolBlock, ReturnWork) != S_OK) return Fail("XAsyncRun on thread pool");
    if (WaitForSingleObject(pooled.event, DispatchTimeoutMs) != WAIT_OBJECT_0) return Fail("thread pool completion callback");
    if (pooled.status != RunResult || XAsyncGetStatus(&poolBlock, true) != RunResult || !pooled.returned) return Fail("thread pool status");
    if (g_workContext.load() != &pooled) return Fail("thread pool work context");

    void* immediate = nullptr;
    if (FAILED(XTaskQueueCreate(3, 3, &immediate))) return Fail("async immediate queue");
    Completion immediateRun;
    AsyncBlock immediateBlock{};
    immediateBlock.queue = immediate;
    immediateBlock.context = &immediateRun;
    immediateBlock.callback = RecordCompletion;
    if (XAsyncRun(&immediateBlock, ReturnWork) != S_OK || !immediateRun.returned || immediateRun.status != RunResult) return Fail("immediate queue run");
    if (XAsyncGetStatus(&immediateBlock, false) != RunResult) return Fail("immediate queue status");
    XTaskQueueCloseHandle(immediate);

    AsyncBlock processBlock{};
    if (XAsyncRun(&processBlock, ReturnWork) != S_OK || XAsyncGetStatus(&processBlock, true) != RunResult) return Fail("run on the process task queue");

    void* process = nullptr;
    if (!XTaskQueueGetCurrentProcessTaskQueue(&process)) return Fail("process task queue");
    XTaskQueueSetCurrentProcessTaskQueue(nullptr);
    AsyncBlock orphan{};
    const HRESULT orphaned = XAsyncRun(&orphan, ReturnWork);
    XTaskQueueSetCurrentProcessTaskQueue(process);
    XTaskQueueCloseHandle(process);
    if (orphaned != NoTaskQueue || XAsyncGetStatus(&orphan, false) != NoTaskQueue) return Fail("run with no process task queue");
    return 0;
}

int AsyncZeroBlock() {
    AsyncBlock zero{};
    std::size_t size = 1;
    std::size_t used = 1;
    XAsyncCancel(&zero);
    XAsyncComplete(&zero, S_OK, 0);
    if (XAsyncGetStatus(&zero, false) != S_OK || XAsyncGetStatus(&zero, true) != S_OK) return Fail("zeroed block status");
    if (XAsyncGetResultSize(&zero, &size) != S_OK || size != 0) return Fail("zeroed block result size");
    if (XAsyncSchedule(&zero, 0) != E_INVALIDARG) return Fail("zeroed block schedule");
    if (XAsyncGetResult(&zero, nullptr, 0, nullptr, &used) != S_OK || used != 0) return Fail("zeroed block result");
    if (!Zeroed(zero)) return Fail("zeroed block written");
    return 0;
}

int AsyncProviderCalls() {
    void* manual = nullptr;
    if (FAILED(XTaskQueueCreate(0, 0, &manual))) return Fail("provider queue");
    Recorder recorder;
    AsyncBlock block{};
    block.queue = manual;
    if (XAsyncBegin(&block, &recorder, &g_identity, "Payload", PayloadProvider) != S_OK || !recorder.Is({OpBegin})) return Fail("XAsyncBegin");
    std::size_t size = 1;
    std::size_t used = 0;
    unsigned char buffer[16] = {};
    if (XAsyncGetStatus(&block, false) != E_PENDING) return Fail("provider pending status");
    if (XAsyncGetResultSize(&block, &size) != E_PENDING || size != 0) return Fail("provider pending result size");
    if (XAsyncGetResult(&block, &g_identity, sizeof(buffer), buffer, &used) != E_PENDING) return Fail("provider pending result");
    if (XAsyncSchedule(&block, 0) != E_UNEXPECTED) return Fail("scheduled twice");
    if (XAsyncBegin(&block, &recorder, &g_identity, "Payload", PayloadProvider) != E_INVALIDARG || !recorder.Is({OpBegin})) return Fail("begin on a block in use");
    if (!XTaskQueueDispatch(manual, 0, 0) || !recorder.Is({OpBegin, OpDoWork})) return Fail("provider do work");
    if (XAsyncGetStatus(&block, false) != S_OK) return Fail("provider completed status");
    XAsyncCancel(&block);
    if (!recorder.Is({OpBegin, OpDoWork}) || XAsyncGetStatus(&block, false) != S_OK) return Fail("cancel after completion reached the provider");
    if (XAsyncGetResultSize(&block, &size) != S_OK || size != sizeof(Payload)) return Fail("provider result size");
    if (XAsyncGetResult(&block, &g_identity, sizeof(buffer), buffer, &used) != S_OK || used != sizeof(Payload)) return Fail("provider result");
    if (std::memcmp(buffer, &Payload, sizeof(Payload)) != 0 || recorder.resultBufferSize != sizeof(buffer)) return Fail("provider payload");
    if (!recorder.Is({OpBegin, OpDoWork, OpGetResult, OpCleanup})) return Fail("provider op sequence");
    if (XAsyncGetResult(&block, &g_identity, sizeof(buffer), buffer, &used) != IllegalMethodCall) return Fail("result retrieved twice");
    if (XAsyncGetStatus(&block, false) != S_OK || XAsyncGetResultSize(&block, &size) != S_OK || size != 0) return Fail("status after result");
    if (XTaskQueueDispatch(manual, 1, 0)) return Fail("provider without callback queued a completion");

    ResultInCallback inCallback;
    AsyncBlock callbackBlock{};
    callbackBlock.queue = manual;
    callbackBlock.context = &inCallback;
    callbackBlock.callback = ResultCompletion;
    if (XAsyncBegin(&callbackBlock, &inCallback.recorder, &g_identity, "Payload", PayloadProvider) != S_OK) return Fail("payload call with callback");
    if (!XTaskQueueDispatch(manual, 0, 0) || !XTaskQueueDispatch(manual, 1, 0)) return Fail("payload call with callback dispatch");
    if (inCallback.waited != S_OK || inCallback.result != S_OK || inCallback.used != sizeof(Payload) || inCallback.value != Payload) return Fail("result read in the completion callback");
    if (inCallback.opsInCallback != 3 || !inCallback.recorder.Is({OpBegin, OpDoWork, OpGetResult, OpCleanup})) return Fail("cleanup order with a completion callback");

    Recorder mismatched;
    unsigned char untouched[16] = {};
    if (XAsyncBegin(&block, &mismatched, &g_identity, "Payload", PayloadProvider) != S_OK || !XTaskQueueDispatch(manual, 0, 0)) return Fail("block reuse");
    if (XAsyncGetResult(&block, &g_otherIdentity, sizeof(untouched), untouched, &used) != E_INVALIDARG) return Fail("identity mismatch");
    if (!mismatched.Is({OpBegin, OpDoWork, OpCleanup}) || untouched[0] != 0) return Fail("identity mismatch reached the provider");
    if (XAsyncGetResult(&block, &g_identity, sizeof(buffer), buffer, &used) != IllegalMethodCall) return Fail("identity mismatch kept the block");

    Recorder nullBuffer;
    if (XAsyncBegin(&block, &nullBuffer, &g_identity, "Payload", PayloadProvider) != S_OK || !XTaskQueueDispatch(manual, 0, 0)) return Fail("null buffer call");
    if (XAsyncGetResult(&block, &g_identity, sizeof(buffer), nullptr, &used) != E_INVALIDARG || !nullBuffer.Is({OpBegin, OpDoWork, OpCleanup})) return Fail("null result buffer");

    Recorder shortBuffer;
    if (XAsyncBegin(&block, &shortBuffer, &g_identity, "Payload", PayloadProvider) != S_OK || !XTaskQueueDispatch(manual, 0, 0)) return Fail("small buffer call");
    if (XAsyncGetResult(&block, &g_identity, 2, buffer, &used) != NotSufficientBuffer || !shortBuffer.Is({OpBegin, OpDoWork, OpCleanup})) return Fail("small result buffer");

    Recorder failedBegin;
    if (XAsyncBegin(&block, &failedBegin, nullptr, nullptr, FailBeginProvider) != S_OK) return Fail("begin with a failing provider");
    if (XAsyncGetStatus(&block, true) != E_FAIL || !failedBegin.Is({OpBegin, OpCleanup})) return Fail("failed begin status");
    if (XAsyncGetResult(&block, nullptr, 0, nullptr, nullptr) != E_FAIL) return Fail("failed begin result");

    Recorder failedWork;
    failedWork.workResult = WorkFailure;
    if (XAsyncBegin(&block, &failedWork, nullptr, nullptr, WorkResultProvider) != S_OK || !XTaskQueueDispatch(manual, 0, 0)) return Fail("failing work call");
    if (XAsyncGetStatus(&block, false) != WorkFailure || !failedWork.Is({OpBegin, OpDoWork, OpCleanup})) return Fail("do work failure status");

    Recorder silentWork;
    if (XAsyncBegin(&block, &silentWork, nullptr, nullptr, WorkResultProvider) != S_OK || !XTaskQueueDispatch(manual, 0, 0)) return Fail("silent work call");
    if (XAsyncGetStatus(&block, false) != E_UNEXPECTED) return Fail("do work success without completion");

    Recorder stepped;
    stepped.steps = 2;
    if (XAsyncBegin(&block, &stepped, nullptr, nullptr, StepProvider) != S_OK) return Fail("rescheduling call");
    for (int step = 0; step < 2; ++step) {
        if (!XTaskQueueDispatch(manual, 0, 0) || XAsyncGetStatus(&block, false) != E_PENDING) return Fail("do work rescheduled with E_PENDING");
    }
    if (!XTaskQueueDispatch(manual, 0, 0) || XAsyncGetStatus(&block, false) != S_OK) return Fail("rescheduled work completion");
    if (!stepped.Is({OpBegin, OpDoWork, OpDoWork, OpDoWork, OpCleanup}) || XTaskQueueDispatch(manual, 0, 0)) return Fail("rescheduled op sequence");

    Recorder delayed;
    delayed.delay = ScheduleDelayMs;
    const ULONGLONG scheduledAt = GetTickCount64();
    if (XAsyncBegin(&block, &delayed, nullptr, nullptr, StepProvider) != S_OK) return Fail("delayed call");
    if (XTaskQueueDispatch(manual, 0, 0)) return Fail("delayed work ran before its delay");
    if (!XTaskQueueDispatch(manual, 0, DispatchTimeoutMs)) return Fail("delayed work never ran");
    const ULONGLONG elapsed = GetTickCount64() - scheduledAt;
    if (elapsed + 16 < ScheduleDelayMs || XAsyncGetStatus(&block, false) != S_OK || !delayed.Is({OpBegin, OpDoWork, OpCleanup})) return Fail("delayed work");
    XTaskQueueCloseHandle(manual);
    return 0;
}

int AsyncCancel() {
    void* manual = nullptr;
    if (FAILED(XTaskQueueCreate(0, 0, &manual))) return Fail("cancel queue");
    Recorder recorder;
    Completion completion;
    AsyncBlock block{};
    block.queue = manual;
    block.context = &completion;
    block.callback = RecordCompletion;
    if (XAsyncBegin(&block, &recorder, nullptr, nullptr, CancelProvider) != S_OK) return Fail("cancelable begin");
    XAsyncCancel(&block);
    if (!recorder.Is({OpBegin, OpCancel})) return Fail("cancel op");
    if (XAsyncGetStatus(&block, false) != E_ABORT) return Fail("canceled status");
    if (completion.returned) return Fail("completion callback ran outside the completion port");
    if (!XTaskQueueDispatch(manual, 1, 0) || !completion.returned || completion.status != E_ABORT) return Fail("canceled completion callback");
    if (!XTaskQueueDispatch(manual, 0, 0) || !recorder.Is({OpBegin, OpCancel, OpCleanup})) return Fail("work after cancel");
    XAsyncCancel(&block);
    if (!recorder.Is({OpBegin, OpCancel, OpCleanup}) || XAsyncGetStatus(&block, true) != E_ABORT) return Fail("cancel after completion");

    Recorder late;
    Completion lateCompletion;
    AsyncBlock lateBlock{};
    lateBlock.queue = manual;
    lateBlock.context = &lateCompletion;
    lateBlock.callback = RecordCompletion;
    if (XAsyncBegin(&lateBlock, &late, nullptr, nullptr, CancelProvider) != S_OK) return Fail("cancel before work begin");
    XAsyncCancel(&lateBlock);
    if (!XTaskQueueDispatch(manual, 0, 0) || !late.Is({OpBegin, OpCancel})) return Fail("do work ran on a canceled call");
    if (!XTaskQueueDispatch(manual, 1, 0) || lateCompletion.status != E_ABORT || !late.Is({OpBegin, OpCancel, OpCleanup})) return Fail("canceled call completion after its work");

    Recorder early;
    Completion earlyCompletion;
    AsyncBlock earlyBlock{};
    earlyBlock.queue = manual;
    earlyBlock.context = &earlyCompletion;
    earlyBlock.callback = RecordCompletion;
    if (XAsyncBegin(&earlyBlock, &early, nullptr, nullptr, CompleteInBeginProvider) != S_OK) return Fail("complete in begin");
    if (XAsyncSchedule(&earlyBlock, 0) != E_INVALIDARG) return Fail("schedule after completion without payload");
    if (!XTaskQueueDispatch(manual, 1, 0) || earlyCompletion.status != S_OK || XTaskQueueDispatch(manual, 0, 0)) return Fail("complete in begin callback");
    if (!early.Is({OpBegin, OpCleanup})) return Fail("complete in begin op sequence");

    Completion stranded;
    AsyncBlock strandedBlock{};
    strandedBlock.queue = manual;
    strandedBlock.context = &stranded;
    strandedBlock.callback = RecordCompletion;
    if (XAsyncRun(&strandedBlock, ReturnWork) != S_OK || !XTaskQueueDispatch(manual, 0, 0)) return Fail("call before terminate");
    if (stranded.returned) return Fail("completion ran before terminate");
    if (FAILED(XTaskQueueTerminate(manual, true, nullptr, nullptr))) return Fail("terminate with a queued completion");
    if (!stranded.returned || stranded.status != RunResult || stranded.thread != GetCurrentThreadId()) return Fail("completion queued at termination");
    if (XAsyncGetStatus(&strandedBlock, true) != RunResult) return Fail("status after termination");

    AsyncBlock idleBlock{};
    idleBlock.queue = manual;
    if (XAsyncBegin(&idleBlock, nullptr, nullptr, nullptr, IdleProvider) != S_OK) return Fail("begin on a terminated queue");
    if (XAsyncSchedule(&idleBlock, 0) != E_ABORT) return Fail("schedule on a terminated queue");
    if (XAsyncSchedule(&idleBlock, 0) != E_UNEXPECTED) return Fail("schedule again after the queue refused the work");
    XAsyncComplete(&idleBlock, E_ABORT, 0);
    if (XAsyncGetStatus(&idleBlock, true) != E_ABORT) return Fail("status after scheduling on a terminated queue");
    XTaskQueueCloseHandle(manual);
    return 0;
}

int AsyncConcurrency(void* pool) {
    Counter chain;
    chain.target = 5;
    AsyncBlock chainBlock{};
    chainBlock.queue = pool;
    chainBlock.context = &chain;
    chainBlock.callback = ChainCompletion;
    if (XAsyncRun(&chainBlock, ReturnWork) != S_OK) return Fail("chained run");
    if (WaitForSingleObject(chain.event, DispatchTimeoutMs) != WAIT_OBJECT_0 || chain.count != 5 || chain.failures != 0) return Fail("block reused from its completion callback");

    Counter swarm;
    swarm.target = SwarmSize;
    AsyncBlock* blocks = new AsyncBlock[SwarmSize]();
    for (int i = 0; i < SwarmSize; ++i) {
        blocks[i].queue = pool;
        blocks[i].context = &swarm;
        blocks[i].callback = SwarmCompletion;
        if (XAsyncRun(&blocks[i], ReturnWork) != S_OK) return Fail("swarm run");
    }
    if (WaitForSingleObject(swarm.event, DispatchTimeoutMs) != WAIT_OBJECT_0 || swarm.failures != 0) return Fail("swarm completions");
    for (int i = 0; i < SwarmSize; ++i) {
        if (XAsyncGetStatus(&blocks[i], true) != RunResult) return Fail("swarm status");
    }
    delete[] blocks;

    Completer completer;
    std::thread completing([&] {
        for (;;) {
            WaitForSingleObject(completer.request, INFINITE);
            if (completer.stop) return;
            XAsyncComplete(completer.block, E_ABORT, 0);
            SetEvent(completer.done);
        }
    });
    int failure = 0;
    for (int i = 0; i < Repeats && failure == 0; ++i) {
        AsyncBlock block{};
        block.queue = pool;
        completer.block = &block;
        if (XAsyncBegin(&block, &completer, nullptr, nullptr, CompleterProvider) != S_OK) {
            failure = Fail("completer begin");
            break;
        }
        XAsyncCancel(&block);
        if (XAsyncGetStatus(&block, true) != E_ABORT) {
            failure = Fail("completer status");
            break;
        }
        const AsyncBlock snapshot = block;
        if (WaitForSingleObject(completer.done, DispatchTimeoutMs) != WAIT_OBJECT_0) {
            failure = Fail("completer timed out");
            break;
        }
        if (std::memcmp(&snapshot, &block, sizeof(block)) != 0) failure = Fail("block written after the waiting status returned");
    }
    completer.stop = true;
    SetEvent(completer.request);
    completing.join();
    return failure;
}

int AsyncScenario() {
    void* pool = nullptr;
    if (FAILED(XTaskQueueCreate(1, 1, &pool))) return Fail("async pool queue");
    if (AsyncRun(pool) != 0 || AsyncZeroBlock() != 0 || AsyncProviderCalls() != 0 || AsyncCancel() != 0 || AsyncConcurrency(pool) != 0) return 1;
    if (FAILED(XTaskQueueTerminate(pool, true, nullptr, nullptr))) return Fail("async pool terminate");
    XTaskQueueCloseHandle(pool);
    std::printf("async ok\n");
    return 0;
}

int Async() {
    HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread watchdog([finished] {
        if (WaitForSingleObject(finished, WatchdogMs) != WAIT_OBJECT_0) {
            std::printf("FAIL async scenario blocked for %lu ms\n", WatchdogMs);
            std::fflush(stdout);
            TerminateProcess(GetCurrentProcess(), 1);
        }
    });
    const int result = AsyncScenario();
    SetEvent(finished);
    watchdog.join();
    CloseHandle(finished);
    return result;
}

int AsyncTerminated(bool beforeBegin) {
    void* manual = nullptr;
    if (FAILED(XTaskQueueCreate(0, 0, &manual))) return Fail("terminated queue");
    Completion completion;
    AsyncBlock block{};
    block.queue = manual;
    block.context = &completion;
    block.callback = RecordCompletion;
    if (beforeBegin) {
        XTaskQueueTerminate(manual, true, nullptr, nullptr);
        XAsyncBegin(&block, nullptr, nullptr, nullptr, ScheduleProvider);
        return Fail("begin on a terminated queue returned");
    }
    if (XAsyncBegin(&block, nullptr, nullptr, nullptr, ScheduleProvider) != S_OK) return Fail("terminated begin");
    XTaskQueueTerminate(manual, true, nullptr, nullptr);
    return Fail("terminate with a call in flight returned");
}

int Startup() {
    const RuntimeOptions defaults{0, nullptr};
    if (XGameRuntimeInitializeWithOptions(&defaults) != S_OK) return Fail("initialize with default options");
    for (std::uint32_t feature = 0; feature <= 21; ++feature) {
        if (!XGameRuntimeIsFeatureAvailable(feature)) return Fail("known feature reported unavailable");
    }
    if (XGameRuntimeIsFeatureAvailable(22) || XGameRuntimeIsFeatureAvailable(0xffffffffu)) return Fail("unknown feature reported available");
    if (XThreadIsTimeSensitive()) return Fail("thread starts time-sensitive");
    if (XThreadSetTimeSensitive(true) != S_OK || !XThreadIsTimeSensitive()) return Fail("set time-sensitive");
    bool other = true;
    std::thread([&] { other = XThreadIsTimeSensitive(); }).join();
    if (other) return Fail("time-sensitive flag leaked to another thread");
    if (XThreadSetTimeSensitive(false) != S_OK || XThreadIsTimeSensitive()) return Fail("clear time-sensitive");
    XThreadAssertNotTimeSensitive();
    char identifier[64] = {};
    if (XPackageGetCurrentProcessPackageIdentifier(sizeof(identifier), identifier) != static_cast<HRESULT>(0x89245200u)) return Fail("package identifier of an unpackaged process");
    std::printf("startup ok\n");
    return 0;
}

constexpr HRESULT InvalidContainerName = static_cast<HRESULT>(0x80830001u);
constexpr HRESULT UpdateTooBig = static_cast<HRESULT>(0x80830005u);
constexpr HRESULT BufferTooSmall = static_cast<HRESULT>(0x80830007u);
constexpr HRESULT BlobNotFound = static_cast<HRESULT>(0x80830008u);
constexpr HRESULT HandleExpired = static_cast<HRESULT>(0x8083000Du);
constexpr std::int64_t SaveQuota = 256ll * 1024 * 1024;
constexpr const char* Scid = "00000000-0000-0000-0000-00001234abcd";

struct SaveSeen {
    int count = 0;
    SaveContainerInfo container{};
    char name[64]{};
    char displayName[64]{};
};

bool SeeContainer(const SaveContainerInfo* info, void* context) {
    auto* seen = static_cast<SaveSeen*>(context);
    ++seen->count;
    seen->container = *info;
    std::snprintf(seen->name, sizeof(seen->name), "%s", info->name);
    std::snprintf(seen->displayName, sizeof(seen->displayName), "%s", info->displayName);
    return true;
}

bool SeeBlob(const SaveBlobInfo*, void* context) {
    ++static_cast<SaveSeen*>(context)->count;
    return true;
}

bool StopAtFirstBlob(const SaveBlobInfo*, void* context) {
    ++static_cast<SaveSeen*>(context)->count;
    return false;
}

int Containers(void* provider, const char* prefix) {
    SaveSeen seen;
    const HRESULT result = prefix == nullptr ? XGameSaveEnumerateContainerInfo(provider, &seen, SeeContainer) : XGameSaveEnumerateContainerInfoByName(provider, prefix, &seen, SeeContainer);
    return FAILED(result) ? -1 : seen.count;
}

bool Blob(const SaveBlob& blob, const char* name, const char* data) {
    return std::strcmp(blob.info.name, name) == 0 && blob.info.size == std::strlen(data) && std::memcmp(blob.data, data, blob.info.size) == 0;
}

HRESULT Write(void* container, const char* displayName, const char* name, const char* data) {
    void* update = nullptr;
    HRESULT result = XGameSaveCreateUpdate(container, displayName, &update);
    if (SUCCEEDED(result)) result = XGameSaveSubmitBlobWrite(update, name, reinterpret_cast<const std::uint8_t*>(data), std::strlen(data));
    if (SUCCEEDED(result)) result = XGameSaveSubmitUpdate(update);
    XGameSaveCloseUpdate(update);
    return result;
}

int GameSaveSync(void* provider) {
    std::int64_t quota = 0;
    if (XGameSaveGetRemainingQuota(provider, &quota) != S_OK || quota != SaveQuota) return Fail("initial quota");
    void* container = nullptr;
    for (const char* bad : {"", "a..b", "slot.", "a.b/c", "sl ot", "a/"}) {
        if (XGameSaveCreateContainer(provider, bad, &container) != InvalidContainerName) return Fail("invalid container name accepted");
    }
    if (XGameSaveCreateContainer(provider, "slots/one", &container) != S_OK) return Fail("create container");
    if (Containers(provider, nullptr) != 0) return Fail("container listed before its first update");

    void* update = nullptr;
    const std::uint8_t three[] = {1, 2, 3};
    if (XGameSaveCreateUpdate(container, "Slot One", &update) != S_OK) return Fail("create update");
    if (XGameSaveSubmitBlobWrite(update, "alpha", reinterpret_cast<const std::uint8_t*>("hello"), 5) != S_OK) return Fail("write alpha");
    if (XGameSaveSubmitBlobWrite(update, "beta", three, sizeof(three)) != S_OK) return Fail("write beta");
    if (XGameSaveSubmitBlobWrite(update, "alpha", three, sizeof(three)) != E_INVALIDARG) return Fail("second change to one blob accepted");
    if (XGameSaveSubmitUpdate(update) != S_OK) return Fail("submit update");
    if (XGameSaveSubmitUpdate(update) != HandleExpired) return Fail("update submitted twice");
    if (XGameSaveSubmitBlobDelete(update, "beta") != HandleExpired) return Fail("change after submit");
    XGameSaveCloseUpdate(update);

    SaveSeen seen;
    if (XGameSaveGetContainerInfo(provider, "slots/one", &seen, SeeContainer) != S_OK || seen.count != 1) return Fail("container info");
    if (std::strcmp(seen.name, "slots/one") != 0 || std::strcmp(seen.displayName, "Slot One") != 0 || seen.container.blobCount != 2 || seen.container.totalSize != 8 || seen.container.needsSync || seen.container.lastModifiedTime < 1600000000) return Fail("container info fields");
    if (Containers(provider, "slots/") != 1 || Containers(provider, "other") != 0) return Fail("enumerate containers by name");
    if (XGameSaveGetRemainingQuota(provider, &quota) != S_OK || quota != SaveQuota - 8) return Fail("quota after update");

    SaveSeen blobs;
    if (XGameSaveEnumerateBlobInfo(container, &blobs, SeeBlob) != S_OK || blobs.count != 2) return Fail("enumerate blobs");
    SaveSeen beta;
    if (XGameSaveEnumerateBlobInfoByName(container, "be", &beta, SeeBlob) != S_OK || beta.count != 1) return Fail("enumerate blobs by name");
    SaveSeen stopped;
    if (XGameSaveEnumerateBlobInfo(container, &stopped, StopAtFirstBlob) != S_OK || stopped.count != 1) return Fail("stop blob enumeration");

    alignas(SaveBlob) unsigned char buffer[256];
    std::uint32_t count = 0;
    const std::size_t needed = 2 * sizeof(SaveBlob) + 6 + 5 + 5 + 3;
    if (XGameSaveReadBlobData(container, nullptr, &count, needed - 1, reinterpret_cast<SaveBlob*>(buffer)) != BufferTooSmall) return Fail("read into a small buffer");
    if (XGameSaveReadBlobData(container, nullptr, &count, needed, reinterpret_cast<SaveBlob*>(buffer)) != S_OK || count != 2) return Fail("read all blobs");
    const auto* read = reinterpret_cast<SaveBlob*>(buffer);
    if (!Blob(read[0], "alpha", "hello") || read[1].info.size != 3 || std::memcmp(read[1].data, three, 3) != 0) return Fail("read blob contents");
    const char* missing[] = {"alpha", "gamma"};
    count = 2;
    if (XGameSaveReadBlobData(container, missing, &count, sizeof(buffer), reinterpret_cast<SaveBlob*>(buffer)) != BlobNotFound) return Fail("read a missing blob");

    std::vector<std::uint8_t> big(16 * 1024 * 1024 + 1);
    if (XGameSaveCreateUpdate(container, nullptr, &update) != S_OK) return Fail("create big update");
    if (XGameSaveSubmitBlobWrite(update, "big", big.data(), big.size()) != S_OK || XGameSaveSubmitUpdate(update) != UpdateTooBig) return Fail("update over 16 MiB");
    XGameSaveCloseUpdate(update);
    XGameSaveCloseContainer(container);
    return 0;
}

int GameSaveAsync(void* provider) {
    void* container = nullptr;
    if (XGameSaveCreateContainer(provider, "slots/one", &container) != S_OK) return Fail("reopen container");
    AsyncBlock block{};
    const char* names[] = {"Alpha"};
    if (XGameSaveReadBlobDataAsync(container, names, 1, &block) != S_OK || XAsyncGetStatus(&block, true) != S_OK) return Fail("read blob async");
    std::size_t size = 0;
    if (XAsyncGetResultSize(&block, &size) != S_OK || size != sizeof(SaveBlob) + 6 + 5) return Fail("read blob async size");
    alignas(SaveBlob) unsigned char buffer[256];
    std::uint32_t count = 0;
    if (XGameSaveReadBlobDataResult(&block, size, reinterpret_cast<SaveBlob*>(buffer), &count) != S_OK || count != 1) return Fail("read blob async result");
    if (!Blob(*reinterpret_cast<SaveBlob*>(buffer), "alpha", "hello")) return Fail("read blob async contents");

    void* update = nullptr;
    if (XGameSaveCreateUpdate(container, nullptr, &update) != S_OK || XGameSaveSubmitBlobDelete(update, "alpha") != S_OK) return Fail("delete alpha");
    block = AsyncBlock{};
    AsyncBlock second{};
    if (XGameSaveSubmitUpdateAsync(update, &block) != S_OK) return Fail("submit update async");
    if (XGameSaveSubmitUpdateAsync(update, &second) != HandleExpired) return Fail("update submitted twice async while pending");
    if (XAsyncGetStatus(&block, true) != S_OK || XGameSaveSubmitUpdateResult(&block) != S_OK) return Fail("submit update async result");
    second = AsyncBlock{};
    if (XGameSaveSubmitUpdateAsync(update, &second) != HandleExpired) return Fail("update submitted twice async");
    XGameSaveCloseUpdate(update);
    SaveSeen seen;
    if (XGameSaveGetContainerInfo(provider, "slots/one", &seen, SeeContainer) != S_OK || seen.container.blobCount != 1 || std::strcmp(seen.displayName, "Slot One") != 0) return Fail("container after async delete");

    block = AsyncBlock{};
    std::int64_t quota = 0;
    if (XGameSaveGetRemainingQuotaAsync(provider, &block) != S_OK || XAsyncGetStatus(&block, true) != S_OK || XGameSaveGetRemainingQuotaResult(&block, &quota) != S_OK || quota != SaveQuota - 3) return Fail("quota async");

    block = AsyncBlock{};
    if (XGameSaveDeleteContainerAsync(provider, "slots/one", &block) != S_OK || XAsyncGetStatus(&block, true) != S_OK || XGameSaveDeleteContainerResult(&block) != S_OK) return Fail("delete container async");
    if (Containers(provider, nullptr) != 0) return Fail("container listed after delete");
    if (XGameSaveDeleteContainer(provider, "slots/one") != S_OK) return Fail("delete a missing container");
    XGameSaveCloseContainer(container);
    return 0;
}

int GameSave() {
    void* provider = nullptr;
    void* user = reinterpret_cast<void*>(1);
    if (XGameSaveInitializeProvider(user, Scid, false, &provider) != S_OK || provider == nullptr) return Fail("initialize provider");
    if (GameSaveSync(provider) != 0) return 1;
    XGameSaveCloseProvider(provider);

    AsyncBlock block{};
    provider = nullptr;
    if (XGameSaveInitializeProviderAsync(user, Scid, false, &block) != S_OK || XAsyncGetStatus(&block, true) != S_OK) return Fail("initialize provider async");
    if (XGameSaveInitializeProviderResult(&block, &provider) != S_OK || provider == nullptr) return Fail("initialize provider result");
    if (GameSaveAsync(provider) != 0) return 1;

    void* container = nullptr;
    if (XGameSaveCreateContainer(provider, "keep", &container) != S_OK || Write(container, "Kept", "data", "kept") != S_OK) return Fail("write kept container");
    XGameSaveCloseContainer(container);
    XGameSaveCloseProvider(provider);
    std::printf("gamesave ok\n");
    return 0;
}

struct Monitored {
    std::atomic<int> work{0};
    std::atomic<int> completion{0};
    std::atomic<std::uint32_t> last{0xffffffffu};
    std::atomic<void*> queue{nullptr};
    HANDLE called = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Monitored() = default;
    Monitored(const Monitored&) = delete;
    Monitored& operator=(const Monitored&) = delete;
    ~Monitored() { CloseHandle(called); }
};

void Monitor(void* context, void* queue, std::uint32_t port) {
    auto* seen = static_cast<Monitored*>(context);
    (port == 0 ? seen->work : seen->completion).fetch_add(1);
    seen->last = port;
    seen->queue = queue;
    SetEvent(seen->called);
}

bool AwaitCount(Monitored& seen, const std::atomic<int>& count, int expected) {
    while (count < expected) {
        if (WaitForSingleObject(seen.called, 10000) != WAIT_OBJECT_0) return false;
    }
    return count == expected;
}

constexpr HRESULT InvalidHandle = static_cast<HRESULT>(0x8924010Cu);

struct Ordered {
    Monitored seen;
    std::atomic<int> monitorFirst{0};
    std::atomic<int> runs{0};
};

void CheckMonitored(void* context, bool) {
    auto* ordered = static_cast<Ordered*>(context);
    if (ordered->seen.work == ordered->runs.fetch_add(1) + 1) ordered->monitorFirst.fetch_add(1);
}

void* g_reentryQueue = nullptr;
void* g_otherQueue = nullptr;

void TerminateOwnQueue(void*, bool) {
    XTaskQueueDispatch(g_otherQueue, 0, 0);
    XTaskQueueTerminate(g_reentryQueue, true, nullptr, nullptr);
}

struct Churn {
    void* source = nullptr;
    void* port = nullptr;
    std::atomic<bool> stop{false};
    std::atomic<int> failures{0};
};

struct Tally {
    std::atomic<int> ran{0};
    std::atomic<int> canceled{0};
    HANDLE signaled = CreateEventW(nullptr, FALSE, FALSE, nullptr);
};

void Record(void* context, bool canceled) {
    auto* tally = static_cast<Tally*>(context);
    (canceled ? tally->canceled : tally->ran).fetch_add(1);
    SetEvent(tally->signaled);
}

int Guarded(const char* name, int (*scenario)()) {
    HANDLE finished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread watchdog([finished, name] {
        if (WaitForSingleObject(finished, WatchdogMs) != WAIT_OBJECT_0) {
            std::printf("FAIL %s scenario blocked for %lu ms\n", name, WatchdogMs);
            std::fflush(stdout);
            TerminateProcess(GetCurrentProcess(), 1);
        }
    });
    const int result = scenario();
    SetEvent(finished);
    watchdog.join();
    CloseHandle(finished);
    return result;
}

int Composite() {
    void* queue = nullptr;
    if (FAILED(XTaskQueueCreate(0, 0, &queue))) return Fail("composite source queue");
    void* port = nullptr;
    if (XTaskQueueGetPort(queue, 0, nullptr) != E_POINTER) return Fail("get port without output");
    if (XTaskQueueGetPort(queue, 2, &port) != E_INVALIDARG) return Fail("get port of an invalid port");
    if (FAILED(XTaskQueueGetPort(queue, 0, &port)) || port == nullptr) return Fail("get work port");
    if (XTaskQueueCreateComposite(port, port, nullptr) != E_POINTER) return Fail("composite without output");
    void* composite = nullptr;
    if (FAILED(XTaskQueueCreateComposite(port, port, &composite))) return Fail("create composite");
    void* compositePort = nullptr;
    if (FAILED(XTaskQueueGetPort(composite, 1, &compositePort)) || compositePort != port) return Fail("composite completion port is the source work port");

    Tally tally;
    if (FAILED(XTaskQueueSubmitCallback(composite, 1, &tally, Record))) return Fail("submit composite completion");
    if (XTaskQueueDispatch(queue, 1, 0)) return Fail("source completion port received composite work");
    if (!XTaskQueueDispatch(queue, 0, 0) || tally.ran != 1) return Fail("source work port runs composite completion");

    Monitored source;
    Monitored joined;
    RegistrationToken sourceToken{};
    RegistrationToken joinedToken{};
    if (XTaskQueueRegisterMonitor(queue, nullptr, nullptr, &sourceToken) != E_POINTER) return Fail("monitor without callback");
    if (XTaskQueueRegisterMonitor(queue, nullptr, Monitor, nullptr) != E_POINTER) return Fail("monitor without token");
    if (FAILED(XTaskQueueRegisterMonitor(queue, &source, Monitor, &sourceToken))) return Fail("register source monitor");
    if (FAILED(XTaskQueueRegisterMonitor(composite, &joined, Monitor, &joinedToken))) return Fail("register composite monitor");
    XTaskQueueSubmitCallback(composite, 1, &tally, Record);
    if (source.work != 1 || source.completion != 0) return Fail("source monitor sees its work port");
    if (joined.work != 1 || joined.completion != 1 || joined.last != 1) return Fail("composite monitor sees both of its ports");
    if (!XTaskQueueDispatch(source.queue.load(), 0, 0) || tally.ran != 2) return Fail("dispatch through the monitor's queue handle");
    const ULONGLONG submitted = GetTickCount64();
    XTaskQueueSubmitDelayedCallback(queue, 0, 300, &tally, Record);
    if (source.work != 1 && GetTickCount64() - submitted < 300) return Fail("monitor called before the delay elapsed");
    if (!XTaskQueueDispatch(queue, 0, 10000)) return Fail("delayed composite dispatch");
    if (!AwaitCount(source, source.work, 2) || !AwaitCount(joined, joined.work, 2)) return Fail("monitor called when delayed work is queued");
    XTaskQueueUnregisterMonitor(composite, joinedToken);
    XTaskQueueUnregisterMonitor(composite, joinedToken);
    XTaskQueueSubmitCallback(queue, 0, &tally, Record);
    if (source.work != 3 || joined.work != 2) return Fail("unregistered monitor still called");
    if (!XTaskQueueDispatch(queue, 0, 0)) return Fail("dispatch after unregister");

    Tally kept;
    Tally dropped;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    RegistrationToken waiter{};
    XTaskQueueSubmitCallback(queue, 0, &kept, Record);
    XTaskQueueSubmitCallback(composite, 0, &dropped, Record);
    XTaskQueueSubmitDelayedCallback(composite, 0, 60000, &dropped, Record);
    if (FAILED(XTaskQueueRegisterWaiter(composite, 0, event, &dropped, Record, &waiter))) return Fail("register composite waiter");
    g_terminated = false;
    if (FAILED(XTaskQueueTerminate(composite, true, nullptr, Terminated)) || !g_terminated) return Fail("terminate composite");
    if (dropped.canceled != 3 || dropped.ran != 0) {
        std::printf("FAIL composite termination ran=%d canceled=%d\n", dropped.ran.load(), dropped.canceled.load());
        return 1;
    }
    if (XTaskQueueSubmitCallback(composite, 0, &dropped, Record) != E_ABORT) return Fail("submit to terminated composite");
    SetEvent(event);
    if (!XTaskQueueDispatch(queue, 0, 0) || kept.ran != 1 || kept.canceled != 0) return Fail("source work survives composite termination");
    if (XTaskQueueDispatch(queue, 0, 200) || dropped.ran != 0) return Fail("terminated composite waiter fired");
    XTaskQueueUnregisterWaiter(composite, waiter);
    XTaskQueueCloseHandle(composite);
    CloseHandle(event);

    void* pool = nullptr;
    if (FAILED(XTaskQueueCreate(1, 1, &pool))) return Fail("composite pool queue");
    void* poolPort = nullptr;
    XTaskQueueGetPort(pool, 0, &poolPort);
    void* poolComposite = nullptr;
    if (FAILED(XTaskQueueCreateComposite(poolPort, port, &poolComposite))) return Fail("composite of two queues");
    Monitored poolSeen;
    RegistrationToken poolToken{};
    XTaskQueueRegisterMonitor(pool, &poolSeen, Monitor, &poolToken);
    XTaskQueueCloseHandle(pool);
    Tally pooled;
    XTaskQueueSubmitCallback(poolComposite, 0, &pooled, Record);
    if (WaitForSingleObject(pooled.signaled, 10000) != WAIT_OBJECT_0 || pooled.ran != 1) return Fail("composite runs on the source thread pool port");
    if (poolSeen.work != 1) return Fail("composite keeps its source queue alive");
    XTaskQueueSubmitCallback(poolComposite, 1, &pooled, Record);
    if (!XTaskQueueDispatch(queue, 0, 0) || pooled.ran != 2) return Fail("composite completion on the other queue's port");
    XTaskQueueTerminate(poolComposite, true, nullptr, nullptr);
    XTaskQueueCloseHandle(poolComposite);

    std::uint64_t garbage[4] = {};
    void* invalid = nullptr;
    if (XTaskQueueCreateComposite(nullptr, port, &invalid) != InvalidHandle || invalid != nullptr) return Fail("composite of a null port");
    if (XTaskQueueCreateComposite(port, garbage, &invalid) != InvalidHandle || invalid != nullptr) return Fail("composite of a foreign port");

    void* closed = nullptr;
    XTaskQueueCreate(0, 0, &closed);
    void* closedPort = nullptr;
    XTaskQueueGetPort(closed, 0, &closedPort);
    Tally pendingWork;
    XTaskQueueSubmitCallback(closed, 0, &pendingWork, Record);
    XTaskQueueCloseHandle(closed);
    void* reopened = nullptr;
    if (FAILED(XTaskQueueCreateComposite(closedPort, closedPort, &reopened))) return Fail("closed queue with pending work stays alive");
    if (!XTaskQueueDispatch(reopened, 0, 0) || pendingWork.ran != 1 || pendingWork.canceled != 0) return Fail("pending work of a closed queue runs");
    void* gone = nullptr;
    XTaskQueueCreate(0, 0, &gone);
    void* gonePort = nullptr;
    XTaskQueueGetPort(gone, 1, &gonePort);
    XTaskQueueCloseHandle(gone);
    if (XTaskQueueCreateComposite(gonePort, gonePort, &invalid) != InvalidHandle) return Fail("composite of a released queue's port");

    Tally survivor;
    XTaskQueueSubmitCallback(reopened, 0, &survivor, Record);
    void* sourceOfReopened = nullptr;
    XTaskQueueCreate(0, 0, &sourceOfReopened);
    void* livePort = nullptr;
    XTaskQueueGetPort(sourceOfReopened, 0, &livePort);
    void* user = nullptr;
    XTaskQueueCreateComposite(livePort, livePort, &user);
    Tally inUse;
    XTaskQueueSubmitCallback(user, 0, &inUse, Record);
    XTaskQueueSubmitCallback(sourceOfReopened, 0, &inUse, Record);
    if (FAILED(XTaskQueueTerminate(sourceOfReopened, true, nullptr, nullptr))) return Fail("terminate the source of a composite");
    if (inUse.canceled != 1 || inUse.ran != 0) return Fail("source termination cancels only its own work");
    if (XTaskQueueSubmitCallback(sourceOfReopened, 0, &inUse, Record) != E_ABORT) return Fail("submit to the terminated source");
    if (FAILED(XTaskQueueSubmitCallback(user, 0, &inUse, Record))) return Fail("composite still accepts work after its source terminated");
    if (!XTaskQueueDispatch(user, 0, 0) || !XTaskQueueDispatch(user, 0, 0) || inUse.ran != 2) return Fail("composite work runs after its source terminated");
    XTaskQueueTerminate(user, true, nullptr, nullptr);
    XTaskQueueCloseHandle(user);
    XTaskQueueCloseHandle(sourceOfReopened);
    XTaskQueueTerminate(reopened, true, nullptr, nullptr);
    XTaskQueueCloseHandle(reopened);
    if (survivor.canceled != 1) return Fail("work on a reopened port canceled with its composite");

    void* immediate = nullptr;
    XTaskQueueCreate(3, 3, &immediate);
    Ordered ordered;
    RegistrationToken orderedToken{};
    XTaskQueueRegisterMonitor(immediate, &ordered.seen, Monitor, &orderedToken);
    for (int i = 0; i < 3; ++i) XTaskQueueSubmitCallback(immediate, 0, &ordered, CheckMonitored);
    if (ordered.runs != 3 || ordered.monitorFirst != 3) return Fail("immediate port calls monitors before the callback");
    XTaskQueueUnregisterMonitor(immediate, orderedToken);
    XTaskQueueTerminate(immediate, true, nullptr, nullptr);
    XTaskQueueCloseHandle(immediate);

    Churn churn;
    XTaskQueueCreate(3, 3, &churn.source);
    XTaskQueueGetPort(churn.source, 0, &churn.port);
    Monitored churnSeen;
    RegistrationToken churnToken{};
    XTaskQueueRegisterMonitor(churn.source, &churnSeen, Monitor, &churnToken);
    std::atomic<int> churnSubmits{0};
    std::thread submitter([&] {
        while (!churn.stop) {
            if (FAILED(XTaskQueueSubmitCallback(churn.source, 0, nullptr, Count))) churn.failures.fetch_add(1);
            churnSubmits.fetch_add(1);
        }
    });
    std::thread creator([&] {
        for (int i = 0; i < 2000; ++i) {
            void* composite = nullptr;
            if (FAILED(XTaskQueueCreateComposite(churn.port, churn.port, &composite))) {
                churn.failures.fetch_add(1);
                continue;
            }
            XTaskQueueCloseHandle(composite);
        }
        churn.stop = true;
    });
    creator.join();
    submitter.join();
    if (churn.failures != 0 || churnSeen.work != churnSubmits) return Fail("composites created while the source is in use");
    XTaskQueueUnregisterMonitor(churn.source, churnToken);
    XTaskQueueTerminate(churn.source, true, nullptr, nullptr);
    XTaskQueueCloseHandle(churn.source);

    XTaskQueueUnregisterMonitor(queue, sourceToken);
    XTaskQueueTerminate(queue, true, nullptr, nullptr);
    XTaskQueueCloseHandle(queue);
    std::printf("composite ok\n");
    return 0;
}

int TerminateReentry() {
    if (FAILED(XTaskQueueCreate(0, 0, &g_reentryQueue)) || FAILED(XTaskQueueCreate(0, 0, &g_otherQueue))) return Fail("terminate reentry queues");
    XTaskQueueSubmitCallback(g_otherQueue, 0, nullptr, Count);
    XTaskQueueSubmitCallback(g_reentryQueue, 0, nullptr, TerminateOwnQueue);
    XTaskQueueDispatch(g_reentryQueue, 0, 0);
    return Fail("waiting termination from a dispatched callback returned");
}

struct SlowMonitor {
    std::atomic<int> calls{0};
    std::atomic<bool> inside{false};
    HANDLE entered = CreateEventW(nullptr, FALSE, FALSE, nullptr);
};

void Slow(void* context, void*, std::uint32_t) {
    auto* slow = static_cast<SlowMonitor*>(context);
    slow->inside = true;
    slow->calls.fetch_add(1);
    SetEvent(slow->entered);
    Sleep(200);
    slow->inside = false;
}

struct Unregistering {
    void* queue = nullptr;
    RegistrationToken token{};
};

void SelfUnregister(void* context, void*, std::uint32_t) {
    auto* self = static_cast<Unregistering*>(context);
    XTaskQueueUnregisterMonitor(self->queue, self->token);
}

struct Remover {
    void* queue = nullptr;
    RegistrationToken target{};
    std::atomic<int> calls{0};
};

void RemoveOther(void* context, void*, std::uint32_t) {
    auto* remover = static_cast<Remover*>(context);
    if (remover->calls.fetch_add(1) == 0) XTaskQueueUnregisterMonitor(remover->queue, remover->target);
}

int MonitorScenario() {
    void* queue = nullptr;
    if (FAILED(XTaskQueueCreate(0, 0, &queue))) return Fail("monitor queue");
    SlowMonitor slow;
    RegistrationToken token{};
    XTaskQueueRegisterMonitor(queue, &slow, Slow, &token);
    std::thread submitter([queue] { XTaskQueueSubmitCallback(queue, 0, nullptr, Count); });
    if (WaitForSingleObject(slow.entered, 5000) != WAIT_OBJECT_0) return Fail("slow monitor called");
    XTaskQueueUnregisterMonitor(queue, token);
    const bool stillInside = slow.inside;
    submitter.join();
    if (stillInside) return Fail("unregister returned while the monitor was running");
    XTaskQueueSubmitCallback(queue, 0, nullptr, Count);
    if (slow.calls != 1) return Fail("monitor called after unregister");

    Remover remover;
    remover.queue = queue;
    Monitored target;
    RegistrationToken removerToken{};
    XTaskQueueRegisterMonitor(queue, &remover, RemoveOther, &removerToken);
    XTaskQueueRegisterMonitor(queue, &target, Monitor, &remover.target);
    XTaskQueueSubmitCallback(queue, 0, nullptr, Count);
    XTaskQueueSubmitCallback(queue, 0, nullptr, Count);
    if (remover.calls != 2 || target.work != 0) {
        std::printf("FAIL monitor unregistering another remover=%d target=%d\n", remover.calls.load(), target.work.load());
        return 1;
    }
    XTaskQueueUnregisterMonitor(queue, removerToken);

    std::atomic<bool> stop{false};
    std::atomic<int> submitted{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&, i] {
            while (!stop) {
                XTaskQueueSubmitCallback(queue, i & 1, nullptr, Count);
                submitted.fetch_add(1);
            }
        });
    }
    for (int round = 0; round < 200; ++round) {
        Monitored seen;
        RegistrationToken churn{};
        XTaskQueueRegisterMonitor(queue, &seen, Monitor, &churn);
        XTaskQueueUnregisterMonitor(queue, churn);
        const int after = seen.work + seen.completion;
        Sleep(0);
        if (seen.work + seen.completion != after) {
            stop = true;
            for (std::thread& thread : threads) thread.join();
            return Fail("monitor called after unregister under concurrent submits");
        }
    }
    stop = true;
    for (std::thread& thread : threads) thread.join();
    while (XTaskQueueDispatch(queue, 0, 0)) {}
    while (XTaskQueueDispatch(queue, 1, 0)) {}
    XTaskQueueTerminate(queue, true, nullptr, nullptr);
    XTaskQueueCloseHandle(queue);
    std::printf("monitor ok\n");
    return 0;
}

int MonitorReentry() {
    void* queue = nullptr;
    if (FAILED(XTaskQueueCreate(0, 0, &queue))) return Fail("reentry queue");
    Unregistering self;
    self.queue = queue;
    XTaskQueueRegisterMonitor(queue, &self, SelfUnregister, &self.token);
    XTaskQueueSubmitCallback(queue, 0, nullptr, Count);
    return Fail("unregistering a monitor from its own callback returned");
}

struct SelfWaiter {
    void* queue = nullptr;
    RegistrationToken token{};
    std::atomic<int> calls{0};
    HANDLE fired = CreateEventW(nullptr, FALSE, FALSE, nullptr);
};

void UnregisterSelf(void* context, bool) {
    auto* self = static_cast<SelfWaiter*>(context);
    self->calls.fetch_add(1);
    XTaskQueueUnregisterWaiter(self->queue, self->token);
    SetEvent(self->fired);
}

int WaitRepeated(void* queue, bool manual) {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Tally tally;
    RegistrationToken token{};
    if (FAILED(XTaskQueueRegisterWaiter(queue, 1, event, &tally, Record, &token)) || token.token == 0) return Fail("register waiter");
    for (int i = 1; i <= 5; ++i) {
        SetEvent(event);
        if (manual && !XTaskQueueDispatch(queue, 1, 5000)) return Fail("manual waiter dispatch");
        if (WaitForSingleObject(tally.signaled, 5000) != WAIT_OBJECT_0) return Fail("waiter fired");
        if (manual ? XTaskQueueDispatch(queue, 1, 100) : WaitForSingleObject(tally.signaled, 100) == WAIT_OBJECT_0) return Fail("auto-reset waiter fired twice");
        if (tally.ran != i || tally.canceled != 0) return Fail("waiter count");
    }
    XTaskQueueUnregisterWaiter(queue, token);
    SetEvent(event);
    if (manual ? XTaskQueueDispatch(queue, 1, 300) : WaitForSingleObject(tally.signaled, 300) == WAIT_OBJECT_0) return Fail("waiter fired after unregister");
    CloseHandle(event);
    return 0;
}

int WaiterScenario() {
    void* pool = nullptr;
    void* manual = nullptr;
    void* immediate = nullptr;
    if (FAILED(XTaskQueueCreate(1, 1, &pool)) || FAILED(XTaskQueueCreate(0, 0, &manual)) || FAILED(XTaskQueueCreate(3, 3, &immediate))) return Fail("waiter queues");
    Tally tally;
    RegistrationToken token{};
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (XTaskQueueRegisterWaiter(pool, 0, nullptr, &tally, Record, &token) != E_INVALIDARG) return Fail("waiter without handle");
    if (XTaskQueueRegisterWaiter(pool, 2, event, &tally, Record, &token) != E_INVALIDARG) return Fail("waiter on an invalid port");
    if (XTaskQueueRegisterWaiter(pool, 0, event, &tally, nullptr, &token) != E_POINTER) return Fail("waiter without callback");
    if (XTaskQueueRegisterWaiter(pool, 0, event, &tally, Record, nullptr) != E_POINTER) return Fail("waiter without token");
    if (WaitRepeated(pool, false) != 0 || WaitRepeated(manual, true) != 0 || WaitRepeated(immediate, false) != 0) return 1;

    if (FAILED(XTaskQueueRegisterWaiter(manual, 0, event, &tally, Record, &token))) return Fail("manual-reset waiter");
    SetEvent(event);
    for (int i = 0; i < 3; ++i) {
        if (!XTaskQueueDispatch(manual, 0, 5000)) return Fail("signaled manual-reset waiter fires again");
    }
    if (tally.ran != 3) return Fail("manual-reset waiter count");
    XTaskQueueUnregisterWaiter(manual, token);
    ResetEvent(event);
    while (XTaskQueueDispatch(manual, 0, 0)) {}
    if (XTaskQueueDispatch(manual, 0, 200)) return Fail("manual-reset waiter after unregister");

    SelfWaiter self;
    self.queue = immediate;
    HANDLE selfEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    XTaskQueueRegisterWaiter(immediate, 0, selfEvent, &self, UnregisterSelf, &self.token);
    SetEvent(selfEvent);
    if (WaitForSingleObject(self.fired, 10000) != WAIT_OBJECT_0) return Fail("self-unregistering waiter fired");
    SetEvent(selfEvent);
    if (WaitForSingleObject(self.fired, 200) == WAIT_OBJECT_0 || self.calls != 1) return Fail("waiter unregistered from its own callback");
    CloseHandle(self.fired);

    void* dropped = nullptr;
    XTaskQueueCreate(1, 1, &dropped);
    Tally orphan;
    HANDLE orphanEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    RegistrationToken orphanToken{};
    XTaskQueueRegisterWaiter(dropped, 0, orphanEvent, &orphan, Record, &orphanToken);
    XTaskQueueRegisterWaiter(dropped, 1, orphanEvent, &orphan, Record, &orphanToken);
    XTaskQueueCloseHandle(dropped);
    SetEvent(orphanEvent);
    if (WaitForSingleObject(orphan.signaled, 300) == WAIT_OBJECT_0 || orphan.ran != 0 || orphan.canceled != 0) return Fail("waiters of a closed queue called");
    CloseHandle(orphanEvent);

    Tally canceled;
    HANDLE idle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    RegistrationToken first{};
    RegistrationToken second{};
    XTaskQueueRegisterWaiter(pool, 0, idle, &canceled, Record, &first);
    XTaskQueueRegisterWaiter(pool, 1, idle, &canceled, Record, &second);
    if (first.token == second.token) return Fail("waiter tokens are distinct");
    XTaskQueueTerminate(pool, true, nullptr, nullptr);
    if (canceled.canceled != 2 || canceled.ran != 0) return Fail("terminate cancels each waiter once");
    if (XTaskQueueRegisterWaiter(pool, 0, idle, &canceled, Record, &first) != E_ABORT) return Fail("waiter on a terminated queue");
    SetEvent(idle);
    Sleep(100);
    if (canceled.canceled != 2 || canceled.ran != 0) return Fail("terminated waiter fired");
    XTaskQueueUnregisterWaiter(pool, second);

    Tally racing;
    HANDLE storm = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    void* racer = nullptr;
    for (int round = 0; round < 50; ++round) {
        XTaskQueueCreate(1, 1, &racer);
        RegistrationToken stormToken{};
        XTaskQueueRegisterWaiter(racer, 0, storm, &racing, Record, &stormToken);
        std::thread signaler([storm] { for (int i = 0; i < 20; ++i) SetEvent(storm); });
        if (round & 1) {
            XTaskQueueUnregisterWaiter(racer, stormToken);
        }
        signaler.join();
        XTaskQueueTerminate(racer, true, nullptr, nullptr);
        XTaskQueueCloseHandle(racer);
    }
    const int settled = racing.ran + racing.canceled;
    Sleep(100);
    if (racing.ran + racing.canceled != settled) return Fail("waiter fired after its queue terminated");
    if (racing.canceled > 25) return Fail("unregistered waiter canceled at termination");

    XTaskQueueTerminate(manual, true, nullptr, nullptr);
    XTaskQueueTerminate(immediate, true, nullptr, nullptr);
    XTaskQueueCloseHandle(pool);
    XTaskQueueCloseHandle(manual);
    XTaskQueueCloseHandle(immediate);
    CloseHandle(event);
    CloseHandle(selfEvent);
    CloseHandle(idle);
    CloseHandle(storm);
    std::printf("waiter ok\n");
    return 0;
}

constexpr HRESULT ProviderMismatch = static_cast<HRESULT>(0x8083000Fu);

HRESULT SaveFilesFolder(void* user, char* folder, std::size_t size) {
    AsyncBlock block{};
    const HRESULT begun = XGameSaveFilesGetFolderWithUiAsync(user, Scid, &block);
    if (FAILED(begun)) return begun;
    XAsyncGetStatus(&block, true);
    return XGameSaveFilesGetFolderWithUiResult(&block, size, folder);
}

bool WriteBytes(const std::string& path, DWORD size) {
    HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const std::vector<char> bytes(size, 'x');
    DWORD done = 0;
    const bool written = WriteFile(file, bytes.data(), size, &done, nullptr) && done == size;
    CloseHandle(file);
    return written;
}

HRESULT UnderAFile(bool files);

int GameSaveFiles() {
    void* user = reinterpret_cast<void*>(1);
    const HRESULT failed = UnderAFile(false);
    if (SUCCEEDED(failed) || failed == ProviderMismatch) return Fail("provider under a file");
    std::int64_t quota = 0;
    if (XGameSaveFilesGetRemainingQuota(user, nullptr, &quota) != E_INVALIDARG || XGameSaveFilesGetRemainingQuota(user, Scid, nullptr) != E_INVALIDARG) return Fail("files quota with a null argument");
    if (XGameSaveFilesGetRemainingQuota(user, Scid, &quota) != S_OK || quota != SaveQuota) return Fail("initial files quota");
    AsyncBlock block{};
    if (XGameSaveFilesGetFolderWithUiAsync(user, nullptr, &block) != E_INVALIDARG || XGameSaveFilesGetFolderWithUiAsync(user, Scid, nullptr) != E_INVALIDARG) return Fail("folder with a null argument");

    wchar_t local[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return Fail("LOCALAPPDATA");
    char utf8[MAX_PATH * 3] = {};
    if (WideCharToMultiByte(CP_UTF8, 0, local, -1, utf8, sizeof(utf8), nullptr, nullptr) == 0) return Fail("LOCALAPPDATA as UTF-8");
    char expected[MAX_PATH * 3 + 64] = {};
    std::snprintf(expected, sizeof(expected), "%s\\AnyBox\\1234ABCD\\XGameSave\\", utf8);

    if (XGameSaveFilesGetFolderWithUiAsync(user, Scid, &block) != S_OK || XAsyncGetStatus(&block, true) != S_OK) return Fail("folder async");
    std::size_t size = 0;
    if (XAsyncGetResultSize(&block, &size) != S_OK || size != std::strlen(expected) + 1) return Fail("folder size");
    char folder[MAX_PATH * 3 + 64] = {};
    if (XGameSaveFilesGetFolderWithUiResult(&block, size, nullptr) != E_INVALIDARG) return Fail("folder into a null buffer");
    if (XGameSaveFilesGetFolderWithUiResult(&block, size - 1, folder) != BufferTooSmall) return Fail("folder into a small buffer");
    if (SaveFilesFolder(user, folder, sizeof(folder)) != S_OK || std::strcmp(folder, expected) != 0) return Fail("folder path");

    const std::string root = folder;
    if (!CreateDirectoryA((root + "slot1").c_str(), nullptr) || !CreateDirectoryA((root + "slot1/deep").c_str(), nullptr)) return Fail("create save folders");
    if (!WriteBytes(root + "slot1/state.dat", 100) || !WriteBytes(root + "slot1/deep/more.dat", 20) || !WriteBytes(root + "loose.dat", 7)) return Fail("write save files");
    if (XGameSaveFilesGetRemainingQuota(user, Scid, &quota) != S_OK || quota != SaveQuota - 120) return Fail("files quota after writes");

    void* provider = nullptr;
    if (XGameSaveInitializeProvider(user, Scid, false, &provider) != ProviderMismatch || provider != nullptr) return Fail("XGameSave after XGameSaveFiles");
    block = AsyncBlock{};
    if (XGameSaveInitializeProviderAsync(user, Scid, false, &block) != S_OK || XAsyncGetStatus(&block, true) != ProviderMismatch) return Fail("XGameSave async after XGameSaveFiles");
    if (XGameSaveInitializeProviderResult(&block, &provider) != ProviderMismatch || provider != nullptr) return Fail("XGameSave async result after XGameSaveFiles");
    char again[MAX_PATH * 3 + 64] = {};
    if (SaveFilesFolder(nullptr, again, sizeof(again)) != S_OK || std::strcmp(again, expected) != 0) return Fail("folder on resume");

    if (!CreateDirectoryA((root + "slot2").c_str(), nullptr)) return Fail("create sparse folder");
    HANDLE huge = CreateFileA((root + "slot2/huge.dat").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (huge == INVALID_HANDLE_VALUE) return Fail("create sparse file");
    DWORD returned = 0;
    LARGE_INTEGER end{};
    end.QuadPart = SaveQuota;
    const bool sparse = DeviceIoControl(huge, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) && SetFilePointerEx(huge, end, nullptr, FILE_BEGIN) && SetEndOfFile(huge);
    CloseHandle(huge);
    if (!sparse) return Fail("sparse file");
    if (XGameSaveFilesGetRemainingQuota(user, Scid, &quota) != S_OK || quota != 0) return Fail("files quota over the limit");
    std::printf("gamesavefiles ok\n");
    return 0;
}

int GameSaveFilesMismatch() {
    void* user = reinterpret_cast<void*>(1);
    void* provider = nullptr;
    void* container = nullptr;
    if (XGameSaveInitializeProvider(user, Scid, false, &provider) != S_OK) return Fail("initialize provider");
    if (XGameSaveCreateContainer(provider, "keep", &container) != S_OK || Write(container, "Kept", "data", "kept") != S_OK) return Fail("write kept container");
    XGameSaveCloseContainer(container);
    XGameSaveCloseProvider(provider);
    char folder[MAX_PATH * 3 + 64] = {};
    if (SaveFilesFolder(user, folder, sizeof(folder)) != ProviderMismatch) return Fail("XGameSaveFiles after XGameSave");
    std::int64_t quota = 0;
    if (XGameSaveFilesGetRemainingQuota(user, Scid, &quota) != S_OK || quota != SaveQuota - 4) return Fail("files quota counts XGameSave blobs");
    std::printf("gamesavefiles mismatch ok\n");
    return 0;
}

int GameSaveFilesStop() {
    char folder[MAX_PATH * 3 + 64] = {};
    SaveFilesFolder(reinterpret_cast<void*>(1), folder, sizeof(folder));
    return Fail("folder over XGameSave containers returned");
}

int GameSaveFolderStop(bool async) {
    void* provider = nullptr;
    AsyncBlock block{};
    if (async) {
        if (XGameSaveInitializeProviderAsync(reinterpret_cast<void*>(1), Scid, false, &block) == S_OK) XAsyncGetStatus(&block, true);
    } else {
        XGameSaveInitializeProvider(reinterpret_cast<void*>(1), Scid, false, &provider);
    }
    return Fail("provider over XGameSaveFiles files returned");
}

HRESULT UnderAFile(bool files) {
    wchar_t local[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return S_FALSE;
    const std::wstring blocker = std::wstring(local) + L"\\blocker";
    HANDLE file = CreateFileW(blocker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return S_FALSE;
    CloseHandle(file);
    if (!SetEnvironmentVariableW(L"LOCALAPPDATA", blocker.c_str())) return S_FALSE;
    char folder[MAX_PATH * 3 + 64] = {};
    void* provider = nullptr;
    const HRESULT result = files ? SaveFilesFolder(reinterpret_cast<void*>(1), folder, sizeof(folder)) : XGameSaveInitializeProvider(reinterpret_cast<void*>(1), Scid, false, &provider);
    if (!SetEnvironmentVariableW(L"LOCALAPPDATA", local)) return S_FALSE;
    return result;
}

int GameSaveEmptyFolder() {
    const HRESULT failed = UnderAFile(true);
    if (SUCCEEDED(failed) || failed == ProviderMismatch) return Fail("folder under a file");
    void* provider = nullptr;
    if (XGameSaveInitializeProvider(reinterpret_cast<void*>(1), Scid, false, &provider) != S_OK || provider == nullptr) return Fail("provider over an empty folder");
    XGameSaveCloseProvider(provider);
    std::printf("gamesave empty folder ok\n");
    return 0;
}

int PersistentLocalStorage() {
    constexpr HRESULT InsufficientBuffer = static_cast<HRESULT>(0x8007007Au);
    std::uint32_t titleId = 0;
    if (FAILED(XGameGetXboxTitleId(&titleId))) return Fail("XGameGetXboxTitleId");
    wchar_t local[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return Fail("LOCALAPPDATA");
    char utf8[MAX_PATH * 3] = {};
    if (WideCharToMultiByte(CP_UTF8, 0, local, -1, utf8, sizeof(utf8), nullptr, nullptr) == 0) return Fail("LOCALAPPDATA as UTF-8");
    char expected[MAX_PATH * 3 + 64] = {};
    std::snprintf(expected, sizeof(expected), "%s\\AnyBox\\%08X\\PersistentLocalStorage\\", utf8, titleId);

    std::size_t size = 0;
    if (XPersistentLocalStorageGetPathSize(&size) != S_OK || size != std::strlen(expected) + 1) return Fail("path size");
    std::vector<char> path(size + 8, '#');
    std::size_t used = 12345;
    if (XPersistentLocalStorageGetPath(size - 1, path.data(), &used) != InsufficientBuffer || used != 12345 || path[0] != '#') return Fail("path into a small buffer");
    if (XPersistentLocalStorageGetPath(0, nullptr, &used) != InsufficientBuffer || used != 12345) return Fail("path into no buffer");
    if (XPersistentLocalStorageGetPath(size, path.data(), &used) != S_OK || used != size || std::strcmp(path.data(), expected) != 0) return Fail("path");
    std::fill(path.begin(), path.end(), '#');
    if (XPersistentLocalStorageGetPath(path.size(), path.data(), nullptr) != S_OK || std::strcmp(path.data(), expected) != 0) return Fail("path without pathUsed");

    const std::string root = path.data();
    const DWORD attributes = GetFileAttributesA(root.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return Fail("path is a folder");
    const bool present = GetFileAttributesA((root + "cache.bin").c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!WriteBytes(root + "cache.bin", 4096)) return Fail("write into the storage folder");
    std::int64_t quota = 0;
    if (XGameSaveFilesGetRemainingQuota(reinterpret_cast<void*>(1), Scid, &quota) != S_OK || quota != SaveQuota) return Fail("save quota after a storage write");

    const std::string grown = root + "sub\\grown.bin";
    DeleteFileA(grown.c_str());
    if (!CreateDirectoryA((root + "sub").c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return Fail("create a storage subfolder");
    StorageSpace before{};
    if (XPersistentLocalStorageGetSpaceInfo(&before) != S_OK) return Fail("space info");
    if (!WriteBytes(grown, 1234)) return Fail("write a second file");
    StorageSpace space{};
    if (XPersistentLocalStorageGetSpaceInfo(&space) != S_OK) return Fail("space info after a write");
    if (space.usedBytes != before.usedBytes + 1234) return Fail("used bytes after a write");
    if (space.totalFreeBytes != space.availableFreeBytes || space.usedBytes + space.availableFreeBytes != space.totalBytes) return Fail("space info sums");
    std::printf("pls path %s cache %s used %llu total %llu free %llu\n", root.c_str(), present ? "present" : "absent", static_cast<unsigned long long>(space.usedBytes), static_cast<unsigned long long>(space.totalBytes), static_cast<unsigned long long>(space.availableFreeBytes));
    std::printf("pls ok\n");
    return 0;
}

}

int DynamicLoad() {
    const HMODULE runtime = LoadLibraryExW(L"xgameruntime.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (runtime == nullptr) return Fail("dynamic load of xgameruntime.dll");
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(runtime, path, MAX_PATH);
    const wchar_t* slash = wcsrchr(path, L'\\');
    const wchar_t* base = slash != nullptr ? slash + 1 : path;
    if (_wcsicmp(base, L"ab_xgameruntime.dll") != 0) return Fail("xgameruntime.dll loaded by name is not AnyBox's");
    if (GetModuleHandleW(L"XGameRuntime.dll") != runtime) return Fail("GetModuleHandleW of the runtime");
    HMODULE handle = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, "xgameruntime.dll", &handle) || handle != runtime) return Fail("GetModuleHandleExA of the runtime");
    if (LoadLibraryA("xgameruntime") != runtime) return Fail("LoadLibraryA without an extension");
    if (GetProcAddress(runtime, "XSystemGetDeviceType") == nullptr) return Fail("an export of the redirected runtime");
    if (LoadLibraryW(L"kernel32.dll") != GetModuleHandleW(L"kernel32.dll")) return Fail("a DLL AnyBox does not serve loads unchanged");
    std::printf("dynamic load ok\n");
    return 0;
}

int ApiImpl() {
    const HMODULE runtime = LoadLibraryExW(L"xgameruntime.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (runtime == nullptr) return Fail("dynamic load of xgameruntime.dll");
    using ErrorReport = void (*)(HRESULT, const char*);
    using QueryApi = HRESULT (*)(const GUID*, const GUID*, void**);
    const auto report = reinterpret_cast<ErrorReport>(reinterpret_cast<void*>(GetProcAddress(runtime, "XErrorReport")));
    const auto query = reinterpret_cast<QueryApi>(reinterpret_cast<void*>(GetProcAddress(runtime, "QueryApiImpl")));
    for (const char* name : {"InitializeApiImpl", "InitializeApiImplEx", "InitializeApiImplEx2", "UninitializeApiImpl"}) {
        if (GetProcAddress(runtime, name) == nullptr) return Fail(name);
    }
    if (report == nullptr || query == nullptr) return Fail("XErrorReport or QueryApiImpl");
    report(static_cast<HRESULT>(0x89240107), "fake title error report");
    const GUID provider{0x11111111, 0x2222, 0x3333, {0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb}};
    const GUID api{0xcafef00d, 0x0102, 0x0304, {0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c}};
    void* implementation = reinterpret_cast<void*>(1);
    query(&provider, &api, &implementation);
    return Fail("QueryApiImpl returned");
}

void* g_api = nullptr;

void* ApiSlot(int slot) {
    return (*static_cast<void* const* const*>(g_api))[slot];
}

int OpenThreadingApi() {
    const HMODULE runtime = LoadLibraryExW(L"xgameruntime.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (runtime == nullptr) return Fail("dynamic load of xgameruntime.dll");
    using Initialize = HRESULT (*)(std::uint64_t, std::uint64_t, std::uint64_t, void*);
    using QueryApi = HRESULT (*)(const GUID*, const GUID*, void**);
    const auto initialize = reinterpret_cast<Initialize>(reinterpret_cast<void*>(GetProcAddress(runtime, "InitializeApiImplEx2")));
    const auto query = reinterpret_cast<QueryApi>(reinterpret_cast<void*>(GetProcAddress(runtime, "QueryApiImpl")));
    if (initialize == nullptr || query == nullptr) return Fail("API-impl entry points");
    if (initialize(0x0000633600002712ull, 0x000065f400001832ull, 3, nullptr) != S_OK) return Fail("handshake");
    const GUID threading{0x073b7dcb, 0x1fcf, 0x4030, {0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28}};
    if (query(&threading, &threading, &g_api) != S_OK || g_api == nullptr) return Fail("QueryApiImpl for the threading interface");
    return 0;
}

HRESULT ApiProvider(std::uint32_t op, const AsyncProviderData* data) {
    using Schedule = HRESULT (*)(void*, AsyncBlock*, std::uint32_t);
    using Complete = void (*)(void*, AsyncBlock*, HRESULT, std::size_t);
    if (op == OpBegin) return reinterpret_cast<Schedule>(ApiSlot(9))(g_api, data->async, 0);
    if (op == OpDoWork) reinterpret_cast<Complete>(ApiSlot(10))(g_api, data->async, S_OK, 0);
    return S_OK;
}

int ApiThreading() {
    if (OpenThreadingApi() != 0) return 1;
    using Create = HRESULT (*)(void*, std::uint32_t, std::uint32_t, void**);
    using Delayed = HRESULT (*)(void*, void*, std::uint32_t, std::uint32_t, void*, void (*)(void*, bool));
    using Terminate = HRESULT (*)(void*, void*, bool, void*, void (*)(void*));
    using Close = void (*)(void*, void*);
    using Begin = HRESULT (*)(void*, AsyncBlock*, void*, const void*, const char*, AsyncProvider);
    using Status = HRESULT (*)(void*, AsyncBlock*, bool);
    using Query = HRESULT (*)(void*, const GUID&, void**);
    using Release = ULONG (*)(void*);
    using Assert = void (*)(void*);
    reinterpret_cast<Assert>(ApiSlot(29))(g_api);
    void* queue = nullptr;
    if (reinterpret_cast<Create>(ApiSlot(12))(g_api, 1, 1, &queue) != S_OK || queue == nullptr) return Fail("queue through slot 12");
    HANDLE ran = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const auto signal = [](void* context, bool) { SetEvent(static_cast<HANDLE>(context)); };
    if (reinterpret_cast<Delayed>(ApiSlot(19))(g_api, queue, 0, 0, ran, signal) != S_OK) return Fail("callback through slot 19");
    if (WaitForSingleObject(ran, 5000) != WAIT_OBJECT_0) return Fail("the callback ran");
    CloseHandle(ran);
    using Submit = HRESULT (*)(void*, void*, std::uint32_t, void*, void (*)(void*, bool));
    using Dispatch = bool (*)(void*, void*, std::uint32_t, std::uint32_t);
    void* manual = nullptr;
    if (reinterpret_cast<Create>(ApiSlot(12))(g_api, 0, 0, &manual) != S_OK) return Fail("manual queue through slot 12");
    int counted = 0;
    const auto count = [](void* context, bool) { ++*static_cast<int*>(context); };
    if (reinterpret_cast<Submit>(ApiSlot(18))(g_api, manual, 0, &counted, count) != S_OK) return Fail("callback through slot 18");
    if (counted != 0 || !reinterpret_cast<Dispatch>(ApiSlot(16))(g_api, manual, 0, 0) || counted != 1) return Fail("dispatch through slot 16 runs the callback");
    if (reinterpret_cast<Dispatch>(ApiSlot(16))(g_api, manual, 0, 0)) return Fail("dispatch through slot 16 on an empty port");
    reinterpret_cast<Terminate>(ApiSlot(22))(g_api, manual, true, nullptr, nullptr);
    reinterpret_cast<Close>(ApiSlot(17))(g_api, manual);
    AsyncBlock block{};
    block.queue = queue;
    if (reinterpret_cast<Begin>(ApiSlot(7))(g_api, &block, nullptr, nullptr, nullptr, ApiProvider) != S_OK) return Fail("XAsync through slot 7");
    if (reinterpret_cast<Status>(ApiSlot(3))(g_api, &block, true) != S_OK) return Fail("XAsync completed through slots 9, 10 and 3");
    if (reinterpret_cast<Terminate>(ApiSlot(22))(g_api, queue, true, nullptr, nullptr) != S_OK) return Fail("terminate through slot 22");
    reinterpret_cast<Close>(ApiSlot(17))(g_api, queue);
    const GUID other{0x12345678, 0x1234, 0x1234, {1, 2, 3, 4, 5, 6, 7, 8}};
    void* none = reinterpret_cast<void*>(1);
    if (reinterpret_cast<Query>(ApiSlot(0))(g_api, other, &none) != E_NOINTERFACE || none != nullptr) return Fail("QueryInterface for another interface");
    reinterpret_cast<Release>(ApiSlot(2))(g_api);
    std::printf("api threading ok\n");
    return 0;
}

int ApiUnmapped() {
    if (OpenThreadingApi() != 0) return 1;
    using Register = HRESULT (*)(void*, void*, std::uint32_t, void*, void*, void*, void*);
    reinterpret_cast<Register>(ApiSlot(20))(g_api, nullptr, 0, nullptr, nullptr, nullptr, nullptr);
    return Fail("an unmapped slot returned");
}

int main(int argc, char** argv) {
    const char* scenario = argc > 1 ? argv[1] : "taskqueue";
    if (std::strcmp(scenario, "stub") == 0) {
        XUserAddAsync();
        return Fail("stub returned");
    }
    if (std::strcmp(scenario, "device") == 0) {
        std::printf("device type %u packaged %d\n", XSystemGetDeviceType(), XPackageIsPackagedProcess() ? 1 : 0);
        return 0;
    }
    if (std::strcmp(scenario, "console_id") == 0) {
        char id[39] = {};
        std::size_t used = 0;
        if (XSystemGetConsoleId(sizeof(id) - 1, id, &used) != HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) return Fail("short console id buffer");
        if (XSystemGetConsoleId(sizeof(id), id, &used) != S_OK || used != sizeof(id)) return Fail("console id");
        if (std::strcmp(id, "00000000.00000000.00000000.00000000.00") != 0) return Fail("console id value");
        if (XSystemGetConsoleId(sizeof(id), id, nullptr) != S_OK) return Fail("console id without used");
        std::printf("console id ok\n");
        return 0;
    }
    if (std::strcmp(scenario, "sandbox_id") == 0) {
        char id[16] = {};
        std::size_t used = 0;
        if (XSystemGetXboxLiveSandboxId(sizeof(id) - 1, id, &used) != HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) return Fail("short sandbox id buffer");
        if (XSystemGetXboxLiveSandboxId(sizeof(id), id, &used) != S_OK || used != sizeof("RETAIL")) return Fail("sandbox id");
        if (std::strcmp(id, "RETAIL") != 0) return Fail("sandbox id value");
        if (XSystemGetXboxLiveSandboxId(sizeof(id), id, nullptr) != S_OK) return Fail("sandbox id without used");
        std::printf("sandbox id ok\n");
        return 0;
    }
    if (std::strcmp(scenario, "async") == 0) {
        return Async();
    }
    if (std::strcmp(scenario, "dynamic_load") == 0) {
        return DynamicLoad();
    }
    if (std::strcmp(scenario, "api_impl") == 0) {
        return ApiImpl();
    }
    if (std::strcmp(scenario, "api_threading") == 0) {
        return ApiThreading();
    }
    if (std::strcmp(scenario, "api_unmapped") == 0) {
        return ApiUnmapped();
    }
    if (std::strcmp(scenario, "gamesave") == 0) {
        return GameSave();
    }
    if (std::strcmp(scenario, "taskqueue_composite") == 0) {
        return Guarded("composite", Composite);
    }
    if (std::strcmp(scenario, "taskqueue_monitor") == 0) {
        return Guarded("monitor", MonitorScenario);
    }
    if (std::strcmp(scenario, "taskqueue_monitor_reentry") == 0) {
        return Guarded("monitor reentry", MonitorReentry);
    }
    if (std::strcmp(scenario, "taskqueue_terminate_reentry") == 0) {
        return Guarded("terminate reentry", TerminateReentry);
    }
    if (std::strcmp(scenario, "taskqueue_waiter") == 0) {
        return Guarded("waiter", WaiterScenario);
    }
    if (std::strcmp(scenario, "gamesavefiles") == 0) {
        return GameSaveFiles();
    }
    if (std::strcmp(scenario, "gamesavefiles_mismatch") == 0) {
        return GameSaveFilesMismatch();
    }
    if (std::strcmp(scenario, "gamesavefiles_containers") == 0) {
        return GameSaveFilesStop();
    }
    if (std::strcmp(scenario, "gamesave_folders") == 0) {
        return GameSaveFolderStop(false);
    }
    if (std::strcmp(scenario, "gamesave_folders_async") == 0) {
        return GameSaveFolderStop(true);
    }
    if (std::strcmp(scenario, "gamesave_empty_folder") == 0) {
        return GameSaveEmptyFolder();
    }
    if (std::strcmp(scenario, "pls") == 0) {
        return PersistentLocalStorage();
    }
    if (std::strcmp(scenario, "pls_prompt") == 0) {
        AsyncBlock block{};
        XPersistentLocalStoragePromptUserForSpaceAsync(1, &block);
        return Fail("prompt for space returned");
    }
    if (std::strcmp(scenario, "pls_prompt_result") == 0) {
        AsyncBlock block{};
        XPersistentLocalStoragePromptUserForSpaceResult(&block);
        return Fail("prompt for space result returned");
    }
    if (std::strcmp(scenario, "async_null") == 0) {
        XAsyncGetStatus(nullptr, false);
        return Fail("null async block returned");
    }
    if (std::strcmp(scenario, "async_terminated") == 0) {
        return AsyncTerminated(false);
    }
    if (std::strcmp(scenario, "async_begin_terminated") == 0) {
        return AsyncTerminated(true);
    }
    if (std::strcmp(scenario, "startup") == 0) {
        return Startup();
    }
    if (std::strcmp(scenario, "startup_options") == 0) {
        const RuntimeOptions inlineConfig{1, "<Game configVersion=\"1\"/>"};
        XGameRuntimeInitializeWithOptions(&inlineConfig);
        return Fail("inline config returned");
    }
    if (std::strcmp(scenario, "thread_assert") == 0) {
        XThreadSetTimeSensitive(true);
        XThreadAssertNotTimeSensitive();
        return Fail("assert on a time-sensitive thread returned");
    }
    if (std::strcmp(scenario, "crash") == 0) {
        XGameRuntimeInitialize();
        volatile int* null = nullptr;
        *null = 1;
        return Fail("crash returned");
    }
    if (std::strcmp(scenario, "crash_chained") == 0) {
        XGameRuntimeInitialize();
        g_titleFilter = SetUnhandledExceptionFilter(ChainingFilter);
        volatile int* null = nullptr;
        *null = 1;
        return Fail("crash returned");
    }
    if (std::strcmp(scenario, "crash_recovered") == 0) {
        XGameRuntimeInitialize();
        g_readOnlyPage = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READONLY);
        if (g_readOnlyPage == nullptr) return Fail("VirtualAlloc");
        SetUnhandledExceptionFilter(RecoveringFilter);
        *static_cast<volatile int*>(g_readOnlyPage) = 1;
        std::puts("recovered ok");
        return 0;
    }
    return TaskQueue();
}
