# Voice message transcription: the add-on turns LINE's voice messages into text, offline. Off until
# the user turns it on in the settings, which downloads what it needs (speechModel.py: whisper.cpp
# and the speech model chosen there; or a model file of the user's).
#
# 1. The bridge (menu item "Transcribe voice message", or Alt+Shift+T in the message list, which sends it
#    the private key Control+Alt+Shift+F12) checks that the row is a voice message, writes
#    %LOCALAPPDATA%\LineMR\transcribe-request.txt (status, length in seconds, LINE's process id)
#    and signals Local\lineMR.transcribeRequest.
# 2. Unless the user turned it off, a dialog asks for the speech model and the message's language
#    (`LanguageDialog`).
# 3. A `Job` thread looks for LINE's own copy of the message (voiceCache.py: LINE keeps every voice
#    message it has played as an .m4a file; found by its length) and decodes it (lineMR_audio.exe
#    decode): nothing is played. A message never played has no copy: then lineMR_audio.exe capture
#    starts (WASAPI process loopback of LINE's process tree), Local\lineMR.transcribePlay makes the
#    bridge click the message's play button, LINE downloads the file while it plays, and the job
#    uses the file as soon as it is complete; the loopback recording is only the last resort (it
#    lost all but the first 6 s of a 36 s message once).
# 4. The recording stops at the message's length (or after silence; silence after the last sound is
#    cut off: Whisper makes up words in long silence), then whisper-cli.exe reads the WAV file and
#    prints one segment per line as it goes. Windows' own recognizers were tried first: Microsoft
#    has no Thai recognizer, and Windows voice typing (Win+H) cannot be started by a program.
# 5. `TranscriptDialog` opens when the job starts and shows its state (waiting, listening n of m
#    seconds, converting with an estimated percentage), then the text in an
#    editable field with Copy and Close; Cancel / Escape while it works cancels the job.
#
# Nothing here runs on NVDA's main thread except speech and the dialog (queueHandler /
# wx.CallAfter); the worker waits on child processes with timeouts and can be cancelled (Alt+Shift+T
# again), so NVDA never freezes. The recording is deleted afterwards.

import ctypes
import json
import os
import re
import subprocess
import threading
import time
from ctypes import wintypes

import addonHandler
import api
import config
import gui
import queueHandler
import tones
import ui
import wx
from logHandler import log

from . import bridge, messageSettings, speechModel, voiceCache

addonHandler.initTranslation()

CONFIG_SECTION = "lineMR"
REQUEST_EVENT = r"Local\lineMR.transcribeRequest"
PLAY_EVENT = r"Local\lineMR.transcribePlay"
#: The bridge's command key (accessible_list.cpp: transcribeVoiceMessage()).
COMMAND_KEY = "control+alt+shift+f12"

#: Language choices in the settings: "nvda" = NVDA's language when Whisper knows it, "auto" =
#: Whisper detects it. Detection is slow (a 7 s message: 46-54 s instead of 10 s) and, with a
#: shortened window, made Whisper repeat one word for a minute: a fixed language is the default.
WHISPER_LANGUAGES = ("nvda", "auto", "th", "en", "ja", "zh")
#: Whisper works on 30 s windows; a shorter recording with a known language is given a shorter
#: window (audio context, 50 per second), which made a 7 s Thai message 3x faster with the same text.
_FULL_WINDOW_SECONDS = 30


def whisperLanguage(language=None):
	"""The language code passed to Whisper for a setting (default: the configured one)."""
	language = language or _conf()["transcribeLanguage"]
	if language == "nvda":
		import languageHandler

		code = languageHandler.getLanguage().split("_")[0].lower()
		return code if code in WHISPER_LANGUAGES[2:] else "auto"
	return language if language in WHISPER_LANGUAGES else "auto"

#: Recording limits. LINE's own limit for a voice message is a few minutes; a message of unknown
#: length is recorded until it has been silent for `_SILENCE` seconds, at most `_MAX_UNKNOWN`.
_MAX_UNKNOWN = 20 * 60
_SILENCE = 3.0
#: Seconds from the play request until LINE's sound starts (LINE loads the message first).
_START_ALLOWANCE = 8
#: A recognizer that prints nothing for this long is taken as hung (Whisper prints a segment per
#: 30 s window of audio; large models on slow computers need a few minutes for one). Slow models get
#: two windows' worth of their expected time (`_speed()`).
_ENGINE_SILENCE_TIMEOUT = 300

_kernel32 = ctypes.WinDLL("kernel32")
_kernel32.CreateEventW.argtypes = (ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR)
_kernel32.CreateEventW.restype = wintypes.HANDLE
_kernel32.SetEvent.argtypes = (wintypes.HANDLE,)
_kernel32.SetEvent.restype = wintypes.BOOL
_kernel32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
_kernel32.WaitForSingleObject.restype = wintypes.DWORD
_kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
_kernel32.CloseHandle.restype = wintypes.BOOL


class TranscriptionError(Exception):
	"""A problem the user is told about (the message is already translated)."""


def helperPath():
	return os.path.join(bridge._ADDON_ROOT, "bridge", "lineMR_audio.exe")


def _conf():
	return config.conf[CONFIG_SECTION]


# ---- Engines ------------------------------------------------------------------------------------


class _LineProcess:
	"""A child process whose stdout is read line by line (UTF-8) on the calling thread; stderr lines
	go to `onStderr` (on a reader thread) if given."""

	def __init__(self, args, onStderr=None):
		self.args = args
		self.proc = subprocess.Popen(
			args,
			stdin=subprocess.PIPE,
			stdout=subprocess.PIPE,
			stderr=subprocess.PIPE if onStderr else subprocess.DEVNULL,
			creationflags=subprocess.CREATE_NO_WINDOW,
		)
		self._lines = []
		self._cond = threading.Condition()
		self._ended = False
		threading.Thread(target=self._read, name="lineMR.transcribeReader", daemon=True).start()
		if onStderr:
			threading.Thread(target=self._readStderr, args=(onStderr,), name="lineMR.transcribeStderr", daemon=True).start()

	def _readStderr(self, onStderr):
		for raw in self.proc.stderr:
			onStderr(raw.decode("utf-8", "replace").rstrip("\r\n"))

	def _read(self):
		for raw in self.proc.stdout:
			with self._cond:
				self._lines.append(raw.decode("utf-8", "replace").rstrip("\r\n"))
				self._cond.notify_all()
		with self._cond:
			self._ended = True
			self._cond.notify_all()

	def lines(self, timeout, cancelled):
		"""Yields lines until the process ends; raises TimeoutError after `timeout` s without one."""
		while True:
			with self._cond:
				deadline = time.monotonic() + timeout
				while not self._lines and not self._ended:
					if cancelled():
						return
					left = deadline - time.monotonic()
					if left <= 0:
						raise TimeoutError
					self._cond.wait(min(left, 0.25))
				if not self._lines:
					return
				line = self._lines.pop(0)
			yield line

	def send(self, text):
		try:
			self.proc.stdin.write(text.encode("ascii") + b"\n")
			self.proc.stdin.flush()
		except OSError:
			pass

	def kill(self):
		if self.proc.poll() is None:
			try:
				self.proc.kill()
			except OSError:
				pass

	def wait(self, timeout=5):
		try:
			return self.proc.wait(timeout)
		except subprocess.TimeoutExpired:
			self.kill()
			return None


def enabled():
	return _conf()["transcribeVoiceMessages"]


def selectedModel(modelId=None):
	"""The speech model of a setting's value (default: the configured one)."""
	return speechModel.model(modelId or _conf()["transcribeModel"], _conf()["transcribeCustomModel"])


def ready(m=None):
	"""The program and the model `m` (default: the configured one) are on disk."""
	m = m or selectedModel()
	return speechModel.programInstalled() and m.downloaded()


def available():
	"""On and downloaded."""
	return enabled() and ready()


def unavailableMessage(m=None):
	"""Why Alt+Shift+T does nothing now (None when it works)."""
	if not enabled():
		# Translators: Reported when Alt+Shift+T is pressed on a message while voice message transcription is off.
		return _("Voice message transcription is off. Turn it on in NVDA's settings, category LINE MR.")
	m = m or selectedModel()
	download = speechModel.downloading()
	if download is not None and (download.model is None or download.model.id == m.id):
		# Translators: Reported when Alt+Shift+T is pressed while the speech model downloads ({percent} = how much is done).
		return _("The speech model for voice messages is still downloading ({percent}%).").format(
			percent=download.done * 100 // max(1, download.total)
		)
	if m.id == speechModel.CUSTOM and not m.downloaded():
		# Translators: Reported when the speech model file the user chose is gone ({path} = the file).
		return _("The speech model file {path} was not found. Choose another model in NVDA's settings, category LINE MR.").format(
			path=m.path
		)
	if not ready(m):
		# Translators: Reported when Alt+Shift+T is pressed but the speech model is missing (a download failed).
		return _("The speech model for voice messages is not downloaded yet. LINE MR tries again when NVDA starts.")
	return None


def retryDownload():
	"""Alt+Shift+T while the speech model is missing (a download failed): starts the download again.
	False when that is not the problem."""
	if enabled() and not ready() and speechModel.downloading() is None and selectedModel().id != speechModel.CUSTOM:
		applySettings()
		return True
	return False


def _onDownloaded():
	"""On the download thread: the files are complete."""
	_writeBridgeFlag()
	if enabled():
		_speak(
			# Translators: Spoken when the speech model for voice messages has been downloaded.
			_("Voice message transcription is ready. In a chat, press Alt+Shift+T on a voice message.")
		)


def _writeBridgeFlag():
	conf = _conf()
	messageSettings.writeSettings(conf["directChatSenderNames"], conf["senderOnlyWhenChanged"], available())


def applySettings(announce=True):
	"""After the setting changed or at NVDA start: downloads what is missing while it is on, tells the
	bridge whether to offer "Transcribe voice message"."""
	_writeBridgeFlag()
	if not enabled():
		return
	m = selectedModel()
	size = speechModel.downloadMB(m)
	if m.id == speechModel.CUSTOM:
		m = None  # the user's own file: only the program can be missing
	if speechModel.startDownload(m, _onDownloaded) and announce:
		_speak(
			# Translators: Spoken when the speech model download starts ({size} is in megabytes).
			_("Downloading the speech model for voice messages, {size} MB. LINE MR tells you when it is ready.").format(size=size)
		)


class WhisperEngine:
	def __init__(self, language=None, modelId=None):
		m = selectedModel(modelId)
		problem = unavailableMessage(m)
		if problem:
			raise TranscriptionError(problem)
		self.program, self.model = speechModel.programPath(), m.path
		self.modelId = m.id
		#: Shown in the transcript window.
		self.name = m.name
		self.language = whisperLanguage(language)

	def run(self, wav, cancelled, onProgress):
		"""Yields lines of text; onProgress(percent) as Whisper reports it (after each 30 s window)."""
		threads = max(1, min(8, (os.cpu_count() or 2) - 1))
		args = [self.program, "-m", self.model, "-f", wav, "-l", self.language, "-nt", "-np", "-pp", "-t", str(threads)]
		seconds = _wavSeconds(wav)
		if self.language != "auto" and 0 < seconds < _FULL_WINDOW_SECONDS - 1:
			args += ["-ac", str(int((seconds + 1) * 50))]
		try:
			proc = _LineProcess(args, lambda line: _whisperProgress(line, onProgress))
		except OSError:
			log.error("LINE MR: cannot start Whisper", exc_info=True)
			raise TranscriptionError(
				# Translators: Reported when the speech recognition program cannot be started.
				_("Speech recognition could not be started.")
			) from None
		try:
			timeout = max(_ENGINE_SILENCE_TIMEOUT, 2 * _FULL_WINDOW_SECONDS * _speed(self.modelId, self.language))
			for line in proc.lines(timeout, cancelled):
				text = line.strip()
				# Whisper's notes for non-speech ("[BLANK_AUDIO]", "[Music]", "(laughs)").
				if not text or (text[0] in "[(" and text[-1] in "])"):
					continue
				yield text
			code = proc.wait()
			if code not in (0, None) and not cancelled():
				log.warning("LINE MR: Whisper exited with %s: %s", code, args)
				raise TranscriptionError(
					# Translators: Reported when speech recognition stopped with an error.
					_("Speech recognition failed (exit code {code}).").format(code=code)
				)
		except TimeoutError:
			raise TranscriptionError(_engineTimeout()) from None
		finally:
			proc.kill()


_PROGRESS = re.compile(r"progress\s*=\s*(\d+)%")


def _whisperProgress(line, onProgress):
	# "whisper_print_progress_callback: progress =  68%" (-pp; may pass 100 with a shortened window).
	match = _PROGRESS.search(line)
	if match:
		onProgress(int(match.group(1)))


def _wavSeconds(path):
	"""Length of the recording (16 kHz mono 16-bit, 44-byte header), 0 if unknown."""
	try:
		return max(0, os.path.getsize(path) - 44) / 32000
	except OSError:
		return 0


def _engineTimeout():
	# Translators: Reported when speech recognition stopped making progress.
	return _("Speech recognition stopped responding and was cancelled.")


# ---- Progress estimate --------------------------------------------------------------------------
# Whisper reports progress only after each 30 s window, so a short message would show nothing until
# it is done. The window shows an estimate instead: seconds of work per second of audio, learnt on
# this computer (transcribe-speed.json), separately for a fixed language and for detection.

#: First guess per model: seconds of work per second of audio with a fixed language (detection about
#: doubles it), measured on an i5-1235U with 10 threads, model loading included (Turbo and Medium:
#: 8 s of Thai in 20-24 s, 36 s in 91 s; Large v3: 33 s and 587 s; Whisper's own models assumed alike).
_DEFAULT_SPEED = {
	"typhoon-medium": 3.0,
	"typhoon-turbo": 3.0,
	"typhoon-large": 16.0,
	"whisper-small": 1.0,
	"whisper-medium": 3.0,
	"whisper-turbo": 3.0,
}
#: Short messages cost about as much as this many seconds of audio (loading the model).
_MIN_AUDIO_SECONDS = 5


def _speedPath():
	return os.path.join(bridge.bridgeDataDir(), "transcribe-speed.json")


def _speedKey(modelId, language):
	return "%s/%s" % (modelId, "auto" if language == "auto" else "fixed")


def _speeds():
	"""{"<model>/fixed" or "<model>/auto": seconds per audio second} learnt so far."""
	speeds = {}
	try:
		with open(_speedPath(), encoding="utf-8") as f:
			for k, v in json.load(f).items():
				# The first version had only Typhoon Turbo and wrote "fixed" / "auto".
				k = k if "/" in k else "typhoon-turbo/" + k
				if float(v) > 0:
					speeds[k] = float(v)
	except (OSError, ValueError, TypeError, AttributeError):
		pass
	return speeds


def _speed(modelId, language):
	speeds = _speeds()
	key = _speedKey(modelId, language)
	if key in speeds:
		return speeds[key]
	fixed = _DEFAULT_SPEED.get(modelId, 3.0)
	return fixed * 2 if language == "auto" else fixed


def estimateSeconds(audioSeconds, modelId, language):
	return _speed(modelId, language) * max(_MIN_AUDIO_SECONDS, audioSeconds)


def _learnSpeed(audioSeconds, modelId, language, seconds):
	measured = seconds / max(_MIN_AUDIO_SECONDS, audioSeconds)
	speed = round(0.5 * _speed(modelId, language) + 0.5 * measured, 3)
	speeds = _speeds()
	speeds[_speedKey(modelId, language)] = speed
	try:
		with open(_speedPath(), "w", encoding="utf-8") as f:
			json.dump(speeds, f)
	except OSError:
		pass


# ---- The job ------------------------------------------------------------------------------------


def _signalPlay():
	event = _kernel32.CreateEventW(None, False, False, PLAY_EVENT)
	if event:
		_kernel32.SetEvent(event)
		_kernel32.CloseHandle(event)


def _speak(text):
	queueHandler.queueFunction(queueHandler.eventQueue, ui.message, text)


def _fail(text):
	def report():
		tones.beep(220, 150)
		ui.message(text)

	queueHandler.queueFunction(queueHandler.eventQueue, report)


class Job(threading.Thread):
	"""Records and transcribes one voice message. Its progress (`phase`, `recorded`, `progress`,
	`convertingSince`) is read by the window's timer; the result goes to the window."""

	WAITING, RECORDING, CONVERTING = range(3)

	def __init__(self, pid, seconds, language=None, modelId=None):
		super().__init__(name="lineMR.transcribe", daemon=True)
		self.pid = pid
		self.seconds = seconds
		self.language = language
		self.modelId = modelId
		self._cancelled = False
		self._capture = None
		self.phase = self.WAITING
		#: Seconds recorded so far.
		self.recorded = 0
		#: Whisper's own progress in percent (it reports after each 30 s window), or None.
		self.progress = None
		self.convertingSince = None
		#: Expected seconds of converting (estimateSeconds()).
		self.estimate = None

	def cancel(self):
		self._cancelled = True
		if self._capture:
			self._capture.send("stop")

	def cancelled(self):
		return self._cancelled

	def run(self):
		global _job
		wav = os.path.join(bridge.bridgeDataDir(), "transcribe-%d.wav" % os.getpid())
		try:
			engine = WhisperEngine(self.language, self.modelId)
			wx.CallAfter(TranscriptDialog.open, self)
			source = voiceCache.find(self.seconds)
			if source:
				log.info("LINE MR: transcribing LINE's cached copy of a %d s voice message", self.seconds)
			else:
				source = self._record(wav)
			if self._cancelled:
				return
			if source:
				self._decode(source, wav)
			lines = self._transcribe(engine, wav)
			if self._cancelled:
				return
			if not lines:
				# Translators: Reported when speech recognition found no words in a voice message.
				self._failed(_("No speech was recognised in the voice message."))
				return
			wx.CallAfter(TranscriptDialog.finished, self, "\n".join(lines), engine.name)
		except TranscriptionError as e:
			self._failed(str(e))
		except Exception:
			log.error("LINE MR: transcription failed", exc_info=True)
			# Translators: Reported when transcribing failed for an unexpected reason (details in NVDA's log).
			self._failed(_("Transcribing the voice message failed."))
		finally:
			if self._capture:
				self._capture.kill()
			try:
				os.remove(wav)
			except OSError:
				pass
			_job = None

	def _failed(self, text):
		_fail(text)
		wx.CallAfter(TranscriptDialog.failed, self, text)

	def _record(self, wav):
		if not os.path.isfile(helperPath()):
			# Translators: Reported when the add-on's audio helper program is missing.
			raise TranscriptionError(_("LINE MR's audio helper is missing. Reinstall the add-on."))
		os.makedirs(os.path.dirname(wav), exist_ok=True)
		if self.seconds > 0:
			# The known length: stop at the end, never at a pause inside the message.
			args = [str(self.seconds + _START_ALLOWANCE), "1.5", str(max(0, self.seconds - 1))]
		else:
			args = [str(_MAX_UNKNOWN), str(_SILENCE), "0"]
		self._capture = _LineProcess([helperPath(), "capture", str(self.pid), wav] + args)
		heard = False
		started = False
		# LINE downloads the message when it plays it: its file is better than the recording.
		cached = _NewVoiceFile(self.seconds)
		try:
			for line in self._capture.lines(10, self.cancelled):
				if started and cached.found() and not self._cancelled:
					log.info("LINE MR: LINE downloaded the voice message; transcribing its file")
					self._capture.send("stop")
					self._capture.kill()
					return cached.path
				kind, _sep, value = line.partition("\t")
				if kind == "ready":
					started = True
					self.phase = self.RECORDING
					_signalPlay()
					if self.seconds > 0:
						# Translators: Spoken when the add-on starts recording a voice message of known length.
						_speak(_("Transcribing a voice message of {seconds} seconds. Alt+Shift+T cancels.").format(seconds=self.seconds))
					else:
						# Translators: Spoken when the add-on starts recording a voice message.
						_speak(_("Transcribing the voice message. Alt+Shift+T cancels."))
				elif kind == "level":
					try:
						self.recorded = int(value)
					except ValueError:
						pass
				elif kind == "done":
					heard = value.endswith("\t1")
				elif kind == "error":
					log.warning("LINE MR: recording LINE's sound failed: %s", value)
					raise TranscriptionError(
						# Translators: Reported when LINE's sound cannot be recorded (needs Windows 10 version 2004 or later).
						_("LINE's sound cannot be recorded on this computer (Windows 10 version 2004 or later is needed).")
					)
		except TimeoutError:
			raise TranscriptionError(
				# Translators: Reported when recording LINE's sound did not start or stalled.
				_("Recording the voice message did not work.")
			) from None
		if self._cancelled:
			return
		if not started:
			raise TranscriptionError(_("Recording the voice message did not work."))
		if cached.found():
			return cached.path
		if not heard:
			raise TranscriptionError(
				# Translators: Reported when LINE played nothing while the add-on was recording.
				_("LINE played no sound. If the voice message was already playing, stop it and try again.")
			)
		return None

	def _decode(self, source, wav):
		"""LINE's .m4a copy to the WAV file Whisper reads."""
		os.makedirs(os.path.dirname(wav), exist_ok=True)
		proc = _LineProcess([helperPath(), "decode", source, wav])
		try:
			for line in proc.lines(60, self.cancelled):
				kind, _sep, value = line.partition("\t")
				if kind == "error":
					log.warning("LINE MR: decoding LINE's copy of the voice message failed: %s", value)
					raise TranscriptionError(
						# Translators: Reported when LINE's copy of a voice message cannot be read.
						_("LINE's copy of the voice message cannot be read.")
					)
		except TimeoutError:
			raise TranscriptionError(_("LINE's copy of the voice message cannot be read.")) from None
		finally:
			proc.kill()

	def _setProgress(self, percent):
		self.progress = max(0, min(99, percent))

	def _transcribe(self, engine, wav):
		audioSeconds = _wavSeconds(wav)
		self.estimate = estimateSeconds(audioSeconds, engine.modelId, engine.language)
		self.phase = self.CONVERTING
		self.convertingSince = time.monotonic()
		# Translators: Spoken when the recording is done and speech recognition starts.
		_speak(_("Converting to text..."))
		lines = list(engine.run(wav, self.cancelled, self._setProgress))
		if not self._cancelled:
			_learnSpeed(audioSeconds, engine.modelId, engine.language, time.monotonic() - self.convertingSince)
		return lines

	def percent(self):
		"""Converting progress: Whisper's when it has reported, else the estimate; at most 99."""
		estimated = 0
		if self.estimate and self.convertingSince:
			estimated = int((time.monotonic() - self.convertingSince) * 100 / self.estimate)
		return min(99, max(self.progress or 0, estimated))


class _NewVoiceFile:
	"""Watches LINE's cache for the file of the message being played (a new voice file of its
	length, complete: two looks a second apart see the same size)."""

	def __init__(self, seconds):
		self.seconds = seconds
		self.path = None
		self._known = self._paths()
		self._sizes = {}
		self._next = 0

	@staticmethod
	def _paths():
		paths = set()
		for dirpath, _dirs, files in os.walk(voiceCache.cacheDir()):
			paths.update(os.path.join(dirpath, name) for name in files)
		return paths

	def found(self):
		if self.path:
			return True
		if time.monotonic() < self._next:
			return False
		self._next = time.monotonic() + 0.5
		for path in self._paths() - self._known:
			length = voiceCache.duration(path)
			if length is None or (self.seconds > 0 and abs(length - self.seconds) > 1.2):
				continue
			try:
				size = os.path.getsize(path)
			except OSError:
				continue
			if self._sizes.get(path) == size:
				self.path = path
				return True
			self._sizes[path] = size
		return False


_job = None


def busy():
	return _job is not None


def cancel():
	"""Cancels the running transcription; False if none runs."""
	job = _job
	if job is None:
		return False
	job.cancel()
	# Translators: Reported when the user cancels a voice message transcription.
	ui.message(_("Transcription cancelled."))
	return True


def _readRequest():
	values = {}
	try:
		with open(os.path.join(bridge.bridgeDataDir(), "transcribe-request.txt"), encoding="utf-8") as f:
			for line in f:
				key, _sep, value = line.rstrip("\r\n").partition("\t")
				values[key] = value
	except OSError:
		log.debugWarning("LINE MR: cannot read the transcription request", exc_info=True)
	return values


def _onRequest():
	"""On NVDA's main thread: the bridge was asked to transcribe a message."""
	global _job
	request = _readRequest()
	status = request.get("status")
	if retryDownload():
		return
	problem = unavailableMessage()
	if problem:
		ui.message(problem)
		return
	if status == "notvoice":
		# Translators: Reported when Transcribe voice message is used on another kind of message.
		ui.message(_("Not a voice message."))
		return
	if status == "busy" or _job is not None:
		# Translators: Reported when a transcription is asked for while one runs.
		ui.message(_("A voice message is being transcribed. Alt+Shift+T cancels it."))
		return
	if status != "ok":
		return
	try:
		pid = int(request.get("pid", "0"))
		seconds = int(request.get("seconds", "-1"))
	except ValueError:
		return
	if _conf()["transcribeAskLanguage"]:
		LanguageDialog.ask(lambda language, modelId: _start(pid, seconds, language, modelId))
	else:
		_start(pid, seconds, None, None)


def _start(pid, seconds, language, modelId):
	global _job
	if _job is not None:
		return
	_job = Job(pid, seconds, language, modelId)
	_job.start()


class _RequestListener(threading.Thread):
	def __init__(self):
		super().__init__(name="lineMR.transcribeListener", daemon=True)
		self._stopping = False
		self._event = _kernel32.CreateEventW(None, False, False, REQUEST_EVENT)

	def run(self):
		if not self._event:
			log.error("LINE MR: cannot create the transcription request event")
			return
		while not self._stopping:
			if _kernel32.WaitForSingleObject(self._event, 1000) == 0 and not self._stopping:
				queueHandler.queueFunction(queueHandler.eventQueue, _onRequest)
		_kernel32.CloseHandle(self._event)

	def stop(self):
		self._stopping = True


_listener = None


def startListener():
	global _listener
	if _listener is None:
		_listener = _RequestListener()
		_listener.start()


def stopListener():
	global _listener
	if _listener:
		_listener.stop()
		_listener = None
	if _job:
		_job.cancel()


# ---- The transcript window ----------------------------------------------------------------------


class TranscriptDialog(wx.Dialog):
	"""Opens when a transcription starts: its state and progress while LINE plays the message and
	while it is converted, then the text in an editable field with Copy and Close. Cancel (Escape)
	while it works cancels the transcription."""

	_instance = None
	#: Seconds between the low "still converting" beeps.
	_BEEP_SECONDS = 3

	@classmethod
	def open(cls, job):
		if cls._instance:
			try:
				cls._instance.Destroy()
			except RuntimeError:
				pass
		gui.mainFrame.prePopup()
		cls._instance = dlg = cls(gui.mainFrame, job)
		dlg.Show()
		dlg.Raise()
		gui.mainFrame.postPopup()

	@classmethod
	def _forJob(cls, job):
		dlg = cls._instance
		return dlg if dlg and dlg.job is job else None

	@classmethod
	def finished(cls, job, text, engineName):
		dlg = cls._forJob(job)
		if dlg:
			dlg._showText(text, engineName)

	@classmethod
	def failed(cls, job, text):
		dlg = cls._forJob(job)
		if dlg:
			dlg._showFailure(text)

	def __init__(self, parent, job):
		# Translators: Title of the window that shows a voice message's text.
		super().__init__(parent, title=_("Voice message text"))
		self.job = job
		self._working = True
		self._lastBeep = time.monotonic()
		sizer = wx.BoxSizer(wx.VERTICAL)
		# Translators: Label of the state of a voice message transcription (read-only text).
		sizer.Add(wx.StaticText(self, label=_("&Status:")), flag=wx.LEFT | wx.TOP | wx.RIGHT, border=8)
		self.status = wx.TextCtrl(self, style=wx.TE_READONLY, size=(520, -1))
		sizer.Add(self.status, flag=wx.EXPAND | wx.LEFT | wx.RIGHT, border=8)
		self.gauge = wx.Gauge(self, range=100)
		sizer.Add(self.gauge, flag=wx.EXPAND | wx.ALL, border=8)
		self.textLabel = wx.StaticText(self)
		sizer.Add(self.textLabel, flag=wx.LEFT | wx.RIGHT, border=8)
		self.text = wx.TextCtrl(self, style=wx.TE_MULTILINE | wx.TE_RICH2, size=(520, 260))
		sizer.Add(self.text, proportion=1, flag=wx.EXPAND | wx.LEFT | wx.RIGHT, border=8)
		buttons = wx.BoxSizer(wx.HORIZONTAL)
		# Translators: Button that copies a voice message's text to the clipboard.
		self.copy = wx.Button(self, label=_("&Copy"))
		self.copy.Bind(wx.EVT_BUTTON, self._onCopy)
		buttons.Add(self.copy, flag=wx.RIGHT, border=8)
		self.closeButton = wx.Button(self, wx.ID_CANCEL)
		self.closeButton.Bind(wx.EVT_BUTTON, lambda evt: self.Close())
		buttons.Add(self.closeButton)
		sizer.Add(buttons, flag=wx.ALL | wx.ALIGN_RIGHT, border=8)
		self.SetEscapeId(wx.ID_CANCEL)
		self.Bind(wx.EVT_CLOSE, self._onClose)
		self.SetSizerAndFit(sizer)
		# The text and Copy appear with the result.
		for control in (self.textLabel, self.text, self.copy):
			control.Hide()
		self.Layout()
		self.CentreOnScreen()
		self._update()
		self.status.SetFocus()
		self._timer = wx.Timer(self)
		self.Bind(wx.EVT_TIMER, lambda evt: self._update(), self._timer)
		self._timer.Start(500)

	def _update(self):
		job = self.job
		if job.phase == Job.WAITING:
			# Translators: State of a voice message transcription while the message is fetched from LINE.
			status, percent = _("Getting the voice message from LINE..."), None
		elif job.phase == Job.RECORDING:
			if job.seconds > 0:
				# Translators: State of a voice message transcription while LINE plays the message ({done} and {total} are seconds).
				status = _("Listening to the voice message: {done} of {total} seconds").format(
					done=min(job.recorded, job.seconds), total=job.seconds
				)
				percent = min(99, job.recorded * 100 // job.seconds)
			else:
				# Translators: State of a voice message transcription while LINE plays a message of unknown length ({done} is seconds).
				status, percent = _("Listening to the voice message: {done} seconds").format(done=job.recorded), None
		else:
			percent = job.percent()
			# Translators: State of a voice message transcription while it is converted ({percent} = estimated
			# share done).
			status = _("Converting to text: about {percent}%").format(percent=percent)
			# NVDA's way of saying "still working": a short low beep every few seconds.
			if time.monotonic() - self._lastBeep >= self._BEEP_SECONDS:
				self._lastBeep = time.monotonic()
				tones.beep(330, 40)
		self._setStatus(status)
		if percent is None:
			self.gauge.Pulse()
		else:
			self.gauge.SetValue(percent)

	def _setStatus(self, text):
		if self.status.GetValue() != text:
			self.status.SetValue(text)

	def _stop(self):
		self._working = False
		self._timer.Stop()
		# Translators: Button that closes the voice message window once the work is done.
		self.closeButton.SetLabel(_("Close"))

	def _showText(self, text, engineName):
		self._stop()
		# Translators: State of a voice message transcription when the text is ready.
		self._setStatus(_("Done"))
		self.gauge.SetValue(100)
		# Translators: Label of the text field holding a voice message's text; says which engine made it.
		self.textLabel.SetLabel(_("&Text (made by {engine}, may contain mistakes):").format(engine=engineName))
		self.text.SetValue(text)
		for control in (self.textLabel, self.text, self.copy):
			control.Show()
		self.Fit()
		self.Layout()
		self.text.SetFocus()
		self.text.SetInsertionPoint(0)

	def _showFailure(self, text):
		self._stop()
		self._setStatus(text)
		self.gauge.SetValue(0)

	def _onCopy(self, evt):
		# The text as edited; NVDA reports the copy.
		api.copyToClip(self.text.GetValue(), notify=True)

	def _onClose(self, evt):
		self._timer.Stop()
		if self._working and _job is self.job:
			cancel()
		TranscriptDialog._instance = None
		self.Destroy()

class LanguageDialog(wx.Dialog):
	"""Asked before each transcription: the speech model and the message's language, and "Don't
	show this again" (all three are also in NVDA's settings, category LINE MR). The model chosen
	here becomes the setting; one not downloaded yet is downloaded, and Alt+Shift+T again transcribes the
	message once it is ready."""

	_instance = None

	@classmethod
	def ask(cls, onChosen):
		"""On NVDA's main thread; onChosen(language code, model id) after OK, nothing after Cancel."""
		if cls._instance:
			cls._instance.Raise()
			return
		gui.mainFrame.prePopup()
		cls._instance = dlg = cls(gui.mainFrame, onChosen)
		dlg.Show()
		dlg.Raise()
		gui.mainFrame.postPopup()

	def __init__(self, parent, onChosen):
		# Translators: Title of the dialog asked before a voice message is transcribed.
		super().__init__(parent, title=_("Transcribe voice message"))
		self._onChosen = onChosen
		sizer = wx.BoxSizer(wx.VERTICAL)
		# Translators: Label of the list of speech models asked before a voice message is transcribed.
		sizer.Add(wx.StaticText(self, label=_("Speech &model:")), flag=wx.LEFT | wx.TOP | wx.RIGHT, border=8)
		self._models = modelChoices(_conf()["transcribeCustomModel"])
		self.model = wx.Choice(self, choices=[modelLabel(m) for m in self._models])
		ids = [m.id for m in self._models]
		configured = selectedModel().id
		self.model.SetSelection(ids.index(configured) if configured in ids else 0)
		sizer.Add(self.model, flag=wx.EXPAND | wx.LEFT | wx.RIGHT, border=8)
		# Translators: Label of the list of languages asked before a voice message is transcribed.
		sizer.Add(wx.StaticText(self, label=_("&Language of the voice message:")), flag=wx.LEFT | wx.TOP | wx.RIGHT, border=8)
		self._codes = list(WHISPER_LANGUAGES)
		self.language = wx.Choice(self, choices=languageLabels())
		configured = _conf()["transcribeLanguage"]
		self.language.SetSelection(self._codes.index(configured) if configured in self._codes else self._codes.index("auto"))
		sizer.Add(self.language, flag=wx.EXPAND | wx.LEFT | wx.RIGHT, border=8)
		# Translators: Check box in the dialog asked before a voice message is transcribed.
		self.dontAsk = wx.CheckBox(self, label=_("&Don't show this again (change it in NVDA's settings, category LINE MR)"))
		sizer.Add(self.dontAsk, flag=wx.ALL, border=8)
		buttons = self.CreateStdDialogButtonSizer(wx.OK | wx.CANCEL)
		sizer.Add(buttons, flag=wx.ALL | wx.ALIGN_RIGHT, border=8)
		self.Bind(wx.EVT_BUTTON, self._onOk, id=wx.ID_OK)
		self.Bind(wx.EVT_BUTTON, lambda evt: self.Close(), id=wx.ID_CANCEL)
		self.Bind(wx.EVT_CLOSE, self._onClose)
		self.SetEscapeId(wx.ID_CANCEL)
		self.SetSizerAndFit(sizer)
		self.CentreOnScreen()
		self.model.SetFocus()

	def _onOk(self, evt):
		language = self._codes[max(0, self.language.GetSelection())]
		m = self._models[max(0, self.model.GetSelection())]
		conf = _conf()
		conf["transcribeModel"] = m.id
		if self.dontAsk.GetValue():
			conf["transcribeLanguage"] = language
			conf["transcribeAskLanguage"] = False
		self.Close()
		if not ready(m):
			problem = unavailableMessage(m)
			if not retryDownload() and problem:
				ui.message(problem)
			return
		self._onChosen(language, m.id)

	def _onClose(self, evt):
		LanguageDialog._instance = None
		self.Destroy()


def modelChoices(customPath):
	"""The models to choose from: speechModel.MODELS, then the user's own file if one was chosen."""
	models = list(speechModel.MODELS)
	if customPath:
		models.append(speechModel.CustomModel(customPath))
	return models


def modelLabel(m):
	"""A model's name and what it costs: "Typhoon Whisper Medium (best for Thai, 823 MB, downloaded)"."""
	if m.id == speechModel.CUSTOM:
		# Translators: A speech model choice: a model file the user chose ({name} = its file name).
		return _("Your model file: {name}").format(name=m.name)
	if m.id == "typhoon-medium":
		# Translators: Part of a speech model's name: it writes every language in Thai letters (Typhoon Medium).
		strength = _("Thai only")
	elif m.id == "typhoon-large":
		# Translators: Part of a speech model's name: writes every language in Thai letters, the most accurate for Thai, but many times slower (Typhoon Large v3).
		strength = _("Thai only, most accurate, very slow")
	elif m.id.startswith("typhoon"):
		# Translators: Part of a speech model's name: trained further on Thai (Typhoon models).
		strength = _("best for Thai")
	else:
		# Translators: Part of a speech model's name: OpenAI's general Whisper models.
		strength = _("many languages")
	if m.downloaded():
		# Translators: A downloaded speech model ({name}; {strength} = what it is good at; {size} in megabytes).
		return _("{name} ({strength}, {size} MB, downloaded)").format(name=m.name, strength=strength, size=m.megabytes)
	# Translators: A speech model not downloaded yet ({name}; {strength} = what it is good at; {size} in megabytes).
	return _("{name} ({strength}, {size} MB download)").format(name=m.name, strength=strength, size=m.megabytes)


def languageLabels():
	"""Labels of WHISPER_LANGUAGES, in that order."""
	return [
		# Translators: Language choice for voice messages: NVDA's own language.
		_("Same as NVDA"),
		# Translators: Language choice for voice messages: the speech recognizer detects it (slower).
		_("Detect automatically (slower)"),
		# Translators: A language of voice messages.
		_("Thai"),
		# Translators: A language of voice messages.
		_("English"),
		# Translators: A language of voice messages.
		_("Japanese"),
		# Translators: A language of voice messages.
		_("Chinese"),
	]
