# LINE MR: announces an incoming LINE call in any application, and answers, declines or ends LINE
# calls from any application with NVDA+Shift+Control+A / D.
#
# LINE's call process (LineCall.exe) shows the incoming-call notification (a Qt Quick window,
# CallMessageToastWindow), and keyboard users may not reach it while they work in another application. The
# bridge writes "Incoming call: <caller>, ..." to %LOCALAPPDATA%\LineMR\incoming-call.txt and
# signals the event Local\lineMR.incomingCall, which this plugin waits for and speaks (NVDA drops
# UI Automation notifications from background windows). Alt+A / Alt+D work only while LINE is in
# front: they are commands of LINE's app modules (appModules/line/calls.py).

import ctypes
import os
import threading
from ctypes import wintypes

import addonHandler
import globalPluginHandler
import queueHandler
import ui
from logHandler import log
from scriptHandler import script
from speech.priorities import Spri

addonHandler.initTranslation()

#: Same category as the app module's commands.
SCRIPT_CATEGORY = "LINE MR"

_kernel32 = ctypes.WinDLL("kernel32")
_kernel32.CreateEventW.argtypes = (ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR)
_kernel32.CreateEventW.restype = wintypes.HANDLE
_kernel32.SetEvent.argtypes = (wintypes.HANDLE,)
_kernel32.SetEvent.restype = wintypes.BOOL
_kernel32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
_kernel32.WaitForSingleObject.restype = wintypes.DWORD
_kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
_kernel32.CloseHandle.restype = wintypes.BOOL
_WAIT_OBJECT_0 = 0

CALL_EVENT_NAME = r"Local\lineMR.incomingCall"
CALL_TEXT_FILE = os.path.join(os.environ.get("LOCALAPPDATA", ""), "LineMR", "incoming-call.txt")


def _calls():
	# Imported when needed: the add-on's appModules folder joins NVDA's appModules package at start-up.
	from appModules.line import calls

	return calls


def _speakIncomingCall():
	try:
		with open(CALL_TEXT_FILE, encoding="utf-8") as f:
			text = f.read().strip()
	except OSError:
		log.debugWarning("LINE MR: cannot read %s", CALL_TEXT_FILE, exc_info=True)
		return
	if not text:
		return
	try:
		inLine = _calls().lineInFront()
	except Exception:
		log.debugWarning("LINE MR: cannot tell whether LINE is in front", exc_info=True)
		inLine = False
	if inLine:
		# Translators: Spoken after "Incoming call: <caller>" while LINE is in front; names the add-on's answer and decline commands.
		hint = _("Alt+A answers, Alt+D declines.")
	else:
		# Translators: Spoken after "Incoming call: <caller>" while another application is in front; names the add-on's answer and decline commands.
		hint = _("NVDA+Shift+Control+A answers, NVDA+Shift+Control+D declines.")
	ui.message(f"{text}. {hint}", speechPriority=Spri.NOW)


class _CallListener(threading.Thread):
	"""Waits for the bridge's incoming-call event and speaks the call on NVDA's main thread."""

	def __init__(self):
		super().__init__(name="lineMR.callListener", daemon=True)
		self._stopping = False
		self._event = _kernel32.CreateEventW(None, False, False, CALL_EVENT_NAME)

	def run(self):
		if not self._event:
			log.error("LINE MR: cannot create the incoming-call event")
			return
		while not self._stopping:
			if _kernel32.WaitForSingleObject(self._event, 1000) == _WAIT_OBJECT_0 and not self._stopping:
				queueHandler.queueFunction(queueHandler.eventQueue, _speakIncomingCall)
		_kernel32.CloseHandle(self._event)

	def stop(self):
		self._stopping = True


def _repairHelperBridges():
	try:
		from appModules.line import bridge

		bridge.repairHelperBridges()
	except Exception:
		log.error("LINE MR: installing the bridge for LINE's helper processes failed", exc_info=True)


class GlobalPlugin(globalPluginHandler.GlobalPlugin):
	def __init__(self):
		super().__init__()
		self._listener = _CallListener()
		self._listener.start()
		# LINE downloads and updates its helper processes (LineCall.exe, LineMediaPlayer.exe) on its
		# own; give every version the bridge before a call comes in or a photo is opened.
		threading.Thread(target=_repairHelperBridges, name="lineMR.helperBridgeRepair", daemon=True).start()

	def terminate(self):
		self._listener.stop()
		super().terminate()

	@script(
		# Translators: Describes the command that answers an incoming LINE call.
		description=_("Answers an incoming LINE call (from any application)"),
		category=SCRIPT_CATEGORY,
		gesture="kb:NVDA+shift+control+a",
	)
	def script_answerCall(self, gesture):
		if not _calls().answer():
			# Translators: Reported by the answer / decline commands when LINE shows no incoming call.
			ui.message(_("No incoming LINE call."))

	@script(
		# Translators: Describes the command that declines an incoming LINE call or ends the current one.
		description=_("Declines an incoming LINE call (from any application), or ends the current call"),
		category=SCRIPT_CATEGORY,
		gesture="kb:NVDA+shift+control+d",
	)
	def script_declineCall(self, gesture):
		if not _calls().declineOrEnd(endCall=True):
			# Translators: Reported by the decline / hang-up command when LINE shows no call.
			ui.message(_("No LINE call."))
