# Native message-action shortcuts — experimental reference

This independent NVDA add-on explores using LINE's native accessibility with three remappable commands:

| Default gesture | Command |
|---|---|
| Alt+R | Reply |
| Delete | Delete the message from this device |
| Shift+Delete | Unsend |

The gesture category is **LINE 原生訊息快捷鍵**. The example UI and messages are primarily Traditional Chinese. Exact menu-label candidates for other languages are included but have not received live testing.

This directory is reference source, not wired into the main add-on or release build. Its independent add-on ID is `lineNativeActions`, with experimental version `0.1.0`; LINE MR's version is unchanged. The design uses NVDA's script/input-gesture facilities and native UI Automation objects; it does not include LINE MR's in-process bridge.

## Scope

- Only commands from a confirmed native message-list context, or a recognized native message popup, are accepted. Editors and other applications receive the original key.
- It finds LINE's native `LcContextMenu`, selects a unique exact enabled action, and invokes it once. A popup already open must have Reply and Copy/Share entries so that a chat-list Delete menu is not mistaken for a message menu.
- Reply waits for held modifiers to be released before opening the menu. If Invoke returns but the original Reply button is still focused in the same popup, one bounded completion check may send Enter. Changed focus, another element, or a later dialog does not receive Enter.
- Deletion/unsend confirmations remain native and are left to the user. There is no confirmation automation.
- No DLL installation/repair, paint capture, message reformatting, focus-on-window-open behavior, or Enter/Tab/Shift+Tab navigation override is included. LINE itself may naturally move focus after Reply.
- Alt+C and Alt+I are proposed conveniences, not implemented by this example.

## Test status and limitations

The observer verified that LINE's native tree exposes messages, editor and the Reply/Delete/Unsend popup without `line_a11y.dll` loaded. The user reported Unsend working. The latest Reply completion change has not yet been live-validated, and Delete has not been end-to-end tested on a real message. Native Qt class names, UIA ancestry and action behavior may vary by LINE version.

Run `python tests/test_actions.py` for 13 offline test methods. They cover scope checks, editor pass-through, exact/unique/enabled matching, one-shot actions, stale request cancellation, foreground changes, missing items, timeout, absent UIA cached properties, modifier release, and guarded Reply completion. They mock NVDA/UIA; they neither modify real messages nor establish real-UI correctness.

Run `python build.py` to create `dist/lineNativeActions-0.1.0-test.nvda-addon`. No Qt SDK is needed. For an isolated experiment, keep LINE MR disabled and its bridge unloaded; otherwise results are not a native-only comparison. This example does not change those settings for you.

The source is provided for discussion and adaptation rather than merging as a production replacement. See [the proposal](../../proposals/native-accessibility-shortcuts.zh_TW.md). GPL v2; see `addon/COPYING.txt`.
