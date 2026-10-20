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
- Icon-only buttons are named from their icons, in LINE's own language (Thai or English).
- Keyboard: Tab and Shift+Tab reach every button, checkbox and link in reading order (window content first, title-bar buttons last); Space or Enter presses the focused control. LINE itself only lets Tab reach its text fields.
- NVDA follows focus as you move (LINE normally suppresses these notifications; the bridge turns them back on).
- Checkboxes report and change their state ("checked" / "not checked").
- Clickable texts such as "Log in with smartphone >" are links; after one switches the screen, focus moves to the first field of the new screen.
- Entering the login window reads its instructions (for example the QR-code instructions); NVDA+B reads them again.
- LINE's window-shadow windows and its unnamed layout groups are hidden from object navigation.
- Main window: sidebar tabs, chat header and message-bar buttons are named. The chat list reads name, member count, last message, time and unread count; the friend list reads names, member counts and group sections.
- Chat list: Up/Down, Page Up/Page Down and Home/End browse chats quickly without opening them (opening a chat makes LINE pause for up to two seconds); Enter opens the chat and moves to its message input. The chat on the right changes only when you press Enter.
- Messages: each message reads as sender (or "You"), text, time and read receipt; messages without text say what they are: photo, photo album, video, sticker, LINE emoji, file (with its name and size) or call (with its length or status); the list starts at the newest message. Home goes to the oldest loaded message (LINE then loads older ones; press Home again to go further back), End to the newest. New messages from others are announced while LINE is in front.
- Context menus: the Applications key (or Shift+F10) on a message opens LINE's menu with focus on its first item; Up/Down and Home/End move, Enter or Space chooses, Escape closes it and returns to the message.
- Incoming calls: LINE MR says "Incoming call" with the caller's name, even while you work in another application. Press NVDA+Shift+Control+A to answer or NVDA+Shift+Control+D to decline, from any application. When LINE is in front, focus moves to the call notification's Answer button (Tab reaches Decline).
- Pages that open in their own window are read: clicking a group's name or its member count (for example "(5)") opens the member list; members open their profile; the chat menu's Photos & videos, Links and Files open a window named after the chat. Focus starts on the page's first control and its texts are read. Pages that open inside the main window get focus too.
- Other windows: Settings (More > Settings; its sections and sub-sections are radio buttons, combo boxes carry their label), profile settings, Albums and Notes, Search friends, and Create group (friends are checkboxes: Space ticks one). Polls open as a web page, read in browse mode.
- Picture messages (cards from official accounts such as banks) are pictures with the text drawn into them, so LINE gives no text to read: they are reported as "photo or sticker". Press NVDA+R on such a message to read its text with Windows OCR. When LINE could not download a card, it shows "Unable to load" with a "Try again" button, which is what you hear.

## Commands

| Command | Action |
|---|---|
| Alt+1 / Alt+2 / Alt+3 | Friends / Chats / Add friends tab |
| Alt+L | Move to the chat list or friend list |
| Alt+M | Move to the messages of the open chat |
| Alt+I | Move to the message input of the open chat |
| NVDA+Alt+1 … NVDA+Alt+9 | Read a recent message (1 = newest) |
| NVDA+Shift+Control+A | Answer an incoming LINE call (works in any application) |
| NVDA+Shift+Control+D | Decline an incoming LINE call (works in any application) |
| NVDA+Shift+Control+L | Report whether the LINE MR bridge is running (and offer to install it) |

Commands can be changed under NVDA menu, Preferences, Input gestures, category "LINE MR".

## How the bridge works

The bridge is `line_a11y.dll`, a Qt platform-theme plugin placed in `%LOCALAPPDATA%\LINE\bin\current\platformthemes`. LINE's Qt loads it at start-up; it returns no theme (so LINE looks exactly the same) and installs accessibility interfaces for LINE's custom widgets. It reads text by recording what each widget paints, so it does not depend on LINE's internal memory layout. No LINE files are modified; to remove it, delete that DLL.

Logs: `%LOCALAPPDATA%\LineMR\logs\bridge.log`.

LINE MR is an independent project and is not affiliated with or endorsed by LY Corporation / LINE.
