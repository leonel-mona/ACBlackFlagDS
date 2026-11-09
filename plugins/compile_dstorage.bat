@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe" /c /O2 /GL /MD /arch:AVX2 /D_WIN32_WINNT=0x0A00 /DNDEBUG /Fo:"E:\Games\AC Black Flag Resynced\plugins\dstorage_proxy.obj" "E:\Games\AC Black Flag Resynced\plugins\dstorage_proxy.cpp"
if errorlevel 1 exit /b 1
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\link.exe" /RELEASE /OPT:REF /LTCG /DLL /OUT:"E:\Games\AC Black Flag Resynced\plugins\dstorage.dll" "E:\Games\AC Black Flag Resynced\plugins\dstorage_proxy.obj" kernel32.lib