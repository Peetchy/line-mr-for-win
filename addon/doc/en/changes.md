# What's new in LINE MR

## Version 0.5.6

### Settings
- NVDA's settings, category LINE MR, show whether the bridge is running and have a button to disconnect it (use LINE as it is, without LINE MR) or to connect it again. LINE restarts either way.

### Messages
- In the messages, Alt+R replies to the selected message and moves to the message input, Delete deletes it and Shift+Delete unsends it (the items of LINE's message menu; LINE's own confirmation is left to you). Elsewhere these keys work as usual; they can be changed in NVDA's Input gestures (contributed by wengweng0312).
- Choosing Reply in a message's menu moves to the message input (contributed by wengweng0312).
- While replying, the bar above the message input is reachable: Shift+Tab from the message input goes to Cancel reply and Go to the original message. Cancel reply returns to the message input.
- New option in NVDA's settings, category LINE MR: "Show the sender's name in one-to-one chats" reads the other person's name before their messages (off by default). Your own long messages that LINE draws without a time are read as yours (contributed by wengweng0312).
- Enter in the chat list stays in the opened chat's message input: LINE sometimes moved focus into the messages once the chat had loaded. In an open chat, Tab always goes messages, message input, then the buttons below it.
- @ mentions in group chats: after typing @, Up/Down read the members LINE suggests, typing filters them and Enter chooses one. Before, the arrows did nothing there (contributed by wengweng0312). In the message input a chosen member reads "@name" (before, NVDA read nothing for it).
- Messages read LINE's own text (LINE 26.6 and later): the sender and time on every message, whole replies ("X replied to Y's message ...") and full file names, also for messages not on screen. LINE MR adds what LINE leaves out: "You" before your own messages, stickers and calls, and reactions (contributed by wengweng0312).

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
