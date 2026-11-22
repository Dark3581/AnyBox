#include <windows.h>

#if defined(FAKE_MODULE_STOP)
extern "C" __declspec(dllimport) HRESULT D3D12XboxCreateDevice();
#endif

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) {
        return TRUE;
    }
#if defined(FAKE_MODULE_FAIL)
    return FALSE;
#elif defined(FAKE_MODULE_STOP)
    D3D12XboxCreateDevice();
    return TRUE;
#elif defined(FAKE_MODULE_CRASH)
    volatile int* null = nullptr;
    *null = 1;
    return TRUE;
#else
    return TRUE;
#endif
}
