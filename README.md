# LINE MR for Windows (Line Mai Roo)

**Screen reader access to LINE for Windows with NVDA.**

LINE MR is a free, open-source NVDA add-on that makes the LINE desktop app usable without sight. With it, NVDA reads LINE's buttons, lists and messages, every control can be reached from the keyboard, and incoming calls are announced and can be answered. It does not modify LINE's own files.

It is the Windows counterpart of [LINE MR for Mac](https://github.com/Peetchy/line-mr-for-mac) (VoiceOver).

> **ภาษาไทย:** LINE MR (ไลน์ไม่รู้) เป็นส่วนเสริมฟรีสำหรับ NVDA ที่ทำให้ผู้พิการทางการเห็นใช้ LINE บน Windows ได้ด้วยตนเอง NVDA จะอ่านปุ่ม รายการแชท และข้อความ ใช้งานทุกส่วนได้ด้วยแป้นพิมพ์ และแจ้งสายเรียกเข้าพร้อมรับสายได้ โดยไม่แก้ไขไฟล์ของ LINE ดู[คู่มือภาษาไทย](addon/doc/th/readme.md)

**[⬇ Download the latest version (`lineMR.nvda-addon`)](https://github.com/Peetchy/line-mr-for-win/releases/latest/download/lineMR.nvda-addon)** · [All releases and release notes](https://github.com/Peetchy/line-mr-for-win/releases)

---

## Contents

- [Features](#features)
- [Requirements](#requirements)
- [Installation](#installation)
- [Keyboard commands](#keyboard-commands--คำสั่งแป้นพิมพ์)
- [Documentation and languages](#documentation-and-languages)
- [Privacy and safety](#privacy-and-safety)
- [Troubleshooting](#troubleshooting)
- [How it works](#how-it-works)
- [Development](#development)
- [Contributing and feedback](#contributing-and-feedback)
- [License](#license)

## Features

**Reading**
- Names for LINE's unlabelled buttons, text fields, checkboxes, tabs and menus. The names follow LINE's own language (English, Thai, Japanese, Simplified or Traditional Chinese).
- Chat list: each chat reads its name, member count, last message, time and unread count. The open chat reads "Selected" and pinned chats read "Pinned".
- Messages: who sent each message, its text, time and read receipt. Messages without text say what they are: photo, album, video, sticker, voice message (with its length), file, location, call, link and reactions.
- Long messages ("See more") read line by line like a document, with line feeds only where the writer made them.

**Keyboard**
- Tab reaches every control in a logical order, and Space or Enter presses it.
- Arrow keys browse the chat list and friend list without opening chats, which keeps LINE fast.
- Enter on a message opens what it holds: a link, a photo, a video, a file, a voice message or the full text of a long message.
- The Applications key opens a message's menu (Reply, Copy, Unsend, Delete, ...). LINE MR adds "React" and, in groups, "Show <name>'s profile".
- Photos & videos, links and files lists can be browsed, opened and saved. In the video player, Space plays or pauses and the arrows seek.

**Windows and dialogs**
- Settings, profiles, member lists, chat search, sharing, stickers and emoji, and the create chat / group / OpenChat pages all work with the keyboard.
- LINE's photo and video viewer and its call program are supported too.

**Calls**
- Incoming calls are announced from any application. Alt+A answers and Alt+D declines or hangs up.
- NVDA keeps its volume while LINE lowers other sounds during a call.

**Maintenance**
- LINE MR puts its bridge back after LINE updates itself.
- It tells you when the bridge cannot load, and why (for example Smart App Control or antivirus software).
- It checks for new versions of LINE MR. **What's new** in NVDA's settings lists the changes of each version.

## Requirements

| | |
|---|---|
| Operating system | Windows 10 or 11, 64-bit |
| Screen reader | NVDA 2024.1 or later (tested with NVDA 2026.1.1) |
| LINE | LINE for Windows 26.5 (Qt 6.6). **Recommended: [LINE from the Microsoft Store](https://apps.microsoft.com/detail/XPFCC4CD725961)** ("LINE Desktop", publisher LY Corporation). This is the version LINE MR is developed and tested with. It installs LINE into `%LOCALAPPDATA%\LINE` and keeps it up to date. |

## Installation

1. Install [LINE from the Microsoft Store](https://apps.microsoft.com/detail/XPFCC4CD725961) and sign in.
2. [Download `lineMR.nvda-addon`](https://github.com/Peetchy/line-mr-for-win/releases/latest/download/lineMR.nvda-addon), open the file while NVDA is running, confirm the installation and restart NVDA.
3. Start LINE. LINE MR asks to add its bridge to LINE; choose **Yes**. LINE restarts once, and LINE MR is then active.

To check that it works, press **NVDA+Shift+Control+L** in LINE: NVDA says whether the bridge is running.

**Updates.** When NVDA starts, LINE MR checks for a new release and offers to install it. You can turn this off or check right away in NVDA menu > Preferences > Settings > **LINE MR**, which also has the **What's new** button.

**Uninstalling.** Remove LINE MR in NVDA's Add-on Store (installed add-ons). The bridge is a single file, `platformthemes\line_a11y.dll`, in LINE's folder; deleting it, or reinstalling LINE, removes it from LINE.

## Keyboard commands / คำสั่งแป้นพิมพ์

| Command | Action | การทำงาน |
|---|---|---|
| Alt+1 / Alt+2 / Alt+3 | Friends / Chats / Add friends tab | แท็บเพื่อน / แชท / เพิ่มเพื่อน |
| Alt+C | Move to the chat list (switches to the Chats tab first if needed) | ไปที่รายการแชท (สลับไปแท็บแชทก่อนถ้าจำเป็น) |
| Alt+L | Move to the chat list or friend list | ไปที่รายการแชทหรือรายชื่อเพื่อน |
| Alt+F | Move to the search field of the chat list or friend list | ไปที่ช่องค้นหาของรายการแชทหรือรายชื่อเพื่อน |
| Alt+M | Move to the messages of the open chat (starts at the newest) | ไปที่ข้อความของแชทที่เปิดอยู่ (เริ่มที่ข้อความล่าสุด) |
| Alt+I | Move to the message input | ไปที่ช่องพิมพ์ข้อความ |
| NVDA+Alt+1 … 9 | Read a recent message (1 = newest) | อ่านข้อความล่าสุด (1 = ใหม่สุด) |
| Alt+A / Alt+D in LINE (NVDA+Shift+Control+A / D from any application) | Answer / decline an incoming LINE call; Alt+D in LINE ends the current call | รับสาย / ปฏิเสธสายเรียกเข้าของ LINE (Alt+A / Alt+D ใน LINE, NVDA+Shift+Control+A / D จากทุกโปรแกรม); Alt+D ใน LINE วางสายที่กำลังคุยอยู่ |
| NVDA+Shift+Control+L | Report whether the bridge is running; press twice to copy a report for the developer | บอกว่าตัวเชื่อมทำงานอยู่หรือไม่ กดสองครั้งเพื่อคัดลอกรายงานสำหรับผู้พัฒนา |

### In lists and messages

| Where | Keys |
|---|---|
| Chat list, friend list | Up/Down, Page Up/Page Down, Home/End browse without opening a chat; Enter opens it; Applications key or Shift+F10 opens the chat's menu |
| Messages | Up/Down one message; Page Up/Page Down about a screen; Home the oldest loaded message (again: load older ones); End the newest; Enter or Space opens a link, photo, video, file, voice message or long text; Applications key or Shift+F10 the message's menu; Tab / Shift+Tab back into the chat returns to the message read last |
| Message menu | "React": Left/Right choose a reaction, Enter gives it ("Remove reaction" once you reacted); in groups "Show <name>'s profile" |
| Chat search field | Enter searches; Up/Down go to older / newer matches; LINE MR reads the result; Alt+M moves to it |
| Stickers and emoji | The first Enter selects one and plays LINE's preview; the second Enter sends it |
| Photos & videos, links, files | Arrows move; Enter opens; Applications key offers Open, Save, Copy link and Copy text |
| Video player | Space plays or pauses; Left/Right seek 5 seconds; Page Up/Page Down seek 30 seconds |

Tab order in the chat list panel: buttons, chat folders, search field, chat list. In a chat: header buttons, note, message list, message input, then the message bar buttons (stickers ... attach file). Commands can be changed in NVDA's Input gestures, category "LINE MR". Full guide: [English](addon/doc/en/readme.md), [ภาษาไทย](addon/doc/th/readme.md), [日本語](addon/doc/ja/readme.md), [简体中文](addon/doc/zh_CN/readme.md), [繁體中文](addon/doc/zh_TW/readme.md).

## Documentation and languages

| Language | User guide | What's new |
|---|---|---|
| English | [readme](addon/doc/en/readme.md) | [changes](addon/doc/en/changes.md) |
| ภาษาไทย | [คู่มือ](addon/doc/th/readme.md) | [มีอะไรใหม่](addon/doc/th/changes.md) |
| 日本語 | [ガイド](addon/doc/ja/readme.md) | [新機能](addon/doc/ja/changes.md) |
| 简体中文 | [指南](addon/doc/zh_CN/readme.md) | [新功能](addon/doc/zh_CN/changes.md) |
| 繁體中文 | [指南](addon/doc/zh_TW/readme.md) | [新功能](addon/doc/zh_TW/changes.md) |

The guide is also available in NVDA: Add-on Store > installed add-ons > LINE MR > Help. The Japanese and Chinese translations are new, and corrections from native speakers are welcome.

## Privacy and safety

- **Nothing leaves your computer.** LINE MR reads LINE's screen inside LINE's own process and gives the result to NVDA. It sends no chat contents anywhere. The only network access is the update check, which asks GitHub for the latest release.
- **LINE's files stay unchanged.** The bridge is one added file (`platformthemes\line_a11y.dll`) that LINE's Qt loads like its own plug-ins. It reads LINE's user interface through Qt's public interfaces only, and never reads LINE's memory directly.
- **Crash protection.** If LINE crashes twice in a row with the bridge, message reading pauses itself and LINE MR tells you how to turn it back on.
- **Diagnostics are opt-in.** The report copied by NVDA+Shift+Control+L (pressed twice) holds versions, folders (with user names removed) and the bridge's state, but no chat contents. It is only placed on your clipboard, so you decide whether to send it.

## Troubleshooting

| Problem | What to do |
|---|---|
| NVDA reads "grouping" or nothing in LINE | Press NVDA+Shift+Control+L. If the bridge is not running, accept the offer to install it and let LINE restart. Closing LINE's window only hides it in the notification area: quit LINE from there, or restart the computer. |
| "The bridge could not be loaded" | Windows Smart App Control or antivirus software can block or remove the bridge. LINE MR names the likely cause. Allow `line_a11y.dll` in your antivirus software, or use the Microsoft Store version of LINE. |
| Messages are not read after LINE crashed | Message reading is paused for safety after two crashes in a row. LINE MR says how to turn it back on. |
| Calls are not announced | LINE downloads its call program separately. LINE MR adds the bridge there by itself; restart LINE once after LINE's first call. |

If the problem remains, press NVDA+Shift+Control+L twice. Paste the report into a [new issue](https://github.com/Peetchy/line-mr-for-win/issues) and describe what you did and what NVDA said.

## How it works

LINE for Windows is built with Qt 6 (Widgets and Qt Quick). Its custom controls (`LcButton`, `LcText`, `LcTextField`, `LcListView`, ...) expose only their class names to UI Automation, so a screen reader hears "grouping" almost everywhere. LINE also installs a handler that discards Qt's accessibility events. LINE MR has two parts:

| Part | What it does |
|---|---|
| **Bridge** (`bridge/`, C++, `line_a11y.dll`) | A Qt platform-theme plugin that LINE loads from `LINE\bin\current\platformthemes`. It returns no theme, so LINE looks exactly the same. It removes LINE's no-op accessibility handler and installs `QAccessible` interfaces for LINE's controls (names, roles, states, actions). It reads text and icons by recording what each control paints, and adds keyboard handling where LINE has none. The same bridge also runs in LINE's call program (`LineCall.exe`) and its media viewer (`LineMediaPlayer.exe`). |
| **NVDA add-on** (`addon/`, Python) | App modules for LINE and its helper programs. They install and repair the bridge, add the commands, announce calls, keep NVDA's volume during calls, and check for updates. The bridge DLL ships inside the add-on. |

Design rules: LINE's own files are never modified, the bridge is inert outside LINE's processes, and it never uses memory offsets or raw memory reads. It relies only on Qt's meta-object data and on what LINE paints.

## Development

### Prerequisites

- Visual Studio 2022 with the C++ workload (includes CMake and Ninja)
- Python 3
- The Qt 6.6.3 MSVC SDK:

```powershell
python -m pip install aqtinstall markdown comtypes
python -m aqt install-qt windows desktop 6.6.3 win64_msvc2019_64 --archives qtbase qtdeclarative --outputdir $HOME\Qt
```

### Build, install, test

```powershell
tools\build.ps1                      # bridge\build\line_a11y.dll + dist\lineMR-<version>.nvda-addon
tools\install-bridge.ps1 -Restart    # put the bridge into LINE (and its helpers) and restart LINE
tools\dev-install-addon.ps1          # copy the add-on into NVDA's add-ons folder and restart NVDA
python tests\check_addon_imports.py  # the add-on only imports modules NVDA ships
python tests\verify_uia_tree.py      # login-window UIA checks against the running LINE
python tests\verify_main_window.py   # main-window checks (tags, named tabs, readable list rows)
```

For a normal install, open `dist\lineMR-<version>.nvda-addon` with NVDA.

### Continuous integration and releases

`.github/workflows/build.yml` builds the bridge (Qt 6.6.3 from aqtinstall, MSVC) and the add-on on every push and pull request, and attaches the `.nvda-addon` file to the run. A tag `v<version>` that matches `version` in `addon/manifest.ini` publishes a GitHub release. Its notes come from the tagged commit's message and this README's "Keyboard commands" section (`tools/release_notes.py`). Before tagging, add the version's user-facing changes to `addon/doc/<language>/changes.md`.

### Diagnostic tools

| Tool | Purpose |
|---|---|
| `python tools\uia_inspect.py [--depth N] [--invoke NAME \| --toggle NAME]` | Prints LINE's live UIA tree (name, value, class, patterns, states); can invoke or toggle elements |
| `python tools\uia_events.py [--keys tab,down]` | Logs the UIA focus events NVDA follows |
| `python tools\post_keys.py down up contextmenu`, `python tools\focus_tag.py list_chats` | Send keys or move focus inside LINE only (safe while someone else uses the computer) |
| `python tools\focus_line.py` | Brings LINE's window to the foreground |
| `python tools\nvda_speech_probe.py` | Presses global keys and prints NVDA's speech |
| `python tools\dbgview.py [--grep TEXT]` | Shows `OutputDebugString` output (Qt's debug output) |
| `New-Item $env:LOCALAPPDATA\LineMR\dump-request` | The bridge writes `widget-dump.txt` (widgets, LINE properties, painted text, models, accessible tree), `quick-dump.txt` (Qt Quick items) and `delegates-dump.txt` (list row templates) |
| `New-Item $env:LOCALAPPDATA\LineMR\type-probe` | With a chat open: what each visible message row draws (`type-probe.txt`) |

Logs and crash data live in `%LOCALAPPDATA%\LineMR`: `logs\bridge.log` (plus `linecall.log` and `linemediaplayer.log`), `crash.dmp` after a heap corruption (read it with WinDbg's `cdb -z`), and `breadcrumbs.bin` (the bridge's last 64 actions, copied into the next session's log after a crash). NVDA's speech appears in `%TEMP%\nvda.log` as `Speaking [...]` when NVDA's log level is "debug" or "input/output".

### Source layout

| Path | Purpose |
|---|---|
| `bridge/src/plugin_main.cpp` | Plugin entry point and start-up |
| `bridge/src/text_capture.*` | Recovers painted text, images and LINE's icons with a recording paint engine |
| `bridge/src/line_introspect.*` | Class and property helpers, icon names, labels in five languages, reading order |
| `bridge/src/accessible_widgets.*` | Buttons, checkboxes, texts, inputs, menus |
| `bridge/src/accessible_list.*` | `LcListView` rows: chat list, friends, messages, media grids |
| `bridge/src/accessible_quick.*` | LINE's Qt Quick (QML) windows: profiles, member lists, friend pickers, calls |
| `bridge/src/full_text_view.*` | The line-by-line reader for long messages |
| `bridge/src/video_controls.*` | Keyboard control of the video player |
| `bridge/src/call_install.*` | Keeps the bridge in LINE's call and media helper programs |
| `bridge/src/a11y_event_filter.*`, `a11y_announce.*`, `a11y_diagnostics.*` | Keyboard and focus fixes, announcements, on-demand dumps |
| `addon/appModules/line/` | NVDA app module for LINE (`bridge.py` installs and checks the DLL) |
| `addon/appModules/linecall.py`, `linemediaplayer.py` | App modules for the call program and the media viewer |
| `addon/globalPlugins/` | Call announcements and commands, update check, settings panel |
| `addon/locale/`, `addon/doc/` | Translations, user guides and "What's new" in five languages |
| `tools/`, `tests/` | Build, install, inspection and checks |

## Contributing and feedback

Bug reports, translations and pull requests are welcome in [GitHub Issues](https://github.com/Peetchy/line-mr-for-win/issues). For a bug, please include the report from NVDA+Shift+Control+L (pressed twice), your LINE and NVDA versions, and the steps that lead to the problem. A new user-visible label or message needs all five languages: the bridge's `kLabels` in `line_introspect.cpp`, the add-on's `.po` files and the guides.

## License

GNU General Public License, version 2 or (at your option) any later version. See [`COPYING.txt`](COPYING.txt).

LINE MR is an unofficial, independent project. It is not affiliated with or endorsed by LY Corporation or LINE. LINE is a trademark of LY Corporation.
