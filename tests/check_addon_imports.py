"""Checks that every module the add-on imports exists in the installed NVDA.

NVDA ships a frozen subset of the standard library (no filecmp, for example), so an import
that works in a normal Python can break the app module. Also compiles every add-on file.

Usage: python tests/check_addon_imports.py [--nvda "C:\\Program Files\\NVDA"]
"""

import argparse
import ast
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ADDON = os.path.join(ROOT, "addon")


def nvda_modules(nvda_dir):
	names = set(sys.builtin_module_names)
	with zipfile.ZipFile(os.path.join(nvda_dir, "library.zip")) as z:
		for n in z.namelist():
			top = n.split("/")[0]
			names.add(top[:-4] if top.endswith(".pyc") else top)
	for n in os.listdir(nvda_dir):
		if n.endswith(".pyd"):
			names.add(n.split(".")[0])
	return names


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("--nvda", default=os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"), "NVDA"))
	args = ap.parse_args()
	available = nvda_modules(args.nvda)
	problems = 0
	for dirpath, _dirs, files in os.walk(ADDON):
		for name in files:
			if not name.endswith(".py"):
				continue
			path = os.path.join(dirpath, name)
			rel = os.path.relpath(path, ROOT)
			with open(path, encoding="utf-8") as f:
				source = f.read()
			try:
				tree = ast.parse(source, path)
				compile(tree, path, "exec")
			except SyntaxError as e:
				print(f"{rel}:{e.lineno}: {e.msg}")
				problems += 1
				continue
			for node in ast.walk(tree):
				if isinstance(node, ast.Import):
					mods = [a.name for a in node.names]
				elif isinstance(node, ast.ImportFrom) and node.level == 0 and node.module:
					mods = [node.module]
				else:
					continue
				for mod in mods:
					if mod.split(".")[0] not in available:
						print(f"{rel}:{node.lineno}: '{mod}' is not available in NVDA")
						problems += 1
	print("OK" if not problems else f"{problems} problem(s)")
	sys.exit(1 if problems else 0)


if __name__ == "__main__":
	main()
