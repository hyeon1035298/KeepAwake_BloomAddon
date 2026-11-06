# Coffee (Bloom add-on)

[한국어](README.ko.md)

An add-on for the [Bloom](https://github.com/hyeon1035298/Bloom) launcher that keeps the computer awake from the Windows 11 taskbar. A coffee cup sits next to the clock: click it and the computer stays awake, click again and it may sleep. While it is on, the cup is filled and steaming.

- Click the cup to turn it on or off
- Right-click it for a small window: until turned off, 30 minutes, 1 hour, 2 hours, or a number of minutes
- Keep the screen on too, or keep only the system awake and let the screen turn off as usual
- The time left shows in the cup's tooltip and in the window
- After Explorer restarts, a running session carries on with the time it had left

It works on the stock Windows 11 taskbar and on [PillTaskbar](https://github.com/hyeon1035298/PillTaskbar_BloomAddon) alike, and does not need PillTaskbar. With the Monitor Brightness and Memory Clean add-ons the row reads coffee, brightness, RAM, clock, and only one of their windows is open at a time. While the screen is kept on, Monitor Brightness does not dim it when idle.

## How it works

The add-on runtime and host from PillTaskbar load the mod in `mod/` into Explorer: the host finds the taskbar function the mod uses to reach the taskbar's XAML, injects a small runtime that provides the Windhawk API the mod uses, and keeps it running. The host itself is a separate small process that Bloom starts.

Inside Explorer the mod adds the cup to the tray and keeps one thread of its own that calls `SetThreadExecutionState` (`ES_CONTINUOUS | ES_SYSTEM_REQUIRED`, plus `ES_DISPLAY_REQUIRED` when the screen stays on). Windows holds such a request only while that thread lives, so it ends with the add-on: turning the add-on off or closing Bloom lets the computer sleep again. The session is written to `%LOCALAPPDATA%\Bloom\addons\keep-awake\awake.json`; when Explorer restarts in the same boot, the mod picks it up again.

## Requirements

- Windows 11, 64-bit
- Bloom 0.19.0 or later
- An internet connection on the first start (see below)

## Install

1. Download `KeepAwake_BloomAddon-<version>.zip` from [Releases](https://github.com/hyeon1035298/KeepAwake_BloomAddon/releases).
2. Unzip it into `%LOCALAPPDATA%\Bloom\user-addons\keep-awake\`. The folder name must be `keep-awake`.
3. Restart Bloom, open Settings › Add-ons and turn on "Coffee".

Bloom checks every file against the SHA-256 hashes in `addon.json` before it starts the add-on. To remove the add-on, turn it off and delete the folder.

### First start

The mod reaches the taskbar's XAML through internal functions of `taskbar.dll`, so the host needs that DLL's symbols. On the first start, and again after a Windows update changes the DLL, it downloads the matching public symbol file from Microsoft's symbol server and keeps only the offsets. Nothing from Microsoft ships with the add-on.

## Settings

In Bloom, Settings › Add-ons › Coffee:

- **Keep awake**: the screen too (the default), or the system only. A change applies at once to a running session.
- **Default time**: how long a click keeps it on: until turned off (the default), 30 minutes, 1 hour, 2 hours or custom.
- **Custom time**: the minutes for a custom default time (45 to start with), and where the window's custom field starts.

The window's picks apply to the session they start; the next click starts from these settings again. The light or dark look follows Bloom's theme, and the window's glass follows Bloom's glass effect.

## Build from source

You need a clang that targets `x86_64-w64-mingw32` (llvm-mingw, or the compiler that ships with Windhawk), Python 3, and a bash shell such as Git Bash. The build uses PillTaskbar's add-on tools, so clone both repositories side by side:

```
git clone https://github.com/hyeon1035298/KeepAwake_BloomAddon
git clone https://github.com/hyeon1035298/PillTaskbar_BloomAddon
cd KeepAwake_BloomAddon
export CXX=/path/to/llvm-mingw/bin/clang++.exe   # optional, defaults to Windhawk's compiler
bash build.sh
```

`PILL_ROOT` points `build.sh` at a PillTaskbar_BloomAddon checkout elsewhere. llvm-mingw has no C++/WinRT headers; set `CPPWINRT_INCLUDE` to a folder that the Windows SDK's `cppwinrt.exe` wrote, as the workflow does. The result lands in `build/addon`.

The workflow in `.github/workflows/build.yml` builds every push against the PillTaskbar_BloomAddon commit pinned in `PILL_REF`. A `v*` tag publishes a release with the zip, its SHA-256, and a `SOURCE.txt` that names the commits of both repositories.

## License

The add-on is free software under the GNU General Public License, version 3 only. See `LICENSE`.

The runtime and host bundle MinHook (BSD-2-Clause) and Zydis (MIT). Credits and license texts are in `THIRD_PARTY.md` and, in the release zip, `THIRD_PARTY_NOTICES.txt`.

This program comes with no warranty. It runs code inside explorer.exe, so use it at your own risk. Windows is a trademark of Microsoft. This project is not affiliated with Microsoft, Raycast or the authors of other keep-awake tools.
