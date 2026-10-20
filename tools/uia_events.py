"""Logs UI Automation focus-changed events (what NVDA follows) while optionally pressing keys.

Usage: python tools/uia_events.py [--seconds N] [--keys tab,tab,shift+tab]
"""

import argparse
import ctypes
import sys

sys.coinit_flags = 0  # COINIT_MULTITHREADED: events arrive without a message loop
import threading
import time

import comtypes
import comtypes.client
from comtypes import COMObject

sys.path.insert(0, __import__("os").path.dirname(__file__))
import uia_inspect  # noqa: E402
from uia_inspect import UIA  # noqa: E402

user32 = ctypes.windll.user32


class FocusHandler(COMObject):
	_com_interfaces_ = [UIA.IUIAutomationFocusChangedEventHandler]

	def HandleFocusChangedEvent(self, sender):
		try:
			print(f"{time.strftime('%H:%M:%S')} FOCUS {uia_inspect.describe(sender)[0]}", flush=True)
		except Exception as e:  # noqa: BLE001
			print("FOCUS <error>", e, flush=True)
		return 0


def press(combo):
	keys = {"tab": 0x09, "shift": 0x10, "enter": 0x0D, "space": 0x20, "ctrl": 0x11, "alt": 0x12,
	        "down": 0x28, "up": 0x26, "left": 0x25, "right": 0x27, "escape": 0x1B, "backspace": 0x08}
	keys.update({"nvda": 0x2D, "home": 0x24, "end": 0x23, "pageup": 0x21, "pagedown": 0x22})
	# Single characters: digits and letters map to their virtual-key codes.
	codes = [keys[k] if k in keys else ord(k.upper()) for k in combo.split("+")]
	# Navigation keys are "extended" keys; without the flag Windows reports them as numpad keys.
	extended = {0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x2D, 0x2E}
	for c in codes:
		user32.keybd_event(c, 0, 1 if c in extended else 0, 0)
	for c in reversed(codes):
		user32.keybd_event(c, 0, 3 if c in extended else 2, 0)


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("--seconds", type=float, default=5)
	ap.add_argument("--keys", default="")
	args = ap.parse_args()
	sys.stdout.reconfigure(encoding="utf-8")
	uia = comtypes.client.CreateObject(UIA.CUIAutomation, interface=UIA.IUIAutomation)
	handler = FocusHandler()
	uia.AddFocusChangedEventHandler(None, handler)

	def keys():
		for k in filter(None, args.keys.split(",")):
			time.sleep(1.0)
			print(f"{time.strftime('%H:%M:%S')} KEY {k}", flush=True)
			press(k)

	threading.Thread(target=keys, daemon=True).start()
	time.sleep(args.seconds)
	uia.RemoveAllEventHandlers()


if __name__ == "__main__":
	main()
