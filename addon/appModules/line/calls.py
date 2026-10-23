# LINE MR: answering, declining and ending LINE calls.
#
# LINE's call process (LineCall.exe) shows the incoming-call notification (a Qt Quick window,
# CallMessageToastWindow) and the call windows. The bridge tags their buttons ("lineMR_call-answer",
# "lineMR_call-decline", "lineMR_call-end"); the functions below press them through UI Automation.
# Alt+A / Alt+D are commands of LINE's app modules (LINE.exe, LineCall.exe, LineMediaPlayer.exe:
# CallCommandsAppModule); the global plugin (globalPlugins/lineMR.py) offers NVDA+Shift+Control+A / D
# from any application.

import ctypes
from ctypes import wintypes

import addonHandler
import api
import appModuleHandler
import ui
import UIAHandler
from logHandler import log
from NVDAObjects.UIA import UIA
from scriptHandler import script

addonHandler.initTranslation()

#: Same category as the app module's other commands.
SCRIPT_CATEGORY = "LINE MR"

#: The processes of LINE for Windows (NVDA app module names).
LINE_APPS = ("line", "linecall", "linemediaplayer")

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


def _visibleWindows(appNames, first=False):
	"""Visible top-level windows of the given NVDA app names (only the first one if `first`)."""
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
			if names[pid.value] in appNames:
				found.append(hwnd)
				if first:
					return False
		return True

	_user32.EnumWindows(_EnumWindowsProc(visit), 0)
	return found


def callWindowShown():
	"""Does LineCall.exe show a window (an incoming call, a call, its notifications)? Cheap: no UIA."""
	return bool(_visibleWindows(("linecall",), first=True))


def lineInFront():
	"""Is a window of LINE (or its call process or photo viewer) the foreground window?"""
	fg = api.getForegroundObject()
	return bool(fg and fg.appModule and fg.appModule.appName in LINE_APPS)


def _findCallButton(tag):
	"""The tagged button of LINE's call notification or call window, as an NVDA object, or None."""
	handler = UIAHandler.handler
	if handler is None:
		return None
	for hwnd in _visibleWindows(("line", "linecall")):
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


def _pressButton(tag, done):
	"""Presses the tagged call button; False if LINE shows none."""
	button = _findCallButton(tag)
	if button is None:
		return False
	try:
		button.doAction()
	except Exception:
		log.error("LINE MR: pressing %s failed", tag, exc_info=True)
		return True
	ui.message(done)
	return True


def answer():
	"""Answers a ringing LINE call; False if none rings."""
	# Translators: Reported after answering a LINE call.
	return _pressButton("lineMR_call-answer", _("Answering"))


def declineOrEnd(endCall):
	"""Declines a ringing LINE call or, if `endCall`, ends the current one; False if there is none."""
	# Translators: Reported after declining a LINE call.
	if _pressButton("lineMR_call-decline", _("Call declined")):
		return True
	# Translators: Reported after ending (hanging up) a LINE call.
	return endCall and _pressButton("lineMR_call-end", _("Call ended"))


class CallCommandsAppModule(appModuleHandler.AppModule):
	"""Alt+A / Alt+D in LINE's processes. Without a LINE call the keys go to LINE."""

	@script(
		# Translators: Describes the command (in LINE) that answers an incoming LINE call.
		description=_("Answers an incoming LINE call"),
		category=SCRIPT_CATEGORY,
		gesture="kb:alt+a",
	)
	def script_answerCallInLine(self, gesture):
		if not callWindowShown() or not answer():
			gesture.send()

	@script(
		# Translators: Describes the command (in LINE) that declines an incoming LINE call or ends the current one.
		description=_("Declines an incoming LINE call, or ends the current call"),
		category=SCRIPT_CATEGORY,
		gesture="kb:alt+d",
	)
	def script_declineCallInLine(self, gesture):
		if not callWindowShown() or not declineOrEnd(endCall=True):
			gesture.send()
