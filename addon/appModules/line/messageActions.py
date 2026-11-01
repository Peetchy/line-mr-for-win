"""Remappable NVDA scripts delegate message commands to the in-process bridge."""

import api
import keyboardHandler
import time
from logHandler import log
from NVDAObjects.UIA import UIA
from . import elements

_COMMAND_KEYS = {
    "reply": "control+alt+shift+f9",
    "delete": "control+alt+shift+f10",
    "unsend": "control+alt+shift+f11",
}


def _messageList(obj):
    for _depth in range(8):
        if obj is None or not isinstance(obj, UIA):
            return None
        if (obj.UIAAutomationId or "").endswith(elements.TAG_PREFIX + "list_messages"):
            return obj
        obj = obj.parent
    return None


def run(app, action, gesture):
    try:
        focus = api.getFocusObject()
        if focus.processID != app.processID or _messageList(focus) is None:
            gesture.send()
            return
        now = time.monotonic()
        last = getattr(app, "_messageActionLast", None)
        app._messageActionLast = (action, now)
        if last and last[0] == action and now - last[1] < 0.5:
            return
        # The bridge resolves its own popup menu, activates an exact item once,
        # and leaves all confirmation dialogs untouched.
        keyboardHandler.KeyboardInputGesture.fromName(_COMMAND_KEYS[action]).send()
        log.info("LINE MR: dispatched message command %s to bridge", action)
    except Exception:
        log.debugWarning("LINE MR: dispatching message command %s failed", action, exc_info=True)
