# AC Black Flag HDD Preloader

## Overview

A lightweight standalone tool that warms Windows file cache with critical `.forge` files **before** you launch the game. This significantly reduces initial loading times on mechanical HDDs by using RAM as a read-through cache.

## How It Works

The preloader scans the game directory, identifies Chunk 3 core gameplay assets (bootstrap, shared, and location forges), and sequentially reads each file at maximum possible speed. Windows automatically caches these pages in available RAM, so when the game loads them later, they return in <1ms instead of 15-30ms on a typical 7200 RPM hard drive.

**Key characteristics:**
- Sequential I/O optimized (`FILE_FLAG_SEQUENTIAL_SCAN`)
- 4 MB read buffers for optimal HDD throughput
- Only processes ~25-30 GB of critical files (not all 67.5 GB)
- No DirectStorage bypass required (works alongside it)

## Usage

### Quick Start (Manual)

```batch
cd "E:\Games\AC Black Flag Resynced"
acbf_hdd_preloader.exe
```

Then launch the game normally:
```batch
start ACBlackFlag.exe
```

### Desktop Shortcut Setup

Create two shortcuts:

**Preloader shortcut:**
```
Target: E:\Games\AC Black Flag Resynced\acbf_hdd_preloader.exe
Working dir: E:\Games\AC Black Flag Resynced
```

**Game shortcut (recommended workflow):**
```
Target: cmd.exe /c "E:\Games\AC Black Flag Resynced\acbf_hdd_preloader.exe & start "" "E:\Games\AC Black Flag Resynced\ACBlackFlag.exe""
Working dir: E:\Games\AC Black Flag Resynced
```

Or use a batch file:

**launch_game.bat:**
```batch
@echo off
cd /d "E:\Games\AC Black Flag Resynced"
echo Running HDD preloader...
acbf_hdd_preloader.exe
echo Starting game...
start "" ACBlackFlag.exe
```

## Configuration

Edit `ACBlackFlagFix.ini`:

```ini
[HDDPreloader]
Enabled = true          ; Enable/disable the preloader behavior
BufferMB = 4            ; Read buffer size per file (1-16 MB)
AutoPreload = false     ; Auto-run on launcher start (requires admin)
WarmPatches = false     ; Also cache patch files (slower but more complete)
DisableDirectStorage = false ; Force fallback to standard I/O
```

## Critical Files Cached

The preloader targets these Chunk 3 files (~25-30 GB total):

| File | Purpose |
|------|---------|
| `DataPC_boot.forge` | Game bootstrap/shutdown sequences |
| `DataPC_shared_00.forge` | Core engine assets (UI, menus) |
| `DataPC_RED_TitleScreen.forge` | Title screen |
| `DataPC_WhiteRoom.forge` | Loading screens |
| `DataPC_OBD_CaribbeanSea.forge` | Caribbean sea zones |
| `DataPC_OBD_Charleston.forge` | Charleston city |
| `DataPC_OBD_Delirium.forge` | Delirium zone |
| `DataPC_OBD_MayanTemple.forge` | Mayan Temple area |
| `DataPC_OBD_NorthCarolina.forge` | North Carolina zone |
| `DataPC_OBD_Principe.forge` | Principe island |
| `DataPC_OBD_Epilogue.forge` | Epilogue content |
| `DataPC_OBD_Rift_Drop_*.forge` | Animus glitch zones (4 variants) |

**Excluded files:** Patch files (`_patch_`), sound files (`_sound_`), navigation data (`_nav.`), floor data (`_floor.`), GI data (`_gi.`), DX12 variants (`_dx12.`), and language-specific versions are skipped as they're either less critical or cached on-demand.

## Expected Performance

### Before Preloading
- First zone transition: 30-60 seconds
- UI/menu lag: noticeable stuttering
- Texture pop-in: visible on HDD

### After Preloading
- First zone transition: 5-15 seconds (depending on zone distance)
- UI/menu transitions: near-instant
- Texture streaming: reduced pop-in

## Technical Details

### Compilation
Requires Microsoft Visual C++ Compiler for C++17 (part of VS Build Tools).

```bash
cd E:\Games\AC Black Flag Resynced\plugins
build_preloader.cmd
```

Output: `acbf_hdd_preloader.exe` (~15-30 KB release build)

### Binary Specs
- **Architecture:** x64
- **Runtime:** Static CRT (`/MT`) - no VC runtime dependencies
- **Optimization:** Release, `/O2`, `/GL` (whole program optimization)
- **Target:** Windows 10+ (`_WIN32_WINNT=0x0A00`)

### System Requirements
- **RAM:** Minimum 8 GB recommended (32 GB ideal for full cache retention)
- **OS:** Windows 10 October 2018 Update or later
- **Storage:** SSD for preloader binary itself (though game can be on HDD)

## Troubleshooting

### "No critical .forge files found!"
Make sure you're running the preloader from the game's root directory (where `ACBlackFlag.exe` is located), not from `plugins\`.

### Preloader seems slow
This is normal on 7200 RPM drives reading ~25-30 GB. Expect 5-10 minutes depending on your drive's sustained sequential read speed.

### Cache gets evicted before game starts
If you have heavy background usage (browser, other apps), Windows may evict cached pages. Close unnecessary applications before running the preloader.

### Better results? Try Scenario B
If you still see stutters during mid-game streaming, you can force the engine to use standard Win32 I/O instead of DirectStorage:

```cmd
ren dstorage.dll dstorage.dll.disabled
```

This makes all streaming go through ReadFile, which hits the preloader cache. However, this may cause micro-stutters during fast traversal since the fallback path is slower than native DirectStorage.

## Logging

The preloader writes detailed logs to:
- Console output during execution
- `ACBlackFlagHDDPreloader.log` in the game directory

Check these logs if you encounter issues.

## Credits

Built as part of the AC Black Flag Resynced modding toolkit to address severe HDD loading performance issues caused by DirectStorage's 100+ async I/O operations without adequate prefetching.
