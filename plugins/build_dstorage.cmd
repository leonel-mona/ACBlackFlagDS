@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
cl /c /O2 /GL /MD /arch:AVX2 /D_WIN32_WINNT=0x0A00 /DNDEBUG /Fo:dstorage_proxy.obj dstorage_proxy.cpp
if errorlevel 1 exit /b 1
link /RELEASE /OPT:REF /LTCG /DLL /OUT:dstorage.dll dstorage_proxy.obj kernel32.lib