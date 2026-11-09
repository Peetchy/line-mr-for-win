# Download from YouTube: a video (MP4) or its sound (MP3) from a YouTube link in a LINE message,
# also a whole playlist (user request 2026-11-09).
#
# 1. The bridge (message menu item "Download video from YouTube", or Alt+Shift+Y in the message list,
#    which sends it the private key Control+Alt+Shift+F5) finds the message's YouTube address
#    (LINE's own message text, else the drawn lines), writes %LOCALAPPDATA%\LineMR\youtube-request.txt
#    (status, url) and signals Local\lineMR.youtubeRequest.
# 2. A notice says to download only what the user may download (YouTube's terms); it shows every time
#    until "Don't show this again" (user decision).
# 3. `YouTubeDialog` opens and a worker makes the download module ready (youtubeModule.py: downloads
#    it the first time, updates yt-dlp & co. when a newer module is released), then asks yt-dlp about
#    the address (is it available? title, length, qualities; the playlist too if the address has one).
# 4. The dialog offers: this video or the whole playlist, Video (MP4) or Audio (MP3), the quality
#    (with the size where YouTube says it), the folder (Downloads; remembered). Download runs
#    yt-dlp with a progress line per step; Cancel stops it and removes the partial files.
#
# Nothing runs on NVDA's main thread except speech and the dialog; child processes have no window.

import ctypes
import os
import subprocess
import threading
import time
from ctypes import wintypes

import addonHandler
import config
import gui
import queueHandler
import tones
import ui
import wx
from logHandler import log

from . import bridge, youtubeCore, youtubeModule

addonHandler.initTranslation()

CONFIG_SECTION = "lineMR"
REQUEST_EVENT = r"Local\lineMR.youtubeRequest"
#: The bridge's command key (a11y_event_filter.cpp: Key_F5 -> LineAccessibleList::downloadYouTube()).
COMMAND_KEY = "control+alt+shift+f5"
#: Seconds yt-dlp may take to answer about an address.
_PROBE_TIMEOUT = 90
#: Seconds without any output before a download is taken as stuck.
_STALL_TIMEOUT = 300

_kernel32 = ctypes.WinDLL("kernel32")
_kernel32.CreateEventW.argtypes = (ctypes.c_void_p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR)
_kernel32.CreateEventW.restype = wintypes.HANDLE
_kernel32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
_kernel32.WaitForSingleObject.restype = wintypes.DWORD
_kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
_kernel32.CloseHandle.restype = wintypes.BOOL

_shell32 = ctypes.WinDLL("shell32")
_ole32 = ctypes.WinDLL("ole32")


class _GUID(ctypes.Structure):
	_fields_ = [("Data1", wintypes.DWORD), ("Data2", wintypes.WORD), ("Data3", wintypes.WORD), ("Data4", ctypes.c_ubyte * 8)]


_shell32.SHGetKnownFolderPath.argtypes = (ctypes.POINTER(_GUID), wintypes.DWORD, wintypes.HANDLE, ctypes.POINTER(ctypes.c_wchar_p))
_shell32.SHGetKnownFolderPath.restype = ctypes.HRESULT
_ole32.CoTaskMemFree.argtypes = (ctypes.c_void_p,)
_ole32.CoTaskMemFree.restype = None


def downloadsFolder():
	"""The user's Downloads folder (FOLDERID_Downloads; it may have been moved to another drive)."""
	guid = _GUID(0x374DE290, 0x123F, 0x4565, (ctypes.c_ubyte * 8)(0x91, 0x64, 0x39, 0xC4, 0x92, 0x5E, 0x46, 0x7B))
	path = ctypes.c_wchar_p()
	try:
		_shell32.SHGetKnownFolderPath(ctypes.byref(guid), 0, None, ctypes.byref(path))
		result = path.value
		_ole32.CoTaskMemFree(path)
		if result:
			return result
	except OSError:
		pass
	return os.path.join(os.path.expanduser("~"), "Downloads")


def _conf():
	return config.conf[CONFIG_SECTION]


def enabled():
	return _conf()["youtubeDownload"]


def _speak(text):
	queueHandler.queueFunction(queueHandler.eventQueue, ui.message, text)


def _readRequest():
	values = {}
	try:
		with open(os.path.join(bridge.bridgeDataDir(), "youtube-request.txt"), encoding="utf-8") as f:
			for line in f:
				key, _sep, value = line.rstrip("\r\n").partition("\t")
				values[key] = value
	except OSError:
		log.debugWarning("LINE MR: cannot read the YouTube request", exc_info=True)
	return values


def _onRequest():
	"""On NVDA's main thread: the bridge looked at the message."""
	request = _readRequest()
	status = request.get("status")
	if status == "nolink":
		# Translators: Reported when Download from YouTube is used on a message without a YouTube link.
		ui.message(_("This message has no YouTube link."))
	elif status == "busy":
		# Translators: Reported when the message could not be looked at (LINE was busy); pressing again usually works.
		ui.message(_("LINE is busy. Press Alt+Shift+Y again."))
	elif status == "ok" and request.get("url"):
		# Not from inside NVDA's queue processing: dialogs there freeze NVDA's core (see saveMessage.py).
		wx.CallAfter(start, request["url"])


def start(url):
	"""Starts a download from `url` (on NVDA's main thread): the notice, then the dialog."""
	if YouTubeDialog.instance:
		# One at a time: the running one comes to the front.
		YouTubeDialog.instance.Raise()
		# Translators: Reported when a YouTube download is asked for while the download window is open.
		ui.message(_("The YouTube download window is already open."))
		return
	address = youtubeCore.parseAddress(url)
	if address is None:
		ui.message(_("This message has no YouTube link."))
		return
	if _conf()["youtubeShowNotice"]:
		NoticeDialog.ask(lambda: YouTubeDialog.open(address))
	else:
		YouTubeDialog.open(address)


class _RequestListener(threading.Thread):
	def __init__(self):
		super().__init__(name="lineMR.youtubeListener", daemon=True)
		self._stopping = False
		self._event = _kernel32.CreateEventW(None, False, False, REQUEST_EVENT)

	def run(self):
		if not self._event:
			log.error("LINE MR: cannot create the YouTube request event")
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
	dlg = YouTubeDialog.instance
	if dlg and dlg.worker:
		dlg.worker.cancel()


# ---- Child processes ----------------------------------------------------------------------------


def _popen(args):
	return subprocess.Popen(
		args,
		stdin=subprocess.DEVNULL,
		stdout=subprocess.PIPE,
		stderr=subprocess.PIPE,
		creationflags=subprocess.CREATE_NO_WINDOW,
	)


def _killTree(proc):
	"""yt-dlp starts FFmpeg and Deno: the whole tree goes."""
	if proc.poll() is not None:
		return
	try:
		subprocess.run(
			["taskkill", "/T", "/F", "/PID", str(proc.pid)],
			stdout=subprocess.DEVNULL,
			stderr=subprocess.DEVNULL,
			creationflags=subprocess.CREATE_NO_WINDOW,
			timeout=10,
		)
	except (OSError, subprocess.SubprocessError):
		pass
	try:
		proc.kill()
	except OSError:
		pass


def _programs():
	d = youtubeModule.directory()
	return (
		youtubeModule.programPath("yt-dlp"),
		youtubeModule.programPath("deno"),
		os.path.join(d, "ffmpeg"),
		os.path.join(d, "cache"),
	)


class _Cancelled(Exception):
	pass


class _Failed(Exception):
	"""A problem the user is told about (translated)."""


def failureText(reason, detail=""):
	"""Words for youtubeCore.errorReason() keys and youtubeModule.ModuleError reasons."""
	texts = {
		# Translators: Reported when a YouTube video cannot be downloaded because it is private or for members only.
		"private": _("This video is private or for channel members only."),
		# Translators: Reported when a YouTube video needs a signed-in, adult account.
		"age": _("This video is age-restricted and cannot be downloaded without signing in."),
		# Translators: Reported when a YouTube video is blocked in the user's country.
		"region": _("This video is not available in your country."),
		# Translators: Reported for a live stream or a premiere that has not started.
		"live": _("Live streams and premieres cannot be downloaded."),
		# Translators: Reported when YouTube refuses the download and asks to sign in.
		"signin": _("YouTube refused the download for now (it asks to sign in). Try again later."),
		# Translators: Reported when YouTube or GitHub cannot be reached.
		"network": _("YouTube could not be reached. Check the internet connection."),
		# Translators: Reported when a YouTube video was removed or does not exist.
		"unavailable": _("This video is unavailable: it was removed, or the link is wrong."),
		# Translators: Reported when the download module could not be downloaded (no connection).
		"offline": _("The YouTube download module could not be downloaded. Check the internet connection."),
		# Translators: Reported when the downloaded download module was damaged.
		"damaged": _("The YouTube download module was damaged while downloading. Try again."),
		# Translators: Reported when no download module has been published yet.
		"notFound": _("The YouTube download module is not available yet. Try again later."),
	}
	if reason in texts:
		return texts[reason]
	# Translators: Reported when a YouTube download failed for another reason ({detail}: yt-dlp's message, in English).
	return _("The download failed: {detail}").format(detail=detail or "?")


class _Worker(threading.Thread):
	"""Prepares (module, address) or downloads, reporting to the dialog through wx.CallAfter."""

	def __init__(self, dialog, target, *args):
		super().__init__(name="lineMR.youtube", daemon=True)
		self.dialog = dialog
		self._target = target
		self._args = args
		self._cancelled = False
		self._proc = None

	def cancel(self):
		self._cancelled = True
		proc = self._proc
		if proc is not None:
			_killTree(proc)

	def cancelled(self):
		return self._cancelled

	def _post(self, method, *args):
		wx.CallAfter(lambda: self.dialog and getattr(self.dialog, method)(self, *args))

	def run(self):
		try:
			self._target(self, *self._args)
		except (_Cancelled, youtubeModule.Cancelled):
			log.info("LINE MR: YouTube download cancelled")
		except _Failed as e:
			self._post("failed", str(e))
		except youtubeModule.ModuleError as e:
			log.warning("LINE MR: YouTube module not ready: %s", e)
			self._post("failed", failureText(e.reason))
		except Exception as e:
			log.error("LINE MR: YouTube download failed", exc_info=True)
			self._post("failed", failureText("other", str(e)))

	# -- preparing -------------------------------------------------------------------------------

	def prepare(self, address):
		def progress(state, done, total):
			self._post("moduleProgress", state, done, total)

		updated, version = youtubeModule.ensure(progress, self.cancelled)
		if updated:
			log.info("LINE MR: YouTube module updated to %s", version)
		video, playlist = address
		self._post("checkingLink")
		results = {}
		threads = []
		if video:
			threads.append(threading.Thread(target=self._probe, args=(youtubeCore.videoUrl(video), False, results), daemon=True))
		if playlist:
			threads.append(threading.Thread(target=self._probe, args=(youtubeCore.playlistUrl(playlist), True, results), daemon=True))
		for t in threads:
			t.start()
		for t in threads:
			t.join()
		if self.cancelled():
			raise _Cancelled
		info = results.get(False)
		playlistInfo = results.get(True)
		if isinstance(info, Exception) and not isinstance(playlistInfo, youtubeCore.PlaylistInfo):
			raise info
		if not isinstance(info, youtubeCore.VideoInfo):
			info = None
		if not isinstance(playlistInfo, youtubeCore.PlaylistInfo) or playlistInfo.count < 1:
			if info is None:
				raise playlistInfo if isinstance(playlistInfo, Exception) else _Failed(failureText("unavailable"))
			playlistInfo = None
		if info and info.isLive and not playlistInfo:
			raise _Failed(failureText("live"))
		self._post("showOptions", info, playlistInfo)

	def _probe(self, url, playlist, results):
		ytdlp, deno, _ffmpeg, cache = _programs()
		proc = _popen(youtubeCore.probeArgs(ytdlp, deno, cache, url, playlist))
		killer = threading.Timer(_PROBE_TIMEOUT, _killTree, (proc,))
		killer.start()
		try:
			out, err = proc.communicate()
		finally:
			killer.cancel()
		if self.cancelled():
			return
		if proc.returncode != 0:
			reason, detail = youtubeCore.errorReason(err.decode("utf-8", "replace"))
			log.info("LINE MR: YouTube address not available (%s): %s", reason, detail)
			results[playlist] = _Failed(failureText(reason, detail))
			return
		import json

		try:
			doc = json.loads(out.decode("utf-8"))
		except ValueError:
			results[playlist] = _Failed(failureText("other", "no answer"))
			return
		if playlist:
			results[True] = youtubeCore.PlaylistInfo(doc) if doc.get("_type") == "playlist" else None
		else:
			results[False] = youtubeCore.VideoInfo(doc)

	# -- downloading -----------------------------------------------------------------------------

	def download(self, url, folder, audio, height, kbps, playlist):
		ytdlp, deno, ffmpeg, cache = _programs()
		os.makedirs(folder, exist_ok=True)
		before = set(_listParts(folder))
		args = youtubeCore.downloadArgs(ytdlp, deno, ffmpeg, cache, folder, url, audio, height, kbps, playlist)
		log.info("LINE MR: YouTube download started (%s, %s)", "playlist" if playlist else "video", "mp3" if audio else "mp4 %s" % height)
		self._proc = proc = _popen(args)
		errors = []
		threading.Thread(target=lambda: errors.extend(l.decode("utf-8", "replace") for l in proc.stderr), daemon=True).start()
		lines = []
		cond = threading.Condition()

		def read():
			for raw in proc.stdout:
				with cond:
					lines.append(raw.decode("utf-8", "replace").rstrip("\r\n"))
					cond.notify_all()
			with cond:
				lines.append(None)
				cond.notify_all()

		threading.Thread(target=read, daemon=True).start()
		files = []
		last = time.monotonic()
		while True:
			with cond:
				while not lines:
					if self.cancelled():
						break
					if time.monotonic() - last > _STALL_TIMEOUT:
						_killTree(proc)
						raise _Failed(failureText("network"))
					cond.wait(0.25)
				line = lines.pop(0) if lines else None
			if self.cancelled():
				_killTree(proc)
				_removeParts(folder, before)
				raise _Cancelled
			if line is None:
				break
			last = time.monotonic()
			if line.startswith("FILE "):
				files.append(line[5:].strip())
				self._post("fileDone", len(files))
				continue
			progress = youtubeCore.parseProgress(line)
			if progress:
				self._post("downloadProgress", *progress)
			elif youtubeCore.isProcessingLine(line):
				self._post("processing")
		proc.wait()
		stderr = "".join(errors)
		if not files:
			_removeParts(folder, before)
			reason, detail = youtubeCore.errorReason(stderr)
			raise _Failed(failureText(reason, detail))
		# A playlist goes on past unavailable videos: they are counted.
		failed = sum(1 for l in stderr.splitlines() if l.startswith("ERROR:")) if playlist else 0
		self._post("finished", files, failed)


def _listParts(folder):
	"""yt-dlp's partial files in the folder and its sub-folders (a playlist gets one)."""
	for root, _dirs, names in os.walk(folder):
		for name in names:
			if name.endswith((".part", ".ytdl")) or ".part-Frag" in name or ".temp." in name:
				yield os.path.join(root, name)


def _removeParts(folder, before):
	for path in set(_listParts(folder)) - before:
		try:
			os.remove(path)
		except OSError:
			pass


# ---- Dialogs ------------------------------------------------------------------------------------


class NoticeDialog(wx.Dialog):
	"""YouTube's terms allow downloading only where YouTube offers it or the owner agrees: said before
	each download until "Don't show this again"."""

	_instance = None

	@classmethod
	def ask(cls, proceed):
		if cls._instance:
			cls._instance.Raise()
			return
		gui.mainFrame.prePopup()
		cls._instance = dlg = cls(gui.mainFrame, proceed)
		dlg.Show()
		dlg.Raise()
		gui.mainFrame.postPopup()

	def __init__(self, parent, proceed):
		# Translators: Title of the notice shown before a download from YouTube.
		super().__init__(parent, title=_("Download from YouTube"))
		self._proceed = proceed
		sizer = wx.BoxSizer(wx.VERTICAL)
		text = _(
			# Translators: Notice shown before a download from YouTube.
			"Download only videos you have the right to download: your own, ones the owner allows you to "
			"download, or ones under a free licence. YouTube's terms of service do not allow downloading other "
			"videos. You are responsible for how you use what you download."
		)
		# A read-only field, so NVDA reads the whole notice and it can be reviewed line by line.
		self.text = wx.TextCtrl(self, value=text, style=wx.TE_MULTILINE | wx.TE_READONLY, size=(460, 110))
		sizer.Add(self.text, flag=wx.EXPAND | wx.ALL, border=8)
		# Translators: Check box of the notice shown before a download from YouTube.
		self.dontShow = wx.CheckBox(self, label=_("&Don't show this again"))
		sizer.Add(self.dontShow, flag=wx.LEFT | wx.RIGHT, border=8)
		buttons = wx.BoxSizer(wx.HORIZONTAL)
		# Translators: Button of the notice shown before a download from YouTube.
		ok = wx.Button(self, wx.ID_OK, label=_("&Continue"))
		ok.SetDefault()
		buttons.Add(ok, flag=wx.RIGHT, border=8)
		buttons.Add(wx.Button(self, wx.ID_CANCEL))
		sizer.Add(buttons, flag=wx.ALL | wx.ALIGN_RIGHT, border=8)
		self.Bind(wx.EVT_BUTTON, self._onOk, id=wx.ID_OK)
		self.Bind(wx.EVT_BUTTON, lambda evt: self.Close(), id=wx.ID_CANCEL)
		self.Bind(wx.EVT_CLOSE, self._onClose)
		self.SetEscapeId(wx.ID_CANCEL)
		self.SetSizerAndFit(sizer)
		self.CentreOnScreen()
		self.text.SetFocus()

	def _onOk(self, evt):
		if self.dontShow.GetValue():
			_conf()["youtubeShowNotice"] = False
		proceed = self._proceed
		self.Close()
		proceed()

	def _onClose(self, evt):
		NoticeDialog._instance = None
		self.Destroy()


def _megabytes(size):
	return max(1, round(size / 1_000_000)) if size else 0


def _duration(seconds):
	seconds = int(seconds or 0)
	h, rest = divmod(seconds, 3600)
	m, s = divmod(rest, 60)
	return "%d:%02d:%02d" % (h, m, s) if h else "%d:%02d" % (m, s)


class YouTubeDialog(wx.Dialog):
	"""One window from start to end: the module and the address are checked (state + gauge), then the
	choices, then the download's progress, then where the files are (Open folder). Cancel / Escape
	stops whatever runs."""

	instance = None
	#: Seconds between the low "still working" beeps while nothing measurable happens.
	_BEEP_SECONDS = 3

	@classmethod
	def open(cls, address):
		gui.mainFrame.prePopup()
		cls.instance = dlg = cls(gui.mainFrame, address)
		dlg.Show()
		dlg.Raise()
		gui.mainFrame.postPopup()
		dlg.worker = _Worker(dlg, _Worker.prepare, address)
		dlg.worker.start()

	def __init__(self, parent, address):
		# Translators: Title of the window that downloads from YouTube.
		super().__init__(parent, title=_("Download from YouTube"))
		self.address = address
		self.worker = None
		self.files = []
		self._info = None
		self._playlist = None
		self._percent = None
		self._lastBeep = time.monotonic()
		self._lastTone = -1
		self._folder = _conf()["youtubeFolder"] or downloadsFolder()
		sizer = wx.BoxSizer(wx.VERTICAL)
		# Translators: Label of the state of a download from YouTube (read-only text).
		sizer.Add(wx.StaticText(self, label=_("&Status:")), flag=wx.LEFT | wx.TOP | wx.RIGHT, border=8)
		self.status = wx.TextCtrl(self, style=wx.TE_READONLY, size=(480, -1))
		sizer.Add(self.status, flag=wx.EXPAND | wx.LEFT | wx.RIGHT, border=8)
		self.gauge = wx.Gauge(self, range=100)
		sizer.Add(self.gauge, flag=wx.EXPAND | wx.ALL, border=8)

		# The choices, shown once the address is checked.
		self.options = wx.Panel(self)
		grid = wx.BoxSizer(wx.VERTICAL)
		# Translators: Label of the title of the YouTube video (read-only text).
		grid.Add(wx.StaticText(self.options, label=_("&Title:")))
		self.title = wx.TextCtrl(self.options, style=wx.TE_READONLY)
		grid.Add(self.title, flag=wx.EXPAND | wx.BOTTOM, border=8)
		# Translators: Label of the choice between the linked video and its whole playlist.
		self.whatLabel = wx.StaticText(self.options, label=_("&Download:"))
		grid.Add(self.whatLabel)
		self.what = wx.Choice(self.options)
		self.what.Bind(wx.EVT_CHOICE, lambda evt: self._fillQualities())
		grid.Add(self.what, flag=wx.EXPAND | wx.BOTTOM, border=8)
		# Translators: Label of the choice between video and audio in the YouTube download window.
		grid.Add(wx.StaticText(self.options, label=_("&Format:")))
		self.format = wx.Choice(
			self.options,
			# Translators: Formats a YouTube video can be downloaded in.
			choices=[_("Video (MP4)"), _("Audio only (MP3)")],
		)
		self.format.SetSelection(1 if _conf()["youtubeFormat"] == "mp3" else 0)
		self.format.Bind(wx.EVT_CHOICE, lambda evt: self._fillQualities())
		grid.Add(self.format, flag=wx.EXPAND | wx.BOTTOM, border=8)
		# Translators: Label of the quality (resolution or bit rate) in the YouTube download window.
		grid.Add(wx.StaticText(self.options, label=_("&Quality:")))
		self.quality = wx.Choice(self.options)
		grid.Add(self.quality, flag=wx.EXPAND | wx.BOTTOM, border=8)
		# Translators: Label of the folder the YouTube download is saved to (read-only text).
		grid.Add(wx.StaticText(self.options, label=_("Save &in:")))
		row = wx.BoxSizer(wx.HORIZONTAL)
		self.folder = wx.TextCtrl(self.options, value=self._folder, style=wx.TE_READONLY)
		row.Add(self.folder, proportion=1, flag=wx.RIGHT, border=8)
		# Translators: Button that chooses the folder for YouTube downloads.
		browse = wx.Button(self.options, label=_("&Browse..."))
		browse.Bind(wx.EVT_BUTTON, self._onBrowse)
		row.Add(browse)
		grid.Add(row, flag=wx.EXPAND)
		self.options.SetSizer(grid)
		sizer.Add(self.options, flag=wx.EXPAND | wx.ALL, border=8)

		buttons = wx.BoxSizer(wx.HORIZONTAL)
		# Translators: Button that starts the download from YouTube.
		self.downloadButton = wx.Button(self, label=_("Down&load"))
		self.downloadButton.Bind(wx.EVT_BUTTON, self._onDownload)
		buttons.Add(self.downloadButton, flag=wx.RIGHT, border=8)
		# Translators: Button that opens the folder with the downloaded files.
		self.openFolder = wx.Button(self, label=_("&Open folder"))
		self.openFolder.Bind(wx.EVT_BUTTON, self._onOpenFolder)
		buttons.Add(self.openFolder, flag=wx.RIGHT, border=8)
		self.closeButton = wx.Button(self, wx.ID_CANCEL)
		self.closeButton.Bind(wx.EVT_BUTTON, lambda evt: self.Close())
		buttons.Add(self.closeButton)
		sizer.Add(buttons, flag=wx.ALL | wx.ALIGN_RIGHT, border=8)
		self.SetEscapeId(wx.ID_CANCEL)
		self.Bind(wx.EVT_CLOSE, self._onClose)
		self.SetSizer(sizer)
		self.options.Hide()
		self.downloadButton.Hide()
		self.openFolder.Hide()
		self.Fit()
		self.CentreOnScreen()
		# Translators: State of a download from YouTube while it starts.
		self._setStatus(_("Checking the download module..."))
		self.status.SetFocus()
		self._timer = wx.Timer(self)
		self.Bind(wx.EVT_TIMER, lambda evt: self._tick(), self._timer)
		self._timer.Start(500)

	# -- worker reports (main thread) ----------------------------------------------------------

	def _current(self, worker):
		return bool(self) and worker is self.worker

	def moduleProgress(self, worker, state, done, total):
		if not self._current(worker):
			return
		if state == "downloading" and total:
			percent = done * 100 // total
			# Translators: State while the YouTube download module is downloaded ({done}/{total} in megabytes).
			self._setStatus(_("Downloading the YouTube download module: {done} of {total} MB").format(
				done=done // 1_000_000, total=max(1, total // 1_000_000)))
			self._setPercent(percent)
		elif state == "installing":
			# Translators: State while the downloaded YouTube download module is unpacked.
			self._setStatus(_("Installing the YouTube download module..."))
			self._setPercent(None)

	def checkingLink(self, worker):
		if not self._current(worker):
			return
		# Translators: State of a download from YouTube while the link is checked.
		self._setStatus(_("Checking the link..."))
		self._setPercent(None)

	def showOptions(self, worker, info, playlist):
		if not self._current(worker):
			return
		self.worker = None
		self._info, self._playlist = info, playlist
		self._setPercent(0)
		self.what.Clear()
		if info:
			# Translators: Choice in the YouTube download window: only the video the link points to ({length} e.g. 3:25).
			self.what.Append(_("This video only ({length})").format(length=_duration(info.duration)) if info.duration else _("This video only"))
		if playlist:
			# Translators: Choice in the YouTube download window: every video of the link's playlist.
			self.what.Append(_("Whole playlist: {title} ({count} videos)").format(title=playlist.title, count=playlist.count))
		self.what.SetSelection(0)
		show = self.what.GetCount() > 1
		self.whatLabel.Show(show)
		self.what.Show(show)
		self.title.SetValue(info.title if info else playlist.title)
		self._fillQualities()
		self.options.Show()
		self.downloadButton.Show()
		self.downloadButton.SetDefault()
		# Translators: State of a download from YouTube when the link is fine and the choices show.
		self._setStatus(_("Ready to download. Choose the format and quality."))
		self.Fit()
		(self.what if show else self.format).SetFocus()

	def downloadProgress(self, worker, status, percent, index, count):
		if not self._current(worker):
			return
		if count and index:
			overall = ((index - 1) + percent / 100) * 100 / count
			# Translators: State of a playlist download ({index} of {count} videos; {percent} of this video).
			text = _("Downloading video {index} of {count}: {percent}%").format(index=index, count=count, percent=int(percent))
		else:
			overall = percent
			# Translators: State of a download from YouTube ({percent} done).
			text = _("Downloading: {percent}%").format(percent=int(percent))
		if status == "finished" and not count:
			# Translators: State of a download from YouTube while FFmpeg joins video and sound or makes the MP3.
			text = _("Processing...")
		self._setStatus(text)
		self._setPercent(int(overall))

	def processing(self, worker):
		if self._current(worker):
			self._setStatus(_("Processing..."))

	def fileDone(self, worker, count):
		pass

	def finished(self, worker, files, failed):
		if not self._current(worker):
			return
		self.worker = None
		self.files = files
		self._stop()
		self.gauge.SetValue(100)
		if len(files) == 1:
			# Translators: State when a YouTube download is saved ({name}: the file's name).
			text = _("Saved as {name}").format(name=os.path.basename(files[0]))
		else:
			# Translators: State when a playlist download is saved ({count} files).
			text = _("Saved {count} files").format(count=len(files))
		if failed:
			# Translators: Added when some videos of a playlist could not be downloaded ({count}).
			text += " " + _("{count} videos could not be downloaded.").format(count=failed)
		self._setStatus(text)
		self.openFolder.Show()
		self.Layout()
		self.Fit()
		tones.beep(880, 80)
		ui.message(text)
		self.openFolder.SetFocus()

	def failed(self, worker, text):
		if not self._current(worker):
			return
		self.worker = None
		self._stop()
		self.options.Hide()
		self.downloadButton.Hide()
		self.gauge.SetValue(0)
		self._setStatus(text)
		self.Fit()
		ui.message(text)
		self.status.SetFocus()

	# -- choices ------------------------------------------------------------------------------

	def _wholePlaylist(self):
		return self._playlist is not None and (self._info is None or self.what.GetSelection() == 1)

	def _audio(self):
		return self.format.GetSelection() == 1

	def _fillQualities(self):
		conf = _conf()
		self.quality.Clear()
		self._qualities = []
		if self._audio():
			for kbps in youtubeCore.AUDIO_KBPS:
				size = (self._info.duration or 0) * kbps * 125 if self._info and not self._wholePlaylist() else 0
				self._qualities.append(kbps)
				self.quality.Append(self._qualityLabel("%d kbps" % kbps, size))
			preferred = conf["youtubeAudioKbps"]
			self.quality.SetSelection(self._qualities.index(preferred) if preferred in self._qualities else 1)
			return
		if self._wholePlaylist() or not self._info:
			heights = list(youtubeCore.PLAYLIST_HEIGHTS)
		else:
			heights = self._info.heights() or list(youtubeCore.PLAYLIST_HEIGHTS)
		for h in heights:
			size = self._info.videoSize(h) if self._info and not self._wholePlaylist() else 0
			self._qualities.append(h)
			self.quality.Append(self._qualityLabel("%dp" % h, size))
		chosen = youtubeCore.defaultHeight(heights, conf["youtubeVideoHeight"])
		self.quality.SetSelection(self._qualities.index(chosen) if chosen in self._qualities else 0)

	def _qualityLabel(self, name, size):
		if size:
			# Translators: A quality in the YouTube download window with its size ({size} in megabytes).
			return _("{quality} (about {size} MB)").format(quality=name, size=_megabytes(size))
		return name

	def _onBrowse(self, evt):
		with wx.DirDialog(
			self,
			# Translators: Title of the dialog choosing where YouTube downloads go.
			_("Choose the folder for YouTube downloads"),
			defaultPath=self._folder,
			style=wx.DD_DEFAULT_STYLE,
		) as dialog:
			if dialog.ShowModal() != wx.ID_OK:
				return
			self._folder = dialog.GetPath()
		self.folder.SetValue(self._folder)

	def _onDownload(self, evt):
		if self.worker:
			return
		conf = _conf()
		audio = self._audio()
		quality = self._qualities[max(0, self.quality.GetSelection())]
		conf["youtubeFormat"] = "mp3" if audio else "mp4"
		if audio:
			conf["youtubeAudioKbps"] = quality
		else:
			conf["youtubeVideoHeight"] = quality
		conf["youtubeFolder"] = "" if self._folder == downloadsFolder() else self._folder
		whole = self._wholePlaylist()
		video, playlist = self.address
		url = youtubeCore.playlistUrl(playlist) if whole else youtubeCore.videoUrl(video)
		self.options.Hide()
		self.downloadButton.Hide()
		self.Fit()
		# Translators: State of a download from YouTube when it starts.
		self._setStatus(_("Starting the download..."))
		self._setPercent(0)
		self._lastTone = -1
		self.status.SetFocus()
		self.worker = _Worker(self, _Worker.download, url, self._folder, audio, None if audio else quality,
			quality if audio else youtubeCore.DEFAULT_KBPS, whole)
		self.worker.start()

	def _onOpenFolder(self, evt):
		target = os.path.dirname(self.files[0]) if self.files else self._folder
		try:
			if len(self.files) == 1 and os.path.isfile(self.files[0]):
				# Explorer with the file selected.
				subprocess.Popen(["explorer", "/select,", self.files[0]])
			else:
				os.startfile(target)
		except OSError:
			log.warning("LINE MR: cannot open %s", target, exc_info=True)

	# -- state ------------------------------------------------------------------------------

	def _setStatus(self, text):
		if self.status.GetValue() != text:
			self.status.SetValue(text)

	def _setPercent(self, percent):
		self._percent = percent
		if percent is None:
			self.gauge.Pulse()
			return
		self.gauge.SetValue(max(0, min(100, percent)))
		# NVDA's progress bar beeps: higher as it fills, every 5 %.
		step = percent // 5
		if step != self._lastTone:
			self._lastTone = step
			tones.beep(110 * 2 ** (percent / 25.0), 40)

	def _tick(self):
		if self.worker is None:
			return
		if self._percent is None:
			self.gauge.Pulse()
			if time.monotonic() - self._lastBeep >= self._BEEP_SECONDS:
				self._lastBeep = time.monotonic()
				tones.beep(330, 40)

	def _stop(self):
		self._timer.Stop()
		# Translators: Button that closes the YouTube download window once the work is done.
		self.closeButton.SetLabel(_("Close"))

	def _onClose(self, evt):
		self._timer.Stop()
		worker = self.worker
		self.worker = None
		if worker:
			worker.cancel()
			# Translators: Reported when a download from YouTube is cancelled.
			ui.message(_("Download cancelled."))
		YouTubeDialog.instance = None
		self.Destroy()
