SevenTTY
========
SSH client, local shell, and terminal emulator for classic Mac OS 7/8/9.

A fork of [ssheven](https://github.com/cy384/ssheven) by cy384, expanded into a full-featured terminal environment.

![SevenTTY screenshot](screenshot.png)

features
--------
* **Multi-window**: Cmd+N opens independent windows, each with their own tabs
* **Tabbed sessions**: Cmd+T for local shell tabs, Cmd+S for SSH tabs, Cmd+1-8 to switch
* **Local shell**: built-in command interpreter with 30+ commands
  * File operations: `ls`, `cd`, `cat`, `cp`, `mv`, `rm`, `mkdir`, `touch`, and more
  * System info: `ps`, `free`, `df`, `uname`, `date`
  * Mac-specific: `getinfo`, `chown`, `settype`, `setcreator`, `chmod`, `label`
  * Tab completion, command history (up/down arrows), colorized `ls` output
* **SSH client**: password and public key authentication, known hosts verification
* **Scrollback**: Shift+Page Up/Down to scroll through history (100 lines per session)
* **Copy/paste**: mouse text selection with Cmd+C/V
* **16-color terminal**: xterm-compatible with bold, italic, underline, reverse video
* **Configurable**: font size, foreground/background colors, terminal type string

system requirements
-------------------
* **CPU**: any PPC processor, or at least a 68030 (68040 strongly recommended)
* **RAM**: 2 MB
* **Disk**: fits on a floppy
* **System**: 7.1 or later (versions below 7.5 require the Thread Manager extension)
* **Network**: Open Transport required for SSH (local shell works without it)

keyboard shortcuts
------------------
| Shortcut | Action |
|----------|--------|
| Cmd+N | New window |
| Cmd+T | New local shell tab |
| Cmd+S | New SSH tab |
| Cmd+W | Close tab/window |
| Cmd+D | Disconnect SSH / close tab / quit |
| Cmd+K | Connect (SSH) |
| Cmd+1-8 | Switch tabs |
| Cmd+C/V | Copy/paste |
| Shift+PgUp/PgDn | Scroll through history |

build
-----
Requires [Retro68](https://github.com/autc04/Retro68/) with Universal Headers and cmake.

Dependencies (mbedtls, libssh2, libvterm) are pulled in as submodules and built automatically.

```bash
mkdir build-m68k && cd build-m68k
cmake .. -DCMAKE_TOOLCHAIN_FILE=/path/to/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
cmake --build . --parallel $(nproc)
```

The script `build-ssheven.bash` can also be used to build a fat binary.

license
-------
Licensed under the BSD 2 clause license, see `LICENSE` file.

Based on ssheven by [cy384](https://github.com/cy384/ssheven).
