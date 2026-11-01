# Lines in LINE's text fields (the message input, other multi-line fields).
#
# Qt's UI Automation text ranges know only line breaks as lines, so in a text area that wraps a long
# paragraph each Down arrow read the whole paragraph again. The bridge reports where the lines it
# draws start (UIA HelpText "lineMR-lines:...", LineAccessibleInput): such fields get the full-text
# view's text, whose lines are those lines (fullText.FullTextView).
#
# Other fields keep Qt's ranges, with one fix:
# Qt 6.6's UI Automation text ranges expand a position to its line by searching backwards for the
# first character that is not a line break. On an empty line (the caret between two line breaks, or
# after a line break at the end of the text) that search lands on the line above, so NVDA read the
# previous line again instead of "blank". This overlay gives such a position an empty line: the line
# break that ends it, or nothing at the end of the text.

import UIAHandler
import controlTypes
import textInfos
from NVDAObjects.UIA import UIA, UIATextInfo

from . import fullText

_LINE_BREAKS = ("\n", "\r", " ", " ")


def emptyLineRange(textRange, document, uia):
	"""For a collapsed UIA range on an empty line: a range covering that line (its line break, or the
	collapsed range at the end of the text). None when the line is not empty.

	`uia` holds the UIA constants (UIAHandler in NVDA; comtypes' UIAutomationClient in tests).
	"""
	start = uia.TextPatternRangeEndpoint_Start
	end = uia.TextPatternRangeEndpoint_End
	before = textRange.Clone()
	if before.MoveEndpointByUnit(start, uia.TextUnit_Character, -1) and before.GetText(1) not in _LINE_BREAKS:
		return None
	atEnd = textRange.CompareEndpoints(start, document, end) >= 0
	if atEnd:
		# A line break at the end of the text: the caret is on the empty last line.
		return textRange.Clone() if before.CompareEndpoints(start, textRange, start) != 0 else None
	line = textRange.Clone()
	line.MoveEndpointByUnit(end, uia.TextUnit_Character, 1)
	return line if line.GetText(1) in _LINE_BREAKS else None


class QtTextInfo(UIATextInfo):
	def expand(self, unit):
		if unit == textInfos.UNIT_LINE and self.isCollapsed:
			try:
				line = emptyLineRange(self._rangeObj, self.obj.UIATextPattern.DocumentRange, UIAHandler)
			except Exception:
				line = None
			if line is not None:
				self._rangeObj = line
				return
		super().expand(unit)


class QtEditableText(UIA):
	_TextInfo = QtTextInfo


def chooseOverlayClasses(obj, clsList):
	if obj.role != controlTypes.Role.EDITABLETEXT or fullText.FullTextView in clsList:
		return
	if fullText.hasLineData(obj):
		clsList.insert(0, fullText.FullTextView)
	else:
		clsList.insert(0, QtEditableText)
