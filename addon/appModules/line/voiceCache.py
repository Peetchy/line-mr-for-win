# LINE's copies of voice messages: once a voice message has been played, LINE keeps it in
# %LOCALAPPDATA%\LINE\Cache\<hash>\<hash>\m\<x>\<name> as AAC in an MPEG-4 file (brand "M4A ", no
# extension; videos are "*_temp.mp4" with brand "mp42"). Nothing in a file name ties it to a message,
# so the message is found by its length: the bridge reads it from the bubble ("00:36"), the file's
# `mvhd` box holds it exactly (36.16 s). A message never played has no file: playing it makes LINE
# download it.

import os
import struct

from logHandler import log

#: How far the file's length may be from the whole seconds drawn in the bubble.
_TOLERANCE = 1.2
#: Voice messages are small (LINE's limit is a few minutes of AAC); larger files are videos.
_MAX_BYTES = 20 * 1024 * 1024


def cacheDir():
	return os.path.join(os.environ.get("LOCALAPPDATA", ""), "LINE", "Cache")


def duration(path):
	"""Length in seconds of an M4A voice file, None for anything else."""
	try:
		with open(path, "rb") as f:
			head = f.read(12)
			if head[4:8] != b"ftyp" or head[8:12] != b"M4A ":
				return None
			data = head + f.read(1 << 16)
	except OSError:
		return None
	i = data.find(b"mvhd")
	if i < 0 or len(data) < i + 36:
		return None
	if data[i + 4] == 1:
		timescale, length = struct.unpack(">IQ", data[i + 24 : i + 36])
	else:
		timescale, length = struct.unpack(">II", data[i + 16 : i + 24])
	return length / timescale if timescale else None


def voiceFiles():
	"""{path: (seconds, size, modified)} of every voice file in LINE's cache."""
	found = {}
	for dirpath, _dirs, files in os.walk(cacheDir()):
		for name in files:
			path = os.path.join(dirpath, name)
			try:
				st = os.stat(path)
			except OSError:
				continue
			if st.st_size > _MAX_BYTES or st.st_size < 64:
				continue
			seconds = duration(path)
			if seconds is not None:
				found[path] = (seconds, st.st_size, st.st_mtime)
	return found


def matching(files, seconds):
	"""Paths of `files` (from voiceFiles()) whose length fits a message of `seconds` (drawn as m:ss,
	so the whole seconds, rounded down or to the nearest)."""
	return [path for path, (length, _size, _mtime) in files.items() if abs(length - seconds) <= _TOLERANCE]


def find(seconds):
	"""The cached file of a voice message of `seconds`, if exactly one fits; else None."""
	if seconds <= 0:
		return None
	try:
		candidates = matching(voiceFiles(), seconds)
	except Exception:
		log.debugWarning("LINE MR: cannot look through LINE's cache", exc_info=True)
		return None
	if len(candidates) == 1:
		return candidates[0]
	if candidates:
		log.info("LINE MR: %d cached voice messages are %d seconds long; playing the message to find it", len(candidates), seconds)
	return None
