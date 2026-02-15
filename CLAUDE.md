# SevenTTY

SSH client, local shell, and terminal emulator for classic Mac OS 7/8/9.
Fork of [ssheven](https://github.com/cy384/ssheven) by cy384. GitHub: LXXero/SevenTTY

## Build Environment

### Retro68 Cross-Compiler

SevenTTY builds with [Retro68](https://github.com/autc04/Retro68/), a GCC cross-compiler for classic Macintosh. The toolchain lives at:

```
~/git/Retro68-build/toolchain/
├── m68k-apple-macos/     # 68K toolchain
│   └── cmake/retro68.toolchain.cmake
└── powerpc-apple-macos/  # PPC toolchain
    └── cmake/retroppc.toolchain.cmake
```

Retro68 provides Universal Interfaces headers (Mac Toolbox APIs) and the `add_application()` cmake macro which handles Mac resource forks, MacBinary packaging, and HFS disk image creation.

### Building

Always delete and recreate build directories when cmake config changes (target rename, new files, etc.) — stale CMakeCache.txt causes cryptic errors.

```bash
# 68K build
mkdir build-m68k && cd build-m68k
cmake .. -DCMAKE_TOOLCHAIN_FILE=~/git/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
cmake --build . --parallel $(nproc)

# PPC build
mkdir build-ppc && cd build-ppc
cmake .. -DCMAKE_TOOLCHAIN_FILE=~/git/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
cmake --build . --parallel $(nproc)

# Fat binary (both architectures combined)
./build-fat.bash
```

Build outputs: `SevenTTY.bin` (MacBinary), `SevenTTY.dsk` (HFS disk image), `SevenTTY.APPL` (raw application)

### Deploy to QEMU Emulator

```bash
hmount ~/git/quadra800/full-os8.1.hda
hdel :SevenTTY 2>/dev/null
hcopy -m build-m68k/SevenTTY.bin :
humount
# Then run: ~/git/quadra800/run.sh
```

The `hcopy -m` flag reads MacBinary format. The embedded app name comes from the cmake target name (`SevenTTY`). Use `hls -l` to verify files on the HFS image.

## Dependencies (git submodules, built automatically)

- **opentransport-mbedtls** — TLS library patched for classic Mac OS (no real RNG, uses mediocre entropy)
- **opentransport-libssh2** — SSH library patched to use Open Transport instead of BSD sockets
- **libvterm** — terminal emulator library (VT100/xterm), patched with cmake build

Do NOT modify files inside these submodule directories.

## Source File Layout

| File | Lines | Purpose |
|------|-------|---------|
| `app.c` | ~2100 | Main event loop, window/session management, menus, preferences, connection |
| `app.h` | ~195 | All shared types: `struct session`, `struct window_context`, `struct console_metrics`, `struct preferences` |
| `console.c` | ~1350 | Terminal drawing (color + fast mode), font metrics, scrollback, cursor, mouse selection, tab bar |
| `console.h` | ~40 | Console function prototypes |
| `shell.c` | ~2800 | Local shell: 30+ commands (ls, cd, cat, cp, mv, rm, mkdir, ps, free, df, etc.), tab completion, history |
| `shell.h` | ~12 | Shell function prototypes |
| `net.c` | ~690 | SSH networking: Open Transport TCP, libssh2 session, read thread, known hosts |
| `net.h` | ~12 | Network function prototypes |
| `debug.c` | ~200 | libssh2 and Open Transport error code to string conversion |
| `debug.h` | ~12 | Debug macros: `OT_CHECK()`, `SSH_CHECK()` |
| `unicode.h` | ~65K | Mac Roman ↔ Unicode translation table (auto-generated, do not edit) |
| `constants.r` | ~124 | All `#define` constants: version, resource IDs, menu IDs, dialog IDs |
| `resources.r` | ~624 | Rez resource definitions: dialogs, alerts, menus, SIZE, version, icons |
| `icons.r` | ~490 | Application and file icon pixel data |
| `build-fat.bash` | ~48 | Script to build combined 68K+PPC fat binary |

## Architecture

### Multi-Window / Multi-Session

```
windows[MAX_WINDOWS=8]        sessions[MAX_SESSIONS=8]
┌─────────────────────┐       ┌──────────────────────┐
│ window_context      │       │ session              │
│   win (WindowPtr)   │──────▶│   type (SSH/LOCAL)   │
│   session_ids[]     │       │   vterm/vts          │
│   num_sessions      │       │   scrollback[][]     │
│   active_session_idx│       │   shell state        │
│   size_x, size_y    │       │   SSH channel/endpoint│
└─────────────────────┘       │   window_id          │
                              └──────────────────────┘
```

- Each window has its own tab bar and can hold multiple sessions
- Sessions are either `SESSION_SSH` (threaded network I/O) or `SESSION_LOCAL` (shell interpreter)
- Convenience macros: `ACTIVE_WIN`, `ACTIVE_S` — use these to access current window/session
- Helper functions: `find_window_context(WindowPtr)`, `window_for_session(idx)`, `active_session_global()`

### Scrollback

Compact ring buffer per session: `struct sb_cell scrollback[100][80]` = 32KB/session.
Each `sb_cell` is 4 bytes (char, fg, bg, attrs) vs ~36 bytes for `VTermScreenCell`.

- `sb_pushline()` / `sb_popline()` — vterm callbacks when lines scroll off screen
- `get_cell_scrolled()` — returns cell from scrollback or live screen based on `scroll_offset`
- Shift+PageUp/Down to scroll, any other key snaps back to live

### Local Shell

Shell commands are dispatched in `shell_execute()` via a long if/else chain (not a function table).
Each command is `static void cmd_foo(int idx, int argc, char* argv[])` where `idx` is the session index.

Output goes through `vt_write(idx, str, len)` and `vt_print_long(idx, num)`.
The shell has its own working directory per session (`shell_vRefNum`, `shell_dirID`).

### SSH Threading

SSH uses cooperative threads via the Mac Thread Manager. The read thread polls `libssh2_channel_read()` and writes data into the session's vterm. The main thread handles keyboard input → `libssh2_channel_write()`.

## Classic Mac OS API Quick Reference

### Memory Manager (used by `free` command)
- `ApplicationZone()`, `SystemZone()` — heap zone pointers
- `FreeMem()`, `FreeMemSys()` — free bytes in app/system heap
- `MaxBlock()` — largest contiguous free block
- `TempFreeMem()`, `TempMaxMem(&grow)` — temporary memory (outside app heap)

### Process Manager (used by `ps` command)
- `GetNextProcess(&psn)` — iterate running processes
- `GetProcessInformation(&psn, &info)` — get ProcessInfoRec
- `MacGetCurrentProcess(&psn)` — identify self

### File Manager (used by shell file commands)
- `PBHGetVInfo()` — volume info (for `df`)
- `PBGetCatInfo()` — file/folder metadata
- `HCreate()`, `HDelete()`, `HRename()`, `FileCopy()` — file operations
- `FSSpec` + `vRefNum`/`dirID` — how classic Mac OS identifies files

### Open Transport (networking)
- `OTOpenEndpoint()` — create TCP endpoint
- `OTConnect()` — TCP connect
- `OTSnd()` / `OTRcv()` — send/receive data

## Constants Naming Convention

All constants live in `constants.r` (shared by C code and Rez resources):

- `APP_*` — application-level: version, partition size, icon IDs
- `SSH_BUFFER_SIZE` — network buffer size
- `DEFAULT_TERM_STRING` — terminal type ("xterm-16color")
- `MBAR_MAIN` — menu bar resource ID
- `MENU_*` — menu resource IDs
- `FMENU_*` — File menu item indices
- `DLOG_*` / `DITL_*` / `ALRT_*` — dialog/alert resource IDs
- `CNTL_PREF_*` — preferences dialog control IDs

## Gotchas and Lessons Learned

- **Memory is precious**: 2MB partition. Scrollback uses compact 4-byte cells, not full VTermScreenCells (would be ~950KB/session)
- **Page keys as control chars**: `kPageDownCharCode` = 0x0C = form feed. Must intercept page up/down/home/end in `shell_input()` before they reach vterm
- **Shift keys are separate bits**: `shiftKey` (0x0200) is left shift only. Must check `(shiftKey | rightShiftKey)` for both
- **`hcopy -m` filename length**: HFS has a 31-char filename limit. If the source path is too long, copy to /tmp first with a shorter name
- **cmake cache after renames**: Always nuke `build-*/` when renaming source files or the cmake target
- **C89 only**: Retro68 targets C89/C90. No `//` comments in headers shared with Rez, no mixed declarations and code, no VLAs
- **Rez resource compiler**: `.r` files are processed by Rez, not the C compiler. They share `constants.r` via `#include` but use Rez syntax for resource definitions
- **Thread Manager**: Required for SSH (cooperative multitasking). System 7.5+ has it built in; 7.1-7.4 need the extension
- **Open Transport**: Required for networking. Not available on very old System 7 installs. Local shell works without it
