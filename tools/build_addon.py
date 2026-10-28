"""Builds dist/lineMR-<version>.nvda-addon from addon/ plus the bridge DLL.

Usage: python tools/build_addon.py [--dll PATH] [--stage-only]

Compiles locale/*/LC_MESSAGES/nvda.po to .mo, renders doc/*/readme.md and changes.md to HTML and
bundles bridge/build/line_a11y.dll as bridge/line_a11y.dll and COPYING.txt inside the add-on.
"""

import argparse
import ast
import configparser
import os
import shutil
import struct
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ADDON = os.path.join(ROOT, "addon")
STAGE = os.path.join(ROOT, "build", "addon")
DIST = os.path.join(ROOT, "dist")


def parse_po(path):
	"""Minimal .po reader: returns {msgid: msgstr} for translated, non-fuzzy entries."""
	entries = {}
	msgid = msgstr = None
	section = None
	fuzzy = False

	def flush():
		if msgid is not None and msgstr and not fuzzy:
			entries[msgid] = msgstr

	with open(path, encoding="utf-8") as f:
		for raw in f:
			line = raw.strip()
			if line.startswith("#,") and "fuzzy" in line:
				fuzzy = True
				continue
			if not line or line.startswith("#"):
				if section == "msgstr" and not line:
					flush()
					msgid = msgstr = section = None
					fuzzy = False
				continue
			if line.startswith("msgid "):
				if section == "msgstr":
					flush()
					fuzzy = False
				msgid, msgstr, section = ast.literal_eval(line[6:]), None, "msgid"
			elif line.startswith("msgstr "):
				msgstr, section = ast.literal_eval(line[7:]), "msgstr"
			elif line.startswith('"'):
				if section == "msgid":
					msgid += ast.literal_eval(line)
				elif section == "msgstr":
					msgstr += ast.literal_eval(line)
	if section == "msgstr":
		flush()
	return entries


def write_mo(entries, path):
	"""GNU .mo writer (same layout as CPython's Tools/i18n/msgfmt.py)."""
	keys = sorted(entries)
	ids = b""
	strs = b""
	offsets = []
	for k in keys:
		kb, vb = k.encode("utf-8"), entries[k].encode("utf-8")
		offsets.append((len(ids), len(kb), len(strs), len(vb)))
		ids += kb + b"\0"
		strs += vb + b"\0"
	keystart = 7 * 4 + 16 * len(keys)
	valuestart = keystart + len(ids)
	koffsets, voffsets = [], []
	for o1, l1, o2, l2 in offsets:
		koffsets += [l1, o1 + keystart]
		voffsets += [l2, o2 + valuestart]
	header = struct.pack("Iiiiiii", 0x950412DE, 0, len(keys), 7 * 4, 7 * 4 + len(keys) * 8, 0, 0)
	with open(path, "wb") as f:
		f.write(header)
		f.write(struct.pack(f"{len(koffsets)}i", *koffsets))
		f.write(struct.pack(f"{len(voffsets)}i", *voffsets))
		f.write(ids)
		f.write(strs)


def render_docs():
	try:
		import markdown
	except ImportError:
		markdown = None
	docroot = os.path.join(STAGE, "doc")
	for dirpath, _dirs, files in os.walk(docroot):
		for name in files:
			# The guide and "What's new" (shown from the add-on's settings).
			if name not in ("readme.md", "changes.md"):
				continue
			src = os.path.join(dirpath, name)
			with open(src, encoding="utf-8") as f:
				text = f.read()
			# NVDA's folder names (zh_CN) as HTML language tags (zh-CN).
			lang = os.path.basename(dirpath).replace("_", "-")
			body = markdown.markdown(text, extensions=["tables"]) if markdown else f"<pre>{text}</pre>"
			html = (
				f'<!DOCTYPE html>\n<html lang="{lang}">\n<head>\n<meta charset="utf-8">\n'
				f"<title>LINE MR</title>\n</head>\n<body>\n{body}\n</body>\n</html>\n"
			)
			with open(os.path.join(dirpath, name[:-3] + ".html"), "w", encoding="utf-8") as f:
				f.write(html)
			os.remove(src)


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("--dll", default=os.path.join(ROOT, "bridge", "build", "line_a11y.dll"))
	ap.add_argument("--stage-only", action="store_true", help="only build build/addon (for dev installs)")
	args = ap.parse_args()

	if os.path.isdir(STAGE):
		shutil.rmtree(STAGE)
	shutil.copytree(ADDON, STAGE, ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "*.pot"))
	for dirpath, _dirs, files in os.walk(os.path.join(STAGE, "locale")):
		for name in files:
			if name.endswith(".po"):
				po = os.path.join(dirpath, name)
				write_mo(parse_po(po), po[:-3] + ".mo")
				os.remove(po)
	render_docs()
	if not os.path.isfile(args.dll):
		sys.exit(f"bridge DLL not found: {args.dll} (build it with tools\\build.ps1 first)")
	os.makedirs(os.path.join(STAGE, "bridge"), exist_ok=True)
	shutil.copy2(args.dll, os.path.join(STAGE, "bridge", "line_a11y.dll"))
	shutil.copy2(os.path.join(ROOT, "COPYING.txt"), STAGE)

	manifest = configparser.ConfigParser()
	with open(os.path.join(STAGE, "manifest.ini"), encoding="utf-8") as f:
		manifest.read_string("[m]\n" + f.read())
	version = manifest["m"]["version"].strip('"')
	print(f"Staged {STAGE}")
	if args.stage_only:
		return

	os.makedirs(DIST, exist_ok=True)
	out = os.path.join(DIST, f"lineMR-{version}.nvda-addon")
	with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
		for dirpath, _dirs, files in os.walk(STAGE):
			for name in files:
				full = os.path.join(dirpath, name)
				z.write(full, os.path.relpath(full, STAGE))
	print(f"Built {out}")


if __name__ == "__main__":
	main()
