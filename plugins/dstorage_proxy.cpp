#define _WIN32_WINNT 0x0600
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "../../../third_party/minhook-1.3.4/include/MinHook.h"

// Define NTSTATUS and related types if not defined
typedef LONG NTSTATUS;

typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef struct _OBJECT_ATTRIBUTES {
    ULONG Length;
    HANDLE RootDirectory;
    PUNICODE_STRING ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;

typedef struct _IO_STATUS_BLOCK {
    union {
        NTSTATUS Status;
        PVOID Pointer;
    } DUMMYUNIONNAME;
    ULONG_PTR Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

typedef VOID (NTAPI *PIO_APC_ROUTINE)(
    PVOID ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock,
    ULONG Reserved
);

// Pointers into dstorage_original.dll
typedef HRESULT (WINAPI* PFN_GetFactoryCore)(REFIID, void**);
typedef HRESULT (WINAPI* PFN_SetConfigCore)(UINT32);
typedef HRESULT (WINAPI* PFN_CreateCodecCore)(UINT32, REFIID, void**);

static PFN_GetFactoryCore  g_GetFactory = nullptr;
static PFN_SetConfigCore   g_SetConfig  = nullptr;
static PFN_CreateCodecCore g_CreateCodec = nullptr;

// MinHook function pointer types
typedef NTSTATUS (NTAPI *PFN_NtReadFile)(
    HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
    PLARGE_INTEGER ByteOffset, PULONG Key);

typedef NTSTATUS (NTAPI* PFN_NtCreateFile)(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
    PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER AllocationSize, ULONG FileAttributes,
    ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength);

typedef NTSTATUS (NTAPI* PFN_NtOpenFile)(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
    PIO_STATUS_BLOCK IoStatusBlock, ULONG ShareAccess, ULONG OpenOptions);

static PFN_NtReadFile g_RealNtReadFile = nullptr;
static PFN_NtCreateFile g_RealNtCreateFile = nullptr;
static PFN_NtOpenFile g_RealNtOpenFile = nullptr;

static HMODULE g_Orig = nullptr;

// Config flag
static int g_EnableLevel1 = 1;

// Logging helper
static void LogWrite(const char* msg) {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    WCHAR* slash = wcsrchr(path, L'\\');
    if (slash) {
        wcscpy(slash + 1, L"DirectStorageProxy.log");
    } else {
        wcscpy(path, L"DirectStorageProxy.log");
    }

    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(h, msg, (DWORD)strlen(msg), &written, nullptr);
        CloseHandle(h);
    }
}

// Check if path ends with .forge (case-insensitive)
static bool IsForgeFile(const wchar_t* path) {
    if (!path) return false;
    size_t len = wcslen(path);
    if (len < 6) return false;
    const wchar_t* ext = path + len - 6;
    return _wcsicmp(ext, L".forge") == 0;
}

// Get full path from ObjectAttributes
static bool GetPathFromObjectAttributes(POBJECT_ATTRIBUTES objAttr, wchar_t* outPath, DWORD outSize) {
    if (!objAttr || !objAttr->ObjectName || !outPath || outSize == 0) {
        return false;
    }

    PUNICODE_STRING name = objAttr->ObjectName;
    if (!name->Buffer || name->Length == 0) {
        return false;
    }

    size_t charCount = name->Length / sizeof(wchar_t);
    if (charCount >= outSize) {
        charCount = outSize - 1;
    }
    wcsncpy(outPath, name->Buffer, charCount);
    outPath[charCount] = L'\0';

    return true;
}

// Hooked NtCreateFile for Level 1 flag rewriting
NTSTATUS NTAPI HookedNtCreateFile(
    PHANDLE FileHandle,
    ACCESS_MASK DesiredAccess,
    POBJECT_ATTRIBUTES ObjectAttributes,
    PIO_STATUS_BLOCK IoStatusBlock,
    PLARGE_INTEGER AllocationSize,
    ULONG FileAttributes,
    ULONG ShareAccess,
    ULONG CreateDisposition,
    ULONG CreateOptions,
    PVOID EaBuffer,
    ULONG EaLength)
{
    if (!g_EnableLevel1) {
        return g_RealNtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
                                  AllocationSize, FileAttributes, ShareAccess, CreateDisposition,
                                  CreateOptions, EaBuffer, EaLength);
    }

    wchar_t filePath[MAX_PATH];
    ULONG modifiedOptions = CreateOptions;

    if (GetPathFromObjectAttributes(ObjectAttributes, filePath, MAX_PATH)) {
        if (IsForgeFile(filePath)) {
            // Clear FILE_RANDOM_ACCESS (0x800) and FILE_NO_INTERMEDIATE_BUFFERING (0x8)
            // Add FILE_SEQUENTIAL_ONLY (0x4)
            modifiedOptions &= ~0x800;
            modifiedOptions &= ~0x8;
            modifiedOptions |= 0x4;

            char logbuf[512];
            snprintf(logbuf, sizeof(logbuf),
                     "[L1] NtCreateFile: .forge file detected, flags 0x%08lX -> 0x%08lX path=\"%ls\"\n",
                     CreateOptions, modifiedOptions, filePath);
            LogWrite(logbuf);
        }
    }

    return g_RealNtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
                              AllocationSize, FileAttributes, ShareAccess, CreateDisposition,
                              modifiedOptions, EaBuffer, EaLength);
}

// Hooked NtOpenFile for Level 1 flag rewriting
NTSTATUS NTAPI HookedNtOpenFile(
    PHANDLE FileHandle,
    ACCESS_MASK DesiredAccess,
    POBJECT_ATTRIBUTES ObjectAttributes,
    PIO_STATUS_BLOCK IoStatusBlock,
    ULONG ShareAccess,
    ULONG OpenOptions)
{
    if (!g_EnableLevel1) {
        return g_RealNtOpenFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
                                ShareAccess, OpenOptions);
    }

    wchar_t filePath[MAX_PATH];
    ULONG modifiedOptions = OpenOptions;

    if (GetPathFromObjectAttributes(ObjectAttributes, filePath, MAX_PATH)) {
        if (IsForgeFile(filePath)) {
            // Clear FILE_RANDOM_ACCESS (0x800) and FILE_NO_INTERMEDIATE_BUFFERING (0x8)
            // Add FILE_SEQUENTIAL_ONLY (0x4)
            modifiedOptions &= ~0x800;
            modifiedOptions &= ~0x8;
            modifiedOptions |= 0x4;

            char logbuf[512];
            snprintf(logbuf, sizeof(logbuf),
                     "[L1] NtOpenFile: .forge file detected, flags 0x%08lX -> 0x%08lX path=\"%ls\"\n",
                     OpenOptions, modifiedOptions, filePath);
            LogWrite(logbuf);
        }
    }

    return g_RealNtOpenFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
                            ShareAccess, modifiedOptions);
}

// Hooked NtReadFile - logging ONLY, no cache or data modification
NTSTATUS NTAPI HookedNtReadFile(
    HANDLE FileHandle,
    HANDLE Event,
    PIO_APC_ROUTINE ApcRoutine,
    PVOID ApcContext,
    PIO_STATUS_BLOCK IoStatusBlock,
    PVOID Buffer,
    ULONG Length,
    PLARGE_INTEGER ByteOffset,
    PULONG Key)
{
    NTSTATUS status = g_RealNtReadFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset, Key);

    if (!Event && !ApcRoutine && IoStatusBlock) {
        wchar_t filePath[MAX_PATH];
        DWORD res = GetFinalPathNameByHandleW(FileHandle, filePath, MAX_PATH, 0);
        if (res && wcscmp(filePath, L"") != 0) {
            size_t len = wcslen(filePath);
            if (len >= 6) {
                const wchar_t* ext = filePath + len - 6;
                if (_wcsicmp(ext, L".forge") == 0) {
                    char logbuf[512];
                    LONGLONG longOffset = ByteOffset ? ByteOffset->QuadPart : 0;
                    unsigned long long bytesRead = IoStatusBlock->Information;
                    snprintf(logbuf, sizeof(logbuf),
                             "[IO] ReadFile path=\"%ls\" offset=%lld length=%lu status=0x%08X bytes=%llu\n",
                             filePath,
                             (long long)longOffset,
                             Length,
                             (unsigned int)status,
                             bytesRead);
                    LogWrite(logbuf);
                }
            }
        }
    }

    return status;
}

// Thread to press Space key 5s after launch — REMOVED

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        WCHAR iniPath[MAX_PATH];
        GetModuleFileNameW(nullptr, iniPath, MAX_PATH);
        WCHAR* slash = wcsrchr(iniPath, L'\\');
        if (slash) {
            wcscpy(slash + 1, L"ACBlackFlagFix.ini");
        } else {
            wcscpy(iniPath, L"ACBlackFlagFix.ini");
        }
        g_EnableLevel1 = GetPrivateProfileIntW(L"HDDPreloader", L"EnableLevel1", 1, iniPath);

        char cfgBuf[256];
        snprintf(cfgBuf, sizeof(cfgBuf), "[Config] EnableLevel1=%d\n", g_EnableLevel1);
        LogWrite(cfgBuf);

        g_Orig = LoadLibraryW(L"dstorage_original.dll");
        if (g_Orig) {
            g_GetFactory = reinterpret_cast<PFN_GetFactoryCore>(reinterpret_cast<uintptr_t>(GetProcAddress(g_Orig, "DStorageGetFactoryCore")));
            g_SetConfig  = reinterpret_cast<PFN_SetConfigCore>(reinterpret_cast<uintptr_t>(GetProcAddress(g_Orig, "DStorageSetConfigurationCore")));
            g_CreateCodec = reinterpret_cast<PFN_CreateCodecCore>(reinterpret_cast<uintptr_t>(GetProcAddress(g_Orig, "DStorageCreateCompressionCodecCore")));
            char buf[64];
            snprintf(buf, sizeof(buf), "[Proxy] loaded, PID=%lu\n", GetCurrentProcessId());
            LogWrite(buf);
        }

        MH_Initialize();
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll) {
            g_RealNtReadFile = reinterpret_cast<PFN_NtReadFile>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtReadFile")));
            if (g_RealNtReadFile) {
                MH_CreateHook(reinterpret_cast<LPVOID>(g_RealNtReadFile), reinterpret_cast<LPVOID>(&HookedNtReadFile), reinterpret_cast<LPVOID*>(&g_RealNtReadFile));
            }

            g_RealNtCreateFile = reinterpret_cast<PFN_NtCreateFile>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtCreateFile")));
            if (g_RealNtCreateFile) {
                MH_CreateHook(reinterpret_cast<LPVOID>(g_RealNtCreateFile), reinterpret_cast<LPVOID>(&HookedNtCreateFile), reinterpret_cast<LPVOID*>(&g_RealNtCreateFile));
            }

            g_RealNtOpenFile = reinterpret_cast<PFN_NtOpenFile>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtOpenFile")));
            if (g_RealNtOpenFile) {
                MH_CreateHook(reinterpret_cast<LPVOID>(g_RealNtOpenFile), reinterpret_cast<LPVOID>(&HookedNtOpenFile), reinterpret_cast<LPVOID*>(&g_RealNtOpenFile));
            }

            MH_EnableHook(MH_ALL_HOOKS);
        }

        // No thread to press space

        DisableThreadLibraryCalls(hModule);
    } else if (reason == DLL_PROCESS_DETACH) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();

        if (g_Orig) {
            FreeLibrary(g_Orig);
            g_Orig = nullptr;
        }
    }
    return TRUE;
}

extern "C" {

HRESULT WINAPI DStorageGetFactory(REFIID riid, void** ppv)
{
    if (!g_GetFactory) {
        LogWrite("[DStorageGetFactory] g_GetFactory not loaded, returning E_FAIL\n");
        return E_FAIL;
    }
    HRESULT hr = g_GetFactory(riid, ppv);
    char line[256];
    snprintf(line, sizeof(line),
             "GetFactory(riid={%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}) -> hr=0x%08X, factory=%p\n",
             riid.Data1, riid.Data2, riid.Data3,
             riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3],
             riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7],
             (unsigned int)hr, ppv ? *ppv : nullptr);
    LogWrite(line);
    return hr;
}

HRESULT WINAPI DStorageSetConfiguration(UINT32 flags)
{
    if (!g_SetConfig) return E_FAIL;
    return g_SetConfig(flags);
}

HRESULT WINAPI DStorageCreateCompressionCodec(UINT32 format, REFIID riid, void** codec)
{
    if (!g_CreateCodec) return E_FAIL;
    return g_CreateCodec(format, riid, codec);
}

} // extern "C"
