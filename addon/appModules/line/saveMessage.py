# Save message (Ctrl+S in the message list; remappable). The script sends the bridge its private key
# Control+Alt+Shift+F8 (messageActions.py); the bridge looks at the message (saveMessage() in
# accessible_list.cpp) and:
#   - photos, videos, files, voice messages: chooses "Save as..." in LINE's own message menu, and
#     LINE asks where to save (status "media");
#   - text messages: chooses LINE's Copy (so a long message is whole), takes the text from the
#     clipboard and puts the clipboard back; locations and contacts: the text as read (status
#     "text"): this module asks where to save it as a .txt file;
#   - stickers, calls, dates, notices: "unsupported"; no Save as / Copy in the menu (an expired
#     file): "nosave".
# The same channel says when Alt+R / Alt+Shift+R / Delete / Shift+Delete / Alt+T find no such item
# in LINE's menu for the message (status "unavailable", kind = the command).
# It writes %LOCALAPPDATA%\LineMR\save-request.txt + save-text.txt and signals
# Local\lineMR.saveRequest.

import ctypes
import os
import re
import threading
from ctypes import wintypes

import addonHandler
import gui
import queueHandler
import ui
import wx
from logHandler import log

from . import bridge

addonHandler.initTranslation()

REQUEST_EVENT = r"Local\lineMR.saveRequest"

_kernel32 = ctypes.WinDLL("kernel32")
_kernel32.CreateEventW.argtypes = (ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR)
_kernel32.CreateEventW.restype = wintypes.HANDLE
_kernel32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
_kernel32.WaitForSingleObject.restype = wintypes.DWORD
_kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
_kernel32.CloseHandle.restype = wintypes.BOOL


def _readRequest():
	values = {}
	directory = bridge.bridgeDataDir()
	try:
		with open(os.path.join(directory, "save-request.txt"), encoding="utf-8") as f:
			for line in f:
				key, _sep, value = line.rstrip("\r\n").partition("\t")
				values[key] = value
		if values.get("status") == "text":
			with open(os.path.join(directory, "save-text.txt"), encoding="utf-8") as f:
				values["text"] = f.read()
	except OSError:
		log.debugWarning("LINE MR: cannot read the save request", exc_info=True)
	return values


def kindName(kind):
	"""The bridge's message kind, as said to the user."""
	names = {
		# Translators: A kind of LINE message that cannot be saved.
		"Sticker": _("stickers"),
		# Translators: A kind of LINE message that cannot be saved.
		"Call message": _("calls"),
		# Translators: A kind of LINE message that cannot be saved.
		"Date": _("dates"),
		# Translators: A kind of LINE message that cannot be saved (e.g. "X left the group").
		"Notice": _("notices"),
	}
	return names.get(kind)


def _fileName(name):
	"""A file name from the bridge's suggestion ("<chat> 2026-11-07 1530"): no characters Windows
	refuses."""
	name = re.sub(r'[\\/:*?"<>|\x00-\x1f]+', " ", name or "LINE").strip(" .") or "LINE"
	return name[:120] + ".txt"


def _sayUnavailable(command):
	"""Alt+R, Alt+Shift+R, Delete, Shift+Delete, Alt+T on a message LINE offers no such item for (Reply
	and reactions in Keep Memo, Unsend on someone else's message, dates)."""
	messages = {
		# Translators: Reported when Alt+R is pressed on a message that cannot be replied to (for example in Keep Memo).
		"Reply": _("This message cannot be replied to."),
		# Translators: Reported when Alt+Shift+R is pressed on a message that takes no reactions (Keep Memo, dates, notices, calls).
		"React": _("This message takes no reactions."),
		# Translators: Reported when Delete is pressed on a message LINE cannot delete.
		"Delete": _("This message cannot be deleted."),
		# Translators: Reported when Shift+Delete is pressed on a message that cannot be unsent (someone else's, or too old).
		"Unsend": _("This message cannot be unsent."),
		# Translators: Reported when Alt+T is pressed on a message LINE cannot translate.
		"Translate": _("This message cannot be translated."),
	}
	# Translators: Reported when a message command is not offered by LINE for the selected message.
	ui.message(messages.get(command) or _("Not available for this message."))


def _onRequest():
	"""On NVDA's main thread: the bridge looked at the message."""
	request = _readRequest()
	status = request.get("status")
	if status == "text" and request.get("text"):
		# Not from here: a modal dialog run inside NVDA's queue processing blocks NVDA's core, and its
		# watchdog takes NVDA as frozen (it restarted NVDA). wx runs it from its own event loop.
		wx.CallAfter(_saveText, request["text"], request.get("name"))
	elif status == "unsupported":
		kind = kindName(request.get("kind"))
		if kind:
			# Translators: Reported when Ctrl+S is pressed on a message that cannot be saved ({kind}: stickers, calls, ...).
			ui.message(_("Saving {kind} is not supported.").format(kind=kind))
		else:
			# Translators: Reported when Ctrl+S is pressed on a message that cannot be saved.
			ui.message(_("Saving this kind of message is not supported."))
	elif status == "nosave":
		# Translators: Reported when LINE offers no way to save a message (for example an expired file).
		ui.message(_("LINE offers no way to save this message (it may have expired)."))
	elif status == "unavailable":
		_sayUnavailable(request.get("kind"))
	elif status == "busy":
		# Translators: Reported when the message could not be looked at (LINE was busy); pressing again usually works.
		ui.message(_("LINE is busy. Press Ctrl+S again."))
	# "media": LINE's own Save as dialog opens.


def _saveText(text, name):
	gui.mainFrame.prePopup()
	try:
		with wx.FileDialog(
			gui.mainFrame,
			# Translators: Title of the dialog that saves a LINE message as a text file.
			_("Save message as text file"),
			defaultFile=_fileName(name),
			# Translators: File type in the dialog that saves a LINE message as a text file.
			wildcard=_("Text files (*.txt)") + "|*.txt",
			style=wx.FD_SAVE | wx.FD_OVERWRITE_PROMPT,
		) as dialog:
			if dialog.ShowModal() != wx.ID_OK:
				return
			path = dialog.GetPath()
	finally:
		gui.mainFrame.postPopup()
	if not path.lower().endswith(".txt"):
		path += ".txt"
	try:
		# With a byte order mark: older programs then read Thai and other scripts right too.
		with open(path, "w", encoding="utf-8-sig", newline="\r\n") as f:
			f.write(text)
	except OSError as e:
		log.warning("LINE MR: cannot save the message to %s", path, exc_info=True)
		# Translators: Reported when a message could not be written to the chosen file ({error} = the reason).
		ui.message(_("The message could not be saved: {error}").format(error=e.strerror or e))
		return
	# Translators: Reported after a message was saved as a text file ({name} = the file name).
	ui.message(_("Saved as {name}").format(name=os.path.basename(path)))


class _RequestListener(threading.Thread):
	def __init__(self):
		super().__init__(name="lineMR.saveListener", daemon=True)
		self._stopping = False
		self._event = _kernel32.CreateEventW(None, False, False, REQUEST_EVENT)

	def run(self):
		if not self._event:
			log.error("LINE MR: cannot create the save request event")
			return
		while not self._stopping:
			if _kernel32.WaitForSingleObject(self._event, 1000) == 0 and not self._stopping:
				queueHandler.queueFunction(queueHandler.eventQueue, _onRequest)
		_kernel32.CloseHandle(self._event)

	def stop(self):
		self._stopping = True


_listener = None


def startListener():
	global _listener
	if _listener is None:
		_listener = _RequestListener()
		_listener.start()


def stopListener():
	global _listener
	if _listener:
		_listener.stop()
		_listener = None
