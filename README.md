# Line MR for Windows (Line Mai Roo)

An NVDA add-on that makes **LINE for Windows** (`%LOCALAPPDATA%\LINE\bin\current\LINE.exe`, 26.5.0.3975, Qt 6.6.3) usable with a screen reader. It is the Windows counterpart of `line-mr-for-mac` (VoiceOver).

ทำให้ผู้ใช้ NVDA ใช้ LINE บน Windows ได้ เช่น อ่านปุ่มที่ไม่มีชื่อ ช่องข้อความ กล่องกาเครื่องหมาย และข้อความต่าง ๆ ใน LINE โดยไม่แก้ไขไฟล์ของ LINE

## How it works

LINE is a Qt 6 Widgets app. Its custom widgets (`LcButton`, `LcText`, `LcTextField`, ...) expose only their class names to UI Automation, so NVDA reads "grouping" everywhere. The project has two parts:

| Part | What it does |
|---|---|
| **Bridge** (`bridge/`, C++, `line_a11y.dll`) | A Qt "windows" platform-theme plugin that LINE's Qt loads from `LINE\bin\current\platformthemes`. It returns no theme (LINE looks the same), installs `QAccessible` interfaces for the `Lc*` widgets (names, roles, states, press/toggle actions) and reads text by recording what each widget paints, without memory offsets. |
| **NVDA add-on** (`addon/`, Python) | App module for `LINE.exe`: installs or updates the bridge when needed (LINE updates remove it), hides layout noise, and adds commands. Ships the bridge DLL inside the add-on. |

## Requirements

- Windows 10/11 x64, NVDA 2024.1 or later (tested with 2026.1.1)
- LINE for Windows 26.5.0.3975 (other Qt 6.6 builds are expected to work). **Install [LINE from the Microsoft Store](https://apps.microsoft.com/detail/XPFCC4CD725961)** ("LINE Desktop", Store ID XPFCC4CD725961, publisher LY Corporation) for the best compatibility: that is the version LINE MR is developed and tested with. It installs LINE into `%LOCALAPPDATA%\LINE` and keeps it updated, so the bridge can be added there. Other installers may put LINE somewhere else or ship a different build.
  แนะนำให้ติดตั้ง [LINE จาก Microsoft Store](https://apps.microsoft.com/detail/XPFCC4CD725961) (LINE Desktop ผู้เผยแพร่ LY Corporation) เพื่อให้ทำงานเข้ากันได้มากที่สุด เพราะเป็นรุ่นที่ใช้พัฒนาและทดสอบ LINE MR
- To build: Visual Studio 2022 (C++ workload, includes CMake and Ninja), Python 3, and the Qt 6.6.3 MSVC SDK:

```powershell
python -m pip install aqtinstall markdown comtypes
python -m aqt install-qt windows desktop 6.6.3 win64_msvc2019_64 --archives qtbase qtdeclarative --outputdir $HOME\Qt
```

## Build, install, test

```powershell
tools\build.ps1                      # bridge\build\line_a11y.dll + dist\lineMR-<version>.nvda-addon
tools\install-bridge.ps1 -Restart    # put the bridge into LINE and restart LINE
tools\dev-install-addon.ps1          # copy the add-on into NVDA's add-ons folder and restart NVDA
python tests\check_addon_imports.py  # the add-on only imports modules NVDA ships
python tests\verify_uia_tree.py      # login-window UIA checks against the running LINE
```

For a normal install, open `dist\lineMR-<version>.nvda-addon` with NVDA.

## Continuous build and releases

`.github/workflows/build.yml` builds the bridge (Qt 6.6.3 SDK from aqtinstall, MSVC) and the add-on on every push and pull request; the `.nvda-addon` file is attached to the run as an artifact. Pushing a tag `v<version>` that matches `version` in `addon/manifest.ini` (for example `v0.3.0`) also creates a GitHub release with the add-on, which is the download URL to give the NVDA Add-on Store.

## Tools

- `python tools\uia_inspect.py [--depth N] [--invoke NAME | --toggle NAME] [--nth N]`: prints LINE's live UIA tree (name, value, class, patterns and toggle state) and can invoke or toggle elements.
- `New-Item $env:LOCALAPPDATA\LineMR\dump-request`: the bridge writes `%LOCALAPPDATA%\LineMR\widget-dump.txt` (widget tree, LINE properties, painted text, item models, accessible tree).
- `python tools\dbgview.py [--grep TEXT]`: shows `OutputDebugString` output (Qt's debug output, for example with `QT_DEBUG_PLUGINS=1`).
- `python tools\uia_events.py [--keys tab,down]`: logs UIA focus events (what NVDA follows).
- `python tools\post_keys.py down up contextmenu` and `python tools\focus_tag.py list_chats`: send keys / move focus inside LINE only (safe while the user works); `tools\nvda_speech_probe.py` presses global keys and prints NVDA's speech.
- `python tests\verify_main_window.py`: main-window checks (tags, named tabs, readable list rows).
- `python tools\focus_line.py`: brings LINE's window to the foreground (so NVDA reads it).
- Crash dumps: on a heap-corruption crash the bridge writes `%LOCALAPPDATA%\LineMR\crash.dmp` (read it with WinDbg's `cdb -z`). Creating `%LOCALAPPDATA%\LineMR\heap-check` makes the bridge validate the heap around every render (slow). Dump requests also write `quick-dump.txt` (Qt Quick item trees) and `delegates-dump.txt` (LcListView row templates).
- Bridge log: `%LOCALAPPDATA%\LineMR\logs\bridge.log`. What NVDA speaks appears in `%TEMP%\nvda.log` as `Speaking [...]` when NVDA's log level is "debug" or "input/output".
- Crash breadcrumbs: the bridge's last 64 actions live in `%LOCALAPPDATA%\LineMR\breadcrumbs.bin`; after a crash they are copied into the next session's `bridge.log` (`[guard]` lines). `widget-dump.txt` requests also write `resource-list.txt` (LINE's call-related resource names), and an opened call window is dumped to `call-window.txt`.

## Layout

| Path | Purpose |
|---|---|
| `bridge/src/plugin_main.cpp` | Plugin entry point and bootstrap |
| `bridge/src/text_capture.*` | Recovers painted text with a recording paint engine |
| `bridge/src/line_introspect.*` | Class/property helpers, icon names, Thai/English labels, reading order |
| `bridge/src/accessible_widgets.*` | Buttons, checkboxes, texts, inputs, menus |
| `bridge/src/accessible_list.*` | `LcListView` rows (chat list, friends, messages) |
| `bridge/src/accessible_quick.*` | LINE's Qt Quick (QML) windows: member list, profiles, ... |
| `bridge/src/a11y_announce.*` | UIA notification announcements |
| `bridge/src/a11y_event_filter.*`, `a11y_diagnostics.*` | Event fixes, on-demand dumps |
| `addon/appModules/line/` | NVDA app module (`bridge.py` installs and checks the DLL) |
| `addon/locale/th/`, `addon/doc/` | Thai translation, user guide (English and Thai) |
| `tools/`, `tests/` | Build, install, inspection and checks |

## Status

- Done: login window; keyboard access (Tab to every control, Space/Enter); main window (sidebar, chat list, friend list, message list, chat header and input bar); chat-list browsing with arrows; Home/End in messages; keyboard context menus; Qt Quick pages (member list, profiles) and other windows read and focused when opened; heap-corruption crash fixed; add-on commands (Alt+1/2/3, Alt+L/M/I, NVDA+Alt+1…9); new-message announcements (untested with a real incoming message); crash guard; bridge install/update from NVDA.
- Next: message kinds (stickers vs photos, files), section folding state, error/notice announcements, call window (incoming-call buttons are named; the in-call view is partly Qt Quick and not yet checked).

## License

GNU General Public License, version 2 or (at your option) any later version. See [`COPYING.txt`](COPYING.txt).

This is an unofficial, independent project. It is not affiliated with or endorsed by LY Corporation / LINE.
