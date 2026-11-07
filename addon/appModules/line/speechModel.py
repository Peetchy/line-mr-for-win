# What voice message transcription needs on disk, downloaded when the user turns it on.
#
# Two files of the add-on's GitHub release "whisper-1" (tools/whisper/README.md says how they are
# made):
#   - lineMR-whisper-x64.zip: whisper.cpp's whisper-cli.exe and its DLLs, built with the C runtime
#     linked in (the official builds need the Visual C++ runtime, which not every PC has);
#   - ggml-typhoon-whisper-turbo-q8_0.bin: SCB 10X's Typhoon Whisper Turbo (Whisper large-v3-turbo
#     trained further on ~11,000 hours of Thai; MIT), converted for whisper.cpp, 8-bit.
# Both go to %LOCALAPPDATA%\LineMR\whisper (removed with the add-on, installTasks.py). Downloads
# resume after an interruption (HTTP Range) and are checked against their SHA-256 before use;
# `installed.txt` records which set is complete.

import hashlib
import os
import shutil
import threading
import urllib.error
import urllib.request
import zipfile

import addonHandler
import queueHandler
import ui
from logHandler import log

from . import bridge

addonHandler.initTranslation()

#: Changes whenever the files change: an older set is downloaded again.
SET_ID = "whisper-1"
_BASE_URL = "https://github.com/Peetchy/line-mr-for-win/releases/download/" + SET_ID + "/"
PROGRAM_ZIP = ("lineMR-whisper-x64.zip", "928347788f7d29fb643519a016f41838e8c0fd71f6747ddb6096959de6deac4e", 5271278)
MODEL = ("ggml-typhoon-whisper-turbo-q8_0.bin", "4797a76f85ad1d7681d1716984bd1a37ffcd58e866e4540e83d06ca72db941a4", 874188075)
#: Shown to the user before the download (megabytes, rounded).
DOWNLOAD_MB = (PROGRAM_ZIP[2] + MODEL[2] + 500000) // 1000000
_TIMEOUT = 30


def directory():
	return os.path.join(bridge.bridgeDataDir(), "whisper")


def programPath():
	return os.path.join(directory(), "bin", "whisper-cli.exe")


def modelPath():
	return os.path.join(directory(), MODEL[0])


def _markerPath():
	return os.path.join(directory(), "installed.txt")


def installed():
	"""True when the current set of files is complete."""
	try:
		with open(_markerPath(), encoding="ascii") as f:
			if f.read().strip() != SET_ID:
				return False
	except OSError:
		return False
	try:
		return os.path.isfile(programPath()) and os.path.getsize(modelPath()) == MODEL[2]
	except OSError:
		return False


def remove():
	"""Deletes everything downloaded (a running download is cancelled first)."""
	cancelDownload()
	shutil.rmtree(directory(), ignore_errors=True)


class DownloadCancelled(Exception):
	pass


class _Download(threading.Thread):
	def __init__(self, onDone):
		super().__init__(name="lineMR.speechModelDownload", daemon=True)
		self._cancelled = False
		self._onDone = onDone
		#: Bytes done / total, for the settings panel.
		self.done = 0
		self.total = PROGRAM_ZIP[2] + MODEL[2]

	def cancel(self):
		self._cancelled = True

	def run(self):
		global _download
		ok = False
		try:
			os.makedirs(directory(), exist_ok=True)
			try:
				os.remove(_markerPath())
			except OSError:
				pass
			zipPath = self._fetch(*PROGRAM_ZIP, offset=0)
			binDir = os.path.join(directory(), "bin")
			shutil.rmtree(binDir, ignore_errors=True)
			with zipfile.ZipFile(zipPath) as z:
				z.extractall(binDir)
			os.remove(zipPath)
			self._fetch(*MODEL, offset=PROGRAM_ZIP[2], final=modelPath())
			with open(_markerPath(), "w", encoding="ascii") as f:
				f.write(SET_ID)
			ok = True
			log.info("LINE MR: speech model downloaded to %s", directory())
		except DownloadCancelled:
			log.info("LINE MR: speech model download cancelled")
		except Exception as e:
			log.error("LINE MR: speech model download failed", exc_info=True)
			_say(_downloadFailed(e))
		finally:
			_download = None
		if ok:
			self._onDone()

	def _fetch(self, name, sha256, size, offset, final=None):
		"""Downloads one file to <name>.part (resuming one that was cut off) and checks it."""
		part = os.path.join(directory(), name + ".part")
		have = os.path.getsize(part) if os.path.isfile(part) else 0
		if have > size:
			os.remove(part)
			have = 0
		digest = hashlib.sha256()
		if have:
			with open(part, "rb") as f:
				while chunk := f.read(1 << 20):
					digest.update(chunk)
		if have < size:
			request = urllib.request.Request(_BASE_URL + name, headers={"User-Agent": "lineMR-nvda-addon"})
			if have:
				request.add_header("Range", "bytes=%d-" % have)
			with urllib.request.urlopen(request, timeout=_TIMEOUT) as response:
				if have and response.status != 206:
					# The server sent the whole file: start over.
					have = 0
					digest = hashlib.sha256()
				with open(part, "ab" if have else "wb") as f:
					while chunk := response.read(1 << 16):
						if self._cancelled:
							raise DownloadCancelled
						f.write(chunk)
						digest.update(chunk)
						have += len(chunk)
						self.done = offset + have
		self.done = offset + have
		if digest.hexdigest() != sha256:
			os.remove(part)
			raise ValueError("checksum mismatch: " + name)
		target = final or os.path.join(directory(), name)
		os.replace(part, target)
		return target


_download = None


def downloading():
	"""The running download (with .done / .total) or None."""
	return _download


def startDownload(onDone):
	"""Starts downloading the files unless they are there or a download runs; onDone runs on the
	download thread once they are complete."""
	global _download
	if _download is not None or installed():
		return False
	_download = _Download(onDone)
	_download.start()
	return True


def cancelDownload():
	d = _download
	if d is not None:
		d.cancel()
		d.join(10)


def _say(text):
	queueHandler.queueFunction(queueHandler.eventQueue, ui.message, text)


def _downloadFailed(error):
	if isinstance(error, (urllib.error.URLError, OSError)) and not isinstance(error, urllib.error.HTTPError):
		# Translators: Reported when the speech model for voice messages could not be downloaded (no connection, disk full, ...).
		return _(
			"The speech model for voice messages could not be downloaded. Check the internet connection "
			"and free disk space; LINE MR tries again when NVDA starts."
		)
	# Translators: Reported when the downloaded speech model was damaged or the server refused it.
	return _("The speech model for voice messages could not be downloaded (the download was damaged). LINE MR tries again when NVDA starts.")
