"""Checks LINE's login window through UI Automation (run with LINE showing its login window
and the bridge installed: tools\\install-bridge.ps1 -Restart).

Verifies that the bridge names the email / password fields, the log-in and title-bar buttons,
exposes the checkboxes with a toggle state and reads the QR-code instructions. Expected names
follow LINE's UI language (Thai or English).

Usage: python tests/verify_uia_tree.py
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))

import comtypes  # noqa: E402
import uia_inspect  # noqa: E402
from uia_inspect import UIA  # noqa: E402

THAI = {
	"email": "อีเมล",
	"password": "รหัสผ่าน",
	"login": "เข้าสู่ระบบ",
	"autoLogin": "เข้าสู่ระบบอัตโนมัติ",
	"close": "ปิด",
	"minimize": "ย่อหน้าต่าง",
	"qr": "คิวอาร์โค้ด",
}
ENGLISH = {
	"email": "Email address",
	"password": "Password",
	"login": "Log in",
	"autoLogin": "Log in automatically",
	"close": "Close",
	"minimize": "Minimize",
	"qr": "QR code",
}


def collect(uia, el, out):
	walker = uia.RawViewWalker
	stack = [el]
	while stack:
		e = stack.pop()
		try:
			out.append((uia_inspect.CONTROL_TYPES.get(e.CurrentControlType, "?"), e.CurrentName or "",
			            e.CurrentClassName or "", e))
			child = walker.GetFirstChildElement(e)
		except comtypes.COMError:
			continue
		while child:
			stack.append(child)
			try:
				child = walker.GetNextSiblingElement(child)
			except comtypes.COMError:
				break


def main():
	sys.stdout.reconfigure(encoding="utf-8")
	pids = uia_inspect.find_line_pids()
	if not pids:
		sys.exit("LINE.exe is not running")
	uia = comtypes.client.CreateObject(UIA.CUIAutomation, interface=UIA.IUIAutomation)
	elements = []
	import ctypes
	for pid in pids:
		for hwnd in uia_inspect.top_windows(pid):
			if ctypes.windll.user32.IsWindowVisible(hwnd):
				collect(uia, uia.ElementFromHandle(hwnd), elements)
	if not any(cls == "LoginWindow" for _t, _n, cls, _e in elements):
		sys.exit("LINE's login window is not showing (these checks cover the login window)")

	names = THAI if any(n == THAI["login"] for _t, n, _c, _e in elements) else ENGLISH
	failures = []

	def expect(ctype, key, cls=None, toggle=False):
		hits = [e for t, n, c, e in elements if t == ctype and n == names[key] and (cls is None or c == cls)]
		label = f"{ctype} {names[key]!r}"
		if not hits:
			failures.append(f"missing {label}")
			return
		if toggle and not hits[0].GetCurrentPattern(UIA.UIA_TogglePatternId):
			failures.append(f"{label} has no Toggle pattern")
		print(f"ok   {label}")

	expect("Edit", "email", "LcTextField")
	expect("Edit", "password", "PasswordTextField")
	expect("Button", "login", "LcButton")
	expect("Button", "close", "LcButton")
	expect("Button", "minimize", "LcButton")
	expect("CheckBox", "autoLogin", "LcCheckBox", toggle=True)
	expect("Text", "qr", "LcText")
	instructions = [n for t, n, c, _e in elements if t == "Text" and c == "LcText" and len(n) > 30]
	if instructions:
		print(f"ok   QR-code instructions: {instructions[0][:40]}…")
	else:
		failures.append("QR-code instructions not readable")
	unreachable = [n for t, n, c, e in elements
	               if t in ("Button", "CheckBox", "Edit", "Hyperlink") and c.startswith(("Lc", "Password"))
	               and e.CurrentIsEnabled and not e.CurrentIsKeyboardFocusable]
	if unreachable:
		failures.append(f"not reachable with Tab: {', '.join(unreachable)}")
	else:
		print("ok   every enabled LINE control is reachable with Tab")
	if any(t == "Hyperlink" and c == "LcText" for t, _n, c, _e in elements):
		print("ok   clickable LINE text is exposed as a link")
	else:
		failures.append("no clickable LcText exposed as a link")
	unnamed = [c for t, n, c, _e in elements if t in ("Button", "CheckBox", "Edit") and not n]
	if unnamed:
		failures.append(f"unnamed controls: {', '.join(unnamed)}")
	else:
		print("ok   every button, checkbox and edit has a name")

	for f in failures:
		print(f"FAIL {f}")
	print("PASS" if not failures else f"{len(failures)} failure(s)")
	sys.exit(1 if failures else 0)


if __name__ == "__main__":
	main()
