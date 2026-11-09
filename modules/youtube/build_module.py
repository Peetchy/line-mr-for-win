"""Builds LINE MR's YouTube download module (a release "ytdl-v<date>" of this repository).

The module is three programs, each packed as a zip of its own so that the add-on downloads only the
ones that changed (yt-dlp changes every week or two, FFmpeg and Deno rarely):

  yt-dlp  github.com/yt-dlp/yt-dlp        yt-dlp.exe (PyInstaller build, yt-dlp-ejs inside), Unlicense
  deno    github.com/denoland/deno        deno.exe, MIT: yt-dlp runs YouTube's JavaScript challenges in it
  ffmpeg  github.com/BtbN/FFmpeg-Builds   ffmpeg.exe + ffprobe.exe + DLLs of the newest release branch,
                                          LGPL build (libmp3lame included): joins video and audio
                                          streams, makes MP3

Each program is downloaded from its project's release and checked against the SHA-256 that release
publishes. module.json lists the components (id, version, file, url, sha256, size) and is what the
add-on reads (addon/appModules/line/youtubeModule.py).

With --previous (the module.json of the last module release), a component whose upstream version is
unchanged is not rebuilt: its zip is copied from the previous release (--previous-dir), so its
SHA-256 stays the same and users do not download it again. If nothing changed, the script writes
changed=false to $GITHUB_OUTPUT and builds nothing (unless --force).

Usage (the workflow .github/workflows/youtube-module.yml runs it):
  python modules/youtube/build_module.py --tag ytdl-v2026.11.09 --out out
      [--previous prev/module.json --previous-dir prev] [--force]
"""

import argparse
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile

REPO = "Peetchy/line-mr-for-win"
RELEASE_BASE = "https://github.com/%s/releases/download/" % REPO
SCHEMA = 1


def _request(url, accept=None):
	headers = {"User-Agent": "lineMR-module-build"}
	if accept:
		headers["Accept"] = accept
	token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
	if token and url.startswith("https://api.github.com/"):
		headers["Authorization"] = "Bearer " + token
	return urllib.request.Request(url, headers=headers)


def fetch(url, accept=None):
	with urllib.request.urlopen(_request(url, accept), timeout=120) as response:
		return response.read()


def api(path):
	return json.loads(fetch("https://api.github.com/" + path, "application/vnd.github+json"))


def sha256(data):
	return hashlib.sha256(data).hexdigest()


def download(url, expected):
	data = fetch(url)
	if sha256(data) != expected.lower():
		raise SystemExit("checksum mismatch: %s" % url)
	print("  downloaded %s (%d bytes, checksum ok)" % (url, len(data)))
	return data


def _hexAfter(text, name=None):
	"""The first SHA-256 in `text` (on the line naming `name`, if given)."""
	for line in text.splitlines():
		if name and name not in line:
			continue
		m = re.search(r"\b[0-9a-fA-F]{64}\b", line)
		if m:
			return m.group(0).lower()
	raise SystemExit("no checksum for %s" % (name or "file"))


# ---- Upstream versions ------------------------------------------------------------------------


def latestYtDlp():
	release = api("repos/yt-dlp/yt-dlp/releases/latest")
	tag = release["tag_name"]
	assets = {a["name"]: a["browser_download_url"] for a in release["assets"]}
	return {"id": "yt-dlp", "version": tag, "url": assets["yt-dlp.exe"], "sums": assets["SHA2-256SUMS"]}


def latestDeno():
	release = api("repos/denoland/deno/releases/latest")
	tag = release["tag_name"]
	name = "deno-x86_64-pc-windows-msvc.zip"
	assets = {a["name"]: a["browser_download_url"] for a in release["assets"]}
	return {"id": "deno", "version": tag.lstrip("v"), "tag": tag, "url": assets[name], "sums": assets[name + ".sha256sum"]}


_FFMPEG_ASSET = re.compile(r"^ffmpeg-n(\d+)\.(\d+)(?:\.(\d+))?-\d+-g[0-9a-f]+-win64-lgpl-shared-\d+\.\d+\.zip$")


def latestFfmpeg():
	"""The newest release branch's LGPL shared build of BtbN's newest dated build (the "latest" tag
	is rolling and its files change daily). Its version is the branch's point release (n9.0.2): the
	daily rebuilds of the same point release do not make a new component."""
	for release in api("repos/BtbN/FFmpeg-Builds/releases?per_page=10"):
		if not release["tag_name"].startswith("autobuild-"):
			continue
		best = None
		for asset in release["assets"]:
			m = _FFMPEG_ASSET.match(asset["name"])
			if not m:
				continue
			key = tuple(int(x or 0) for x in m.groups())
			if best is None or key > best[0]:
				best = (key, asset)
		if best is None:
			continue
		sums = next(a["browser_download_url"] for a in release["assets"] if a["name"] == "checksums.sha256")
		key, asset = best
		return {
			"id": "ffmpeg",
			"version": "%d.%d.%d" % key,
			"name": asset["name"],
			"url": asset["browser_download_url"],
			"sums": sums,
			"build": release["tag_name"],
		}
	raise SystemExit("no FFmpeg build found")


# ---- Packing ----------------------------------------------------------------------------------


def _zip(files):
	"""A zip (bytes) of {name in the zip: bytes}."""
	out = io.BytesIO()
	with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
		for name, data in sorted(files.items()):
			z.writestr(name, data)
	return out.getvalue()


def packYtDlp(c):
	exe = download(c["url"], _hexAfter(fetch(c["sums"]).decode(), "yt-dlp.exe"))
	license = fetch("https://raw.githubusercontent.com/yt-dlp/yt-dlp/%s/LICENSE" % c["version"])
	return {"yt-dlp.exe": exe, "LICENSE.txt": license}


def packDeno(c):
	data = download(c["url"], _hexAfter(fetch(c["sums"]).decode("utf-8", "replace")))
	with zipfile.ZipFile(io.BytesIO(data)) as z:
		exe = z.read("deno.exe")
	license = fetch("https://raw.githubusercontent.com/denoland/deno/%s/LICENSE.md" % c["tag"])
	return {"deno.exe": exe, "LICENSE.txt": license}


def packFfmpeg(c):
	data = download(c["url"], _hexAfter(fetch(c["sums"]).decode(), c["name"]))
	files = {}
	with zipfile.ZipFile(io.BytesIO(data)) as z:
		for info in z.infolist():
			parts = info.filename.split("/")
			# <top>/bin/{ffmpeg,ffprobe}.exe and *.dll; <top>/LICENSE.txt. Not ffplay, headers, import libs.
			if len(parts) == 3 and parts[1] == "bin" and (parts[2] in ("ffmpeg.exe", "ffprobe.exe") or parts[2].endswith(".dll")):
				files[parts[2]] = z.read(info)
			elif len(parts) == 2 and parts[1] == "LICENSE.txt":
				files["LICENSE.txt"] = z.read(info)
	if "ffmpeg.exe" not in files or "ffprobe.exe" not in files:
		raise SystemExit("ffmpeg.exe / ffprobe.exe missing in " + c["name"])
	return files


PACKERS = {"yt-dlp": (latestYtDlp, packYtDlp), "deno": (latestDeno, packDeno), "ffmpeg": (latestFfmpeg, packFfmpeg)}


def smokeTest(cid, files, workDir):
	"""Runs the program once (only possible on Windows): a broken build must not reach users."""
	if os.name != "nt":
		print("  (not on Windows: %s not run)" % cid)
		return
	folder = os.path.join(workDir, "test-" + cid)
	shutil.rmtree(folder, ignore_errors=True)
	os.makedirs(folder)
	for name, data in files.items():
		with open(os.path.join(folder, name), "wb") as f:
			f.write(data)
	exe = {"yt-dlp": "yt-dlp.exe", "deno": "deno.exe", "ffmpeg": "ffmpeg.exe"}[cid]
	args = {"yt-dlp": ["--version"], "deno": ["--version"], "ffmpeg": ["-hide_banner", "-encoders"]}[cid]
	result = subprocess.run([os.path.join(folder, exe)] + args, capture_output=True, text=True, timeout=120)
	if result.returncode != 0:
		raise SystemExit("%s does not run: %s" % (exe, result.stderr[-500:]))
	# MP3 needs LAME in the build.
	if cid == "ffmpeg" and "libmp3lame" not in result.stdout:
		raise SystemExit("this FFmpeg build has no MP3 encoder (libmp3lame)")
	print("  %s runs: %s" % (exe, result.stdout.strip().splitlines()[0][:80]))
	shutil.rmtree(folder, ignore_errors=True)


def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("--tag", required=True, help="release tag, ytdl-v<yyyy.mm.dd>[.n]")
	parser.add_argument("--out", required=True)
	parser.add_argument("--previous", help="module.json of the last module release")
	parser.add_argument("--previous-dir", help="folder with the last release's component zips")
	parser.add_argument("--force", action="store_true", help="rebuild every component")
	args = parser.parse_args()
	if not re.match(r"^ytdl-v\d{4}\.\d{2}\.\d{2}(\.\d+)?$", args.tag):
		raise SystemExit("bad tag " + args.tag)

	previous = {}
	if args.previous and os.path.isfile(args.previous):
		with open(args.previous, encoding="utf-8") as f:
			previous = {c["id"]: c for c in json.load(f).get("components", [])}

	os.makedirs(args.out, exist_ok=True)
	components, changed = [], []
	for cid, (latest, pack) in PACKERS.items():
		upstream = latest()
		old = previous.get(cid)
		file = "%s-%s.zip" % (cid, upstream["version"])
		print("%s %s (previous: %s)" % (cid, upstream["version"], old["version"] if old else "none"))
		oldZip = os.path.join(args.previous_dir or "", old["file"]) if old else None
		if not args.force and old and old["version"] == upstream["version"] and oldZip and os.path.isfile(oldZip):
			with open(oldZip, "rb") as f:
				data = f.read()
			if sha256(data) != old["sha256"]:
				raise SystemExit("previous %s does not match its module.json" % old["file"])
			file = old["file"]
			print("  unchanged: %s copied from the previous release" % file)
		else:
			files = pack(upstream)
			smokeTest(cid, files, args.out)
			data = _zip(files)
			changed.append("%s %s" % (cid, upstream["version"]))
		with open(os.path.join(args.out, file), "wb") as f:
			f.write(data)
		components.append({
			"id": cid,
			"version": upstream["version"],
			"file": file,
			"url": RELEASE_BASE + args.tag + "/" + file,
			"sha256": sha256(data),
			"size": len(data),
		})

	if not changed and not args.force:
		print("nothing changed")
		_output("changed", "false")
		return
	module = {"schema": SCHEMA, "version": args.tag[len("ytdl-v"):], "components": components}
	with open(os.path.join(args.out, "module.json"), "w", encoding="utf-8") as f:
		json.dump(module, f, indent="\t")
		f.write("\n")
	with open(os.path.join(args.out, "notes.md"), "w", encoding="utf-8") as f:
		f.write("YouTube download module of LINE MR (downloaded by the add-on when needed; not an add-on release).\n\n")
		f.write("| Component | Version | SHA-256 |\n|---|---|---|\n")
		for c in components:
			f.write("| %s | %s | `%s` |\n" % (c["id"], c["version"], c["sha256"]))
		f.write("\nChanged: %s\n" % (", ".join(changed) or "none (forced build)"))
	print("module %s: %s" % (module["version"], ", ".join(changed) or "forced"))
	_output("changed", "true")


def _output(name, value):
	path = os.environ.get("GITHUB_OUTPUT")
	if path:
		with open(path, "a", encoding="utf-8") as f:
			f.write("%s=%s\n" % (name, value))


if __name__ == "__main__":
	sys.exit(main())
