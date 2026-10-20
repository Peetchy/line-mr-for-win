"""Presses keys in the foreground window and prints what NVDA spoke after each one
(read from %TEMP%\nvda.log; needs NVDA's log level at "input/output" or "debug").

Usage: python tools/nvda_speech_probe.py tab tab space shift+tab
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from uia_events import press  # noqa: E402

LOG = os.path.join(os.environ["TEMP"], "nvda.log")


def main():
	sys.stdout.reconfigure(encoding="utf-8")
	pos = os.path.getsize(LOG)
	for key in sys.argv[1:] or ["tab"]:
		if key != "-":
			press(key)
		time.sleep(1.5)
		with open(LOG, "rb") as f:
			f.seek(pos)
			data = f.read()
		pos += len(data)
		spoken = [line for line in data.decode("utf-8", "replace").splitlines() if line.startswith("Speaking")]
		print(f"{key:10} -> {' | '.join(s[len('Speaking '):] for s in spoken) or '(silence)'}")


if __name__ == "__main__":
	main()
