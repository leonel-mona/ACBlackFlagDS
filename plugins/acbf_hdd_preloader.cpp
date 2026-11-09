// acbf_hdd_preloader.cpp
// ---------------------------------------------------------------------------
// AC Black Flag Resynced - Standalone HDD Preloader
//
// Warms the Windows file cache by sequentially reading the "critical"
// .forge files (Chunk 3 of streaminginstall.ini: bootstrap, shared, and
// core location forges) BEFORE the game launches. Cached reads return
// from RAM in <1 ms instead of 15-30 ms on a 7200 RPM HDD.
//
// Pure C++20, Windows SDK only. No external dependencies.
// Compile (x64 Release):
//   cl /O2 /GL /MT /arch:AVX2 /D_WIN32_WINNT=0x0A00 /Fe:acbf_hdd_preloader.exe acbf_hdd_preloader.cpp
//   g++ -O2 -march=haswell -std=c++20 -static -o acbf_hdd_preloader.exe acbf_hdd_preloader.cpp
//
// Usage:
//   acbf_hdd_preloader.exe [gameDirectory] [--auto-launch]
//   (defaults to the current working directory)
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cwchar>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr size_t kMinFileSizeBytes = 1ULL * 1024 * 1024;
constexpr size_t kDefaultBufferMB  = 4;

std::wstring g_gameDir;
bool g_warmPatches = false;
std::wstring g_warmLanguage;
bool g_autoLaunch = false;

std::wofstream g_log;

void Log(const std::wstring& msg)
{
    std::wstringstream ss;
    const std::time_t t = std::time(nullptr);
    ss << std::put_time(std::localtime(&t), L"%Y-%m-%d %H:%M:%S") << ' ' << msg;
    const std::wstring line = ss.str();
    std::wcout << line << '\n';
    if (g_log.is_open())
        g_log << line << '\n';
}

bool IContains(const std::wstring& s, const std::wstring& sub)
{
    std::wstring a = s, b = sub;
    for (auto& c : a) c = static_cast<wchar_t>(::towlower(c));
    for (auto& c : b) c = static_cast<wchar_t>(::towlower(c));
    return a.find(b) != std::wstring::npos;
}

bool EndsWithCI(const std::wstring& s, const std::wstring& suffix)
{
    if (s.size() < suffix.size())
        return false;
    return IContains(s.substr(s.size() - suffix.size()), suffix);
}

std::wstring FormatBytes(size_t bytes)
{
    std::wstringstream ss;
    ss << std::fixed << std::setprecision(1);
    if (bytes >= (1024ULL * 1024 * 1024))
        ss << (static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0)) << L" GB";
    else if (bytes >= (1024ULL * 1024))
        ss << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << L" MB";
    else
        ss << bytes << L" B";
    return ss.str();
}

void FindForgeFiles(const WCHAR* dirPath, std::vector<std::wstring>& out)
{
    out.clear();
    const fs::path dir(dirPath);
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec))
    {
        if (!entry.is_regular_file(ec))
            continue;
        if (_wcsicmp(entry.path().extension().c_str(), L".forge") != 0)
            continue;
        out.push_back(entry.path().wstring());
    }
    if (ec)
        Log(L"WARNING: directory scan error: " + std::to_wstring(ec.value()));
}

bool IsCriticalForge(const std::wstring& filename)
{
    if (!g_warmLanguage.empty() &&
        IContains(filename, g_warmLanguage) &&
        IContains(filename, L"_sound_"))
        return true;

    const std::vector<std::wstring> hardExcludes = {
        L"_sound_", L"_nav.", L"_floor.", L"_gi."
    };
    for (const auto& ex : hardExcludes)
        if (IContains(filename, ex))
            return false;

    const std::vector<std::wstring> langCodes = {
        L"_eng", L"_fre", L"_ita", L"_ger", L"_spa", L"_jap", L"_bra", L"_chi"
    };
    for (const auto& lang : langCodes)
    {
        if (IContains(filename, lang))
        {
            if (!g_warmLanguage.empty() && lang == g_warmLanguage)
                break;
            return false;
        }
    }

    if (!g_warmPatches && IContains(filename, L"_patch_"))
        return false;

    const std::vector<std::wstring> boot = {
        L"DataPC_boot", L"DataPC_shared_00",
        L"DataPC_RED_TitleScreen", L"DataPC_WhiteRoom"
    };
    for (const auto& b : boot)
        if (IContains(filename, b))
            return true;

    const std::vector<std::wstring> locations = {
        L"CaribbeanSea.forge", L"Charleston.forge", L"Delirium.forge",
        L"MayanTemple.forge", L"NorthCarolina.forge",
        L"Principe.forge", L"Epilogue.forge"
    };
    for (const auto& loc : locations)
        if (EndsWithCI(filename, loc))
            return true;

    if (IContains(filename, L"Rift_Drop_") && EndsWithCI(filename, L".forge"))
        return true;

    return false;
}

size_t WarmForgeFile(const std::wstring& filepath, size_t prefetchMB)
{
    if (prefetchMB < 1)  prefetchMB = 1;
    if (prefetchMB > 64) prefetchMB = 64;
    const DWORD bufBytes = static_cast<DWORD>(prefetchMB) * 1024 * 1024;

    const HANDLE h = CreateFileW(filepath.c_str(),
                                 GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_DELETE,
                                 nullptr,
                                 OPEN_EXISTING,
                                 FILE_FLAG_SEQUENTIAL_SCAN,
                                 nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        Log(L"  ERROR: cannot open " +
            fs::path(filepath).filename().wstring() +
            L" (Win32 error " + std::to_wstring(GetLastError()) + L")");
        return 0;
    }

    ULARGE_INTEGER fileSize = {};
    if (!GetFileSizeEx(h, reinterpret_cast<PLARGE_INTEGER>(&fileSize)))
    {
        Log(L"  ERROR: GetFileSizeEx failed on " +
            fs::path(filepath).filename().wstring());
        CloseHandle(h);
        return 0;
    }
    const SIZE_T total = static_cast<SIZE_T>(fileSize.QuadPart);

    std::vector<char> buffer(bufBytes);
    SIZE_T totalRead = 0;
    bool ok = true;
    while (totalRead < total)
    {
        const DWORD want = static_cast<DWORD>(
            (total - totalRead) < bufBytes ? (total - totalRead) : bufBytes);
        DWORD got = 0;
        if (!ReadFile(h, buffer.data(), want, &got, nullptr) || got == 0)
        {
            ok = false;
            break;
        }
        totalRead += got;
    }
    CloseHandle(h);

    const std::wstring name = fs::path(filepath).filename().wstring();
    if (ok)
        Log(L"  OK   " + name + L"  (" + FormatBytes(totalRead) + L")");
    else
        Log(L"  READ-ERROR " + name +
            L" (stopped after " + FormatBytes(totalRead) + L" of " +
            FormatBytes(total) + L")");
    return static_cast<size_t>(totalRead);
}

size_t GetTotalCriticalSize()
{
    std::vector<std::wstring> files;
    FindForgeFiles(g_gameDir.c_str(), files);

    size_t total = 0;
    for (const auto& f : files)
    {
        if (!IsCriticalForge(fs::path(f).filename().wstring()))
            continue;
        std::error_code ec;
        const auto sz = fs::file_size(f, ec);
        if (!ec && sz >= kMinFileSizeBytes)
            total += static_cast<size_t>(sz);
    }
    return total;
}

bool LaunchGame(const std::wstring& gameDir)
{
    const fs::path exePath = fs::path(gameDir) / L"ACBlackFlag.exe";
    if (!fs::exists(exePath))
    {
        Log(L" ERROR: ACBlackFlag.exe not found at " + exePath.wstring());
        return false;
    }

    Log(L" Launching game: " + exePath.wstring());

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};

    DWORD flags = CREATE_DEFAULT_ERROR_MODE | CREATE_NEW_CONSOLE;

    std::wstring cmdLine = L"\"" + exePath.wstring() + L"\"";
    std::wstring workDir = gameDir;

    BOOL ok = CreateProcessW(
        nullptr,
        const_cast<LPWSTR>(cmdLine.c_str()),
        nullptr,
        nullptr,
        FALSE,
        flags,
        nullptr,
        workDir.c_str(),
        &si,
        &pi
    );

    if (!ok)
    {
        Log(L" ERROR: CreateProcess failed (Win32 error " + std::to_wstring(GetLastError()) + L")");
        return false;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

void PrintUsage()
{
    std::wcout <<
        L"Usage:\n"
        L"  acbf_hdd_preloader.exe [gameDirectory] [--auto-launch|-a]\n"
        L"\n"
        L"Options:\n"
        L"  gameDirectory   Path to AC Black Flag game folder (default: CWD)\n"
        L"  --auto-launch   Launch ACBlackFlag.exe after preloading\n"
        L"  -a              Short alias for --auto-launch\n"
        L"\n"
        L"Config (ACBlackFlagFix.ini):\n"
        L"  [HDDPreloader]\n"
        L"  GameDir = <path>\n"
        L"  WarmPatches = 0|1\n"
        L"  WarmLanguage = eng|fre|ita|ger|spa|jap|bra|chi\n"
        L"  BufferMB = 1..64 (default 4)\n"
        L"  AutoLaunch = 0|1\n"
        L"  AutoPreload = 0|1 (legacy, same as AutoLaunch)\n";
}

} // namespace

int wmain(int argc, wchar_t* argv[])
{
    std::wstring iniPath;
    std::wstring argDir;
    bool argAutoLaunch = false;

    // Parse command line
    for (int i = 1; i < argc; ++i)
    {
        std::wstring arg = argv[i];
        if (arg == L"--auto-launch" || arg == L"-a")
            argAutoLaunch = true;
        else if (!arg.empty() && arg[0] != L'-')
            argDir = arg;
    }

    // Determine game directory
    if (!argDir.empty())
    {
        g_gameDir = argDir;
    }
    else
    {
        wchar_t cwd[MAX_PATH];
        if (GetCurrentDirectoryW(MAX_PATH, cwd))
            g_gameDir = cwd;
    }

    // Open log file in game directory
    const fs::path logPath = fs::path(g_gameDir) / L"HDDPreloader.log";
    g_log.open(logPath, std::ios::app);
    Log(L"=== AC Black Flag HDD Preloader started ===");

    // Load INI config using Windows API
    const fs::path iniFile = fs::path(g_gameDir) / L"ACBlackFlagFix.ini";

    if (fs::exists(iniFile))
    {
        // Read GameDir
        wchar_t buffer[MAX_PATH];
        if (GetPrivateProfileStringW(L"HDDPreloader", L"GameDir", L"", buffer, MAX_PATH, iniFile.c_str()) > 0)
        {
            g_gameDir = buffer;
        }

        // Read WarmPatches
        g_warmPatches = GetPrivateProfileIntW(L"HDDPreloader", L"WarmPatches", 0, iniFile.c_str()) != 0;

        // Read WarmLanguage
        if (GetPrivateProfileStringW(L"HDDPreloader", L"WarmLanguage", L"", buffer, MAX_PATH, iniFile.c_str()) > 0)
        {
            g_warmLanguage = buffer;
        }

        // Read AutoLaunch
        g_autoLaunch = GetPrivateProfileIntW(L"HDDPreloader", L"AutoLaunch", 0, iniFile.c_str()) != 0;

        // Read AutoPreload (legacy alias for AutoLaunch)
        if (GetPrivateProfileIntW(L"HDDPreloader", L"AutoPreload", 0, iniFile.c_str()) != 0)
        {
            g_autoLaunch = true;
        }
    }

    // CLI flag overrides INI
    if (argAutoLaunch)
        g_autoLaunch = true;

    // Validate game directory
    if (!fs::exists(g_gameDir) || !fs::is_directory(g_gameDir))
    {
        Log(L" ERROR: Game directory not found: " + g_gameDir);
        PrintUsage();
        return 1;
    }

    // Verify ACBlackFlag.exe exists
    const fs::path exeCheck = fs::path(g_gameDir) / L"ACBlackFlag.exe";
    if (!fs::exists(exeCheck))
    {
        Log(L" ERROR: ACBlackFlag.exe not found in " + g_gameDir);
        return 1;
    }

    // Log configuration
    Log(L" GameDir: " + g_gameDir);
    Log(L" WarmPatches: " + std::wstring(g_warmPatches ? L"true" : L"false"));
    Log(L" WarmLanguage: " + (g_warmLanguage.empty() ? L"(none)" : g_warmLanguage));
    Log(L" BufferMB: " + std::to_wstring(kDefaultBufferMB));
    Log(L" AutoLaunch: " + std::wstring(g_autoLaunch ? L"true" : L"false"));

    // Find and warm critical files
    std::vector<std::wstring> files;
    FindForgeFiles(g_gameDir.c_str(), files);

    size_t totalCritical = GetTotalCriticalSize();
    Log(L" Total critical size to warm: " + FormatBytes(totalCritical));

    size_t totalWarmed = 0;
    size_t filesWarmed = 0;
    const auto start = std::chrono::steady_clock::now();

    for (const auto& f : files)
    {
        const std::wstring filename = fs::path(f).filename().wstring();
        if (!IsCriticalForge(filename))
            continue;
        totalWarmed += WarmForgeFile(f, kDefaultBufferMB);
        ++filesWarmed;
    }

    const auto end = std::chrono::steady_clock::now();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    Log(L" Warmed " + std::to_wstring(filesWarmed) + L" files (" + FormatBytes(totalWarmed) + L") in " + std::to_wstring(elapsedMs) + L" ms");

    // Auto-launch or prompt
    if (g_autoLaunch)
    {
        Log(L" AutoLaunch enabled - starting game...");
        if (!LaunchGame(g_gameDir))
            return 1;
    }
    else
    {
        Log(L" Launch ACBlackFlag.exe now...");
    }

    Log(L"=== AC Black Flag HDD Preloader finished ===");
    return 0;
}