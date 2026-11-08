# LINE's MPEG-4 files: once a voice message has been played, LINE keeps it in
# %LOCALAPPDATA%\LINE\Cache\<hash>\<hash>\m\<x>\<name> as AAC in an MPEG-4 file (brand "M4A ", no
# extension; videos it has shown are "*_temp.mp4" with brand "mp42"). Nothing in a file name ties it
# to a message: choosing a cached file by its length alone took another message of the same length
# (2026-11-08), so transcribe.py has LINE save the message's own file instead, and uses the cache
# only for a file LINE writes while it plays the message (`duration()`). `mp4Info()` checks that a
# saved file is complete and of the message's length.

import os
import struct

#: How far the file's length may be from the whole seconds drawn in the bubble.
TOLERANCE = 1.2


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


def mp4Info(path):
	"""(seconds, complete) of an MPEG-4 file (complete: its top-level boxes fill the file and include
	`moov` and `mdat`, so it is not still being written), or None for anything else."""
	try:
		size = os.path.getsize(path)
		with open(path, "rb") as f:
			offset, seconds, boxes = 0, None, set()
			while offset + 8 <= size:
				f.seek(offset)
				header = f.read(16)
				boxSize, kind = struct.unpack(">I4s", header[:8])
				if boxSize == 1 and len(header) >= 16:
					boxSize = struct.unpack(">Q", header[8:16])[0]
				elif boxSize == 0:
					boxSize = size - offset
				if boxSize < 8 or (offset == 0 and kind != b"ftyp"):
					return None
				boxes.add(kind)
				if kind == b"moov":
					f.seek(offset + 8)
					data = f.read(min(boxSize - 8, 1 << 16))
					i = data.find(b"mvhd")
					if i >= 0 and len(data) >= i + 36:
						if data[i + 4] == 1:
							timescale, length = struct.unpack(">IQ", data[i + 24 : i + 36])
						else:
							timescale, length = struct.unpack(">II", data[i + 16 : i + 24])
						seconds = length / timescale if timescale else None
				offset += boxSize
	except (OSError, struct.error):
		return None
	if seconds is None:
		return None
	return seconds, offset == size and b"moov" in boxes and b"mdat" in boxes

