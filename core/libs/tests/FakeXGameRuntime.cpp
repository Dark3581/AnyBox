#include <windows.h>

#include <cstddef>
#include <cstdint>

extern "C" {
__declspec(dllexport) HRESULT XGameRuntimeInitialize() { return E_FAIL; }
__declspec(dllexport) HRESULT XGameGetXboxTitleId(std::uint32_t*) { return E_FAIL; }
__declspec(dllexport) HRESULT XTaskQueueCreate(std::uint32_t, std::uint32_t, void**) { return E_FAIL; }
__declspec(dllexport) HRESULT XTaskQueueSubmitCallback(void*, std::uint32_t, void*, void*) { return E_FAIL; }
__declspec(dllexport) HRESULT XTaskQueueSubmitDelayedCallback(void*, std::uint32_t, std::uint32_t, void*, void*) { return E_FAIL; }
__declspec(dllexport) bool XTaskQueueDispatch(void*, std::uint32_t, std::uint32_t) { return false; }
__declspec(dllexport) HRESULT XTaskQueueTerminate(void*, bool, void*, void*) { return E_FAIL; }
__declspec(dllexport) void XTaskQueueCloseHandle(void*) {}
__declspec(dllexport) bool XTaskQueueGetCurrentProcessTaskQueue(void**) { return false; }
__declspec(dllexport) void XTaskQueueSetCurrentProcessTaskQueue(void*) {}
__declspec(dllexport) HRESULT XTaskQueueCreateComposite(void*, void*, void**) { return E_FAIL; }
__declspec(dllexport) HRESULT XTaskQueueGetPort(void*, std::uint32_t, void**) { return E_FAIL; }
__declspec(dllexport) HRESULT XTaskQueueRegisterMonitor(void*, void*, void*, void*) { return E_FAIL; }
__declspec(dllexport) void XTaskQueueUnregisterMonitor(void*, std::uint64_t) {}
__declspec(dllexport) HRESULT XTaskQueueRegisterWaiter(void*, std::uint32_t, void*, void*, void*, void*) { return E_FAIL; }
__declspec(dllexport) void XTaskQueueUnregisterWaiter(void*, std::uint64_t) {}
__declspec(dllexport) HRESULT XAsyncBegin(void*, void*, const void*, const char*, void*) { return E_FAIL; }
__declspec(dllexport) HRESULT XAsyncSchedule(void*, std::uint32_t) { return E_FAIL; }
__declspec(dllexport) void XAsyncComplete(void*, HRESULT, std::size_t) {}
__declspec(dllexport) void XAsyncCancel(void*) {}
__declspec(dllexport) HRESULT XAsyncRun(void*, void*) { return E_FAIL; }
__declspec(dllexport) HRESULT XAsyncGetStatus(void*, bool) { return E_FAIL; }
__declspec(dllexport) HRESULT XAsyncGetResultSize(void*, std::size_t*) { return E_FAIL; }
__declspec(dllexport) HRESULT XAsyncGetResult(void*, const void*, std::size_t, void*, std::size_t*) { return E_FAIL; }
__declspec(dllexport) HRESULT XUserAddAsync() { return E_FAIL; }
__declspec(dllexport) std::uint32_t XSystemGetDeviceType() { return 0xffffffffu; }
__declspec(dllexport) bool XPackageIsPackagedProcess() { return true; }
__declspec(dllexport) HRESULT XGameRuntimeInitializeWithOptions(const void*) { return E_FAIL; }
__declspec(dllexport) bool XGameRuntimeIsFeatureAvailable(std::uint32_t) { return false; }
__declspec(dllexport) HRESULT XPackageGetCurrentProcessPackageIdentifier(std::size_t, char*) { return E_FAIL; }
__declspec(dllexport) HRESULT XThreadSetTimeSensitive(bool) { return E_FAIL; }
__declspec(dllexport) bool XThreadIsTimeSensitive() { return true; }
__declspec(dllexport) void XThreadAssertNotTimeSensitive() {}
__declspec(dllexport) HRESULT XGameSaveInitializeProvider() { return E_FAIL; }
__declspec(dllexport) HRESULT XGameSaveInitializeProviderAsync() { return E_FAIL; }
__declspec(dllexport) HRESULT XGameSaveInitializeProviderResult() { return E_FAIL; }
__declspec(dllexport) HRESULT XGameSaveGetRemainingQuota() { return E_FAIL; }
__declspec(dllexport) HRESULT XGameSaveGetRemainingQuotaAsync() { return E_FAIL; }
__declspec(dllexport) HRESULT XGameSaveGetRemainingQuotaResult() { return E_FAIL; }
__declspec(dllexport) HRESULT XGameSaveDeleteContainer() { return E_FAIL; }
__declspec(dllexport) HRESULT XGameSaveDeleteContainerAsync() { return E_FAIL; }
__declspec(dllexport) HRESULT XGameSaveDeleteContainerResult() { return E_FAIL; }
__declspec(dllexport) HRESULT XGameSaveCreateContainer() { return E_FAIL; }
__declspec(dllexport) HRESULT XGameSaveGetContainerInfo() { return E_FAIL; }
__declspec(dllexport) HRESULT XGameSaveEnumerateContainerInfo() { return E_FAIL; }
__declspec(dllexport) HRESULT XGameSaveEnumerateContainerInfoByName() { return E_FAIL; }
__declspec(dllexport) HRESULT XGameSaveEnumerateBlobInfo() { return E_FAIL; }
__declspec(dllexport) HRESULT XGameSaveEnumerateBlobInfoByName() { return E_FAIL; }
__declspec(dllexport) HRESULT XGameSaveReadBlobData() { return E_FAIL; }
__declspec(dllexport) HRESULT XGameSaveReadBlobDataAsync() { return E_FAIL; }
__declspec(dllexport) HRESULT XGameSaveReadBlobDataResult() { return E_FAIL; }
__declspec(dllexport) HRESULT XGameSaveCreateUpdate() { return E_FAIL; }
__declspec(dllexport) HRESULT XGameSaveSubmitBlobWrite() { return E_FAIL; }
__declspec(dllexport) HRESULT XGameSaveSubmitBlobDelete() { return E_FAIL; }
__declspec(dllexport) HRESULT XGameSaveSubmitUpdate() { return E_FAIL; }
__declspec(dllexport) HRESULT XGameSaveSubmitUpdateAsync() { return E_FAIL; }
__declspec(dllexport) HRESULT XGameSaveSubmitUpdateResult() { return E_FAIL; }
__declspec(dllexport) void XGameSaveCloseProvider(void*) {}
__declspec(dllexport) void XGameSaveCloseContainer(void*) {}
__declspec(dllexport) void XGameSaveCloseUpdate(void*) {}
__declspec(dllexport) HRESULT XGameSaveFilesGetFolderWithUiAsync(void*, const char*, void*) { return E_FAIL; }
__declspec(dllexport) HRESULT XGameSaveFilesGetFolderWithUiResult(void*, std::size_t, char*) { return E_FAIL; }
__declspec(dllexport) HRESULT XGameSaveFilesGetRemainingQuota(void*, const char*, std::int64_t*) { return E_FAIL; }
__declspec(dllexport) HRESULT XSystemGetConsoleId(std::size_t, char*, std::size_t*) { return E_FAIL; }
__declspec(dllexport) HRESULT XSystemGetXboxLiveSandboxId(std::size_t, char*, std::size_t*) { return E_FAIL; }
}
