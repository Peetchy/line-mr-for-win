# Finds LINE controls by the stable tags the bridge puts into their UIA AutomationId
# ("lineMR_gnb-chat", "lineMR_list_messages", ...), independent of LINE's UI language.

import ctypes
import ctypes.wintypes

import api
import UIAHandler
from logHandler import log
from NVDAObjects.UIA import UIA

TAG_PREFIX = "lineMR_"

#: UIA class names of LINE's main window and of a chat opened in its own window.
MAIN_WINDOW = "AllInOneWindow"
CHAT_WINDOW = "ChatWindow"

# Private instance: argtypes set here must not affect NVDA's shared ctypes.windll.user32.
_user32 = ctypes.WinDLL("user32")
_user32.GetForegroundWindow.restype = ctypes.wintypes.HWND
_user32.SetForegroundWindow.argtypes = (ctypes.wintypes.HWND,)
_user32.IsIconic.argtypes = (ctypes.wintypes.HWND,)
_user32.ShowWindow.argtypes = (ctypes.wintypes.HWND, ctypes.c_int)
_SW_RESTORE = 9


def _lineWindowElement():
	"""UIA element of LINE's foreground (main or chat) window, or None."""
	fg = api.getForegroundObject()
	if isinstance(fg, UIA) and fg.appModule and fg.appModule.appName == "line":
		return fg.UIAElement
	return None


def foregroundClass():
	"""UIA class name of LINE's foreground window ("AllInOneWindow", "ChatWindow", ...), or ""."""
	element = _lineWindowElement()
	try:
		return element.cachedClassName if element is not None else ""
	except Exception:
		return ""


def windows(processID, className):
	"""Handles of LINE's shown top-level windows of a UIA class, front to back."""
	handler = UIAHandler.handler
	client = handler.clientObject
	condition = client.CreateAndCondition(
		client.CreatePropertyCondition(UIAHandler.UIA_ProcessIdPropertyId, processID),
		client.CreatePropertyCondition(UIAHandler.UIA_ClassNamePropertyId, className),
	)
	try:
		found = client.GetRootElement().FindAll(UIAHandler.UIA.TreeScope_Children, condition)
		return [found.GetElement(i).CurrentNativeWindowHandle for i in range(found.Length)]
	except Exception:
		log.debugWarning("LINE MR: listing %s windows failed", className, exc_info=True)
		return []


def isForeground(hwnd):
	return _user32.GetForegroundWindow() == hwnd


def bringToFront(hwnd):
	"""Restores and activates a LINE window (NVDA may: it handles the key the user just pressed)."""
	if _user32.IsIconic(hwnd):
		_user32.ShowWindow(hwnd, _SW_RESTORE)
	_user32.SetForegroundWindow(hwnd)


def find(tag, root=None, window=None):
	"""The first visible control tagged `tag` in LINE's foreground window (or the window with handle
	`window`), as an NVDA object."""
	handler = UIAHandler.handler
	if root is not None:
		rootElement = root.UIAElement
	elif window is not None:
		try:
			rootElement = handler.clientObject.ElementFromHandle(window)
		except Exception:
			log.debugWarning("LINE MR: no UIA element for window %s", window, exc_info=True)
			return None
	else:
		rootElement = _lineWindowElement()
	if rootElement is None:
		return None
	condition = handler.clientObject.CreatePropertyCondition(
		UIAHandler.UIA_AutomationIdPropertyId,
		TAG_PREFIX + tag,
	)
	try:
		element = rootElement.FindFirstBuildCache(
			UIAHandler.UIA.TreeScope_Descendants,
			condition,
			handler.baseCacheRequest,
		)
	except Exception:
		log.debugWarning("LINE MR: UIA search for %s failed", tag, exc_info=True)
		return None
	if not element:
		return None
	return UIA(UIAElement=element)


def children(obj):
	"""Direct UIA children of `obj` (one search, cached), e.g. the rows of a list."""
	handler = UIAHandler.handler
	try:
		found = obj.UIAElement.FindAllBuildCache(
			UIAHandler.UIA.TreeScope_Children,
			handler.clientObject.CreateTrueCondition(),
			handler.baseCacheRequest,
		)
	except Exception:
		log.debugWarning("LINE MR: listing children failed", exc_info=True)
		return []
	return [UIA(UIAElement=found.GetElement(i)) for i in range(found.Length)]
