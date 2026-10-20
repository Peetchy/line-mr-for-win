"""Posts key presses straight to LINE's main window (PostMessage WM_KEYDOWN/WM_KEYUP), so tests
cannot type into whatever window the user has in front. NVDA does not see these keys; it only
reacts to the focus changes they cause.

Usage: python tools/post_keys.py down up pagedown contextmenu   [--delay 1.0]
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import subprocess
import time

user32 = ctypes.windll.user32
user32.PostMessageW.argtypes = (wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)
VK = {"down": 0x28, "up": 0x26, "left": 0x25, "right": 0x27, "home": 0x24, "end": 0x23,
      "pageup": 0x21, "pagedown": 0x22, "tab": 0x09, "enter": 0x0D, "space": 0x20, "escape": 0x1B, "apps": 0x5D}
EXTENDED = {0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x5D}


def main_window():
	out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq LINE.exe", "/FO", "CSV", "/NH"],
	                     capture_output=True, text=True).stdout
	pids = {int(p.split('","')[1]) for p in out.splitlines() if p.count('","') > 1}
	best = []
	proc = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)

	def cb(hwnd, _):
		pid = wt.DWORD()
		user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
		if pid.value in pids and user32.IsWindowVisible(hwnd):
			r = wt.RECT()
			user32.GetWindowRect(hwnd, ctypes.byref(r))
			best.append(((r.right - r.left) * (r.bottom - r.top), hwnd))
		return True

	user32.EnumWindows(proc(cb), 0)
	return max(best)[1] if best else None


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("keys", nargs="+")
	ap.add_argument("--delay", type=float, default=1.0)
	args = ap.parse_args()
	hwnd = main_window()
	if not hwnd:
		raise SystemExit("no visible LINE window")
	for k in args.keys:
		if k == "contextmenu":
			# What Windows sends for the Applications key / Shift+F10 (a posted VK_APPS is not enough).
			user32.PostMessageW(hwnd, 0x007B, hwnd, -1)  # WM_CONTEXTMENU from the keyboard
			print(f"{time.strftime('%H:%M:%S')} posted {k}", flush=True)
			time.sleep(args.delay)
			continue
		vk = VK[k]
		scan = user32.MapVirtualKeyW(vk, 0)
		ext = (1 << 24) if vk in EXTENDED else 0
		user32.PostMessageW(hwnd, 0x0100, vk, 1 | (scan << 16) | ext)  # WM_KEYDOWN
		user32.PostMessageW(hwnd, 0x0101, vk, 1 | (scan << 16) | ext | (3 << 30))  # WM_KEYUP
		print(f"{time.strftime('%H:%M:%S')} posted {k}", flush=True)
		time.sleep(args.delay)


if __name__ == "__main__":
	main()
