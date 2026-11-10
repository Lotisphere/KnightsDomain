// xinput1_3.dll proxy.
//
// The game imports XINPUT1_3.dll, so this file placed next to the exe is loaded instead of the system copy.
//  - Every export is a thin forwarder; the real DLL (System32\xinput1_3.dll, else xinput1_4.dll) is loaded lazily on
//    the first forwarded call, never inside DllMain.
//  - DllMain(PROCESS_ATTACH) only records the current thread id (process start-up runs on the main thread, which is
//    UE's game thread) and starts a worker thread. The worker loads <proxy dir>\KnightsDomain\KnightsDomain.dll and
//    calls KnightsDomain_Start(mainThreadId) (formerly QDCoop\QDCoop.dll / QDCoop_Start).
// No SDK, no MinHook here.
#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace {
constexpr DWORD kNotConnected = ERROR_DEVICE_NOT_CONNECTED;

DWORD g_mainThreadId = 0;

// ------------------------------------------------------------------------------------------------ real DLL
std::once_flag g_realOnce;
HMODULE        g_real = nullptr;

void LoadReal() {
    wchar_t sys[MAX_PATH];
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    const std::wstring dir(sys, n);
    g_real = LoadLibraryW((dir + L"\\xinput1_3.dll").c_str());
    if (!g_real) g_real = LoadLibraryW((dir + L"\\xinput1_4.dll").c_str());
    if (!g_real) OutputDebugStringW(L"[KnightsDomain proxy] neither xinput1_3.dll nor xinput1_4.dll could be loaded\n");
}

HMODULE Real() {
    std::call_once(g_realOnce, LoadReal);
    return g_real;
}

// Resolves once; a missing export stays nullptr (callers return ERROR_DEVICE_NOT_CONNECTED).
struct LazyProc {
    const char*        name;  // export name, or MAKEINTRESOURCEA(ordinal)
    std::atomic<void*> ptr{nullptr};
    std::atomic<bool>  resolved{false};

    template <class Fn>
    Fn Get() {
        if (!resolved.load(std::memory_order_acquire)) {
            HMODULE h = Real();
            void* p = h ? reinterpret_cast<void*>(GetProcAddress(h, name)) : nullptr;
            ptr.store(p, std::memory_order_relaxed);
            resolved.store(true, std::memory_order_release);
        }
        return reinterpret_cast<Fn>(ptr.load(std::memory_order_relaxed));
    }
};

LazyProc p_GetState{"XInputGetState"};
LazyProc p_SetState{"XInputSetState"};
LazyProc p_GetCapabilities{"XInputGetCapabilities"};
LazyProc p_Enable{"XInputEnable"};
LazyProc p_GetDSoundAudioDeviceGuids{"XInputGetDSoundAudioDeviceGuids"};
LazyProc p_GetBatteryInformation{"XInputGetBatteryInformation"};
LazyProc p_GetKeystroke{"XInputGetKeystroke"};
LazyProc p_GetStateEx{MAKEINTRESOURCEA(100)};
LazyProc p_WaitForGuideButton{MAKEINTRESOURCEA(101)};
LazyProc p_CancelGuideButtonWait{MAKEINTRESOURCEA(102)};
LazyProc p_PowerOffController{MAKEINTRESOURCEA(103)};

// ------------------------------------------------------------------------------------------------ loader thread
std::wstring ModulePath(HMODULE module) {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(module, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

DWORD WINAPI LoaderThread(LPVOID param) {
    const std::wstring self = ModulePath(static_cast<HMODULE>(param));
    const size_t slash = self.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        OutputDebugStringW(L"[KnightsDomain proxy] cannot determine proxy directory\n");
        return 1;
    }
    const std::wstring modPath = self.substr(0, slash) + L"\\KnightsDomain\\KnightsDomain.dll";
    // LOAD_WITH_ALTERED_SEARCH_PATH: KnightsDomain.dll's own dependencies are also searched next to it.
    HMODULE mod = LoadLibraryExW(modPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!mod) {
        const std::wstring msg = L"[KnightsDomain proxy] LoadLibrary failed (" + std::to_wstring(GetLastError()) + L"): " +
                                 modPath + L"\n";
        OutputDebugStringW(msg.c_str());
        return 1;
    }
    using StartFn = void (*)(uint32_t);
    auto start = reinterpret_cast<StartFn>(GetProcAddress(mod, "KnightsDomain_Start"));
    if (!start) {
        OutputDebugStringW(L"[KnightsDomain proxy] KnightsDomain.dll has no KnightsDomain_Start export\n");
        return 1;
    }
    start(static_cast<uint32_t>(g_mainThreadId));
    return 0;
}
}  // namespace

// ---------------------------------------------------------------------------------------------------- exports
// Signatures follow xinput.h; struct pointers are passed through untouched, so they are typed as void*.
extern "C" {

DWORD WINAPI Proxy_XInputGetState(DWORD userIndex, void* state) {
    auto fn = p_GetState.Get<DWORD(WINAPI*)(DWORD, void*)>();
    return fn ? fn(userIndex, state) : kNotConnected;
}

DWORD WINAPI Proxy_XInputSetState(DWORD userIndex, void* vibration) {
    auto fn = p_SetState.Get<DWORD(WINAPI*)(DWORD, void*)>();
    return fn ? fn(userIndex, vibration) : kNotConnected;
}

DWORD WINAPI Proxy_XInputGetCapabilities(DWORD userIndex, DWORD flags, void* capabilities) {
    auto fn = p_GetCapabilities.Get<DWORD(WINAPI*)(DWORD, DWORD, void*)>();
    return fn ? fn(userIndex, flags, capabilities) : kNotConnected;
}

void WINAPI Proxy_XInputEnable(BOOL enable) {
    auto fn = p_Enable.Get<void(WINAPI*)(BOOL)>();
    if (fn) fn(enable);
}

DWORD WINAPI Proxy_XInputGetDSoundAudioDeviceGuids(DWORD userIndex, GUID* renderGuid, GUID* captureGuid) {
    auto fn = p_GetDSoundAudioDeviceGuids.Get<DWORD(WINAPI*)(DWORD, GUID*, GUID*)>();
    return fn ? fn(userIndex, renderGuid, captureGuid) : kNotConnected;  // absent in xinput1_4
}

DWORD WINAPI Proxy_XInputGetBatteryInformation(DWORD userIndex, BYTE devType, void* info) {
    auto fn = p_GetBatteryInformation.Get<DWORD(WINAPI*)(DWORD, BYTE, void*)>();
    return fn ? fn(userIndex, devType, info) : kNotConnected;
}

DWORD WINAPI Proxy_XInputGetKeystroke(DWORD userIndex, DWORD reserved, void* keystroke) {
    auto fn = p_GetKeystroke.Get<DWORD(WINAPI*)(DWORD, DWORD, void*)>();
    return fn ? fn(userIndex, reserved, keystroke) : kNotConnected;
}

// @100 XInputGetStateEx
DWORD WINAPI Proxy_XInputGetStateEx(DWORD userIndex, void* state) {
    auto fn = p_GetStateEx.Get<DWORD(WINAPI*)(DWORD, void*)>();
    return fn ? fn(userIndex, state) : kNotConnected;
}

// @101 XInputWaitForGuideButton
DWORD WINAPI Proxy_XInputWaitForGuideButton(DWORD userIndex, DWORD flags, void* overlapped) {
    auto fn = p_WaitForGuideButton.Get<DWORD(WINAPI*)(DWORD, DWORD, void*)>();
    return fn ? fn(userIndex, flags, overlapped) : kNotConnected;
}

// @102 XInputCancelGuideButtonWait
DWORD WINAPI Proxy_XInputCancelGuideButtonWait(DWORD userIndex) {
    auto fn = p_CancelGuideButtonWait.Get<DWORD(WINAPI*)(DWORD)>();
    return fn ? fn(userIndex) : kNotConnected;
}

// @103 XInputPowerOffController
DWORD WINAPI Proxy_XInputPowerOffController(DWORD userIndex) {
    auto fn = p_PowerOffController.Get<DWORD(WINAPI*)(DWORD)>();
    return fn ? fn(userIndex) : kNotConnected;
}

}  // extern "C"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Only two things here (loader lock): remember the main thread, start the loader thread.
        g_mainThreadId = GetCurrentThreadId();
        if (HANDLE h = CreateThread(nullptr, 0, &LoaderThread, module, 0, nullptr)) CloseHandle(h);
    }
    return TRUE;
}
