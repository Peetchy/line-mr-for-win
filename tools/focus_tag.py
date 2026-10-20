"""Moves keyboard focus to the LINE control tagged lineMR_<tag> (UIA SetFocus), e.g.
`python tools/focus_tag.py list_chats`. Safer in tests than typing Alt+letter shortcuts, which
end up as text if focus is in a text field."""

import sys

sys.path.insert(0, __import__("os").path.dirname(__file__))
import comtypes.client  # noqa: E402
import uia_inspect  # noqa: E402
from uia_inspect import UIA  # noqa: E402


def main():
	sys.stdout.reconfigure(encoding="utf-8")
	uia = comtypes.client.CreateObject(UIA.CUIAutomation, interface=UIA.IUIAutomation)
	cond = uia.CreatePropertyCondition(UIA.UIA_AutomationIdPropertyId, "lineMR_" + sys.argv[1])
	for pid in uia_inspect.find_line_pids():
		for hwnd in uia_inspect.top_windows(pid):
			el = uia.ElementFromHandle(hwnd).FindFirst(UIA.TreeScope_Descendants, cond)
			if el:
				el.SetFocus()
				print("focused", sys.argv[1])
				return
	sys.exit(f"no control tagged {sys.argv[1]}")


if __name__ == "__main__":
	main()
