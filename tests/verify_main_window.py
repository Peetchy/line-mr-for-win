"""Checks LINE's main window through UI Automation (run while logged in, bridge installed).

Verifies the bridge's tags (the add-on's commands depend on them), named sidebar tabs, and
that the visible rows of the shown list (chats or friends) and of an open chat have text.

Usage: python tests/verify_main_window.py
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))

import comtypes.client  # noqa: E402
import uia_inspect  # noqa: E402
from uia_inspect import UIA  # noqa: E402


def main():
	sys.stdout.reconfigure(encoding="utf-8")
	pids = uia_inspect.find_line_pids()
	if not pids:
		sys.exit("LINE.exe is not running")
	uia = comtypes.client.CreateObject(UIA.CUIAutomation, interface=UIA.IUIAutomation)
	window = None
	for pid in pids:
		for hwnd in uia_inspect.top_windows(pid):
			el = uia.ElementFromHandle(hwnd)
			if el.CurrentClassName == "AllInOneWindow":
				window = el
	if window is None:
		sys.exit("LINE's main window is not showing (log in first)")
	failures = []

	def tagged(tag):
		return window.FindFirst(UIA.TreeScope_Descendants,
		                        uia.CreatePropertyCondition(UIA.UIA_AutomationIdPropertyId, "lineMR_" + tag))

	for tag in ("gnb-friends", "gnb-chat", "gnb-add-friend"):
		el = tagged(tag)
		if not el:
			failures.append(f"sidebar tab {tag} not tagged")
		elif not el.CurrentName:
			failures.append(f"sidebar tab {tag} has no name")
		else:
			print(f"ok   tab {tag}: {el.CurrentName}")

	lists = [(tag, tagged(tag)) for tag in ("list_chats", "list_friends")]
	shown = [(tag, el) for tag, el in lists if el]
	if not shown:
		failures.append("neither the chat list nor the friend list is tagged")
	for tag, el in shown + [("list_messages", tagged("list_messages"))]:
		if not el:
			print(f"--   {tag}: not shown")
			continue
		rows = el.FindAll(UIA.TreeScope_Children, uia.CreateTrueCondition())
		visible = [rows.GetElement(i) for i in range(rows.Length)]
		visible = [r for r in visible if not r.CurrentIsOffscreen]
		named = [r for r in visible if r.CurrentName]
		if not visible:
			failures.append(f"{tag}: no visible rows")
		elif len(named) * 2 < len(visible):
			failures.append(f"{tag}: only {len(named)} of {len(visible)} visible rows have text")
		else:
			print(f"ok   {tag}: {len(named)}/{len(visible)} visible rows have text, e.g. {named[0].CurrentName[:50]!r}")
		if not el.CurrentIsKeyboardFocusable:
			failures.append(f"{tag}: not keyboard focusable")

	unnamed = []
	buttons = window.FindAll(UIA.TreeScope_Descendants,
	                         uia.CreatePropertyCondition(UIA.UIA_ControlTypePropertyId, UIA.UIA_ButtonControlTypeId))
	for i in range(buttons.Length):
		b = buttons.GetElement(i)
		if not b.CurrentIsOffscreen and not b.CurrentName and b.CurrentClassName.startswith("Lc"):
			unnamed.append(b.CurrentClassName)
	if unnamed:
		print(f"note {len(unnamed)} visible LINE button(s) without a name")
	else:
		print("ok   every visible LINE button has a name")

	for f in failures:
		print(f"FAIL {f}")
	print("PASS" if not failures else f"{len(failures)} failure(s)")
	sys.exit(1 if failures else 0)


if __name__ == "__main__":
	main()
