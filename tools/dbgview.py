"""Print OutputDebugString messages (Qt's qDebug output in GUI apps), like Sysinternals DbgView.

Usage: python tools/dbgview.py [--pid PID] [--seconds N] [--grep TEXT]
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import time

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.CreateEventW.restype = wt.HANDLE
kernel32.CreateFileMappingW.restype = wt.HANDLE
kernel32.MapViewOfFile.restype = ctypes.c_void_p

BUFFER_SIZE = 4096


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("--pid", type=int)
	ap.add_argument("--seconds", type=float, default=0, help="stop after N seconds (0 = forever)")
	ap.add_argument("--grep")
	args = ap.parse_args()
	sys.stdout.reconfigure(encoding="utf-8", line_buffering=True)

	ready = kernel32.CreateEventW(None, False, False, "DBWIN_BUFFER_READY")
	data_ready = kernel32.CreateEventW(None, False, False, "DBWIN_DATA_READY")
	mapping = kernel32.CreateFileMappingW(wt.HANDLE(-1), None, 0x04, 0, BUFFER_SIZE, "DBWIN_BUFFER")
	if not (ready and data_ready and mapping):
		sys.exit(f"cannot open DBWIN objects (error {ctypes.get_last_error()}); is another debug viewer running?")
	view = kernel32.MapViewOfFile(mapping, 0x04, 0, 0, BUFFER_SIZE)
	deadline = time.monotonic() + args.seconds if args.seconds else None
	kernel32.SetEvent(ready)
	while deadline is None or time.monotonic() < deadline:
		if kernel32.WaitForSingleObject(data_ready, 200) != 0:
			continue
		raw = ctypes.string_at(view, BUFFER_SIZE)
		pid = struct.unpack_from("<I", raw)[0]
		msg = raw[4:].split(b"\0", 1)[0].decode("utf-8", "replace").rstrip()
		kernel32.SetEvent(ready)
		if args.pid and pid != args.pid:
			continue
		if args.grep and args.grep.lower() not in msg.lower():
			continue
		print(f"[{pid}] {msg}")


if __name__ == "__main__":
	main()
