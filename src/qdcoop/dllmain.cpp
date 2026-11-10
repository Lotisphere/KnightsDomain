// KnightsDomain.dll entry points. Loaded by the xinput1_3 proxy from a worker thread.
#include <Windows.h>

#include <cstdint>

#include "qdcoop/core/Core.h"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}

extern "C" __declspec(dllexport) void KnightsDomain_Start(uint32_t mainThreadId) { qd::core::Start(mainThreadId); }
