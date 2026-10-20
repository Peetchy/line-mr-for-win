# LINE MR: answers or declines an incoming LINE call from any application.
#
# LINE shows the incoming-call notification (a Qt Quick window, CallMessageToastWindow) without
# activating it, so keyboard users cannot reach it while they work in another application. The
# bridge writes "Incoming call: <caller>, ..." to %LOCALAPPDATA%\LineMR\incoming-call.txt and
# signals the event Local\lineMR.incomingCall, which this plugin waits for and speaks (NVDA drops
# UI Automation notifications from background windows). The bridge also tags the notification's
# buttons ("lineMR_call-answer", "lineMR_call-decline"); the commands below press them.

import ctypes
import os
import threading
from ctypes import wintypes

import addonHandler
import appModuleHandler
import globalPluginHandler
import queueHandler
import ui
import UIAHandler
from logHandler import log
from NVDAObjects.UIA import UIA
from scriptHandler import script
from speech.priorities import Spri

addonHandler.initTranslation()

#: Same category as the app module's commands.
SCRIPT_CATEGORY = "LINE MR"

#: LINE windows that never hold the incoming-call notification (large trees: not searched).
SKIPPED_WINDOW_CLASSES = ("AllInOneWindow", "ChatWindow", "shadow_")

# Private instance: argtypes must not be set on ctypes.windll.user32, which NVDA shares.
_user32 = ctypes.WinDLL("user32")
_EnumWindowsProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
_user32.EnumWindows.argtypes = (_EnumWindowsProc, wintypes.LPARAM)
_user32.EnumWindows.restype = wintypes.BOOL
_user32.IsWindowVisible.argtypes = (wintypes.HWND,)
_user32.IsWindowVisible.restype = wintypes.BOOL
_user32.GetWindowThreadProcessId.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.DWORD))
_user32.GetWindowThreadProcessId.restype = wintypes.DWORD
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


def _speakIncomingCall():
	try:
		with open(CALL_TEXT_FILE, encoding="utf-8") as f:
			text = f.read().strip()
	except OSError:
		log.debugWarning("LINE MR: cannot read %s", CALL_TEXT_FILE, exc_info=True)
		return
	if not text:
		return
	# Translators: Spoken after "Incoming call: <caller>"; names the add-on's answer and decline commands.
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


def _lineWindows():
	"""Visible top-level windows of LINE.exe."""
	names = {}
	found = []

	def visit(hwnd, _lParam):
		if _user32.IsWindowVisible(hwnd):
			pid = wintypes.DWORD()
			_user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
			if pid.value not in names:
				try:
					names[pid.value] = appModuleHandler.getAppNameFromProcessID(pid.value)
				except Exception:
					names[pid.value] = ""
			if names[pid.value] == "line":
				found.append(hwnd)
		return True

	_user32.EnumWindows(_EnumWindowsProc(visit), 0)
	return found


def _findCallButton(tag):
	"""The tagged button of LINE's incoming-call notification, as an NVDA object, or None."""
	handler = UIAHandler.handler
	if handler is None:
		return None
	for hwnd in _lineWindows():
		try:
			window = handler.clientObject.ElementFromHandle(hwnd)
			# The notification is CallMessageToastWindow (a Qt Quick type: "..._QMLTYPE_<n>").
			if (window.CurrentClassName or "").startswith(SKIPPED_WINDOW_CLASSES):
				continue
			items = window.FindAllBuildCache(
				UIAHandler.UIA.TreeScope_Descendants,
				handler.clientObject.CreateTrueCondition(),
				handler.baseCacheRequest,
			)
		except Exception:
			log.debugWarning("LINE MR: reading a LINE window failed", exc_info=True)
			continue
		for i in range(items.Length):
			element = items.GetElement(i)
			try:
				automationId = element.CurrentAutomationId or ""
			except Exception:
				continue
			# Qt may prefix the ids of named parents ("parent.lineMR_call-answer").
			if automationId == tag or automationId.endswith("." + tag):
				return UIA(UIAElement=element)
	return None


class GlobalPlugin(globalPluginHandler.GlobalPlugin):
	def __init__(self):
		super().__init__()
		self._listener = _CallListener()
		self._listener.start()

	def terminate(self):
		self._listener.stop()
		super().terminate()

	def _press(self, tag, done):
		button = _findCallButton(tag)
		if button is None:
			# Translators: Reported by the answer / decline commands when LINE shows no incoming call.
			ui.message(_("No incoming LINE call."))
			return
		try:
			button.doAction()
		except Exception:
			log.error("LINE MR: pressing %s failed", tag, exc_info=True)
			return
		ui.message(done)

	@script(
		# Translators: Describes the command that answers an incoming LINE call.
		description=_("Answers an incoming LINE call (from any application)"),
		category=SCRIPT_CATEGORY,
		gesture="kb:NVDA+shift+control+a",
	)
	def script_answerCall(self, gesture):
		# Translators: Reported after answering a LINE call.
		self._press("lineMR_call-answer", _("Answering"))

	@script(
		# Translators: Describes the command that declines an incoming LINE call.
		description=_("Declines an incoming LINE call (from any application)"),
		category=SCRIPT_CATEGORY,
		gesture="kb:NVDA+shift+control+d",
	)
	def script_declineCall(self, gesture):
		# Translators: Reported after declining a LINE call.
		self._press("lineMR_call-decline", _("Call declined"))
