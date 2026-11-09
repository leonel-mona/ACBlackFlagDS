@echo off
setlocal
set "VCTOOLS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
set "OUT=%~dp0..\acbf_hdd_preloader.exe"
set "SRC=%~dp0acbf_hdd_preloader.cpp"
call "%VCTOOLS%" x64
cl /O2 /GL /MT /std:c++20 /arch:AVX2 /D_WIN32_WINNT=0x0A00 /Fe:"%OUT%" "%SRC%"
if errorlevel 1 exit /b 1
echo Build OK.
for %%I in ("%OUT%") do echo Size: %%~zI bytes