# Proposal: prioritize LINE's native accessibility while retaining convenience shortcuts

Thank you for your continued work on LINE accessibility.

After disabling LINE MR and the bridge copies used by LINE and its call/media processes, then fully restarting LINE, I found its native accessibility better than expected. The native interface exposed the chat list, message text, message editor, and time, read-receipt and quoted-reply information for some messages.

This does not establish that native accessibility is complete. I would like to suggest a development direction: use native accessibility wherever it is adequate, concentrate the add-on on user-requested conveniences, and retain bridge support where native functionality remains insufficient.

## Features worth retaining

1. Keep convenient shortcuts such as Alt+C to focus the chat list and Alt+I to focus the message editor. Explicit, user-triggered focus commands remain useful in daily use.
2. In my current environment, native Ctrl+1 and Ctrl+2 switch to the Friends and Chats tabs. However, pressing the shortcut again when already on that tab does not always return focus to its list. Alt+C therefore remains useful even with native tab-switching shortcuts. Other versions and computers have not been verified.
3. Answering, rejecting and ending calls have not yet been tested in a native-only environment. I am not proposing removal of existing call assistance or claiming that native accessibility replaces it.
4. Keep shortcuts for Reply, Delete from this device and Unsend. The attached small, bridge-free add-on demonstrates one possible way to operate the native message menu.

## Reference implementation

Source is in [examples/native-message-actions](../examples/native-message-actions/README.md). This independent experimental add-on provides commands that can be remapped in NVDA's Input Gestures dialog:

- Alt+R: reply to the current message.
- Delete: delete the current message from this device.
- Shift+Delete: unsend the current message.

It does not install or repair DLLs, capture drawing operations, override Enter/Tab/Shift+Tab navigation, or confirm deletion/unsend dialogs. It locates the native message menu only when the user invokes a command. Native Alt+C and Alt+I implementations are not included in this example; they remain requested conveniences in this proposal.

The user has reported Unsend working. Reply previously opened the menu but did not activate the action even though the UIA call returned. The latest example waits for modifier release before opening the menu and permits one guarded Enter only if the original Reply button remains focused in the same menu. That final Reply change has not yet received user validation. Delete has not been end-to-end tested on a real message. Thirteen offline test methods are included; mocked tests do not establish that every native action works in practice.

This is a strategy proposal with reference source, not a request to immediately replace the existing architecture. I hope we can first discuss whether native accessibility plus user-triggered shortcuts fits the project, then verify which areas still require the bridge. This PR does not change the original project's version or existing functionality.
