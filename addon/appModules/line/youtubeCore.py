# What the YouTube download needs that does not depend on NVDA (tests/test_youtube_core.py):
# recognising YouTube addresses, reading yt-dlp's answers, the choices offered, yt-dlp's command line
# and its progress lines.

import re
import urllib.parse

_HOSTS = ("youtube.com", "youtu.be", "youtube-nocookie.com")
_ID = re.compile(r"^[A-Za-z0-9_-]{11}$")
_LIST = re.compile(r"^[A-Za-z0-9_-]{10,64}$")


def parseAddress(url):
	"""(video id or None, playlist id or None) of a YouTube address, or None if it is not one we
	download from (channels, search pages, other sites)."""
	try:
		parts = urllib.parse.urlsplit(url.strip())
	except ValueError:
		return None
	if parts.scheme not in ("http", "https"):
		return None
	host = (parts.hostname or "").lower()
	if not any(host == h or host.endswith("." + h) for h in _HOSTS):
		return None
	query = urllib.parse.parse_qs(parts.query)
	path = [p for p in parts.path.split("/") if p]
	video = None
	if host.endswith("youtu.be"):
		video = path[0] if path else None
	elif path and path[0] in ("shorts", "live", "embed", "v") and len(path) > 1:
		video = path[1]
	elif path[:1] == ["watch"] or not path:
		video = (query.get("v") or [None])[0]
	playlist = (query.get("list") or [None])[0]
	if video and not _ID.match(video):
		video = None
	if playlist and not _LIST.match(playlist):
		playlist = None
	# A mix ("RD...") is endless and made for the viewer: not offered as a playlist.
	if playlist and playlist.startswith("RD"):
		playlist = None
	if not video and not playlist:
		return None
	return video, playlist


def videoUrl(video):
	return "https://www.youtube.com/watch?v=" + video


def playlistUrl(playlist):
	return "https://www.youtube.com/playlist?list=" + playlist


class VideoInfo:
	"""What yt-dlp -J says about one video."""

	def __init__(self, doc):
		self.title = doc.get("title") or doc.get("id") or ""
		self.duration = doc.get("duration")
		self.isLive = bool(doc.get("is_live")) or doc.get("live_status") in ("is_live", "is_upcoming")
		self.formats = [f for f in doc.get("formats") or [] if isinstance(f, dict)]

	def heights(self):
		"""Video heights offered, highest first."""
		heights = {f.get("height") for f in self.formats if f.get("vcodec") not in (None, "none") and isinstance(f.get("height"), int)}
		return sorted((h for h in heights if h >= 144), reverse=True)

	def _size(self, f):
		return f.get("filesize") or f.get("filesize_approx") or 0

	def videoSize(self, height):
		"""Bytes of a download at `height` (best H.264 stream of that height, else any, plus the best
		audio), 0 if unknown."""
		video = [f for f in self.formats if f.get("height") == height and f.get("vcodec") not in (None, "none")]
		if not video:
			return 0
		h264 = [f for f in video if str(f.get("vcodec", "")).startswith("avc1")]
		v = max(h264 or video, key=self._size)
		if v.get("acodec") not in (None, "none"):
			return self._size(v)
		return self._size(v) + self.audioSize()

	def audioSize(self):
		audio = [f for f in self.formats if f.get("vcodec") == "none" and f.get("acodec") not in (None, "none")]
		return max((self._size(f) for f in audio), default=0)


class PlaylistInfo:
	"""What yt-dlp -J --flat-playlist says about a playlist."""

	def __init__(self, doc):
		self.title = doc.get("title") or doc.get("id") or ""
		entries = [e for e in doc.get("entries") or [] if isinstance(e, dict)]
		self.count = doc.get("playlist_count") or len(entries)
		self.duration = sum(e.get("duration") or 0 for e in entries)


#: Heights offered for a playlist (its videos differ: the best up to that height is taken).
PLAYLIST_HEIGHTS = (2160, 1440, 1080, 720, 480, 360)
AUDIO_KBPS = (320, 192, 128)
DEFAULT_HEIGHT = 1080
DEFAULT_KBPS = 192


def defaultHeight(heights, preferred=DEFAULT_HEIGHT):
	"""The preferred height, else the highest below it, else the lowest offered."""
	below = [h for h in heights if h <= preferred]
	return below[0] if below else (heights[-1] if heights else preferred)


#: Reasons yt-dlp gives, as (reason key, pattern in its ERROR line).
_ERRORS = (
	("private", re.compile(r"private video|members-only|join this channel", re.I)),
	("age", re.compile(r"confirm your age|age-restricted|inappropriate for some users", re.I)),
	("region", re.compile(r"not available in your country|geo.?restrict|blocked it in your country", re.I)),
	("live", re.compile(r"live event will begin|premieres in|is not live|this live event", re.I)),
	("signin", re.compile(r"sign in to confirm you.re not a bot|sign in to confirm", re.I)),
	("network", re.compile(r"unable to download (webpage|api page)|getaddrinfo|timed out|connection|network is unreachable|ssl", re.I)),
	("unavailable", re.compile(r"video unavailable|this video is unavailable|has been removed|account .*terminated|does not exist|not found|incomplete youtube id", re.I)),
)


def errorReason(stderr):
	"""(reason key, yt-dlp's last ERROR line) of a failed run; key "other" if not recognised."""
	lines = [line for line in stderr.splitlines() if line.startswith("ERROR:")]
	last = lines[-1][len("ERROR:"):].strip() if lines else ""
	for key, pattern in _ERRORS:
		if pattern.search(last):
			return key, last
	return "other", last


def downloadArgs(ytdlp, deno, ffmpegDir, cacheDir, folder, url, audio, height=None, kbps=DEFAULT_KBPS, playlist=False):
	"""yt-dlp's command line for one download."""
	args = [
		ytdlp,
		"--ignore-config",
		"--no-warnings",
		"--encoding", "utf-8",
		"--newline",
		"--progress",
		"--progress-template", "download:LMR %(progress.status)s %(progress._percent_str)s %(info.playlist_index)s %(info.n_entries)s",
		"--print", "after_move:FILE %(filepath)s",
		"--js-runtimes", "deno:" + deno,
		"--ffmpeg-location", ffmpegDir,
		"--cache-dir", cacheDir,
		"--windows-filenames",
		"--no-mtime",
		"-P", folder,
	]
	if playlist:
		# One folder per playlist, numbered in its order; an unavailable video does not stop the rest.
		args += ["--yes-playlist", "--ignore-errors", "-o", "%(playlist_title)s/%(playlist_index)03d %(title)s.%(ext)s"]
	else:
		args += ["--no-playlist", "-o", "%(title)s.%(ext)s"]
	if audio:
		args += ["-f", "ba/b", "-x", "--audio-format", "mp3", "--audio-quality", "%dK" % kbps]
	else:
		# Best up to the height, H.264 + AAC where YouTube has it (plays everywhere), always an MP4 file.
		sort = ("res:%d," % height if height else "res,") + "vcodec:h264,acodec:aac"
		args += ["-f", "bv*+ba/b", "-S", sort, "--merge-output-format", "mp4", "--remux-video", "mp4"]
	args.append(url)
	return args


def probeArgs(ytdlp, deno, cacheDir, url, playlist):
	args = [ytdlp, "--ignore-config", "--no-warnings", "--encoding", "utf-8", "-J", "--js-runtimes", "deno:" + deno, "--cache-dir", cacheDir]
	args += ["--flat-playlist", "--yes-playlist"] if playlist else ["--no-playlist"]
	args.append(url)
	return args


_PROGRESS = re.compile(r"^LMR (\S+)\s+([\d.]+)%\s+(\S+)\s+(\S+)")


def parseProgress(line):
	"""(status, percent, index or None, count or None) of a progress line, or None."""
	m = _PROGRESS.match(line)
	if not m:
		return None
	status, percent, index, count = m.groups()
	toInt = lambda s: int(s) if s.isdigit() else None
	return status, float(percent), toInt(index), toInt(count)


def isProcessingLine(line):
	"""yt-dlp's lines while FFmpeg joins the streams or makes the MP3."""
	return line.startswith(("[Merger]", "[ExtractAudio]", "[VideoRemuxer]", "[FixupM3u8]", "[VideoConvertor]"))
