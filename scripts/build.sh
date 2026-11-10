#!/usr/bin/env bash
set -euo pipefail

# Compile the proxy with MinHook sources
x86_64-w64-mingw32-g++ -O2 -std=c++17 -Wall -Wextra \
    -shared -static-libgcc -static-libstdc++ \
    -o dstorage.dll \
    plugins/dstorage_proxy.cpp \
    plugins/dstorage.def \
    -lkernel32 \
    -Ithird_party/minhook-1.3.4/include \
    third_party/minhook-1.3.4/src/hook.c \
    third_party/minhook-1.3.4/src/buffer.c \
    third_party/minhook-1.3.4/src/trampoline.c \
    third_party/minhook-1.3.4/src/hde/hde64.c

echo "✅ dstorage.dll listo (colócalo junto a ACBlackFlag.exe)"
