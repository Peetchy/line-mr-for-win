# The YouTube download module: yt-dlp, Deno (runs YouTube's JavaScript challenges for yt-dlp) and
# FFmpeg (joins video and audio, makes MP3). Not part of the add-on: it is released on its own as
# "ytdl-v<date>" in this repository (modules/youtube/build_module.py, made every day a component has
# a new version by .github/workflows/youtube-module.yml) and downloaded the first time the user
# downloads from YouTube.
#
# Before each download `ensure()` looks for the newest module release (at most every
# CHECK_INTERVAL; GitHub allows 60 unauthenticated API requests an hour) and updates the components
# whose SHA-256 changed, so yt-dlp follows YouTube's changes without an add-on update. Offline, an
# installed module is used as it is.
#
# Files: %LOCALAPPDATA%\LineMR\youtube\<component>\ (removed with the add-on, installTasks.py),
# installed.json = {component id: {version, sha256}}, check.json = time and module.json of the last
# check. A component is unpacked into <id>.new and then swapped in, so a failed update leaves the old
# one working.

import hashlib
import json
import os
import re
import shutil
import threading
import time
import urllib.request
import zipfile

from logHandler import log

from . import bridge

REPO = "Peetchy/line-mr-for-win"
RELEASES_URL = "https://api.github.com/repos/%s/releases?per_page=30" % REPO
TAG_PREFIX = "ytdl-v"
#: Where components may be downloaded from (this repository's module releases only).
URL_PREFIX = "https://github.com/%s/releases/download/%s" % (REPO, TAG_PREFIX)
#: Seconds between two checks for a newer module (user decision: at most every 3 hours).
CHECK_INTERVAL = 3 * 3600
TIMEOUT = 30
#: The program of each component, relative to its folder.
PROGRAMS = {"yt-dlp": "yt-dlp.exe", "deno": "deno.exe", "ffmpeg": "ffmpeg.exe"}

_ID = re.compile(r"^(yt-dlp|deno|ffmpeg)$")
_VERSION = re.compile(r"^[0-9A-Za-z._-]{1,40}$")
_FILE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,99}\.zip$")
_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_lock = threading.Lock()


class ModuleError(Exception):
	"""`reason`: "offline" (no module and no connection), "damaged" (checksum / unpacking), "notFound"
	(no module release published)."""

	def __init__(self, reason, detail=""):
		super().__init__("%s: %s" % (reason, detail))
		self.reason = reason


class Cancelled(Exception):
	pass


def directory():
	return os.path.join(bridge.bridgeDataDir(), "youtube")


def programPath(cid):
	return os.path.join(directory(), cid, PROGRAMS[cid])


def parseModule(text):
	"""The components of a module.json (text); raises ValueError for anything unexpected."""
	doc = json.loads(text)
	if not isinstance(doc, dict) or doc.get("schema") != 1 or not isinstance(doc.get("components"), list):
		raise ValueError("not a module description of schema 1")
	version = doc.get("version")
	if not (isinstance(version, str) and _VERSION.match(version)):
		raise ValueError("bad module version")
	components = {}
	for c in doc["components"]:
		if not isinstance(c, dict):
			raise ValueError("component is not an object")
		cid, file, url, sha256, size = c.get("id"), c.get("file"), c.get("url"), c.get("sha256"), c.get("size")
		if not (isinstance(cid, str) and _ID.match(cid) and cid not in components):
			raise ValueError("bad component id %r" % (cid,))
		if not (isinstance(c.get("version"), str) and _VERSION.match(c["version"])):
			raise ValueError("bad version of %s" % cid)
		if not (isinstance(file, str) and _FILE.match(file)):
			raise ValueError("bad file of %s" % cid)
		if not (isinstance(url, str) and url.startswith(URL_PREFIX) and url.endswith("/" + file)):
			raise ValueError("bad url of %s" % cid)
		if not (isinstance(sha256, str) and _SHA256.match(sha256)):
			raise ValueError("bad sha256 of %s" % cid)
		if not (isinstance(size, int) and 1000 <= size <= 1_000_000_000):
			raise ValueError("bad size of %s" % cid)
		components[cid] = {"version": c["version"], "file": file, "url": url, "sha256": sha256, "size": size}
	if set(components) != set(PROGRAMS):
		raise ValueError("components missing: %s" % ", ".join(sorted(set(PROGRAMS) - set(components))))
	return {"version": version, "components": components}


def _readJson(name):
	try:
		with open(os.path.join(directory(), name), encoding="utf-8") as f:
			return json.load(f)
	except (OSError, ValueError):
		return None


def _writeJson(name, value):
	os.makedirs(directory(), exist_ok=True)
	path = os.path.join(directory(), name)
	with open(path + ".tmp", "w", encoding="utf-8") as f:
		json.dump(value, f, indent="\t")
	os.replace(path + ".tmp", path)


def installed():
	"""{component id: {version, sha256}} of the components on disk (whose program exists)."""
	data = _readJson("installed.json")
	if not isinstance(data, dict):
		return {}
	return {cid: v for cid, v in data.items() if cid in PROGRAMS and isinstance(v, dict) and os.path.isfile(programPath(cid))}


def complete():
	return set(installed()) == set(PROGRAMS)


def installedVersion():
	"""The yt-dlp version on disk (what the settings show), or None."""
	c = installed().get("yt-dlp")
	return c.get("version") if c else None


def sizeOnDisk():
	total = 0
	for root, _dirs, files in os.walk(directory()):
		for name in files:
			try:
				total += os.path.getsize(os.path.join(root, name))
			except OSError:
				pass
	return total


def _request(url, accept=None):
	headers = {"User-Agent": "lineMR-nvda-addon"}
	if accept:
		headers["Accept"] = accept
	return urllib.request.Request(url, headers=headers)


def _fetchLatest():
	"""The newest module release's parsed module.json (the API lists releases newest first)."""
	with urllib.request.urlopen(_request(RELEASES_URL, "application/vnd.github+json"), timeout=10) as response:
		releases = json.load(response)
	for release in releases:
		if release.get("draft") or not str(release.get("tag_name", "")).startswith(TAG_PREFIX):
			continue
		asset = next((a for a in release.get("assets", []) if a.get("name") == "module.json"), None)
		if asset is None:
			continue
		url = asset.get("browser_download_url", "")
		if not url.startswith(URL_PREFIX):
			continue
		with urllib.request.urlopen(_request(url), timeout=TIMEOUT) as response:
			data = response.read(64 * 1024 + 1)
		if len(data) > 64 * 1024:
			raise ValueError("module.json too large")
		return parseModule(data.decode("utf-8"))
	raise ModuleError("notFound")


def latest(force=False):
	"""The newest module (from the last check if it was less than CHECK_INTERVAL ago, unless
	`force`), or None if it cannot be looked up (offline, GitHub's limit)."""
	check = _readJson("check.json")
	if not force and isinstance(check, dict) and isinstance(check.get("module"), dict):
		age = time.time() - check.get("time", 0)
		if 0 <= age < CHECK_INTERVAL:
			try:
				return parseModule(json.dumps(check["module"]))
			except ValueError:
				pass
	try:
		module = _fetchLatest()
	except ModuleError:
		raise
	except Exception as e:
		log.debugWarning("LINE MR: YouTube module release not looked up: %s", e)
		return None
	doc = {
		"schema": 1,
		"version": module["version"],
		"components": [dict(c, id=cid) for cid, c in module["components"].items()],
	}
	try:
		_writeJson("check.json", {"time": time.time(), "module": doc})
	except OSError:
		pass
	return module


def needed(module):
	"""The component ids of `module` that are missing or differ from the installed ones."""
	have = installed()
	return [cid for cid, c in module["components"].items() if have.get(cid, {}).get("sha256") != c["sha256"]]


def ensure(onProgress=None, cancelled=lambda: False):
	"""Makes the newest module available (downloads or updates what changed). onProgress(state, done,
	total) with state "checking" / "downloading" / "installing". Returns (updated component ids,
	module version or None when offline with an installed module). Raises ModuleError, Cancelled."""
	with _lock:
		if onProgress:
			onProgress("checking", 0, 0)
		module = latest()
		if module is None:
			if complete():
				log.info("LINE MR: YouTube module not checked (offline?); using the installed one")
				return [], None
			raise ModuleError("offline")
		todo = needed(module)
		if not todo:
			return [], module["version"]
		total = sum(module["components"][cid]["size"] for cid in todo)
		done = 0
		for cid in todo:
			c = module["components"][cid]
			zipPath = _download(c, lambda n, base=done: onProgress and onProgress("downloading", base + n, total), cancelled)
			if onProgress:
				onProgress("installing", done + c["size"], total)
			_install(cid, c, zipPath)
			done += c["size"]
		log.info("LINE MR: YouTube module %s installed (%s)", module["version"], ", ".join(
			"%s %s" % (cid, module["components"][cid]["version"]) for cid in todo))
		return todo, module["version"]


def _download(c, onBytes, cancelled):
	"""Downloads a component zip to <file>.part (resuming one cut off earlier) and checks it."""
	os.makedirs(directory(), exist_ok=True)
	part = os.path.join(directory(), c["file"] + ".part")
	have = os.path.getsize(part) if os.path.isfile(part) else 0
	if have > c["size"]:
		os.remove(part)
		have = 0
	digest = hashlib.sha256()
	if have:
		with open(part, "rb") as f:
			while chunk := f.read(1 << 20):
				digest.update(chunk)
	try:
		if have < c["size"]:
			request = _request(c["url"])
			if have:
				request.add_header("Range", "bytes=%d-" % have)
			with urllib.request.urlopen(request, timeout=TIMEOUT) as response:
				if have and response.status != 206:
					have = 0
					digest = hashlib.sha256()
				with open(part, "ab" if have else "wb") as f:
					while chunk := response.read(1 << 16):
						if cancelled():
							raise Cancelled
						f.write(chunk)
						digest.update(chunk)
						have += len(chunk)
						onBytes(have)
	except Cancelled:
		raise
	except OSError as e:
		raise ModuleError("offline", str(e))
	if digest.hexdigest() != c["sha256"]:
		os.remove(part)
		raise ModuleError("damaged", "checksum mismatch: " + c["file"])
	return part


def _install(cid, c, zipPath):
	"""Unpacks a component next to the old one, then swaps the folders."""
	target = os.path.join(directory(), cid)
	new, old = target + ".new", target + ".old"
	shutil.rmtree(new, ignore_errors=True)
	try:
		with zipfile.ZipFile(zipPath) as z:
			for name in z.namelist():
				# Flat zips of the module's own build; anything with a path is not ours.
				if "/" in name or "\\" in name or name.startswith("."):
					raise ModuleError("damaged", "unexpected file " + name)
			z.extractall(new)
	except zipfile.BadZipFile as e:
		raise ModuleError("damaged", str(e))
	if not os.path.isfile(os.path.join(new, PROGRAMS[cid])):
		raise ModuleError("damaged", PROGRAMS[cid] + " missing")
	shutil.rmtree(old, ignore_errors=True)
	if os.path.isdir(target):
		os.replace(target, old)
	os.replace(new, target)
	shutil.rmtree(old, ignore_errors=True)
	os.remove(zipPath)
	have = _readJson("installed.json")
	have = have if isinstance(have, dict) else {}
	have[cid] = {"version": c["version"], "sha256": c["sha256"]}
	_writeJson("installed.json", have)


def remove():
	"""Deletes the module (asked in the settings)."""
	with _lock:
		shutil.rmtree(directory(), ignore_errors=True)
