@echo off
set PATH=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64;C:\Windows\System32;C:\Windows
link /RELEASE /OPT:REF /LTCG /DLL /OUT:E:\Games\AC Black Flag Resynced\plugins\dstorage.dll E:\Games\AC Black Flag Resynced\plugins\dstorage_proxy.obj kernel32.lib