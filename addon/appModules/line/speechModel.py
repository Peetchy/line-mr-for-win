# What voice message transcription needs on disk, downloaded when the user turns it on.
#
#   - lineMR-whisper-x64.zip (GitHub release "whisper-1"; tools/whisper/README.md says how it is
#     made): whisper.cpp's whisper-cli.exe and its DLLs, built with the C runtime linked in (the
#     official builds need the Visual C++ runtime, which not every PC has);
#   - one speech model of `models()`, chosen in the settings (or in the dialog before a transcription):
#     OpenAI's Whisper models as whisper.cpp publishes them on Hugging Face (ggerganov/whisper.cpp),
#     or SCB 10X's Typhoon Whisper models (trained further on Thai; MIT), converted for whisper.cpp
#     and published with the add-on's release "whisper-1". All are 8-bit (q8_0) except Typhoon
#     large-v3 (5-bit, to keep its memory use near the others'; still ~2 GB and 6-16 times slower
#     than Turbo on an i5-1235U).
#   - or a model file the user downloaded (`CUSTOM`), checked by `checkModelFile()`.
# Everything goes to %LOCALAPPDATA%\LineMR\whisper (removed with the add-on, installTasks.py).
# Downloads resume after an interruption (HTTP Range) and are checked against their SHA-256 before
# use; `installed.txt` records which program build is complete.
#
# The list of models is published in the repository (speech-models.json on the main branch, see
# `CATALOG_URL`), so a model added there shows in the settings and the dialog without an add-on
# update: `refreshCatalog()` fetches it when they open (and at NVDA start) and keeps the last good
# copy in %LOCALAPPDATA%\LineMR\speech-models.json; `BUILTIN` is used until then. Entries are
# checked strictly (`_parseCatalog`): downloads only from this repository's releases or Hugging Face,
# a SHA-256 and a size for every file.

import hashlib
import json
import os
import re
import time
import shutil
import struct
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

#: Changes whenever the program changes: an older build is downloaded again.
SET_ID = "whisper-1"
_RELEASE_URL = "https://github.com/Peetchy/line-mr-for-win/releases/download/" + SET_ID + "/"
_HF_URL = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/"
PROGRAM_ZIP = ("lineMR-whisper-x64.zip", "928347788f7d29fb643519a016f41838e8c0fd71f6747ddb6096959de6deac4e", 5271278)
_TIMEOUT = 30


#: What a model is good at, shown with its name (transcribe.modelLabel() has the translations).
STRENGTHS = ("bestForThai", "thaiOnly", "thaiOnlyAccurate", "manyLanguages")


class Model:
	def __init__(self, id, name, file, sha256, size, baseUrl="", url=None, strength="manyLanguages", speed=None, note=None):
		self.id = id
		#: Shown to the user (not translated: model names).
		self.name = name
		self.file = file
		self.sha256 = sha256
		self.size = size
		self.url = url or baseUrl + file
		#: One of STRENGTHS.
		self.strength = strength
		#: First guess of seconds of work per second of audio (learnt per computer afterwards), or None.
		self.speed = speed
		#: {language code: text} replacing the strength's words, from the published list, or None.
		self.note = note

	@property
	def path(self):
		return os.path.join(directory(), self.file)

	@property
	def megabytes(self):
		return (self.size + 500000) // 1000000

	def downloaded(self):
		try:
			return os.path.getsize(self.path) == self.size
		except OSError:
			return False


#: The choices this version knows (speech-models.json in the repository lists the same and newer ones):
#: the default first, then the other Typhoon models, then Whisper by size. Typhoon Turbo was the only
#: model of the first version (its file name is unchanged, so it is not downloaded again). Speeds were
#: measured on an i5-1235U with 10 threads, model loading included (Turbo and Medium: 8 s of Thai in
#: 20-24 s, 36 s in 91 s; Large v3: 33 s and 587 s; Whisper's own models assumed alike).
BUILTIN = (
	Model("typhoon-turbo", "Typhoon Whisper Turbo", "ggml-typhoon-whisper-turbo-q8_0.bin",
		"4797a76f85ad1d7681d1716984bd1a37ffcd58e866e4540e83d06ca72db941a4", 874188075, _RELEASE_URL,
		strength="bestForThai", speed=3.0),
	Model("typhoon-medium", "Typhoon Whisper Medium", "ggml-typhoon-whisper-medium-q8_0.bin",
		"c315d595bc9d1283e148f4c4ba42b9db34ba017b5113faba74c2be9d6b6e16e8", 823369796, _RELEASE_URL,
		strength="thaiOnly", speed=3.0),
	Model("typhoon-large", "Typhoon Whisper Large v3", "ggml-typhoon-whisper-large-v3-q5_0.bin",
		"0cc61237b9eff9d8bffa8fe665add2f6c320551083878d8c7edc9fe53423fbe2", 1081140203, _RELEASE_URL,
		strength="thaiOnlyAccurate", speed=16.0),
	Model("whisper-small", "Whisper Small", "ggml-small-q8_0.bin",
		"49c8fb02b65e6049d5fa6c04f81f53b867b5ec9540406812c643f177317f779f", 264464607, _HF_URL,
		strength="manyLanguages", speed=1.0),
	Model("whisper-medium", "Whisper Medium", "ggml-medium-q8_0.bin",
		"42a1ffcbe4167d224232443396968db4d02d4e8e87e213d3ee2e03095dea6502", 823369779, _HF_URL,
		strength="manyLanguages", speed=3.0),
	Model("whisper-turbo", "Whisper Large v3 Turbo", "ggml-large-v3-turbo-q8_0.bin",
		"317eb69c11673c9de1e1f0d459b253999804ec71ac4c23c17ecf5fbe24e259a1", 874188075, _HF_URL,
		strength="manyLanguages", speed=3.0),
)

#: The published list of models (raw file of the repository's main branch).
CATALOG_URL = "https://raw.githubusercontent.com/Peetchy/line-mr-for-win/main/speech-models.json"
#: Where a listed model may be downloaded from.
_URL_PREFIXES = ("https://github.com/Peetchy/line-mr-for-win/releases/download/", "https://huggingface.co/")
_ID = re.compile(r"^[a-z0-9][a-z0-9._-]{0,47}$")
_FILE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,99}\.bin$")
_SHA256 = re.compile(r"^[0-9a-f]{64}$")
#: The published list is fetched at most this often (seconds); GitHub's raw files are cached ~5 minutes.
_CATALOG_INTERVAL = 30
_catalog = None
_catalogLock = threading.Lock()
_catalogFetched = 0.0


def _catalogPath():
	return os.path.join(bridge.bridgeDataDir(), "speech-models.json")


def _parseCatalog(data):
	"""The models of a published list (JSON text); raises ValueError for anything not as expected."""
	doc = json.loads(data)
	if not isinstance(doc, dict) or doc.get("schema") != 1 or not isinstance(doc.get("models"), list):
		raise ValueError("not a speech model list of schema 1")
	models, ids, files = [], set(), set()
	for entry in doc["models"]:
		if not isinstance(entry, dict):
			raise ValueError("entry is not an object")
		id, name, file = entry.get("id"), entry.get("name"), entry.get("file")
		url, sha256, size = entry.get("url"), entry.get("sha256"), entry.get("size")
		if not (isinstance(id, str) and _ID.match(id) and id != CUSTOM and id not in ids):
			raise ValueError("bad id %r" % (id,))
		if not (isinstance(name, str) and 0 < len(name) <= 80):
			raise ValueError("bad name for %s" % id)
		if not (isinstance(file, str) and _FILE.match(file) and file not in files):
			raise ValueError("bad file for %s" % id)
		if not (isinstance(url, str) and url.startswith(_URL_PREFIXES) and url.endswith("/" + file)):
			raise ValueError("bad url for %s" % id)
		if not (isinstance(sha256, str) and _SHA256.match(sha256)):
			raise ValueError("bad sha256 for %s" % id)
		if not (isinstance(size, int) and 1000000 <= size <= 10_000_000_000):
			raise ValueError("bad size for %s" % id)
		strength = entry.get("strength", "manyLanguages")
		if strength not in STRENGTHS:
			raise ValueError("bad strength for %s" % id)
		speed = entry.get("speed")
		if speed is not None and not (isinstance(speed, (int, float)) and 0.05 <= speed <= 100):
			raise ValueError("bad speed for %s" % id)
		note = entry.get("note")
		if note is not None and not (
			isinstance(note, dict) and all(isinstance(k, str) and isinstance(v, str) and len(v) <= 120 for k, v in note.items())
		):
			raise ValueError("bad note for %s" % id)
		ids.add(id)
		files.add(file)
		models.append(Model(id, name, file, sha256, size, url=url, strength=strength, speed=speed, note=note))
	if not models:
		raise ValueError("no models")
	return models


def models():
	"""The models to choose from: the published list (last good copy), then any built-in model it no
	longer lists (a user may have chosen or downloaded it); the built-in list until one was fetched."""
	global _catalog
	with _catalogLock:
		if _catalog is None:
			try:
				with open(_catalogPath(), encoding="utf-8") as f:
					_catalog = _parseCatalog(f.read())
			except (OSError, ValueError):
				_catalog = []
		listed = list(_catalog)
	if not listed:
		return list(BUILTIN)
	ids = {m.id for m in listed}
	return listed + [m for m in BUILTIN if m.id not in ids]


def refreshCatalog(onChanged=None):
	"""Fetches the published list in the background; onChanged() (on that thread) when the models
	differ from before. Nothing happens if it was fetched less than _CATALOG_INTERVAL s ago."""
	global _catalogFetched
	now = time.monotonic()
	if _catalogFetched and now - _catalogFetched < _CATALOG_INTERVAL:
		return
	_catalogFetched = now

	def fetch():
		global _catalog
		try:
			request = urllib.request.Request(CATALOG_URL, headers={"User-Agent": "lineMR", "Cache-Control": "no-cache"})
			with urllib.request.urlopen(request, timeout=10) as response:
				data = response.read(256 * 1024 + 1)
			if len(data) > 256 * 1024:
				raise ValueError("list too large")
			text = data.decode("utf-8")
			fetched = _parseCatalog(text)
		except Exception as e:
			log.debugWarning("LINE MR: speech model list not fetched: %s", e)
			return
		before = [(m.id, m.name, m.url, m.sha256, m.size, m.strength, m.note) for m in models()]
		with _catalogLock:
			_catalog = fetched
		try:
			with open(_catalogPath(), "w", encoding="utf-8") as f:
				f.write(text)
		except OSError:
			pass
		after = [(m.id, m.name, m.url, m.sha256, m.size, m.strength, m.note) for m in models()]
		if after != before:
			log.info("LINE MR: speech model list updated: %s", ", ".join(m.id for m in fetched))
			if onChanged:
				onChanged()

	threading.Thread(target=fetch, name="lineMR.speechModelList", daemon=True).start()
#: The setting's value for a model file chosen by the user (its path is a setting of its own).
CUSTOM = "custom"
#: The default: of the Typhoon models the one that needs the least memory (Turbo has 4 decoder layers
#: to Medium's 24: ~1.1 GB to ~1.45 GB while working, measured), as fast as Medium on Thai, and the
#: only Typhoon model that also writes English, Japanese and Chinese (Medium writes them in Thai
#: letters).
DEFAULT = "typhoon-turbo"


class CustomModel:
	"""A model file the user chose (never downloaded or deleted by the add-on)."""

	id = CUSTOM

	def __init__(self, path):
		self.path = path
		self.file = os.path.basename(path)
		self.name = self.file

	@property
	def size(self):
		try:
			return os.path.getsize(self.path)
		except OSError:
			return 0

	@property
	def megabytes(self):
		return (self.size + 500000) // 1000000

	def downloaded(self):
		return bool(self.path) and os.path.isfile(self.path)


def model(id, customPath=""):
	"""The model of a setting's value; an unknown value (or "custom" without a file) gives the
	default."""
	if id == CUSTOM and customPath:
		return CustomModel(customPath)
	for m in models():
		if m.id == id:
			return m
	return model(DEFAULT)


def directory():
	return os.path.join(bridge.bridgeDataDir(), "whisper")


def programPath():
	return os.path.join(directory(), "bin", "whisper-cli.exe")


def _markerPath():
	return os.path.join(directory(), "installed.txt")


def programInstalled():
	"""True when the current build of whisper.cpp is complete."""
	try:
		with open(_markerPath(), encoding="ascii") as f:
			if f.read().strip() != SET_ID:
				return False
	except OSError:
		return False
	return os.path.isfile(programPath())


def downloadMB(m):
	"""What turning `m` on still downloads, in megabytes (rounded)."""
	size = 0 if programInstalled() else PROGRAM_ZIP[2]
	if m is not None and not m.downloaded():
		size += m.size
	return (size + 500000) // 1000000


def downloadedModels():
	return [m for m in models() if m.downloaded()]


def remove():
	"""Deletes everything downloaded (a running download is cancelled first)."""
	cancelDownload()
	shutil.rmtree(directory(), ignore_errors=True)


def removeModel(m):
	"""Deletes one downloaded model (and its partial download); never the user's own file."""
	if isinstance(m, CustomModel):
		return
	if _download is not None and _download.model is not None and _download.model.id == m.id:
		cancelDownload()
	for path in (m.path, m.path + ".part"):
		try:
			os.remove(path)
		except OSError:
			pass


class DownloadCancelled(Exception):
	pass


class _Download(threading.Thread):
	def __init__(self, model, onDone):
		super().__init__(name="lineMR.speechModelDownload", daemon=True)
		self._cancelled = False
		self._onDone = onDone
		#: None: only the program.
		self.model = model
		self._program = not programInstalled()
		#: Bytes done / total, for the settings panel.
		self.done = 0
		self.total = (PROGRAM_ZIP[2] if self._program else 0) + (model.size if model and not model.downloaded() else 0)

	def cancel(self):
		self._cancelled = True

	def run(self):
		global _download
		ok = False
		try:
			os.makedirs(directory(), exist_ok=True)
			offset = 0
			if self._program:
				try:
					os.remove(_markerPath())
				except OSError:
					pass
				zipPath = self._fetch(PROGRAM_ZIP[0], _RELEASE_URL + PROGRAM_ZIP[0], PROGRAM_ZIP[1], PROGRAM_ZIP[2], 0)
				binDir = os.path.join(directory(), "bin")
				shutil.rmtree(binDir, ignore_errors=True)
				with zipfile.ZipFile(zipPath) as z:
					z.extractall(binDir)
				os.remove(zipPath)
				with open(_markerPath(), "w", encoding="ascii") as f:
					f.write(SET_ID)
				offset = PROGRAM_ZIP[2]
			m = self.model
			if m is not None and not m.downloaded():
				self._fetch(m.file, m.url, m.sha256, m.size, offset)
			ok = True
			log.info("LINE MR: speech recognition files downloaded to %s (%s)", directory(), m.id if m else "program")
		except DownloadCancelled:
			log.info("LINE MR: speech model download cancelled")
		except Exception as e:
			log.error("LINE MR: speech model download failed", exc_info=True)
			_say(_downloadFailed(e))
		finally:
			_download = None
		if ok:
			self._onDone()

	def _fetch(self, name, url, sha256, size, offset):
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
					if self._cancelled:
						raise DownloadCancelled
					digest.update(chunk)
		if have < size:
			request = urllib.request.Request(url, headers={"User-Agent": "lineMR-nvda-addon"})
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
		target = os.path.join(directory(), name)
		os.replace(part, target)
		return target


_download = None


def downloading():
	"""The running download (with .model, .done, .total) or None."""
	return _download


def startDownload(m, onDone):
	"""Starts downloading what `m` needs (the program, and the model unless m is None) unless it is
	there; a download of another model is cancelled (its partial file is kept for later). onDone runs
	on the download thread once the files are complete. False when nothing was started."""
	global _download
	if _download is not None:
		if (_download.model is None and m is None) or (_download.model is not None and m is not None and _download.model.id == m.id):
			return False
		cancelDownload()
	if programInstalled() and (m is None or m.downloaded()):
		return False
	_download = _Download(m, onDone)
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


# ---- Model files chosen by the user -------------------------------------------------------------
# whisper.cpp (v1.9.5) reads only its own "ggml" format: magic 0x67676d6c ("lmgg" on disk), eleven
# int32 hyper-parameters, the mel filters, the vocabulary, then the tensors (each: n_dims, name
# length, type, dims, name, data). The check walks all of it, so a cut-off download is caught too.

_GGML_MAGIC = 0x67676D6C
#: ggml tensor types whisper.cpp files use: (elements per block, bytes per block).
_TYPE_SIZES = {
	0: (1, 4),  # f32
	1: (1, 2),  # f16
	2: (32, 18),  # q4_0
	3: (32, 20),  # q4_1
	6: (32, 22),  # q5_0
	7: (32, 24),  # q5_1
	8: (32, 34),  # q8_0
	10: (256, 84),  # q2_K
	11: (256, 110),  # q3_K
	12: (256, 144),  # q4_K
	13: (256, 176),  # q5_K
	14: (256, 210),  # q6_K
	30: (1, 2),  # bf16
}


class ModelCheck:
	"""The result of `checkModelFile()`: `ok`, `message` (translated, for the user), and for a good
	file whether it is an English-only model (vocabulary of 51,864 tokens)."""

	def __init__(self, ok, message, englishOnly=False):
		self.ok = ok
		self.message = message
		self.englishOnly = englishOnly


def _formatOf(head):
	"""A name for a known model format that is not whisper.cpp's, else None."""
	if head[:4] == b"GGUF":
		return "GGUF"
	if head[:2] == b"PK":
		return "PyTorch (.pt)"
	if len(head) >= 9 and head[8:9] == b"{":
		return "safetensors"
	if head[:1] == b"\x08":
		return "ONNX"
	return None


def checkModelFile(path):
	"""Whether whisper-cli can use the file at `path` (structure only; runModelTest() loads it)."""
	try:
		size = os.path.getsize(path)
		with open(path, "rb") as f:
			head = f.read(4 + 11 * 4)
			if len(head) < 48 or struct.unpack("<i", head[:4])[0] != _GGML_MAGIC:
				other = _formatOf(head)
				if other:
					# Translators: Reported when a chosen speech model file has another format ({format}, e.g. GGUF, ONNX).
					return ModelCheck(False, _(
						"This is a {format} file. LINE MR needs a whisper.cpp model (ggml .bin file, for example "
						"ggml-small.bin from huggingface.co/ggerganov/whisper.cpp); convert the model first."
					).format(format=other))
				# Translators: Reported when a chosen file is not a speech model LINE MR can use.
				return ModelCheck(False, _(
					"This is not a whisper.cpp speech model (ggml .bin file, for example ggml-small.bin from "
					"huggingface.co/ggerganov/whisper.cpp)."
				))
			(nVocab, nAudioCtx, nAudioState, nAudioHead, nAudioLayer, nTextCtx, nTextState, nTextHead,
				nTextLayer, nMels, ftype) = struct.unpack("<11i", head[4:])
			if not (nAudioCtx == 1500 and nMels in (80, 128) and 51800 <= nVocab <= 52000
					and 0 < nAudioLayer <= 64 and 0 < nTextLayer <= 64 and 0 < nAudioState <= 4096):
				# Translators: Reported when a chosen file is a ggml file but not a Whisper speech model.
				return ModelCheck(False, _("This ggml file is not a Whisper speech model."))
			melRows, melCols = struct.unpack("<2i", f.read(8))
			f.seek(melRows * melCols * 4, os.SEEK_CUR)
			(count,) = struct.unpack("<i", f.read(4))
			for _i in range(count):
				(length,) = struct.unpack("<i", f.read(4))
				f.seek(length, os.SEEK_CUR)
			while True:
				header = f.read(12)
				if not header:
					break
				nDims, nameLength, kind = struct.unpack("<3i", header)
				dims = struct.unpack("<%di" % nDims, f.read(4 * nDims))
				f.seek(nameLength, os.SEEK_CUR)
				if kind not in _TYPE_SIZES:
					# Translators: Reported when a chosen speech model uses a number format whisper.cpp of this add-on does not know.
					return ModelCheck(False, _("This speech model uses a format this version of LINE MR cannot read."))
				elements = 1
				for d in dims:
					elements *= d
				block, blockBytes = _TYPE_SIZES[kind]
				f.seek(elements // block * blockBytes, os.SEEK_CUR)
				if f.tell() > size:
					raise EOFError
	except (OSError, struct.error, EOFError, ValueError, MemoryError):
		# Translators: Reported when a chosen speech model file is cut off or damaged.
		return ModelCheck(False, _("The speech model file is incomplete or damaged. Download it again."))
	englishOnly = nVocab < 51865
	if englishOnly:
		# Translators: Reported when a chosen speech model can be used but knows only English (.en models).
		message = _("The file can be used, but it is an English-only model: it cannot transcribe Thai or other languages.")
	else:
		# Translators: Reported when a chosen speech model file can be used.
		message = _("The file can be used.")
	return ModelCheck(True, message, englishOnly)


def testModelFile(path, timeout=180):
	"""Loads the model with whisper-cli on one second of silence: True if it worked, False if it
	failed, None if the program is not downloaded (the first transcription then shows it)."""
	import subprocess

	if not programInstalled():
		return None
	wav = os.path.join(directory(), "silence.wav")
	try:
		if not os.path.isfile(wav):
			samples = 16000
			with open(wav, "wb") as f:
				f.write(b"RIFF" + struct.pack("<I", 36 + samples * 2) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, 16000, 32000, 2, 16))
				f.write(b"data" + struct.pack("<I", samples * 2) + bytes(samples * 2))
		result = subprocess.run(
			[programPath(), "-m", path, "-f", wav, "-l", "en", "-nt", "-np", "-ac", "100"],
			stdin=subprocess.DEVNULL,
			stdout=subprocess.DEVNULL,
			stderr=subprocess.DEVNULL,
			timeout=timeout,
			creationflags=subprocess.CREATE_NO_WINDOW,
		)
	except (OSError, subprocess.TimeoutExpired):
		log.warning("LINE MR: testing the speech model %s failed", path, exc_info=True)
		return False
	if result.returncode != 0:
		log.warning("LINE MR: whisper-cli could not use %s (exit code %s)", path, result.returncode)
	return result.returncode == 0
