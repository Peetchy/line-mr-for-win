# Reply / React / Delete / Unsend / Translate / Save on the selected message (Alt+R, Alt+Shift+R,
# Delete, Shift+Delete, Alt+T, Ctrl+S; remappable).
#
# The commands are NVDA scripts, so they can be changed in Input gestures. In the message list
# they send the bridge a private key (Control+Alt+Shift+F9 / F6 / F10 / F11 / F7), and the bridge opens the
# message's menu and chooses LINE's own item (openMessageCommand() in accessible_list.cpp); it
# never answers a confirmation LINE may show. Anywhere else the key goes to LINE unchanged.

import time

import api
import keyboardHandler
from logHandler import log
from NVDAObjects.UIA import UIA

from . import elements

_COMMAND_KEYS = {
	"reply": "control+alt+shift+f9",
	# The bridge's own "React" / "Remove reaction" item of the message menu.
	"react": "control+alt+shift+f6",
	"delete": "control+alt+shift+f10",
	"unsend": "control+alt+shift+f11",
	"translate": "control+alt+shift+f7",
	# Save message (saveMessage.py; the bridge decides how by the kind of message).
	"save": "control+alt+shift+f8",
	# Transcribe voice message (transcribe.py; the bridge checks that it is a voice message).
	"transcribe": "control+alt+shift+f12",
}

# A held key repeats: the command runs once.
_REPEAT_SECONDS = 0.5


def _messageList(obj):
	"""The message list `obj` is in (the list itself or one of its rows), or None."""
	for _depth in range(8):
		if obj is None or not isinstance(obj, UIA):
			return None
		if (obj.UIAAutomationId or "").endswith(elements.TAG_PREFIX + "list_messages"):
			return obj
		obj = obj.parent
	return None


def inMessageList(app):
	"""True when the focus is in LINE's message list."""
	focus = api.getFocusObject()
	return focus.processID == app.processID and _messageList(focus) is not None


#: Windows whose lists hold videos the bridge can open for transcription: Photos & videos (also
#: Links / Files), Notes.
_MEDIA_WINDOWS = ("ContentViewerWindow", "SquarePostListWindow")


def inMediaList(app):
	"""True when the focus is in a list of the Photos & videos or Notes window."""
	focus = api.getFocusObject()
	if focus.processID != app.processID:
		return False
	fg = api.getForegroundObject()
	try:
		window = fg.UIAElement.cachedClassName if isinstance(fg, UIA) else ""
	except Exception:
		window = ""
	for obj in (focus, focus.parent):
		if window in _MEDIA_WINDOWS and isinstance(obj, UIA) and obj.UIAElement.cachedClassName.endswith("ListView"):
			return True
	return False


def run(app, action, gesture, mediaLists=False):
	"""Sends the bridge the command's private key while focus is in the message list (with
	`mediaLists`, also in Photos & videos / Notes); elsewhere the key goes on unchanged."""
	try:
		focus = api.getFocusObject()
		if focus.processID != app.processID or (_messageList(focus) is None and not (mediaLists and inMediaList(app))):
			gesture.send()
			return
		now = time.monotonic()
		last = getattr(app, "_messageActionLast", None)
		app._messageActionLast = (action, now)
		if last and last[0] == action and now - last[1] < _REPEAT_SECONDS:
			return
		keyboardHandler.KeyboardInputGesture.fromName(_COMMAND_KEYS[action]).send()
		log.info("LINE MR: message command %s sent to the bridge", action)
	except Exception:
		log.debugWarning("LINE MR: message command %s failed", action, exc_info=True)
