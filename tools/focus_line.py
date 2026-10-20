"""Brings LINE's main (or login) window to the foreground, like clicking it.

Usage: python tools/focus_line.py [--class LoginWindow]
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import subprocess
import sys

user32 = ctypes.windll.user32


def line_windows():
	out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq LINE.exe", "/FO", "CSV", "/NH"],
	                     capture_output=True, text=True).stdout
	pids = {int(p.split('","')[1]) for p in out.splitlines() if p.count('","') > 1}
	found = []
	proc = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)

	def cb(hwnd, _):
		pid = wt.DWORD()
		user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
		if pid.value in pids and user32.IsWindowVisible(hwnd):
			title = ctypes.create_unicode_buffer(256)
			user32.GetWindowTextW(hwnd, title, 256)
			rect = wt.RECT()
			user32.GetWindowRect(hwnd, ctypes.byref(rect))
			area = (rect.right - rect.left) * (rect.bottom - rect.top)
			found.append((area, hwnd, title.value))
		return True

	user32.EnumWindows(proc(cb), 0)
	return sorted(found, reverse=True)


def main():
	ap = argparse.ArgumentParser()
	ap.parse_args()
	wins = line_windows()
	if not wins:
		sys.exit("no visible LINE window")
	_area, hwnd, title = wins[0]
	# Windows only lets the foreground process hand over the foreground; a synthetic Alt press
	# makes this process eligible (the documented SetForegroundWindow workaround).
	user32.keybd_event(0x12, 0, 0, 0)
	user32.keybd_event(0x12, 0, 2, 0)
	user32.ShowWindow(hwnd, 9)  # SW_RESTORE
	ok = user32.SetForegroundWindow(hwnd)
	print(f"foreground {'ok' if ok else 'FAILED'}: {hwnd:#x} {title!r}")


if __name__ == "__main__":
	main()
