"""Writes the GitHub release notes for a version tag: what changed (the tagged commit's message),
the keyboard commands (README's "Keyboard commands" section) and the add-on's SHA-256.

Usage: python tools/release_notes.py <tag> <add-on file> <output file>
"""

import hashlib
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def changes(tag):
	message = subprocess.run(
		["git", "log", "-1", "--format=%B", tag], cwd=ROOT, capture_output=True, text=True, encoding="utf-8", check=True
	).stdout
	lines = [line for line in message.strip().splitlines() if not line.startswith("Co-Authored-By:")]
	subject, body = lines[0], "\n".join(lines[1:]).strip()
	return f"## What's new\n\n**{subject}**\n\n{body}\n" if body else f"## What's new\n\n**{subject}**\n"


def commands():
	with open(os.path.join(ROOT, "README.md"), encoding="utf-8") as f:
		readme = f.read()
	match = re.search(r"^## Keyboard commands.*?(?=^## )", readme, re.MULTILINE | re.DOTALL)
	if not match:
		return ""
	# Links relative to the repository do not work on the release page.
	section = match.group(0).replace("](addon/", "](https://github.com/Peetchy/line-mr-for-win/blob/main/addon/")
	return section.strip() + "\n"


def main():
	tag, addon, output = sys.argv[1:4]
	with open(addon, "rb") as f:
		sha = hashlib.sha256(f.read()).hexdigest()
	notes = "\n".join([changes(tag), commands(), f"SHA-256 of {os.path.basename(addon)}: `{sha}`\n"])
	with open(output, "w", encoding="utf-8") as f:
		f.write(notes)


if __name__ == "__main__":
	main()
