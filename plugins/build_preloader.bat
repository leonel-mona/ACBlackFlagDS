@echo off
REM Build script for AC Black Flag HDD Preloader
REM This builds a standalone EXE that runs BEFORE the game to warm Windows file cache

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64

if not exist "%~dp0acbf_hdd_preloader.cpp" (
    echo ERROR: Source file not found in %~dp0
    exit /b 1
)

echo Building acbf_hdd_preloader.exe...
cl /O2 /GL /MT /arch:AVX2 /D_WIN32_WINNT=0x0A00 /Fe:"%~dp0..\acbf_hdd_preloader.exe" "%~dp0acbf_hdd_preloader.cpp"

if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)

echo.
echo Build successful!
for %%I in ("%~dp0..\acbf_hdd_preloader.exe") do echo Size: %%~zI bytes
echo.
echo Usage: Run acbf_hdd_preloader.exe from the game directory before launching ACBlackFlag.exe
