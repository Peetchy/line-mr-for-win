# The text view the bridge lays over LINE's full-text window ("See more" under a long message).
#
# The view holds the message as its author wrote it: a line break only where the author started a
# new line. LINE wraps the text to its window; the bridge reports where each of those lines starts
# in the view's UIA HelpText ("lineMR-lines:<generation>:<offsets>") and moves the caret by them.
# Qt's UI Automation text ranges know only line breaks as lines, so this overlay gives NVDA a text
# whose lines are LINE's lines (like a word processor) and whose paragraphs are the author's: the
# end of a line LINE wrapped is not read as a line feed.

import bisect

import textUtils
import UIAHandler
from logHandler import log
from NVDAObjects.UIA import UIA
from textInfos.offsets import OffsetsTextInfo

from . import elements

AUTOMATION_ID = elements.TAG_PREFIX + "fulltext"
_HELP_PREFIX = "lineMR-lines:"


def _utf16Length(text):
	return len(text.encode("utf-16-le")) // 2


def _automationId(obj):
	try:
		return obj.UIAAutomationId or ""
	except Exception:
		return ""


def chooseOverlayClasses(obj, clsList):
	if _automationId(obj) == AUTOMATION_ID:
		clsList.insert(0, FullTextView)


class FullTextInfo(OffsetsTextInfo):
	"""The view's text; offsets are UTF-16 positions, as in Qt."""

	encoding = textUtils.WCHAR_ENCODING

	def _getStoryText(self):
		return self.obj._storyText()

	def _getStoryLength(self):
		return _utf16Length(self._getStoryText())

	def _getCaretOffset(self):
		return self.obj._caretOffsets()[1]

	def _setCaretOffset(self, offset):
		self.obj._select(offset, offset)

	def _getSelectionOffsets(self):
		return self.obj._caretOffsets()

	def _setSelectionOffsets(self, start, end):
		self.obj._select(start, end)

	def _getLineOffsets(self, offset):
		starts = self.obj._lineStarts()
		length = self._getStoryLength()
		if not starts:
			return self._getParagraphOffsets(offset)
		line = max(0, bisect.bisect_right(starts, offset) - 1)
		end = starts[line + 1] if line + 1 < len(starts) else length
		return starts[line], max(end, starts[line])

	def _getParagraphOffsets(self, offset):
		# The author's paragraphs: between line breaks.
		text = self._getStoryText()
		breaks = self.obj._lineBreaks()
		index = bisect.bisect_left(breaks, offset)
		start = breaks[index - 1] + 1 if index > 0 else 0
		end = breaks[index] + 1 if index < len(breaks) else _utf16Length(text)
		return start, end


class FullTextView(UIA):
	"""The bridge's text view over LINE's full-text window."""

	TextInfo = FullTextInfo

	def _get_description(self):
		# HelpText holds the bridge's line offsets, not a description.
		return self.UIAFullDescription or ""

	def _lineData(self):
		"""(generation, line starts) from the bridge, or (None, [])."""
		try:
			help = self.UIAElement.CurrentHelpText or ""
		except Exception:
			help = ""
		if not help.startswith(_HELP_PREFIX):
			return None, []
		try:
			generation, starts = help[len(_HELP_PREFIX):].split(":", 1)
			return generation, [int(s) for s in starts.split(",") if s]
		except ValueError:
			log.debugWarning("LINE MR: unexpected line data %r", help[:80])
			return None, []

	def _lineStarts(self):
		return self._lineData()[1]

	def _storyText(self):
		generation = self._lineData()[0]
		cached = getattr(self, "_lineMRText", None)
		if cached is not None and generation is not None and cached[0] == generation:
			return cached[1]
		try:
			text = self.UIATextPattern.DocumentRange.GetText(-1) or ""
		except Exception:
			log.debugWarning("LINE MR: could not read the full text", exc_info=True)
			text = ""
		breaks = []
		position = 0
		for char in text:
			if char == "\n":
				breaks.append(position)
			position += 2 if ord(char) > 0xFFFF else 1
		self._lineMRText = (generation, text, breaks)
		return text

	def _lineBreaks(self):
		self._storyText()
		return self._lineMRText[2]

	def _caretOffsets(self):
		"""(start, end) of the selection, the caret at `end` (Qt reports the caret as an empty selection)."""
		try:
			pattern = self.UIATextPattern
			selection = pattern.GetSelection()
			if not selection or selection.Length == 0:
				return 0, 0
			selected = selection.GetElement(0)
			before = pattern.DocumentRange.Clone()
			before.MoveEndpointByRange(
				UIAHandler.TextPatternRangeEndpoint_End, selected, UIAHandler.TextPatternRangeEndpoint_Start
			)
			start = _utf16Length(before.GetText(-1) or "")
			end = start + _utf16Length(selected.GetText(-1) or "")
			return start, end
		except Exception:
			log.debugWarning("LINE MR: could not read the caret", exc_info=True)
			return 0, 0

	def _select(self, start, end):
		pattern = self.UIATextPattern
		textRange = pattern.DocumentRange.Clone()
		textRange.MoveEndpointByRange(
			UIAHandler.TextPatternRangeEndpoint_End, textRange, UIAHandler.TextPatternRangeEndpoint_Start
		)
		textRange.MoveEndpointByUnit(UIAHandler.TextPatternRangeEndpoint_End, UIAHandler.TextUnit_Character, end)
		textRange.MoveEndpointByUnit(UIAHandler.TextPatternRangeEndpoint_Start, UIAHandler.TextUnit_Character, start)
		textRange.Select()
