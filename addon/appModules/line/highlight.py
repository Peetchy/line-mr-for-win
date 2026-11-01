# Settings of the bridge's focus highlight (a coloured frame around the focused control in LINE, for
# low-vision users). The bridge reads %LOCALAPPDATA%\LineMR\focus-highlight.ini about once a second,
# in LINE and its call and media processes; no restart is needed.

import os

import addonHandler
from logHandler import log

from . import bridge

addonHandler.initTranslation()

SETTINGS_FILE = "focus-highlight.ini"

#: (key, label, RGB in hex); the key is stored in NVDA's configuration.
COLORS = (
	# Translators: A colour of the focus highlight in LINE.
	("yellow", _("Yellow"), "FFD600"),
	# Translators: A colour of the focus highlight in LINE.
	("orange", _("Orange"), "FF8C00"),
	# Translators: A colour of the focus highlight in LINE.
	("red", _("Red"), "FF2D2D"),
	# Translators: A colour of the focus highlight in LINE.
	("green", _("Green"), "00E060"),
	# Translators: A colour of the focus highlight in LINE.
	("blue", _("Blue"), "2D8CFF"),
	# Translators: A colour of the focus highlight in LINE.
	("white", _("White"), "FFFFFF"),
)
DEFAULT_COLOR = "yellow"
MIN_WIDTH = 1
MAX_WIDTH = 12
DEFAULT_WIDTH = 4


def _hexColor(key):
	for k, _label, value in COLORS:
		if k == key:
			return value
	return COLORS[0][2]


def writeSettings(enabled, color, width):
	"""Writes the highlight settings for the bridge (only when they changed)."""
	path = os.path.join(bridge.bridgeDataDir(), SETTINGS_FILE)
	text = "[highlight]\nenabled=%s\ncolor=%s\nwidth=%d\n" % (
		"true" if enabled else "false",
		_hexColor(color),
		max(MIN_WIDTH, min(MAX_WIDTH, int(width))),
	)
	try:
		with open(path, encoding="ascii") as f:
			if f.read() == text:
				return
	except (OSError, ValueError):
		pass
	try:
		os.makedirs(os.path.dirname(path), exist_ok=True)
		with open(path, "w", encoding="ascii", newline="\n") as f:
			f.write(text)
	except OSError:
		log.error("LINE MR: could not write %s", path, exc_info=True)
