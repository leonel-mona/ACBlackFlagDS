// acbf_hdd_optimizer.cpp
// HDD-optimized asset loader for Assassin's Creed Black Flag Resynced
// Hooks into version.dll (Ultimate ASI Loader) to intercept I/O on .forge files
// Uses IAT hooking for CreateThreadpoolIo and ReadFile, implements read-ahead cache

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <strsafe.h>
#include <intrin.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <inttypes.h>

#ifndef UNICODE
#define UNICODE
#endif

// ========================================================================
// Configuration
// ========================================================================
static struct Config {
    size_t BufferMB;                 // Read buffer size per async op (default 1MB)
    size_t PrefetchAheadMB;          // How many MB to pre-fetch ahead (default 4MB)
    int MaxConcurrentOps;            // Max simultaneous pending async ops per forge file (default 8)
    BOOL Enabled;                    // Master toggle
} g_Config = { 1, 4, 8, TRUE };

static WCHAR g_LogPath[MAX_PATH] = { 0 };
static CRITICAL_SECTION g_LogLock;
static FILE* g_LogFile = nullptr;

// ========================================================================
// Logging
// ========================================================================
static void LogWrite(const WCHAR* fmt, ...) {
    if (!g_LogFile) return;
    EnterCriticalSection(&g_LogLock);
    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(g_LogFile, L"[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfwprintf(g_LogFile, fmt, args);
    va_end(args);
    fwprintf(g_LogFile, L"\n");
    fflush(g_LogFile);
    LeaveCriticalSection(&g_LogLock);
}

static void LogInit() {
    InitializeCriticalSection(&g_LogLock);
    GetModuleFileNameW(GetModuleHandleW(nullptr), g_LogPath, MAX_PATH);
    WCHAR* p = wcsrchr(g_LogPath, L'\\');
    if (p) { wcscpy_s(p + 1, MAX_PATH - (p - g_LogPath + 1), L"ACBlackFlagHDDOptimizer.log"); }
    g_LogFile = _wfopen(g_LogPath, L"a, ccs=UTF-8");
    if (g_LogFile) {
        LogWrite(L"=== ACBF HDD Optimizer Starting ===");
    }
}

static void LogShutdown() {
    if (g_LogFile) {
        LogWrite(L"=== ACBF HDD Optimizer Shutting Down ===");
        fclose(g_LogFile);
        g_LogFile = nullptr;
    }
    DeleteCriticalSection(&g_LogLock);
}

// ========================================================================
// Cache Entry
// ========================================================================
#define CACHE_ALIGNMENT (64 * 1024)
#define MAX_CACHE_SIZE (256 * 1024 * 1024) // 256 MB total
#define MAX_CACHE_ENTRIES 1024

struct CacheEntry {
    WCHAR path[MAX_PATH];
    ULONGLONG offset;
    size_t size;
    BYTE* data;
    ULONGLONG lastAccess;
    BOOL valid;
};

static CacheEntry g_Cache[MAX_CACHE_ENTRIES] = { 0 };
static CRITICAL_SECTION g_CacheLock;
static size_t g_CacheTotalSize = 0;
static volatile LONG64 g_AccessCounter = 0;

static void CacheInit() {
    InitializeCriticalSection(&g_CacheLock);
    for (int i = 0; i < MAX_CACHE_ENTRIES; ++i) {
        g_Cache[i].valid = FALSE;
        g_Cache[i].data = nullptr;
    }
}

static void CacheShutdown() {
    EnterCriticalSection(&g_CacheLock);
    for (int i = 0; i < MAX_CACHE_ENTRIES; ++i) {
        if (g_Cache[i].data) {
            VirtualFree(g_Cache[i].data, 0, MEM_RELEASE);
            g_Cache[i].data = nullptr;
        }
        g_Cache[i].valid = FALSE;
    }
    g_CacheTotalSize = 0;
    LeaveCriticalSection(&g_CacheLock);
    DeleteCriticalSection(&g_CacheLock);
}

static BOOL IsForgeFile(const WCHAR* path) {
    if (!path) return FALSE;
    size_t len = wcslen(path);
    if (len < 6) return FALSE;
    const WCHAR* ext = path + len - 6;
    if (_wcsicmp(ext, L".forge") != 0) return FALSE;
    // Skip patch files (they're tiny deltas)
    if (len >= 12) {
        const WCHAR* patch = path + len - 12;
        if (_wcsnicmp(patch, L"_patch_", 7) == 0) return FALSE;
    }
    return TRUE;
}

static void AlignRange(ULONGLONG offset, size_t size, ULONGLONG* outOffset, size_t* outSize) {
    ULONGLONG alignedOffset = offset & ~(CACHE_ALIGNMENT - 1);
    ULONGLONG end = offset + size;
    ULONGLONG alignedEnd = (end + CACHE_ALIGNMENT - 1) & ~(CACHE_ALIGNMENT - 1);
    *outOffset = alignedOffset;
    *outSize = (size_t)(alignedEnd - alignedOffset);
}

static CacheEntry* CacheFind(const WCHAR* path, ULONGLONG offset) {
    EnterCriticalSection(&g_CacheLock);
    ULONGLONG alignedOffset;
    size_t alignedSize;
    AlignRange(offset, 0, &alignedOffset, &alignedSize);
    for (int i = 0; i < MAX_CACHE_ENTRIES; ++i) {
        if (g_Cache[i].valid && _wcsicmp(g_Cache[i].path, path) == 0 && g_Cache[i].offset == alignedOffset) {
            g_Cache[i].lastAccess = InterlockedIncrement64(&g_AccessCounter);
            CacheEntry* entry = &g_Cache[i];
            LeaveCriticalSection(&g_CacheLock);
            return entry;
        }
    }
    LeaveCriticalSection(&g_CacheLock);
    return nullptr;
}

static CacheEntry* CacheAllocate(const WCHAR* path, ULONGLONG offset, size_t size) {
    EnterCriticalSection(&g_CacheLock);
    ULONGLONG alignedOffset;
    size_t alignedSize;
    AlignRange(offset, size, &alignedOffset, &alignedSize);
    if (alignedSize > g_Config.BufferMB * 1024 * 1024) {
        alignedSize = g_Config.BufferMB * 1024 * 1024;
    }
    // Evict LRU if needed
    while (g_CacheTotalSize + alignedSize > MAX_CACHE_SIZE) {
        LONG64 oldest = LLONG_MAX;
        int victim = -1;
        for (int i = 0; i < MAX_CACHE_ENTRIES; ++i) {
            if (g_Cache[i].valid && g_Cache[i].lastAccess < oldest) {
                oldest = g_Cache[i].lastAccess;
                victim = i;
            }
        }
        if (victim >= 0) {
            if (g_Cache[victim].data) {
                VirtualFree(g_Cache[victim].data, 0, MEM_RELEASE);
                g_Cache[victim].data = nullptr;
            }
            g_CacheTotalSize -= g_Cache[victim].size;
            g_Cache[victim].valid = FALSE;
            LogWrite(L"Cache evicted: %s @ 0x%llx (%zu KB)", g_Cache[victim].path, g_Cache[victim].offset, g_Cache[victim].size / 1024);
        } else {
            break;
        }
    }
    // Find free slot
    int slot = -1;
    for (int i = 0; i < MAX_CACHE_ENTRIES; ++i) {
        if (!g_Cache[i].valid) { slot = i; break; }
    }
    if (slot < 0) {
        LeaveCriticalSection(&g_CacheLock);
        return nullptr;
    }
    BYTE* buffer = (BYTE*)VirtualAlloc(nullptr, alignedSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buffer) {
        LeaveCriticalSection(&g_CacheLock);
        LogWrite(L"Cache alloc failed: %zu KB", alignedSize / 1024);
        return nullptr;
    }
    wcscpy_s(g_Cache[slot].path, MAX_PATH, path);
    g_Cache[slot].offset = alignedOffset;
    g_Cache[slot].size = alignedSize;
    g_Cache[slot].data = buffer;
    g_Cache[slot].lastAccess = InterlockedIncrement64(&g_AccessCounter);
    g_Cache[slot].valid = TRUE;
    g_CacheTotalSize += alignedSize;
    CacheEntry* entry = &g_Cache[slot];
    LeaveCriticalSection(&g_CacheLock);
    LogWrite(L"Cache allocated: %s @ 0x%llx (%zu KB), total %zu MB", path, alignedOffset, alignedSize / 1024, g_CacheTotalSize / (1024*1024));
    return entry;
}

static void CacheReadAsync(const WCHAR* path, HANDLE hFile, ULONGLONG offset, size_t size) {
    // Fire-and-forget background read using thread pool
    // For simplicity, we use a dedicated worker thread approach
    // In production this would use CreateThreadpoolWork
    (void)path; (void)hFile; (void)offset; (void)size;
    // TODO: Implement actual async prefetch
}

// ========================================================================
// Original Function Pointers
// ========================================================================
static BOOL (WINAPI* g_OrigReadFile)(
    HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
    LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped) = nullptr;

static PTP_IO (WINAPI* g_OrigCreateThreadpoolIo)(
    HANDLE fl, PTP_WIN32_IO_CALLBACK pfnio, PVOID pv, PTP_CALLBACK_ENVIRON pcbe) = nullptr;

static BOOL (WINAPI* g_OrigSetFileCompletionNotificationBytes)(
    HANDLE FileHandle, ULONG CompletionNotificationBytes) = nullptr;

static HANDLE (WINAPI* g_OrigCreateFileW)(
    LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) = nullptr;

// ========================================================================
// Hooked Functions
// ========================================================================
BOOL WINAPI HookedReadFile(
    HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
    LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped) {

    if (!g_Config.Enabled) {
        return g_OrigReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);
    }

    // Try to get file path from handle (best effort)
    WCHAR path[MAX_PATH] = { 0 };
    DWORD len = GetFinalPathNameByHandleW(hFile, path, MAX_PATH, VOLUME_NAME_DOS);
    BOOL isForge = (len > 0 && IsForgeFile(path));

    if (!isForge || !lpOverlapped) {
        return g_OrigReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);
    }

    ULONGLONG offset = ((ULONGLONG)lpOverlapped->OffsetHigh << 32) | lpOverlapped->Offset;

    // Check cache
    CacheEntry* entry = CacheFind(path, offset);
    if (entry && entry->data && offset >= entry->offset && 
        (offset + nNumberOfBytesToRead) <= (entry->offset + entry->size)) {
        // Cache hit
        size_t copyOffset = (size_t)(offset - entry->offset);
        size_t copySize = nNumberOfBytesToRead;
        if (copyOffset + copySize > entry->size) copySize = entry->size - copyOffset;
        memcpy(lpBuffer, entry->data + copyOffset, copySize);
        if (lpNumberOfBytesRead) *lpNumberOfBytesRead = (DWORD)copySize;
        LogWrite(L"Cache HIT: %s @ 0x%llx (%u bytes)", path, offset, nNumberOfBytesToRead);
        return TRUE;
    }

    // Cache miss - enlarge read request for prefetch
    size_t prefetchSize = g_Config.PrefetchAheadMB * 1024 * 1024;
    size_t maxRead = g_Config.BufferMB * 1024 * 1024;
    size_t requestSize = nNumberOfBytesToRead + prefetchSize;
    if (requestSize > maxRead) requestSize = maxRead;

    ULONGLONG alignedOffset;
    size_t alignedSize;
    AlignRange(offset, requestSize, &alignedOffset, &alignedSize);

    // Allocate cache entry
    CacheEntry* newEntry = CacheAllocate(path, alignedOffset, alignedSize);
    if (newEntry) {
        // Update overlapped to read larger aligned range
        lpOverlapped->Offset = (DWORD)alignedOffset;
        lpOverlapped->OffsetHigh = (DWORD)(alignedOffset >> 32);
        DWORD origBytes = nNumberOfBytesToRead;

        BOOL result = g_OrigReadFile(hFile, newEntry->data, (DWORD)alignedSize, lpNumberOfBytesRead, lpOverlapped);
        
        if (result && lpNumberOfBytesRead && *lpNumberOfBytesRead > 0) {
            // Copy requested portion to user buffer
            size_t copyOffset = (size_t)(offset - alignedOffset);
            size_t copySize = origBytes;
            if (copyOffset + copySize > (size_t)*lpNumberOfBytesRead) 
                copySize = (size_t)*lpNumberOfBytesRead - copyOffset;
            memcpy(lpBuffer, newEntry->data + copyOffset, copySize);
            *lpNumberOfBytesRead = (DWORD)copySize;
            LogWrite(L"Cache MISS -> Prefetch: %s @ 0x%llx requested %u, read %zu KB aligned", 
                path, offset, origBytes, alignedSize / 1024);
            return TRUE;
        }
    }

    // Fallback to original
    return g_OrigReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);
}

PTP_IO WINAPI HookedCreateThreadpoolIo(
    HANDLE fl, PTP_WIN32_IO_CALLBACK pfnio, PVOID pv, PTP_CALLBACK_ENVIRON pcbe) {

    PTP_IO io = g_OrigCreateThreadpoolIo(fl, pfnio, pv, pcbe);
    
    if (g_Config.Enabled && io && g_OrigSetFileCompletionNotificationBytes) {
        // Set completion notification to 64KB for more efficient completion port delivery
        g_OrigSetFileCompletionNotificationBytes(fl, 64 * 1024);
    }
    return io;
}

HANDLE WINAPI HookedCreateFileW(
    LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {

    HANDLE h = g_OrigCreateFileW(lpFileName, dwDesiredAccess, dwShareMode,
                                 lpSecurityAttributes, dwCreationDisposition,
                                 dwFlagsAndAttributes, hTemplateFile);
    
    if (g_Config.Enabled && h != INVALID_HANDLE_VALUE && g_OrigSetFileCompletionNotificationBytes) {
        if (IsForgeFile(lpFileName)) {
            g_OrigSetFileCompletionNotificationBytes(h, 64 * 1024);
            LogWrite(L"CreateFileW: SetCompletionNotificationBytes on %s", lpFileName);
        }
    }
    return h;
}

// ========================================================================
// IAT Hooking
// ========================================================================
typedef struct _IMPORT_DESCRIPTOR {
    DWORD   OriginalFirstThunk;
    DWORD   TimeDateStamp;
    DWORD   ForwarderChain;
    DWORD   Name;
    DWORD   FirstThunk;
} IMPORT_DESCRIPTOR, *PIMPORT_DESCRIPTOR;

static void* GetProcAddressSafe(HMODULE hMod, const char* name) {
    return (void*)GetProcAddress(hMod, name);
}

static BOOL HookIATFunction(const char* dllName, const char* funcName, void* newFunc, void** origFunc) {
    HMODULE hExe = GetModuleHandleW(nullptr);
    if (!hExe) return FALSE;

    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)hExe;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return FALSE;

    PIMAGE_NT_HEADERS64 nt = (PIMAGE_NT_HEADERS64)((BYTE*)hExe + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return FALSE;

    DWORD importRVA = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!importRVA) return FALSE;

    PIMPORT_DESCRIPTOR importDesc = (PIMPORT_DESCRIPTOR)((BYTE*)hExe + importRVA);
    for (; importDesc->Name; ++importDesc) {
        const char* importedDll = (const char*)((BYTE*)hExe + importDesc->Name);
        if (_stricmp(importedDll, dllName) != 0) continue;

        PIMAGE_THUNK_DATA64 thunk = (PIMAGE_THUNK_DATA64)((BYTE*)hExe + importDesc->FirstThunk);
        for (; thunk->u1.Function; ++thunk) {
            // Check if this thunk points to our target function
            // We need to resolve the original address first
            void* targetAddr = GetProcAddressSafe(GetModuleHandleA(dllName), funcName);
            if (!targetAddr) continue;

            if (thunk->u1.Function == (ULONGLONG)targetAddr) {
                DWORD oldProtect;
                if (VirtualProtect(&thunk->u1.Function, sizeof(ULONGLONG), PAGE_READWRITE, &oldProtect)) {
                    *origFunc = (void*)thunk->u1.Function;
                    thunk->u1.Function = (ULONGLONG)newFunc;
                    VirtualProtect(&thunk->u1.Function, sizeof(ULONGLONG), oldProtect, &oldProtect);
                    LogWrite(L"IAT Hooked: %hs!%hs -> %p", dllName, funcName, newFunc);
                    return TRUE;
                }
            }
        }
    }
    LogWrite(L"IAT Hook FAILED: %hs!%hs", dllName, funcName);
    return FALSE;
}

static void SetupHooks() {
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!hKernel32) return;

    g_OrigSetFileCompletionNotificationBytes = (BOOL (WINAPI*)(HANDLE, ULONG))
        GetProcAddressSafe(hKernel32, "SetFileCompletionNotificationBytes");

    HookIATFunction("KERNEL32.dll", "ReadFile", (void*)HookedReadFile, (void**)&g_OrigReadFile);
    HookIATFunction("KERNEL32.dll", "CreateThreadpoolIo", (void*)HookedCreateThreadpoolIo, (void**)&g_OrigCreateThreadpoolIo);
    HookIATFunction("KERNEL32.dll", "CreateFileW", (void*)HookedCreateFileW, (void**)&g_OrigCreateFileW);

    if (!g_OrigReadFile) {
        g_OrigReadFile = (BOOL (WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED))
            GetProcAddressSafe(hKernel32, "ReadFile");
    }
    if (!g_OrigCreateThreadpoolIo) {
        g_OrigCreateThreadpoolIo = (PTP_IO (WINAPI*)(HANDLE, PTP_WIN32_IO_CALLBACK, PVOID, PTP_CALLBACK_ENVIRON))
            GetProcAddressSafe(hKernel32, "CreateThreadpoolIo");
    }
    if (!g_OrigCreateFileW) {
        g_OrigCreateFileW = (HANDLE (WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE))
            GetProcAddressSafe(hKernel32, "CreateFileW");
    }
}

// ========================================================================
// Config Loading
// ========================================================================
static void LoadConfig() {
    WCHAR iniPath[MAX_PATH];
    GetModuleFileNameW(GetModuleHandleW(nullptr), iniPath, MAX_PATH);
    WCHAR* p = wcsrchr(iniPath, L'\\');
    if (p) wcscpy_s(p + 1, MAX_PATH - (p - iniPath + 1), L"ACBlackFlagFix.ini");
    
    WCHAR val[32];
    if (GetPrivateProfileStringW(L"HDDOptimizer", L"BufferMB", L"1", val, 32, iniPath)) {
        g_Config.BufferMB = (size_t)wcstol(val, nullptr, 10);
    }
    if (GetPrivateProfileStringW(L"HDDOptimizer", L"PrefetchAheadMegabytes", L"4", val, 32, iniPath)) {
        g_Config.PrefetchAheadMB = (size_t)wcstol(val, nullptr, 10);
    }
    if (GetPrivateProfileStringW(L"HDDOptimizer", L"MaxConcurrentOps", L"8", val, 32, iniPath)) {
        g_Config.MaxConcurrentOps = (int)wcstol(val, nullptr, 10);
    }
    if (GetPrivateProfileStringW(L"HDDOptimizer", L"Enabled", L"true", val, 32, iniPath)) {
        g_Config.Enabled = (_wcsicmp(val, L"true") == 0 || _wcsicmp(val, L"1") == 0);
    }
    LogWrite(L"Config: BufferMB=%zu, PrefetchAheadMB=%zu, MaxConcurrentOps=%d, Enabled=%d",
             g_Config.BufferMB, g_Config.PrefetchAheadMB, g_Config.MaxConcurrentOps, g_Config.Enabled);
}

// ========================================================================
// DLL Entry
// ========================================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    (void)hModule; (void)lpReserved;
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        LogInit();
        LoadConfig();
        CacheInit();
        SetupHooks();
        LogWrite(L"HDD Optimizer initialized successfully");
        break;
    case DLL_PROCESS_DETACH:
        CacheShutdown();
        LogShutdown();
        break;
    }
    return TRUE;
}