@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
cd /d "E:\Games\AC Black Flag Resynced\plugins"
cl /c /O2 /GL /MD /arch:AVX2 /D_WIN32_WINNT=0x0A00 /DNDEBUG /Fo:acbf_hdd_optimizer.obj acbf_hdd_optimizer.cpp
if errorlevel 1 exit /b 1
link /RELEASE /OPT:REF /LTCG /DLL /OUT:acbf_hdd_optimizer.dll acbf_hdd_optimizer.obj kernel32.lib