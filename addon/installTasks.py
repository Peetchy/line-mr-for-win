# Removing LINE MR also removes its bridge from LINE.
#
# NVDA runs onUninstall when it starts after the add-on was removed in the Add-on Store, and also
# when it replaces the add-on with a new version: then the new version's folder
# (<name>.pendingInstall) is still beside this one and nothing is removed.
#
# The bridge is deleted from LINE's folder and from LINE's helper processes (LineCall, LineMediaPlayer),
# and LINE MR's data folder (%LOCALAPPDATA%\LineMR: logs, settings) goes too. A running LINE is not
# quit: it keeps the bridge it has loaded until it next starts. A copy Windows will not delete while
# LINE has it loaded is renamed (Qt loads only *.dll), and what is left is deleted at the next
# sign-in. Kept separate
# from appModules/line/bridge.py, which needs NVDA's GUI and the add-on's translations.

import ctypes
import ctypes.wintypes as wintypes
import os
import shutil
import subprocess
import time
import winreg

from logHandler import log

DLL_NAME = "line_a11y.dll"
HELPERS = ("LineCall", "LineMediaPlayer")
PENDING_INSTALL_SUFFIX = ".pendingInstall"
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000

# Private DLL instance: declaring prototypes on ctypes.windll would change them for all of NVDA.
_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
_kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
_kernel32.OpenProcess.restype = wintypes.HANDLE
_kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
_kernel32.QueryFullProcessImageNameW.argtypes = (
	wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)
)
_kernel32.QueryFullProcessImageNameW.restype = wintypes.BOOL


def _updating():
	"""Is NVDA replacing this add-on with another version (rather than removing it)?"""
	here = os.path.dirname(os.path.abspath(__file__))
	return os.path.isdir(here + PENDING_INSTALL_SUFFIX)


def _runningLineDirs():
	try:
		out = subprocess.run(
			["tasklist", "/FO", "CSV", "/NH", "/FI", "IMAGENAME eq LINE.exe"],
			capture_output=True,
			text=True,
			creationflags=subprocess.CREATE_NO_WINDOW,
		).stdout
	except OSError:
		return []
	dirs = []
	for line in out.splitlines():
		parts = [p.strip('"') for p in line.split('","')]
		if len(parts) < 2 or parts[0].lower() != "line.exe" or not parts[1].isdigit():
			continue
		handle = _kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, int(parts[1]))
		if not handle:
			continue
		try:
			buf = ctypes.create_unicode_buffer(1024)
			size = wintypes.DWORD(len(buf))
			if _kernel32.QueryFullProcessImageNameW(handle, 0, buf, ctypes.byref(size)):
				dirs.append(os.path.dirname(buf.value))
		finally:
			_kernel32.CloseHandle(handle)
	return dirs


def _lineDirs():
	"""Every folder with a LINE.exe: the running one's and the standard installs."""
	local = os.environ.get("LOCALAPPDATA", "")
	candidates = _runningLineDirs() + [os.path.join(local, "LINE", "bin", "current")]
	for var in ("ProgramFiles", "ProgramFiles(x86)", "ProgramW6432"):
		base = os.environ.get(var)
		if base:
			candidates.append(os.path.join(base, "LINE", "bin", "current"))
	# LINE keeps its previous version beside the current one (bin\old), with the bridge copied along.
	for directory in list(candidates):
		bin = os.path.dirname(directory)
		try:
			candidates.extend(os.path.join(bin, name) for name in os.listdir(bin))
		except OSError:
			pass
	dirs = []
	for directory in candidates:
		if os.path.isfile(os.path.join(directory, "LINE.exe")) and os.path.normcase(directory) not in (
			os.path.normcase(d) for d in dirs
		):
			dirs.append(directory)
	return dirs


def _bridgeFolders():
	"""platformthemes folders that may hold the bridge: LINE's and each helper version's."""
	folders = []
	roots = []
	for directory in _lineDirs():
		folders.append(os.path.join(directory, "platformthemes"))
		roots.append(os.path.join(os.path.dirname(os.path.dirname(directory)), "Data", "plugin"))
	roots.append(os.path.join(os.environ.get("LOCALAPPDATA", ""), "LINE", "Data", "plugin"))
	for root in roots:
		for helper in HELPERS:
			try:
				versions = os.listdir(os.path.join(root, helper))
			except OSError:
				continue
			folders.extend(os.path.join(root, helper, v, "platformthemes") for v in versions)
	unique = []
	for folder in folders:
		if os.path.normcase(os.path.abspath(folder)) not in (os.path.normcase(os.path.abspath(f)) for f in unique):
			unique.append(folder)
	return unique


def _removeFromFolder(folder):
	"""Deletes the bridge and copies left beside it (.new, .old<ms>); renames a locked one.
	True when copies in use are left (deleted at the next sign-in)."""
	try:
		names = [n for n in os.listdir(folder) if n.lower().startswith(DLL_NAME)]
	except OSError:
		return False
	left = False
	for name in names:
		path = os.path.join(folder, name)
		try:
			os.remove(path)
			log.info("LINE MR: removed %s", path)
		except OSError as e:
			if name.lower() != DLL_NAME:
				left = True  # an older copy still loaded by a running helper process
				continue
			try:
				os.replace(path, "%s.old%d" % (path, int(time.time() * 1000)))
				log.info("LINE MR: %s is in use; renamed, LINE no longer loads it", path)
				left = True
			except OSError:
				# LINE installed for all users (Program Files) needs administrator rights.
				log.error("LINE MR: could not remove %s: %s", path, e)
	return left


def _cleanUpAtSignIn(folders, data):
	"""Deletes what a running LINE kept open once the user signs in again (HKCU RunOnce, no
	administrator rights). The script deletes itself."""
	temp = os.environ.get("TEMP") or os.environ.get("TMP")
	if not temp:
		return
	script = os.path.join(temp, "lineMR-cleanup.cmd")
	lines = ["@echo off"]
	lines += ['del /f /q "%s"' % os.path.join(folder, DLL_NAME + ".old*") for folder in folders]
	if data:
		lines.append('rmdir /s /q "%s"' % data)
	lines.append('del /f /q "%~f0"')
	try:
		# cmd.exe reads scripts in the OEM code page.
		with open(script, "w", encoding="oem", errors="replace") as f:
			f.write("\r\n".join(lines) + "\r\n")
		with winreg.CreateKey(winreg.HKEY_CURRENT_USER, r"Software\Microsoft\Windows\CurrentVersion\RunOnce") as key:
			winreg.SetValueEx(key, "lineMRCleanup", 0, winreg.REG_SZ, 'cmd.exe /c "%s"' % script)
		log.info("LINE MR: files in use are deleted at the next sign-in (%s)", script)
	except OSError as e:
		log.error("LINE MR: could not arrange the clean-up at sign-in: %s", e)


def onUninstall():
	if _updating():
		return
	left = [folder for folder in _bridgeFolders() if _removeFromFolder(folder)]
	data = os.path.join(os.environ.get("LOCALAPPDATA", ""), "LineMR")
	dataLeft = None
	if os.environ.get("LOCALAPPDATA") and os.path.isdir(data):
		# Files a running LINE still has open (its logs) stay until it is gone.
		shutil.rmtree(data, ignore_errors=True)
		log.info("LINE MR: removed %s", data)
		if os.path.isdir(data):
			dataLeft = data
	if left or dataLeft:
		_cleanUpAtSignIn(left, dataLeft)
