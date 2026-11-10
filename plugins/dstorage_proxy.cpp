#define _WIN32_WINNT 0x0600
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <algorithm>
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

// File position information for NtQueryInformationFile/NtSetInformationFile
typedef struct _FILE_STANDARD_INFORMATION {
    LARGE_INTEGER AllocationSize;
    LARGE_INTEGER EndOfFile;
    ULONG NumberOfLinks;
    BOOLEAN DeletePending;
    BOOLEAN Directory;
} FILE_STANDARD_INFORMATION, *PFILE_STANDARD_INFORMATION;

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

typedef NTSTATUS (NTAPI* PFN_NtClose)(HANDLE Handle);

typedef NTSTATUS (NTAPI* PFN_NtQueryInformationFile)(
    HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation,
    ULONG Length, ULONG FileInformationClass);

typedef NTSTATUS (NTAPI* PFN_NtSetInformationFile)(
    HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation,
    ULONG Length, ULONG FileInformationClass);

static PFN_NtReadFile g_RealNtReadFile = nullptr;
static PFN_NtCreateFile g_RealNtCreateFile = nullptr;
static PFN_NtOpenFile g_RealNtOpenFile = nullptr;
static PFN_NtClose g_RealNtClose = nullptr;
static PFN_NtQueryInformationFile g_RealNtQueryInformationFile = nullptr;
static PFN_NtSetInformationFile g_RealNtSetInformationFile = nullptr;

static HMODULE g_Orig = nullptr;

// Config flags
static int g_EnableLevel1 = 1;
static int g_EnableLevel2 = 0;
static int g_EnableLogging = 1;
static int g_CacheMB = 4096;        // default 4 GB
static int g_BlockMB = 1;           // default 1 MiB
static int g_PrefetchBlocks = 4;    // default 4 blocks ahead

// Derived constants
static size_t g_BlockSize = 1024 * 1024;     // 1 MiB default
static size_t g_CacheSize = 0;               // computed from CacheMB
static size_t g_MaxBlocks = 0;               // computed from CacheMB / BlockMB

// Logging helper
static void LogWrite(const char* msg) {
    if (!g_EnableLogging) return;
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

// ============================================================
// Block Cache (Level 2/3)
// ============================================================

// Block cache entry
struct CacheBlock {
    uint64_t file_id;
    uint64_t block_index;
    void* data;
    size_t size;
    uint64_t last_access;  // for LRU
    bool valid;
    CacheBlock* next;      // hash chain
    CacheBlock* lru_prev;  // LRU list
    CacheBlock* lru_next;  // LRU list
};

static CacheBlock* g_CacheTable = nullptr;     // hash table
static size_t g_CacheTableSize = 0;            // power of 2
static CacheBlock g_LruHead = {};             // LRU list head (most recent)
static CacheBlock g_LruTail = {};             // LRU list tail (least recent)

// SRWLock for cache
static SRWLOCK g_CacheLock = SRWLOCK_INIT;

// File handle tracking
struct FileEntry {
    HANDLE handle;
    uint64_t file_id;
    wchar_t path[MAX_PATH];
    uint64_t last_read_offset;   // for sequential detection
    uint64_t last_read_length;
    bool has_last_read;
    FileEntry* next;
};

static FileEntry* g_FileTable = nullptr;
static SRWLOCK g_FileLock = SRWLOCK_INIT;
static uint64_t g_NextFileId = 1;

// Prefetch
struct PrefetchTask {
    uint64_t file_id;
    uint64_t block_index;
    PrefetchTask* next;
};

static PrefetchTask* g_PrefetchHead = nullptr;
static PrefetchTask* g_PrefetchTail = nullptr;
static SRWLOCK g_PrefetchLock = SRWLOCK_INIT;
static HANDLE g_PrefetchEvent = nullptr;
static HANDLE g_PrefetchThread = nullptr;
static DWORD g_PrefetchThreadId = 0;
static volatile LONG g_PrefetchRunning = 0;
static volatile LONG g_PrefetchInFlight = 0;

// Initialize LRU list
static void LruInit() {
    g_LruHead.lru_prev = nullptr;
    g_LruHead.lru_next = &g_LruTail;
    g_LruTail.lru_prev = &g_LruHead;
    g_LruTail.lru_next = nullptr;
}

// Add block to front of LRU (most recent)
static void LruAdd(CacheBlock* block) {
    block->lru_next = g_LruHead.lru_next;
    block->lru_prev = &g_LruHead;
    g_LruHead.lru_next->lru_prev = block;
    g_LruHead.lru_next = block;
}

// Remove block from LRU list
static void LruRemove(CacheBlock* block) {
    block->lru_prev->lru_next = block->lru_next;
    block->lru_next->lru_prev = block->lru_prev;
    block->lru_prev = nullptr;
    block->lru_next = nullptr;
}

// Move block to front of LRU (most recent)
static void LruMoveToFront(CacheBlock* block) {
    LruRemove(block);
    LruAdd(block);
}

// Evict least recently used block
static CacheBlock* LruEvict() {
    CacheBlock* victim = g_LruTail.lru_prev;
    if (victim == &g_LruHead) return nullptr;  // empty
    LruRemove(victim);
    return victim;
}

// Hash function for (file_id, block_index)
static size_t CacheHash(uint64_t file_id, uint64_t block_index) {
    // Simple hash: combine file_id and block_index
    uint64_t h = file_id ^ (block_index * 0x9e3779b97f4a7c15ULL);
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return (size_t)h & (g_CacheTableSize - 1);
}

// Initialize cache with given budget
static bool CacheInit(size_t cache_mb, size_t block_mb) {
    g_BlockSize = block_mb * 1024 * 1024;
    g_CacheSize = cache_mb * 1024 * 1024;
    g_MaxBlocks = g_CacheSize / g_BlockSize;

    if (g_MaxBlocks == 0) g_MaxBlocks = 1;

    // Hash table size: power of 2, at least 2x max blocks for low load factor
    g_CacheTableSize = 1;
    while (g_CacheTableSize < g_MaxBlocks * 4) {
        g_CacheTableSize <<= 1;
    }
    if (g_CacheTableSize < 1024) g_CacheTableSize = 1024;

    // Allocate cache blocks in one big VirtualAlloc
    size_t total_block_mem = g_MaxBlocks * sizeof(CacheBlock);
    g_CacheTable = (CacheBlock*)VirtualAlloc(nullptr, total_block_mem, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_CacheTable) {
        LogWrite("[L2] CacheInit: VirtualAlloc for cache blocks failed\n");
        return false;
    }

    // Zero-initialize
    memset(g_CacheTable, 0, total_block_mem);

    // Initialize LRU
    LruInit();

    char logbuf[256];
    snprintf(logbuf, sizeof(logbuf),
             "[L2] CacheInit: CacheMB=%zu, BlockMB=%zu, MaxBlocks=%zu, TableSize=%zu, BlockSize=%zu\n",
             cache_mb, block_mb, g_MaxBlocks, g_CacheTableSize, g_BlockSize);
    LogWrite(logbuf);
    return true;
}

// Cleanup cache
static void CacheCleanup() {
    if (g_CacheTable) {
        // Free block data memory
        for (size_t i = 0; i < g_MaxBlocks; ++i) {
            if (g_CacheTable[i].data) {
                VirtualFree(g_CacheTable[i].data, 0, MEM_RELEASE);
            }
        }
        VirtualFree(g_CacheTable, 0, MEM_RELEASE);
        g_CacheTable = nullptr;
    }
    LruInit();
}

// Lookup block in cache (must hold g_CacheLock exclusive)
static CacheBlock* CacheLookupLocked(uint64_t file_id, uint64_t block_index) {
    size_t idx = CacheHash(file_id, block_index);
    CacheBlock* entry = &g_CacheTable[idx];
    while (entry) {
        if (entry->valid && entry->file_id == file_id && entry->block_index == block_index) {
            return entry;
        }
        entry = entry->next;
    }
    return nullptr;
}

// Insert or update block in cache (must hold g_CacheLock exclusive)
static CacheBlock* CacheInsertLocked(uint64_t file_id, uint64_t block_index, size_t size) {
    size_t idx = CacheHash(file_id, block_index);
    CacheBlock* entry = &g_CacheTable[idx];

    // Check if already exists
    while (entry) {
        if (entry->valid && entry->file_id == file_id && entry->block_index == block_index) {
            return entry;  // already exists
        }
        if (!entry->next) break;
        entry = entry->next;
    }

    // Need a new block: find free slot or evict LRU
    CacheBlock* newBlock = nullptr;
    for (size_t i = 0; i < g_MaxBlocks; ++i) {
        if (!g_CacheTable[i].valid) {
            newBlock = &g_CacheTable[i];
            break;
        }
    }

    if (!newBlock) {
        // Evict LRU
        newBlock = LruEvict();
        if (!newBlock) return nullptr;
        // Free old data
        if (newBlock->data) {
            VirtualFree(newBlock->data, 0, MEM_RELEASE);
            newBlock->data = nullptr;
        }
        // Remove from old hash chain
        size_t old_idx = CacheHash(newBlock->file_id, newBlock->block_index);
        CacheBlock* old_head = &g_CacheTable[old_idx];
        if (old_head == newBlock) {
            g_CacheTable[old_idx] = *newBlock;  // copy next pointer
            g_CacheTable[old_idx].next = newBlock->next;
        } else {
            while (old_head && old_head->next != newBlock) {
                old_head = old_head->next;
            }
            if (old_head) {
                old_head->next = newBlock->next;
            }
        }
    }

    // Allocate data for new block
    newBlock->data = VirtualAlloc(nullptr, g_BlockSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!newBlock->data) {
        char logbuf[256];
        snprintf(logbuf, sizeof(logbuf), "[L2] CacheInsert: VirtualAlloc for block data failed (file_id=%llu, block=%llu)\n",
                 file_id, block_index);
        LogWrite(logbuf);
        return nullptr;
    }

    // Initialize block
    newBlock->file_id = file_id;
    newBlock->block_index = block_index;
    newBlock->size = size;
    newBlock->last_access = GetTickCount64();
    newBlock->valid = true;
    newBlock->next = nullptr;

    // Insert into hash chain
    if (g_CacheTable[idx].valid) {
        newBlock->next = g_CacheTable[idx].next;
        g_CacheTable[idx].next = newBlock;
    } else {
        g_CacheTable[idx] = *newBlock;  // copy all fields including next=nullptr
    }

    // Add to LRU front
    LruAdd(newBlock);

    return newBlock;
}

// Read data into cache block (must hold g_CacheLock shared or exclusive)
// Returns true on success, false on failure (fail-open)
static bool CacheReadBlockLocked(CacheBlock* block, HANDLE file_handle, uint64_t file_offset) {
    if (!block || !block->data || !block->valid) return false;

    // Save current file position
    FILE_STANDARD_INFORMATION std_info;
    IO_STATUS_BLOCK iosb;
    NTSTATUS status = g_RealNtQueryInformationFile(file_handle, &iosb, &std_info, sizeof(std_info), 5); // FileStandardInformation
    if (status != 0) return false;

    LARGE_INTEGER current_pos;
    status = g_RealNtQueryInformationFile(file_handle, &iosb, &current_pos, sizeof(current_pos), 4); // FilePositionInformation
    if (status != 0) return false;

    // Seek to block offset
    LARGE_INTEGER new_pos;
    new_pos.QuadPart = (LONGLONG)file_offset;
    status = g_RealNtSetInformationFile(file_handle, &iosb, &new_pos, sizeof(new_pos), 4); // FilePositionInformation
    if (status != 0) return false;

    // Read block
    char logbuf[256];
    snprintf(logbuf, sizeof(logbuf), "[PF] Prefetch read: file_id=%llu block=%llu offset=%llu size=%zu\n",
             block->file_id, block->block_index, file_offset, block->size);
    LogWrite(logbuf);

    status = g_RealNtReadFile(file_handle, nullptr, nullptr, nullptr, &iosb, block->data, (ULONG)block->size, &new_pos, nullptr);

    // Restore file position
    g_RealNtSetInformationFile(file_handle, &iosb, &current_pos, sizeof(current_pos), 4);

    if (status != 0) return false;
    block->size = (size_t)iosb.Information;
    return true;
}

// ============================================================
// File Handle Tracking
// ============================================================

// Register a new file handle
static uint64_t FileRegister(HANDLE handle, const wchar_t* path) {
    AcquireSRWLockExclusive(&g_FileLock);

    FileEntry* entry = (FileEntry*)HeapAlloc(GetProcessHeap(), 0, sizeof(FileEntry));
    if (!entry) {
        ReleaseSRWLockExclusive(&g_FileLock);
        return 0;
    }

    entry->handle = handle;
    entry->file_id = g_NextFileId++;
    wcscpy_s(entry->path, MAX_PATH, path ? path : L"");
    entry->last_read_offset = 0;
    entry->last_read_length = 0;
    entry->has_last_read = false;
    entry->next = g_FileTable;
    g_FileTable = entry;

    ReleaseSRWLockExclusive(&g_FileLock);
    return entry->file_id;
}

// Unregister a file handle
static void FileUnregister(HANDLE handle) {
    AcquireSRWLockExclusive(&g_FileLock);

    FileEntry** pp = &g_FileTable;
    while (*pp) {
        if ((*pp)->handle == handle) {
            FileEntry* to_free = *pp;
            *pp = to_free->next;
            HeapFree(GetProcessHeap(), 0, to_free);
            break;
        }
        pp = &(*pp)->next;
    }

    ReleaseSRWLockExclusive(&g_FileLock);
}

// Find file entry by handle
static FileEntry* FileFind(HANDLE handle) {
    AcquireSRWLockShared(&g_FileLock);
    FileEntry* entry = g_FileTable;
    while (entry) {
        if (entry->handle == handle) break;
        entry = entry->next;
    }
    ReleaseSRWLockShared(&g_FileLock);
    return entry;
}

// ============================================================
// Prefetch Thread
// ============================================================

static DWORD WINAPI PrefetchThreadProc(LPVOID) {
    // Set low I/O priority
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    // Note: SetThreadIoPriority requires Windows 7+ and IoPriorityLow = 0
    typedef BOOL (WINAPI* PFN_SetThreadIoPriority)(HANDLE, ULONG);
    static PFN_SetThreadIoPriority pSetThreadIoPriority = nullptr;
    if (!pSetThreadIoPriority) {
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        pSetThreadIoPriority = reinterpret_cast<PFN_SetThreadIoPriority>(GetProcAddress(kernel32, "SetThreadIoPriority"));
    }
    if (pSetThreadIoPriority) {
        pSetThreadIoPriority(GetCurrentThread(), 0); // IoPriorityLow
    }

    while (g_PrefetchRunning) {
        DWORD wait = WaitForSingleObject(g_PrefetchEvent, 100);
        if (wait == WAIT_TIMEOUT) continue;

        // Process prefetch queue
        while (true) {
            if (g_PrefetchInFlight >= 2) break;  // max 2 in-flight

            AcquireSRWLockExclusive(&g_PrefetchLock);
            PrefetchTask* task = g_PrefetchHead;
            if (task) {
                g_PrefetchHead = task->next;
                if (!g_PrefetchHead) g_PrefetchTail = nullptr;
            }
            ReleaseSRWLockExclusive(&g_PrefetchLock);

            if (!task) break;

            // Execute prefetch
            InterlockedIncrement(&g_PrefetchInFlight);

            // Find file handle for this file_id
            // We need to search the file table - this is a limitation
            // For now, we'll skip actual prefetch read if we can't find the handle
            // In a full implementation, we'd store handle in task or have a file_id->handle map
            AcquireSRWLockShared(&g_FileLock);
            FileEntry* file_entry = g_FileTable;
            while (file_entry && file_entry->file_id != task->file_id) {
                file_entry = file_entry->next;
            }
            ReleaseSRWLockShared(&g_FileLock);

            if (file_entry) {
                AcquireSRWLockExclusive(&g_CacheLock);
                uint64_t file_offset = task->block_index * g_BlockSize;
                CacheBlock* block = CacheInsertLocked(task->file_id, task->block_index, g_BlockSize);
                if (block) {
                    CacheReadBlockLocked(block, file_entry->handle, file_offset);
                    LruMoveToFront(block);
                }
                ReleaseSRWLockExclusive(&g_CacheLock);
            }

            HeapFree(GetProcessHeap(), 0, task);
            InterlockedDecrement(&g_PrefetchInFlight);
        }
    }
    return 0;
}

// Queue a prefetch task
static void PrefetchQueue(uint64_t file_id, uint64_t block_index) {
    if (!g_EnableLevel2) return;

    PrefetchTask* task = (PrefetchTask*)HeapAlloc(GetProcessHeap(), 0, sizeof(PrefetchTask));
    if (!task) return;
    task->file_id = file_id;
    task->block_index = block_index;
    task->next = nullptr;

    AcquireSRWLockExclusive(&g_PrefetchLock);
    if (g_PrefetchTail) {
        g_PrefetchTail->next = task;
        g_PrefetchTail = task;
    } else {
        g_PrefetchHead = g_PrefetchTail = task;
    }
    ReleaseSRWLockExclusive(&g_PrefetchLock);

    SetEvent(g_PrefetchEvent);
}

// ============================================================
// Hooked Functions
// ============================================================

// Hooked NtCreateFile for Level 1 flag rewriting + file tracking
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

    NTSTATUS status = g_RealNtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
                                         AllocationSize, FileAttributes, ShareAccess, CreateDisposition,
                                         modifiedOptions, EaBuffer, EaLength);

    // Register file handle for tracking (only for .forge files, read access)
    if (status == 0 && FileHandle && *FileHandle && IsForgeFile(filePath)) {
        uint64_t file_id = FileRegister(*FileHandle, filePath);
        char logbuf[256];
        snprintf(logbuf, sizeof(logbuf), "[L2] File registered: handle=%p file_id=%llu path=\"%ls\"\n",
                 *FileHandle, file_id, filePath);
        LogWrite(logbuf);
    }

    return status;
}

// Hooked NtOpenFile for Level 1 flag rewriting + file tracking
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

    NTSTATUS status = g_RealNtOpenFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
                                       ShareAccess, modifiedOptions);

    // Register file handle for tracking
    if (status == 0 && FileHandle && *FileHandle && IsForgeFile(filePath)) {
        uint64_t file_id = FileRegister(*FileHandle, filePath);
        char logbuf[256];
        snprintf(logbuf, sizeof(logbuf), "[L2] File registered: handle=%p file_id=%llu path=\"%ls\"\n",
                 *FileHandle, file_id, filePath);
        LogWrite(logbuf);
    }

    return status;
}

// Hooked NtClose for file tracking cleanup
NTSTATUS NTAPI HookedNtClose(HANDLE Handle) {
    // Unregister file handle first
    FileEntry* entry = FileFind(Handle);
    if (entry) {
        char logbuf[256];
        snprintf(logbuf, sizeof(logbuf), "[L2] File unregistered: handle=%p file_id=%llu path=\"%ls\"\n",
                 Handle, entry->file_id, entry->path);
        LogWrite(logbuf);
        FileUnregister(Handle);
    }

    return g_RealNtClose(Handle);
}

// Hooked NtReadFile - serves from cache for synchronous reads, feeds prefetcher
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
    // Determine if synchronous (no Event, no APC routine)
    bool is_sync = (!Event && !ApcRoutine);

    // Get file entry for tracking
    FileEntry* file_entry = FileFind(FileHandle);

    // Calculate read offset
    LONGLONG read_offset = 0;
    if (ByteOffset) {
        read_offset = ByteOffset->QuadPart;
    } else if (file_entry && file_entry->has_last_read) {
        // Use current file position - we'd need to query it
        // For simplicity, assume sequential from last read
        read_offset = (LONGLONG)(file_entry->last_read_offset + file_entry->last_read_length);
    } else {
        // Can't determine offset, pass through
        return g_RealNtReadFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset, Key);
    }

    // Check if this is a .forge file
    bool is_forge = file_entry && IsForgeFile(file_entry->path);

    // Try to serve from cache (Level 2/3) for synchronous reads
    if (g_EnableLevel2 && is_sync && is_forge && file_entry) {
        uint64_t file_id = file_entry->file_id;
        uint64_t start_block = (uint64_t)read_offset / g_BlockSize;
        uint64_t end_block = ((uint64_t)read_offset + Length + g_BlockSize - 1) / g_BlockSize;

        // Check if entire read is contained in cached blocks
        bool all_cached = true;
        AcquireSRWLockShared(&g_CacheLock);
        for (uint64_t bi = start_block; bi < end_block; ++bi) {
            CacheBlock* block = CacheLookupLocked(file_id, bi);
            if (!block || block->size == 0) {
                all_cached = false;
                break;
            }
        }
        ReleaseSRWLockShared(&g_CacheLock);

        if (all_cached) {
            // Serve from cache
            AcquireSRWLockExclusive(&g_CacheLock);
            size_t copied = 0;
            for (uint64_t bi = start_block; bi < end_block; ++bi) {
                CacheBlock* block = CacheLookupLocked(file_id, bi);
                if (block && block->data && block->valid) {
                    uint64_t block_start = bi * g_BlockSize;
                    uint64_t offset_in_block = (uint64_t)read_offset + copied - block_start;
                    size_t to_copy = std::min<ULONG>((ULONG)(block->size - offset_in_block), Length - copied);
                    memcpy((char*)Buffer + copied, (char*)block->data + offset_in_block, to_copy);
                    copied += to_copy;
                    block->last_access = GetTickCount64();
                    LruMoveToFront(block);
                }
            }
            ReleaseSRWLockExclusive(&g_CacheLock);

            if (copied == Length) {
                IoStatusBlock->Status = 0;
                IoStatusBlock->Information = Length;
                char logbuf[256];
                snprintf(logbuf, sizeof(logbuf), "[L2] Cache HIT: file_id=%llu offset=%lld len=%lu\n",
                         file_id, read_offset, Length);
                LogWrite(logbuf);
                return 0;  // STATUS_SUCCESS
            }
            // Partial hit - fall through to disk read
        }

        // Cache miss - log it
        char logbuf[256];
        snprintf(logbuf, sizeof(logbuf), "[L2] Cache MISS: file_id=%llu offset=%lld len=%lu blocks=%llu-%llu\n",
                 file_id, read_offset, Length, start_block, end_block - 1);
        LogWrite(logbuf);
    }

    // Perform actual read
    NTSTATUS status = g_RealNtReadFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset, Key);

    // Update file tracking for sequential detection and prefetch
    if (file_entry && is_forge) {
        AcquireSRWLockExclusive(&g_FileLock);
        bool is_sequential = false;
        if (file_entry->has_last_read) {
            uint64_t expected_offset = file_entry->last_read_offset + file_entry->last_read_length;
            if (read_offset == (LONGLONG)expected_offset) {
                is_sequential = true;
            }
        }
        file_entry->last_read_offset = (uint64_t)read_offset;
        file_entry->last_read_length = Length;
        file_entry->has_last_read = true;
        ReleaseSRWLockExclusive(&g_FileLock);

        // Queue prefetch for sequential reads (Level 3)
        if (g_EnableLevel2 && is_sync && is_sequential) {
            uint64_t next_block = ((uint64_t)read_offset + Length + g_BlockSize - 1) / g_BlockSize;
            for (int i = 0; i < g_PrefetchBlocks; ++i) {
                PrefetchQueue(file_entry->file_id, next_block + i);
            }
        }
    }

    // Log I/O for debugging
    if (g_EnableLogging && is_sync && file_entry && IsForgeFile(file_entry->path)) {
        char logbuf[512];
        unsigned long long bytesRead = IoStatusBlock ? IoStatusBlock->Information : 0;
        snprintf(logbuf, sizeof(logbuf),
                 "[IO] ReadFile path=\"%ls\" offset=%lld length=%lu status=0x%08X bytes=%llu\n",
                 file_entry->path,
                 (long long)read_offset,
                 Length,
                 (unsigned int)status,
                 bytesRead);
        LogWrite(logbuf);
    }

    return status;
}

// ============================================================
// DllMain
// ============================================================

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        WCHAR iniPath[MAX_PATH];
        GetModuleFileNameW(nullptr, iniPath, MAX_PATH);
        WCHAR* slash = wcsrchr(iniPath, L'\\');
        if (slash) {
            wcscpy(slash + 1, L"ACBFRHDDFIX.ini");
        } else {
            wcscpy(iniPath, L"ACBFRHDDFIX.ini");
        }

        g_EnableLevel1 = GetPrivateProfileIntW(L"HDDPreloader", L"EnableLevel1", 1, iniPath);
        g_EnableLevel2 = GetPrivateProfileIntW(L"HDDPreloader", L"EnableLevel2", 0, iniPath);
        g_EnableLogging = GetPrivateProfileIntW(L"HDDPreloader", L"EnableLogging", 1, iniPath);
        g_CacheMB = GetPrivateProfileIntW(L"HDDPreloader", L"CacheMB", 4096, iniPath);
        g_BlockMB = GetPrivateProfileIntW(L"HDDPreloader", L"BlockMB", 1, iniPath);
        g_PrefetchBlocks = GetPrivateProfileIntW(L"HDDPreloader", L"PrefetchBlocks", 4, iniPath);

        char cfgBuf[512];
        snprintf(cfgBuf, sizeof(cfgBuf),
                 "[Config] EnableLevel1=%d EnableLevel2=%d EnableLogging=%d CacheMB=%d BlockMB=%d PrefetchBlocks=%d\n",
                 g_EnableLevel1, g_EnableLevel2, g_EnableLogging, g_CacheMB, g_BlockMB, g_PrefetchBlocks);
        LogWrite(cfgBuf);

        // Initialize cache if Level 2 enabled
        if (g_EnableLevel2) {
            if (!CacheInit(g_CacheMB, g_BlockMB)) {
                g_EnableLevel2 = 0;
                LogWrite("[L2] CacheInit failed, Level 2 disabled\n");
            }

            // Create prefetch event and thread
            g_PrefetchEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            g_PrefetchRunning = 1;
            g_PrefetchThread = CreateThread(nullptr, 0, PrefetchThreadProc, nullptr, 0, &g_PrefetchThreadId);
        }

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

            g_RealNtClose = reinterpret_cast<PFN_NtClose>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtClose")));
            if (g_RealNtClose) {
                MH_CreateHook(reinterpret_cast<LPVOID>(g_RealNtClose), reinterpret_cast<LPVOID>(&HookedNtClose), reinterpret_cast<LPVOID*>(&g_RealNtClose));
            }

            g_RealNtQueryInformationFile = reinterpret_cast<PFN_NtQueryInformationFile>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryInformationFile")));
            g_RealNtSetInformationFile = reinterpret_cast<PFN_NtSetInformationFile>(reinterpret_cast<void*>(GetProcAddress(ntdll, "NtSetInformationFile")));

            MH_EnableHook(MH_ALL_HOOKS);
        }

        DisableThreadLibraryCalls(hModule);
    } else if (reason == DLL_PROCESS_DETACH) {
        // Stop prefetch thread
        if (g_PrefetchRunning) {
            g_PrefetchRunning = 0;
            if (g_PrefetchEvent) SetEvent(g_PrefetchEvent);
            if (g_PrefetchThread) {
                WaitForSingleObject(g_PrefetchThread, 2000);
                CloseHandle(g_PrefetchThread);
                g_PrefetchThread = nullptr;
            }
            if (g_PrefetchEvent) {
                CloseHandle(g_PrefetchEvent);
                g_PrefetchEvent = nullptr;
            }
            // Clean up prefetch queue
            AcquireSRWLockExclusive(&g_PrefetchLock);
            PrefetchTask* task = g_PrefetchHead;
            while (task) {
                PrefetchTask* next = task->next;
                HeapFree(GetProcessHeap(), 0, task);
                task = next;
            }
            g_PrefetchHead = g_PrefetchTail = nullptr;
            ReleaseSRWLockExclusive(&g_PrefetchLock);
        }

        // Cleanup cache
        CacheCleanup();

        // Cleanup file table
        AcquireSRWLockExclusive(&g_FileLock);
        FileEntry* entry = g_FileTable;
        while (entry) {
            FileEntry* next = entry->next;
            HeapFree(GetProcessHeap(), 0, entry);
            entry = next;
        }
        g_FileTable = nullptr;
        ReleaseSRWLockExclusive(&g_FileLock);

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

HRESULT WINAPI DStorageGetFactory(REFIID riid, void** ppv) {
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

HRESULT WINAPI DStorageSetConfiguration(UINT32 flags) {
    if (!g_SetConfig) return E_FAIL;
    return g_SetConfig(flags);
}

HRESULT WINAPI DStorageCreateCompressionCodec(UINT32 format, REFIID riid, void** codec) {
    if (!g_CreateCodec) return E_FAIL;
    return g_CreateCodec(format, riid, codec);
}

} // extern "C"