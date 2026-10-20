# Finds LINE controls by the stable tags the bridge puts into their UIA AutomationId
# ("lineMR_gnb-chat", "lineMR_list_messages", ...), independent of LINE's UI language.

import api
import UIAHandler
from logHandler import log
from NVDAObjects.UIA import UIA

TAG_PREFIX = "lineMR_"


def _lineWindowElement():
	"""UIA element of LINE's foreground (main or chat) window, or None."""
	fg = api.getForegroundObject()
	if isinstance(fg, UIA) and fg.appModule and fg.appModule.appName == "line":
		return fg.UIAElement
	return None


def find(tag, root=None):
	"""The first visible control tagged `tag` in LINE's foreground window, as an NVDA object."""
	rootElement = root.UIAElement if root is not None else _lineWindowElement()
	if rootElement is None:
		return None
	handler = UIAHandler.handler
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
