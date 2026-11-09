"""Offline tests of the YouTube download's address, answer and command handling
(addon/appModules/line/youtubeCore.py) and of the module description check (youtubeModule.py).

Usage: python tests/test_youtube_core.py
"""

import importlib.util
import json
import sys
import types
import unittest
from pathlib import Path

LINE = Path(__file__).resolve().parents[1] / "addon" / "appModules" / "line"


def _load(name, extra=None):
	sys.modules.update(extra or {})
	spec = importlib.util.spec_from_file_location("yt_test." + name, LINE / (name + ".py"))
	module = importlib.util.module_from_spec(spec)
	spec.loader.exec_module(module)
	return module


core = _load("youtubeCore")
package = types.ModuleType("yt_test")
package.__path__ = [str(LINE)]
logging = types.ModuleType("logHandler")
logging.log = types.SimpleNamespace(info=lambda *a, **k: None, debugWarning=lambda *a, **k: None)
bridgeStub = types.ModuleType("yt_test.bridge")
bridgeStub.bridgeDataDir = lambda: "."
package.bridge = bridgeStub
moduleModule = _load("youtubeModule", {"yt_test": package, "yt_test.bridge": bridgeStub, "logHandler": logging})


class Addresses(unittest.TestCase):
	def test_videos(self):
		for url, expected in (
			("https://www.youtube.com/watch?v=jNQXAC9IVRw", ("jNQXAC9IVRw", None)),
			("https://youtu.be/jNQXAC9IVRw?si=abc", ("jNQXAC9IVRw", None)),
			("https://m.youtube.com/watch?feature=share&v=jNQXAC9IVRw", ("jNQXAC9IVRw", None)),
			("https://youtube.com/shorts/jNQXAC9IVRw?feature=share", ("jNQXAC9IVRw", None)),
			("https://www.youtube.com/live/jNQXAC9IVRw", ("jNQXAC9IVRw", None)),
			("https://music.youtube.com/watch?v=jNQXAC9IVRw", ("jNQXAC9IVRw", None)),
			("http://www.youtube-nocookie.com/embed/jNQXAC9IVRw", ("jNQXAC9IVRw", None)),
		):
			self.assertEqual(core.parseAddress(url), expected, url)

	def test_playlists(self):
		self.assertEqual(
			core.parseAddress("https://www.youtube.com/watch?v=jNQXAC9IVRw&list=PLBCF2DAC6FFB574DE&index=2"),
			("jNQXAC9IVRw", "PLBCF2DAC6FFB574DE"),
		)
		self.assertEqual(core.parseAddress("https://www.youtube.com/playlist?list=PLBCF2DAC6FFB574DE"), (None, "PLBCF2DAC6FFB574DE"))
		# A mix is endless: only the video.
		self.assertEqual(core.parseAddress("https://www.youtube.com/watch?v=jNQXAC9IVRw&list=RDjNQXAC9IVRw"), ("jNQXAC9IVRw", None))

	def test_not_youtube(self):
		for url in (
			"https://example.com/watch?v=jNQXAC9IVRw",
			"https://youtube.com.evil.example/watch?v=jNQXAC9IVRw",
			"https://www.youtube.com/@YouTube",
			"https://www.youtube.com/results?search_query=x",
			"https://www.youtube.com/watch?v=short",
			"ftp://youtube.com/watch?v=jNQXAC9IVRw",
			"not a url",
		):
			self.assertIsNone(core.parseAddress(url), url)


class Answers(unittest.TestCase):
	INFO = {
		"title": "Me at the zoo",
		"duration": 19,
		"formats": [
			{"height": 240, "vcodec": "avc1.4d400d", "acodec": "none", "filesize": 300000},
			{"height": 240, "vcodec": "vp09", "acodec": "none", "filesize": 200000},
			{"height": 144, "vcodec": "avc1", "acodec": "none", "filesize_approx": 100000},
			{"vcodec": "none", "acodec": "mp4a", "filesize": 50000},
			{"vcodec": "none", "acodec": "opus", "filesize": 60000},
			{"height": 90, "vcodec": "avc1", "acodec": "none"},
		],
	}

	def test_video(self):
		info = core.VideoInfo(self.INFO)
		self.assertEqual(info.heights(), [240, 144])
		self.assertEqual(info.videoSize(240), 360000)
		self.assertEqual(info.videoSize(144), 160000)
		self.assertFalse(info.isLive)
		self.assertTrue(core.VideoInfo({"live_status": "is_upcoming"}).isLive)

	def test_default_height(self):
		self.assertEqual(core.defaultHeight([2160, 1080, 720], 1080), 1080)
		self.assertEqual(core.defaultHeight([2160, 1440], 1080), 1440)
		self.assertEqual(core.defaultHeight([720, 360], 1080), 720)

	def test_playlist(self):
		info = core.PlaylistInfo({"title": "Stories", "entries": [{"duration": 90}, {"duration": None}, {}]})
		self.assertEqual((info.title, info.count, info.duration), ("Stories", 3, 90))

	def test_errors(self):
		self.assertEqual(core.errorReason("WARNING: x\nERROR: [youtube] abc: Private video. Sign in")[0], "private")
		self.assertEqual(core.errorReason("ERROR: [youtube] abc: Video unavailable")[0], "unavailable")
		self.assertEqual(core.errorReason("ERROR: [youtube] x: This video is unavailable")[0], "unavailable")
		self.assertEqual(core.errorReason("ERROR: [youtube] x: Sign in to confirm your age")[0], "age")
		self.assertEqual(core.errorReason("ERROR: Unable to download webpage: <urlopen error>")[0], "network")
		self.assertEqual(core.errorReason("ERROR: something new"), ("other", "something new"))


class Commands(unittest.TestCase):
	def test_video_args(self):
		args = core.downloadArgs("y.exe", "d.exe", "ff", "cache", "out", "URL", audio=False, height=720)
		self.assertIn("res:720,vcodec:h264,acodec:aac", args)
		self.assertIn("--no-playlist", args)
		self.assertEqual(args[-1], "URL")
		self.assertNotIn("-x", args)

	def test_audio_playlist_args(self):
		args = core.downloadArgs("y.exe", "d.exe", "ff", "cache", "out", "URL", audio=True, kbps=320, playlist=True)
		self.assertEqual(args[args.index("--audio-quality") + 1], "320K")
		self.assertIn("--yes-playlist", args)
		self.assertIn("--ignore-errors", args)

	def test_progress(self):
		self.assertEqual(core.parseProgress("LMR downloading  45.3% NA NA"), ("downloading", 45.3, None, None))
		self.assertEqual(core.parseProgress("LMR finished 100.0% 2 13"), ("finished", 100.0, 2, 13))
		self.assertIsNone(core.parseProgress("FILE C:\\x.mp4"))


class ModuleDescription(unittest.TestCase):
	def _doc(self, **change):
		base = "https://github.com/Peetchy/line-mr-for-win/releases/download/ytdl-v2026.11.09/"
		components = [
			{"id": cid, "version": "1.0", "file": cid + "-1.0.zip", "url": base + cid + "-1.0.zip", "sha256": "a" * 64, "size": 5000}
			for cid in ("yt-dlp", "deno", "ffmpeg")
		]
		components[0].update(change)
		return json.dumps({"schema": 1, "version": "2026.11.09", "components": components})

	def test_good(self):
		module = moduleModule.parseModule(self._doc())
		self.assertEqual(sorted(module["components"]), ["deno", "ffmpeg", "yt-dlp"])

	def test_rejected(self):
		for change in (
			{"url": "https://evil.example/yt-dlp-1.0.zip"},
			{"url": "https://github.com/Peetchy/line-mr-for-win/releases/download/v0.6.0/yt-dlp-1.0.zip"},
			{"sha256": "xyz"},
			{"file": "../yt-dlp.zip"},
			{"id": "other"},
			{"size": 10},
		):
			with self.assertRaises(ValueError, msg=str(change)):
				moduleModule.parseModule(self._doc(**change))

	def test_built_module_json(self):
		# The description build_module.py writes is accepted.
		module = {
			"schema": 1,
			"version": "2026.11.09",
			"components": json.loads(self._doc())["components"],
		}
		moduleModule.parseModule(json.dumps(module, indent="\t"))


if __name__ == "__main__":
	unittest.main()
