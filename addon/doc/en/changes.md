# What's new in LINE MR

## Version 0.6.1

- New: download from YouTube links in chats. Alt+Shift+Y on a message with a YouTube link (or "Download video from YouTube" in its menu) checks the link and offers the video (MP4) or its sound (MP3), the quality and, for a playlist link, the whole playlist. The download module (yt-dlp, Deno, FFmpeg; about 130 MB) is downloaded the first time and updated by itself, separately from LINE MR. See "Downloading from YouTube" in the guide.
- Buttons LINE MR names itself now use LINE's own words in LINE's language where LINE has them: "Collapse chats" / "Expand chats" (was "Show or hide chat room") and "Create chat" in the chat list.
- Shift+Tab from the message input back to the messages no longer jumps to the newest message a moment later; arrows, Alt+R, Alt+T and the other message keys act on the message read last (contributed by wengweng0312, #23).

## Version 0.6.0

### Keys
- New: Alt+Shift+R in the messages opens the reactions of the selected message ("React" in its menu); if you have reacted already, it opens them to take yours back.
- Letter and digit commands (Alt+C, Alt+M, Alt+1 ...) also work while the keyboard is in Thai or another non-Latin layout: the key at the same place as on an English keyboard counts, so there is no need to switch to English first.
- Alt+T in the messages now translates the selected message (Translate in LINE's message menu); voice message transcription moved to Alt+Shift+T.
- Alt+C, Alt+L, Alt+F and Alt+1 / Alt+2 / Alt+3 also work in a chat opened in its own window: LINE's main window comes to the front and focus goes there. Alt+M and Alt+I in the main window with the chat room hidden go to the chat window.

### Voice messages to text
- Videos can be turned into text too: Alt+Shift+T on a video in a chat (its menu: "Transcribe video"), in Photos & videos, in Notes or in LINE's viewer. LINE has to save the video into LINE MR's folder; the file is deleted afterwards. A voice message or video longer than 5 minutes asks first, since converting runs on this computer and may take long.
- Transcription always uses the file of the selected message: LINE saves it into LINE MR's folder (%LOCALAPPDATA%\LineMR\media; a Save As window flashes by) and the file is deleted once the text is ready; nothing is played. LINE then offers "Open folder" instead of "Save as..." for that message; once the file is deleted, LINE MR presses "Open folder" in the background (nothing opens), so the menu has "Save as..." again. Before, LINE MR could take another message of the same length from LINE's cache and show its text instead. A voice message LINE cannot save (expired) is played and recorded as before.
- The text of a voice message or video appears while it is converted (every 30 seconds of sound) and can be read and edited meanwhile; new lines are added at the end without moving the cursor, and your changes are kept.
- Recordings longer than 30 seconds are cut at their pauses before converting, so words are no longer repeated or lost every 30 seconds. Copy appears when the text is complete. The list of speech models comes from LINE MR's repository on GitHub: new models appear in the settings and the dialog without updating the add-on.
- New: Alt+Shift+T on a voice message (or "Transcribe voice message" in its menu) turns it into text on this computer; the text opens in a window where it can be read, edited and copied. Off by default: turning it on in NVDA's settings, category LINE MR, downloads the speech model chosen there once. Six models: Typhoon Whisper Turbo (874 MB, the default: best for Thai, also English and Japanese, the least memory of the Typhoon models), Typhoon Whisper Medium and Large v3 (Thai only; Large v3 the most accurate, but very slow), and OpenAI's Whisper Small, Medium and Large v3 Turbo (many languages); or "Browse for a model file" for a whisper.cpp model of your own, which LINE MR checks before using it. The dialog before each transcription asks for the model and the language. See the guide, "Voice messages to text".

### Saving messages
- New: Ctrl+S in the messages saves the selected message. Photos, videos, files and voice messages: LINE's own Save as (LINE asks where). Text messages (whole, also long ones), locations and contacts: a text file (.txt), LINE MR asks where. Stickers, calls, dates and notices: LINE MR says they cannot be saved. Elsewhere Ctrl+S works as usual; it can be changed in NVDA's Input gestures.

### Photos & videos
- Several photos and videos can be saved or shared at once (#9): in a chat's Photos & videos window (chat menu), Space ticks the item under the cursor and NVDA says whether it is checked; Tab reaches LINE's bar under the grid (the number selected, Share, Send to Keep Memo, Download, Cancel). Download asks for a folder; Cancel unticks everything and returns to the grid. Enter still opens the item. An album sent in a chat has its photos there too: LINE itself saves several photos only from this window.

### Chat list
- With the chat room hidden (Show or hide chat room), Enter or Space opens the chat in its own window at once and moves to its message input. Before, the first press only selected the chat and a second press was needed. Space now works like Enter in the chat list. "Show or hide chat room" says correctly whether the chat room is shown when no chat has been opened yet.
- New: Shift+Enter in the chat list opens the chat in its own window (as "Open in new window" in the chat's menu) and moves to its message input.
- The unread count reads "3 unread messages" (was "3 unread"), still at the end of the chat's row.
- Groups and OpenChats say so after the chat's name: "DHCB, Group, 37 members, ...", "English talk, OpenChat, 3051 members, ..." (#5). Official accounts cannot be told apart from friends: LINE gives no sign of them.

### Settings
- NVDA's settings, category LINE MR, show whether the bridge is running and have a button to disconnect it (use LINE as it is, without LINE MR) or to connect it again. LINE restarts either way.
- Removing LINE MR in NVDA's Add-on Store now also removes the bridge from LINE, its call program and its photo viewer, and LINE MR's folder `%LOCALAPPDATA%\LineMR`. A running LINE keeps the bridge until it quits.

### Messages
- Long messages and some received messages lost "React" in their menu: they were taken for notices (#3).
- Opening a chat with Enter keeps LINE's place in it (for example the first unread message) instead of jumping to the newest; Shift+Tab from the message input back to the messages stays on screen (the message read last if visible, else the first one on screen). Contributed by wengweng0312 (#20).
- In the messages, Alt+R replies to the selected message and moves to the message input, Delete deletes it and Shift+Delete unsends it (the items of LINE's message menu; LINE's own confirmation is left to you). Elsewhere these keys work as usual; they can be changed in NVDA's Input gestures (contributed by wengweng0312).
- Choosing Reply in a message's menu moves to the message input (contributed by wengweng0312).
- While replying, the bar above the message input is reachable: Shift+Tab from the message input goes to Cancel reply and Go to the original message. Cancel reply returns to the message input.
- New option in NVDA's settings, category LINE MR: "Show the sender's name in one-to-one chats" reads the other person's name before their messages (off by default). Your own long messages that LINE draws without a time are read as yours (contributed by wengweng0312).
- Enter in the chat list stays in the opened chat's message input: LINE sometimes moved focus into the messages once the chat had loaded. In an open chat, Tab always goes messages, message input, then the buttons below it.
- @ mentions in group chats: after typing @, Up/Down read the members LINE suggests, typing filters them and Enter chooses one. Before, the arrows did nothing there (contributed by wengweng0312). In the message input a chosen member reads "@name" (before, NVDA read nothing for it).
- Messages read LINE's own text (LINE 26.6 and later): the sender and time on every message, whole replies ("X replied to Y's message ...") and full file names, also for messages not on screen. LINE MR adds what LINE leaves out: "You" before your own messages, stickers and calls, and reactions (contributed by wengweng0312). Lengths the bridge reads (calls, and voice messages and videos without LINE's text) are said in words: "7 seconds", "10 minutes 54 seconds" instead of "00:07", "10:54".
- Messages also say what LINE's own text leaves out: a file's size and download deadline, a link preview's title, "Location" before an address, "Contact" and the contact's name, "Photo album" instead of "Photo". Replies start with the reply: "X, Reply to Y: <reply>, Original message: <quoted text>". In Keep Memo every message reads "You", and a Japanese or Chinese LINE's times and read receipts are recognised, so your own messages read "You" there too.
- New option in NVDA's settings, category LINE MR: "Read the sender's name only when the sender changes" (off by default): consecutive messages from the same person start with their text.

## Version 0.5.5

### Updates
- Installed from the NVDA Add-on Store, LINE MR leaves updates to the Store: it no longer checks GitHub, and its settings say so.

### Focus highlight
- For low-vision users: a yellow frame with a black outline shows which control has the focus in LINE (also chats, messages and menu items). Its colour and width can be changed, or it can be turned off, in NVDA's settings, category LINE MR.

### Fixes
- With NVDA 2025.4 or older (32-bit NVDA), LINE MR asked again and again to restart LINE although it was working. It now sees that LINE runs with LINE MR.
- In the message input and other text fields, Up/Down read the line shown on screen: a long paragraph that wraps is read line by line instead of whole again at every line, and an empty line reads "blank" instead of the line above it.
- Up/Down in the message input no longer move focus away (into LINE's sticker and emoji suggestions above it) while you read a text line by line.
- Up on the oldest loaded message makes LINE load older messages, and Up goes on into them (contributed by wengweng0312).
- Settings check boxes changed with the keyboard (for example Notifications > Show message contents) are now saved (contributed by wengweng0312).
- Voice messages that LINE draws in several pieces are recognised, so Enter and Space play them (contributed by wengweng0312).
- Traditional Chinese LINE: read receipts (已讀) and times (下午 2:49) are read at the end of a message, and a received message that says "已讀" is not taken for a receipt (contributed by Keyang556).

## Version 0.5.4

### Long messages
- In the full text of a long message ("See more"), NVDA says "line feed" only where the writer started a new line, like in Word. Up/Down still move by the lines LINE shows.

### Messages
- Some messages with a link (for example a YouTube link) were read as "Call". Only LINE's call messages read "Call" now; video calls, which read "Photo or sticker", read "Call" too.

### Chat list
- The chat that is open reads "Selected" before its name, and pinned chats read "Pinned" after the name.

### Create chat (Chat, Group chat, OpenChat)
- The friend picker opens with focus on its search field. Up/Down, Page Up/Page Down and Home/End move through all friends, and Space ticks one ("checked"). Tab leaves the list for the chosen friends ("Remove <name>") and Next. Shift+Tab goes back to the friend you read last.
- The group profile page has named buttons: Change profile picture, Change background picture and Edit group name. "Members join automatically" is a checkbox that Space turns on or off.
- Create OpenChat: the keywords field and the category list have names. The X in a field reads "Clear text". After Next, focus goes to "Your nickname in this OpenChat", which also reads what the page is for.
- Create text profile (Change profile picture > Create text profile): the colours are checkboxes with a number and a colour name ("Color 4: Light pink"), and Space chooses one. Text color and Background color pick what the colours change. The font list is named "Font".

### Notes
- Posts in Notes (groups and OpenChat) are read with their author, photos or videos, numbers of reactions and comments, and time. The arrow keys move from post to post; Enter plays the post's video in LINE's video player (focus on Play / Pause), and Escape returns to the post.
- The Applications key on a post opens a menu: Open, Save as..., Copy text and More options (LINE's menu). Ctrl+S in LINE's viewer saves the photo or video.

### Settings
- This "What's new" button in NVDA's settings, LINE MR category.

## Version 0.5.3

### Reactions
- The message menu (Applications key) has "React". Left/Right move between Awesome, Like, Funny, Jealous and Sad, Enter gives the reaction, and Escape closes the menu. On a message you reacted to, the item is "Remove reaction".
- Messages say which reactions they have, for example "Reaction: Like" or "Like 3 people".

### Stickers and emoji
- Sticker packages are read by name instead of "Package 1".
- The first Enter selects a sticker or emoji and plays LINE's preview. The second Enter sends it.
- Enter on a sound sticker in a chat plays its sound again.

### Chats
- In groups, the name and member count are one item. The message menu has "Show <name>'s profile".
- Chat search: Enter searches, Up/Down go to older / newer matches, and LINE MR reads the result.
- The Sort menu reads which option is chosen.

### Photos & videos, links, files
- The arrow keys move through the photos, links and files of a chat, and Enter opens them. The Applications key offers Open, Save, Copy link and Copy text.
- In the video player, Space plays or pauses. The arrows seek 5 seconds and Page Up/Page Down 30 seconds.

### Calls
- NVDA keeps its volume while LINE lowers other sounds during a call.
- The video call window reads whether cameras and microphones are on, and Alt+D ends a video call.

## Version 0.5.2
- Enter on a message with a link opens the link in your web browser.
- The Applications key on a voice message, file or photo opens that message's own menu.
- Tab back into the messages returns to the message you read last.
- Reading long messages line by line is fast.
- The field for a friend's display name (Profile > Edit) can be reached.
- Alt+A / Alt+D work while LINE or its call window is in front. From any application, use NVDA+Shift+Control+A / D.

## Version 0.5.1
- Enter or Space opens what a message holds: voice messages and videos play, photos open in LINE's viewer, files open in their program.
- LINE MR also works in LINE's photo and video viewer.
- The chat folder tabs (All, Friends, Groups, OpenChat, Official accounts) filter the list.
- The Applications key in the chat list opens the menu of the chat NVDA just read.
- Installing works on more computers, and LINE MR says why the bridge did not load. NVDA+Shift+Control+L pressed twice copies a report for the developer.

## Version 0.5.0
- Long messages ("See more") read like Notepad, line by line.
- Alt+F moves to the search field. Alt+A / Alt+D answer or decline a call.
- The friend list can be browsed with the arrows without opening chats.
- The Applications key on a text message opens the message's menu (Reply, Copy, Unsend, Delete, ...).
- The message input says whom you write to. Share: chats and friends are checkboxes.
- Japanese, Simplified and Traditional Chinese.

## Version 0.4.5
- Long messages are read in full.
- New Tab order in the chat list panel.
- Fewer messages wrongly read as "Video".

## Version 0.4.4
- Videos, files, locations and stickers are recognised by LINE's own icons.

## Version 0.4.3
- Incoming calls are announced and can be answered.
- Voice messages read as "Voice message" with their length.
- Better Tab order in an open chat. Alt+C moves to the chat list.
