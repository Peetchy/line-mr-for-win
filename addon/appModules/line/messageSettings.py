"""Passes the optional message presentation setting to the bridge without restarting LINE."""

import os

from logHandler import log

from . import bridge


def writeSettings(senderNames):
	path = os.path.join(bridge.bridgeDataDir(), "message-settings.ini")
	text = "[messages]\ndirectChatSenderNames=%s\n" % ("true" if senderNames else "false")
	try:
		with open(path, encoding="ascii") as f:
			if f.read() == text:
				return
	except OSError:
		pass
	try:
		os.makedirs(os.path.dirname(path), exist_ok=True)
		with open(path + ".tmp", "w", encoding="ascii", newline="\n") as f:
			f.write(text)
		os.replace(path + ".tmp", path)
	except OSError:
		log.error("LINE MR: cannot write message presentation settings", exc_info=True)
