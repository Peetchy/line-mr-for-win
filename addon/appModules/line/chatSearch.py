# The search bar of an open chat (the magnifier in the chat's header).
#
# LINE does not filter the messages: it moves the chat to a matching message (its current row) and
# shows "41/42" with Previous / Next result buttons; Up / Down in the search field do the same, and
# Enter searches again from the newest match. None of this reaches NVDA, so after these keys the
# result number and the matching message are spoken. The list button opens all matches in a
# window of their own (ChatSearchResultWindow), where Enter goes to a match in the chat.

import re

import addonHandler
import controlTypes
import core
import ui
from logHandler import log
from NVDAObjects.UIA import UIA
from scriptHandler import script

from . import elements

addonHandler.initTranslation()

#: The bridge's tags for the search field and its arrows (UIA AutomationId).
FIELD_ID = elements.TAG_PREFIX + "search_messages"
ARROW_IDS = frozenset({elements.TAG_PREFIX + "search_previous", elements.TAG_PREFIX + "search_next"})

_COUNTER = re.compile(r"^\s*(\d+)\s*/\s*(\d+)\s*$")
#: How long LINE may take to move to the next match (it loads older messages first).
_WAIT_MS = 2000
_STEP_MS = 100


def _automationId(obj):
	try:
		return obj.UIAAutomationId or ""
	except Exception:
		return ""


def _result():
	"""(counter, message): LINE's "41/42" and the text of the message it moved to, or None for each."""
	counter = None
	field = elements.find("search_messages")
	bar = field.parent if field is not None else None
	if bar is not None:
		for obj in _descendants(bar):
			match = _COUNTER.match(obj.name or "")
			if match:
				counter = (int(match.group(1)), int(match.group(2)))
				break
	message = None
	messages = elements.find("list_messages")
	if messages is not None:
		for row in elements.children(messages):
			if controlTypes.State.SELECTED in row.states:
				message = row.name
				break
	return counter, message


def _descendants(obj, depth=3):
	for child in elements.children(obj):
		yield child
		if depth > 1:
			yield from _descendants(child, depth - 1)


def _speak(counter, message):
	if counter is None:
		# Translators: Spoken after a search in a LINE chat that found no message.
		ui.message(_("No results"))
		return
	# Translators: Spoken after moving to a search result in a LINE chat, e.g. "Result 41 of 42".
	text = _("Result {current} of {total}").format(current=counter[0], total=counter[1])
	ui.message(f"{text}, {message}" if message else text)


def reportResultAfterKey(gesture):
	"""Sends `gesture` to LINE, then speaks the result once LINE has moved to it."""
	try:
		before = _result()
	except Exception:
		log.debugWarning("LINE MR: reading the chat search result failed", exc_info=True)
		before = (None, None)
	gesture.send()

	def check(waited):
		try:
			now = _result()
		except Exception:
			log.debugWarning("LINE MR: reading the chat search result failed", exc_info=True)
			return
		# Both the number and the message move on (a message containing the word twice: only the
		# number, so this waits for the timeout).
		if now[0] is not None and now[0] != before[0] and now[1] and now[1] != before[1]:
			_speak(*now)
		elif waited >= _WAIT_MS:
			# Nothing changed: the first or last match, or no match at all.
			_speak(*now)
		else:
			core.callLater(_STEP_MS, check, waited + _STEP_MS)

	core.callLater(_STEP_MS, check, _STEP_MS)


class ChatSearchField(UIA):
	"""The search field of an open chat."""

	@script(gestures=("kb:upArrow", "kb:downArrow", "kb:enter", "kb:numpadEnter"))
	def script_searchStep(self, gesture):
		reportResultAfterKey(gesture)


class ChatSearchArrow(UIA):
	"""The Previous result / Next result buttons of a chat's search bar."""

	@script(gestures=("kb:space", "kb:enter", "kb:numpadEnter"))
	def script_searchStep(self, gesture):
		reportResultAfterKey(gesture)


def chooseOverlayClasses(obj, clsList):
	automationId = _automationId(obj)
	if automationId == FIELD_ID:
		clsList.insert(0, ChatSearchField)
	elif automationId in ARROW_IDS:
		clsList.insert(0, ChatSearchArrow)
