"""Print LINE's live UI Automation tree (the view NVDA builds its objects from).

Usage:
    python tools/uia_inspect.py [--pid PID] [--depth N] [--all-windows]
                                [--invoke NAME | --toggle NAME] [--nth N]

Needs `pip install comtypes`. Runs outside NVDA, so it shows what the app itself
exposes, before any add-on overlay.
"""

import argparse
import ctypes
import sys

import comtypes
import comtypes.client

comtypes.client.GetModule("UIAutomationCore.dll")
from comtypes.gen import UIAutomationClient as UIA  # noqa: E402

CONTROL_TYPES = {
	v: k[len("UIA_"):-len("ControlTypeId")]
	for k, v in vars(UIA).items()
	if k.startswith("UIA_") and k.endswith("ControlTypeId")
}


def find_line_pids():
	import subprocess

	out = subprocess.run(
		["tasklist", "/FI", "IMAGENAME eq LINE.exe", "/FO", "CSV", "/NH"],
		capture_output=True, text=True,
	).stdout
	pids = []
	for line in out.splitlines():
		parts = [p.strip('"') for p in line.split('","')]
		if len(parts) > 1 and parts[1].isdigit():
			pids.append(int(parts[1]))
	return pids


def top_windows(pid):
	user32 = ctypes.windll.user32
	hwnds = []
	WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

	def cb(hwnd, _):
		p = ctypes.c_ulong()
		user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
		if p.value == pid:
			hwnds.append(hwnd)
		return True

	user32.EnumWindows(WNDENUMPROC(cb), 0)
	return hwnds


def describe(el):
	def safe(f, default=""):
		try:
			return f()
		except comtypes.COMError:
			return default

	ct = CONTROL_TYPES.get(safe(lambda: el.CurrentControlType, 0), "?")
	name = safe(lambda: el.CurrentName) or ""
	cls = safe(lambda: el.CurrentClassName) or ""
	aid = safe(lambda: el.CurrentAutomationId) or ""
	help_ = safe(lambda: el.CurrentHelpText) or ""
	r = safe(lambda: el.CurrentBoundingRectangle, None)
	rect = f"[{r.left},{r.top} {r.right - r.left}x{r.bottom - r.top}]" if r else ""
	offscreen = safe(lambda: el.CurrentIsOffscreen, False)
	focusable = safe(lambda: el.CurrentIsKeyboardFocusable, False)
	value = ""
	try:
		vp = el.GetCurrentPattern(UIA.UIA_ValuePatternId)
		if vp:
			value = vp.QueryInterface(UIA.IUIAutomationValuePattern).CurrentValue or ""
	except comtypes.COMError:
		pass
	patterns = []
	for pid, label in ((UIA.UIA_InvokePatternId, "invoke"), (UIA.UIA_TogglePatternId, "toggle"),
	                   (UIA.UIA_SelectionItemPatternId, "selitem"), (UIA.UIA_TextPatternId, "text")):
		try:
			pat = el.GetCurrentPattern(pid)
		except comtypes.COMError:
			pat = None
		if not pat:
			continue
		if pid == UIA.UIA_TogglePatternId:
			state = pat.QueryInterface(UIA.IUIAutomationTogglePattern).CurrentToggleState
			label += "=" + ("off", "on", "mixed")[state]
		elif pid == UIA.UIA_SelectionItemPatternId:
			if pat.QueryInterface(UIA.IUIAutomationSelectionItemPattern).CurrentIsSelected:
				label += "=selected"
		patterns.append(label)
	parts = [ct]
	if name:
		parts.append(f"name={name!r}")
	if value:
		parts.append(f"value={value[:80]!r}")
	if help_:
		parts.append(f"help={help_!r}")
	if cls:
		parts.append(f"class={cls}")
	if aid:
		parts.append(f"aid={aid}")
	if focusable:
		parts.append("focusable")
	if offscreen:
		parts.append("offscreen")
	if patterns:
		parts.append("<" + ",".join(patterns) + ">")
	parts.append(rect)
	return " ".join(parts), ct, name


def walk(uia, walker, el, depth, max_depth, out, matches, invoke_name):
	text, ct, name = describe(el)
	out.append("  " * depth + text)
	if invoke_name and name == invoke_name:
		matches.append(el)
	if depth >= max_depth:
		return
	try:
		child = walker.GetFirstChildElement(el)
	except comtypes.COMError:
		return
	while child:
		walk(uia, walker, child, depth + 1, max_depth, out, matches, invoke_name)
		try:
			child = walker.GetNextSiblingElement(child)
		except comtypes.COMError:
			break


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("--pid", type=int)
	ap.add_argument("--depth", type=int, default=40)
	ap.add_argument("--all-windows", action="store_true", help="include hidden top-level windows")
	ap.add_argument("--invoke", help="invoke the element with this exact name")
	ap.add_argument("--toggle", help="toggle the element with this exact name")
	ap.add_argument("--nth", type=int, default=0)
	args = ap.parse_args()

	sys.stdout.reconfigure(encoding="utf-8")
	pids = [args.pid] if args.pid else find_line_pids()
	if not pids:
		sys.exit("LINE.exe is not running")
	uia = comtypes.client.CreateObject(UIA.CUIAutomation, interface=UIA.IUIAutomation)
	walker = uia.RawViewWalker
	user32 = ctypes.windll.user32
	matches = []
	for pid in pids:
		for hwnd in top_windows(pid):
			if not args.all_windows and not user32.IsWindowVisible(hwnd):
				continue
			try:
				el = uia.ElementFromHandle(hwnd)
			except comtypes.COMError:
				continue
			out = []
			walk(uia, walker, el, 0, args.depth, out, matches, args.invoke or args.toggle)
			print(f"=== pid {pid} hwnd {hwnd:#x}")
			print("\n".join(out))
	target = args.invoke or args.toggle
	if target:
		if len(matches) <= args.nth:
			sys.exit(f"no element named {target!r} (found {len(matches)})")
		el = matches[args.nth]
		if args.toggle:
			pat = el.GetCurrentPattern(UIA.UIA_TogglePatternId)
			if not pat:
				sys.exit("element has no Toggle pattern")
			pat.QueryInterface(UIA.IUIAutomationTogglePattern).Toggle()
			print(f"toggled {target!r}")
		else:
			pat = el.GetCurrentPattern(UIA.UIA_InvokePatternId)
			if not pat:
				sys.exit("element has no Invoke pattern")
			pat.QueryInterface(UIA.IUIAutomationInvokePattern).Invoke()
			print(f"invoked {target!r}")


if __name__ == "__main__":
	main()
