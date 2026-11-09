// dstorage_proxy.cpp -- v4: PURE FORWARDER (no logging, no wrappers)
// Pass-through ONLY: just forwards to dstorage_original.dll
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

static HMODULE g_Orig = nullptr;

// Force load at process attach
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_Orig = LoadLibraryW(L"dstorage_original.dll");
        DisableThreadLibraryCalls(hModule);
    } else if (reason == DLL_PROCESS_DETACH && !lpReserved && g_Orig) {
        FreeLibrary(g_Orig);
        g_Orig = nullptr;
    }
    return TRUE;
}
