# LINE MR (Line Mai Roo) for NVDA

LINE MR makes [LINE for Windows](https://line.me) usable with NVDA.

LINE draws its own buttons and text, so on its own it exposes almost nothing to screen readers: buttons have no names and texts cannot be read. LINE MR adds a small accessibility bridge to LINE that gives LINE's controls names, roles and actions, and an NVDA app module that tidies up what NVDA reads.

## Installing

**Recommended: install [LINE from the Microsoft Store](https://apps.microsoft.com/detail/XPFCC4CD725961)** ("LINE Desktop" by LY Corporation; in the Microsoft Store app you can also search for "LINE"). LINE MR is developed and tested with the Microsoft Store version, so it gives the best compatibility. It installs LINE into `%LOCALAPPDATA%\LINE` and keeps it up to date. LINE from other sources may be installed somewhere else or be a different build, and the bridge may not install or work there.

1. Install the add-on and restart NVDA.
2. Start LINE (or switch to it). LINE MR asks to install its bridge into LINE. Choose **Yes**. LINE closes and starts again with the bridge.

After a LINE update the bridge may have to be installed again; LINE MR notices and asks again.

## Updating LINE MR

When NVDA starts, LINE MR checks GitHub for a newer release. If there is one, a dialog offers **Update now**, **Later** or **Skip this version**. Update now downloads the new version and hands it to NVDA, which asks you to confirm the installation and then to restart NVDA. To turn the check off or check right away: NVDA menu, Preferences, Settings, category "LINE MR". The command "Checks for a newer version of LINE MR" (no key by default) can be given a key in Input gestures.

## What works now

- Login window: email and password fields, the log-in button, the "log in automatically" checkboxes, the QR-code instructions and the title bar buttons are named.
- Icon-only buttons are named from their icons, in LINE's own language (English, Thai, Japanese, Simplified or Traditional Chinese).
- Keyboard: Tab and Shift+Tab reach every button, checkbox and link in reading order (window content first, title-bar buttons last); Space or Enter presses the focused control. LINE itself only lets Tab reach its text fields.
- NVDA follows focus as you move (LINE normally suppresses these notifications; the bridge turns them back on).
- Checkboxes report and change their state ("checked" / "not checked").
- Clickable texts such as "Log in with smartphone >" are links; after one switches the screen, focus moves to the first field of the new screen.
- Entering the login window reads its instructions (for example the QR-code instructions); NVDA+B reads them again.
- LINE's window-shadow windows and its unnamed layout groups are hidden from object navigation.
- Main window: sidebar tabs, chat header and message-bar buttons are named. The chat list reads name, member count, last message, time and unread count; the friend list reads names, member counts and group sections.
- Chat list: Up/Down, Page Up/Page Down and Home/End browse chats quickly without opening them (opening a chat makes LINE pause for up to two seconds); Enter opens the chat and moves to its message input. The chat on the right changes only when you press Enter.
- Messages: each message reads as sender (or "You"), text, time and read receipt; messages without text say what they are: photo, photo album, video, sticker, LINE emoji, voice message (with its length), file (with its name and size), location (with its address) or call (with its length or status). Entering the list starts at the newest message; when you come back with Tab or Shift+Tab in the same chat, it returns to the message you read last (Alt+M always goes to the newest). Up/Down move one message, Page Up/Page Down about a screen; Home goes to the oldest loaded message (LINE then loads older ones; press Home again to go further back), End to the newest; pressing Home or End again reads that message again. New messages from others are announced while LINE is in front.
- Tab order in the chat list panel: Sort, Show or hide chat list, Create chat, the ad, the search field, then the chat list (Tab goes on to the open chat).
- Tab order in an open chat: chat header buttons, the chat's note (if it has one), "Scroll to latest message", the message list, the message input, then the message bar buttons from the input outwards (Stickers and emoji, Screenshot options, Screenshot, Keep, Attach file).
- Context menus: the Applications key (or Shift+F10) on a message opens LINE's menu with focus on its first item; Up/Down and Home/End move, Enter or Space chooses, Escape closes it and returns to the message.
- Incoming calls: LINE MR says "Incoming call" with the caller's name, even while you work in another application. While a LINE window is in front, press Alt+A to answer or Alt+D to decline; from any other application, press NVDA+Shift+Control+A or NVDA+Shift+Control+D (Alt+A and Alt+D stay with that application). During a call, Alt+D in LINE or NVDA+Shift+Control+D from anywhere ends the call. When no LINE call is ringing, Alt+A and Alt+D do what they normally do in LINE. Focus moves to the call notification's Answer button (Tab reaches Decline; Enter or Space presses). During a call, the call window's buttons are named (End call, Mute microphone, Start video call, Mute speaker) and its texts (caller, call length) are read.
- Pages that open in their own window are read: clicking a group's name or its member count (for example "(5)") opens the member list; members open their profile; the chat menu's Photos & videos, Links and Files open a window named after the chat. Focus starts on the page's first control and its texts are read. Pages that open inside the main window get focus too.
- Other windows: Settings (More > Settings; its sections and sub-sections are radio buttons, combo boxes carry their label), profile settings, Albums and Notes, Search friends, and Create group (friends are checkboxes: Space ticks one). Polls open as a web page, read in browse mode.
- Picture messages (cards from official accounts such as banks) are pictures with the text drawn into them, so LINE gives no text to read: they are reported as "photo or sticker". Press NVDA+R on such a message to read its text with Windows OCR. When LINE could not download a card, it shows "Unable to load" with a "Try again" button, which is what you hear.

## Commands

| Command | Action |
|---|---|
| Alt+1 / Alt+2 / Alt+3 | Friends / Chats / Add friends tab |
| Alt+C | Move to the chat list (switches to the Chats tab first if needed) |
| Alt+L | Move to the chat list or friend list (whichever is shown) |
| Alt+F | Move to the search field of the chat list or friend list |
| Alt+M | Move to the messages of the open chat |
| Alt+I | Move to the message input of the open chat |
| NVDA+Alt+1 … NVDA+Alt+9 | Read a recent message (1 = newest) |
| Alt+A in LINE, NVDA+Shift+Control+A anywhere | Answer an incoming LINE call |
| Alt+D in LINE, NVDA+Shift+Control+D anywhere | Decline an incoming LINE call; during a call, end it |
| NVDA+Shift+Control+L | Report whether the LINE MR bridge is running (and offer to install it); press twice to copy a report for the developer to the clipboard |

Commands can be changed under NVDA menu, Preferences, Input gestures, category "LINE MR".

### Keys in LINE

| Where | Keys |
|---|---|
| Everywhere | Tab / Shift+Tab move between controls; Space or Enter presses buttons, checkboxes and links |
| Chat list | Up/Down, Page Up/Page Down, Home/End browse chats without opening them; Enter opens the chat and moves to its message input |
| Messages | Up/Down: previous / next message; Page Up/Page Down: about a screen; Home: oldest loaded message (again: load older ones); End: newest message; Applications key or Shift+F10: the message's menu; Enter or Space: show the whole text of a long message (read it line by line like in Notepad; Escape goes back), play or pause a voice message, open a photo or video in LINE's viewer (Tab moves through its buttons and thumbnails; Escape closes it and returns to the chat), open a file (PDF, Word, ...) in its program, open a link (the first web address in the message) in your web browser |
| Message menu | Up/Down, Home/End move; Enter or Space chooses; Escape closes and returns to the message |
| Calls | Enter or Space on Answer / Decline; Alt+A answers and Alt+D declines (in LINE; NVDA+Shift+Control+A / D from other applications); Alt+D in LINE ends the current call |

## If LINE MR does not work

Press NVDA+Shift+Control+L in LINE. LINE MR says what is wrong and, where it can, offers to fix it:

- **The bridge is not installed / an older bridge is installed / LINE must be restarted:** choose Yes; LINE closes and starts again with the bridge. Closing LINE's window does not quit LINE (it keeps running in the notification area), so a LINE started before the bridge was installed keeps running without it until it is restarted; LINE MR asks again when you return to LINE (after No: 10 minutes later, or at once with NVDA+Shift+Control+L).
- **LINE is installed for all users** (in Program Files): Windows asks for administrator permission to add the bridge to LINE's folder. Allow it.
- **LINE started without the LINE MR bridge:** after installing, LINE MR checks that LINE really loaded the bridge. If it did not, Windows or antivirus software blocked it. When Windows 11 **Smart App Control** is on, it blocks the bridge (the file is not signed) and LINE MR cannot work; it can be turned off in Windows Security, App & browser control (Windows cannot turn it on again without a reinstall). Antivirus software may also delete or block `line_a11y.dll`: allow the file, then press NVDA+Shift+Control+L in LINE again.
- **This version of LINE is not supported:** LINE is too old for the bridge. Update LINE.
- **Messages are read as "Photo or sticker" or not at all, but buttons work:** LINE closed unexpectedly twice in a row, so the bridge stopped reading message contents to keep LINE stable. Quit LINE normally (from its menu or the notification area) and start it again.

To report a problem, press NVDA+Shift+Control+L **twice** in LINE. LINE MR copies a short report to the clipboard (versions of NVDA, Windows, LINE and Qt, where LINE is installed, the bridge's state and its start-up log, without chat contents and with your user folder replaced by `%USERPROFILE%`). Paste it into your message or GitHub issue.

## How the bridge works

The bridge is `line_a11y.dll`, a Qt platform-theme plugin placed in `%LOCALAPPDATA%\LINE\bin\current\platformthemes`, with a copy for LINE's call program (`LineCall.exe`, which shows incoming calls and the call window) in `%LOCALAPPDATA%\LINE\Data\plugin\LineCall\<version>\platformthemes`, and one for LINE's photo and video viewer (`LineMediaPlayer.exe`) in `%LOCALAPPDATA%\LINE\Data\plugin\LineMediaPlayer\<version>\platformthemes`. LINE's Qt loads it at start-up; it returns no theme (so LINE looks exactly the same) and installs accessibility interfaces for LINE's custom widgets. It reads text by recording what each widget paints, so it does not depend on LINE's internal memory layout. No LINE files are modified; to remove it, delete those DLLs.

Logs: `%LOCALAPPDATA%\LineMR\logs\bridge.log`.

LINE MR is an independent project and is not affiliated with or endorsed by LY Corporation / LINE.
