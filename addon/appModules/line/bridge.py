# Installs and checks LINE MR's in-process accessibility bridge (line_a11y.dll).
#
# LINE's Qt loads plugins only from its own directory, so the bridge is copied to
# %LOCALAPPDATA%\LINE\bin\current\platformthemes\line_a11y.dll, where Qt picks it up as the
# "windows" platform theme at start-up. LINE updates can replace the "current" folder, and the
# DLL is locked while LINE runs, so installing means: quit LINE, copy, start LINE again.

import ctypes
import ctypes.wintypes as wintypes
import enum
import hashlib
import os
import shutil
import subprocess
import threading
import time

import addonHandler
import gui
import ui
import wx
from logHandler import log

addonHandler.initTranslation()

DLL_NAME = "line_a11y.dll"
_ADDON_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BUNDLED_DLL = os.path.join(_ADDON_ROOT, "bridge", DLL_NAME)


class Status(enum.Enum):
	RUNNING = "running"  # installed, current, and loaded in the running LINE
	NOT_LOADED = "notLoaded"  # installed and current, but LINE was started before it was installed
	OUTDATED = "outdated"  # an older bridge is installed
	MISSING = "missing"  # no bridge in LINE's folder (fresh install or LINE update)
	NO_LINE = "noLine"  # LINE for Windows not found
	NOT_BUNDLED = "notBundled"  # the add-on package lacks the DLL (development build)


def lineBinDir():
	return os.path.join(os.environ.get("LOCALAPPDATA", ""), "LINE", "bin")


def installedDllPath():
	return os.path.join(lineBinDir(), "current", "platformthemes", DLL_NAME)


def _sameFile(a, b):
	# NVDA's bundled Python has no filecmp.
	if os.path.getsize(a) != os.path.getsize(b):
		return False
	digests = []
	for path in (a, b):
		with open(path, "rb") as f:
			digests.append(hashlib.sha256(f.read()).digest())
	return digests[0] == digests[1]


# Private DLL instances: declaring prototypes on ctypes.windll would change them for all of NVDA.
_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
_psapi = ctypes.WinDLL("psapi", use_last_error=True)
_kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
_kernel32.OpenProcess.restype = wintypes.HANDLE
_kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
_kernel32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
_kernel32.WaitForSingleObject.restype = wintypes.DWORD
_psapi.EnumProcessModulesEx.argtypes = (
	wintypes.HANDLE,
	ctypes.POINTER(wintypes.HMODULE),
	wintypes.DWORD,
	ctypes.POINTER(wintypes.DWORD),
	wintypes.DWORD,
)
_psapi.GetModuleBaseNameW.argtypes = (wintypes.HANDLE, wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD)
_psapi.GetModuleBaseNameW.restype = wintypes.DWORD

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
SYNCHRONIZE = 0x00100000
LIST_MODULES_ALL = 0x03


def _processModules(pid):
	"""Base names (lower case) of the modules loaded in process `pid`."""
	handle = _kernel32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
	if not handle:
		return set()
	try:
		needed = wintypes.DWORD()
		modules = (wintypes.HMODULE * 1024)()
		if not _psapi.EnumProcessModulesEx(
			handle, modules, ctypes.sizeof(modules), ctypes.byref(needed), LIST_MODULES_ALL
		):
			return set()
		count = min(needed.value // ctypes.sizeof(wintypes.HMODULE), len(modules))
		names = set()
		buf = ctypes.create_unicode_buffer(260)
		for i in range(count):
			if _psapi.GetModuleBaseNameW(handle, modules[i], buf, len(buf)):
				names.add(buf.value.lower())
		return names
	finally:
		_kernel32.CloseHandle(handle)


def status(pid=None):
	if not os.path.isfile(os.path.join(lineBinDir(), "current", "LINE.exe")):
		return Status.NO_LINE
	if not os.path.isfile(BUNDLED_DLL):
		return Status.NOT_BUNDLED
	target = installedDllPath()
	if not os.path.isfile(target):
		return Status.MISSING
	if not _sameFile(BUNDLED_DLL, target):
		return Status.OUTDATED
	if pid and DLL_NAME not in _processModules(pid):
		return Status.NOT_LOADED
	return Status.RUNNING


def describeStatus(st):
	return {
		# Translators: Reported when the LINE accessibility bridge works.
		Status.RUNNING: _("LINE MR bridge is running."),
		# Translators: Reported when the bridge is installed but LINE has to be restarted to load it.
		Status.NOT_LOADED: _("LINE MR bridge is installed, but LINE must be restarted to use it."),
		# Translators: Reported when an older version of the bridge is installed in LINE.
		Status.OUTDATED: _("An older LINE MR bridge is installed in LINE."),
		# Translators: Reported when the bridge is not installed in LINE (for example after a LINE update).
		Status.MISSING: _("LINE MR bridge is not installed in LINE."),
		# Translators: Reported when LINE for Windows cannot be found.
		Status.NO_LINE: _("LINE for Windows was not found."),
		# Translators: Reported when the add-on package does not contain the bridge.
		Status.NOT_BUNDLED: _("This LINE MR package does not contain the bridge."),
	}[st]


def _waitForExit(pid, timeout=10.0):
	handle = _kernel32.OpenProcess(SYNCHRONIZE, False, pid)
	if not handle:
		return True
	try:
		return _kernel32.WaitForSingleObject(handle, int(timeout * 1000)) == 0
	finally:
		_kernel32.CloseHandle(handle)


def _linePids():
	out = subprocess.run(
		["tasklist", "/FI", "IMAGENAME eq LINE.exe", "/FO", "CSV", "/NH"],
		capture_output=True,
		text=True,
		creationflags=subprocess.CREATE_NO_WINDOW,
	).stdout
	pids = []
	for line in out.splitlines():
		parts = [p.strip('"') for p in line.split('","')]
		if len(parts) > 1 and parts[1].isdigit():
			pids.append(int(parts[1]))
	return pids


def installAndRestart():
	"""Quits LINE, installs the bundled bridge and starts LINE again. Runs off the main thread."""
	for pid in _linePids():
		subprocess.run(
			["taskkill", "/PID", str(pid), "/F"],
			capture_output=True,
			creationflags=subprocess.CREATE_NO_WINDOW,
		)
		_waitForExit(pid)
	# Not a crash: keep the bridge's crash guard (%LOCALAPPDATA%\LineMR\session.running) from counting it.
	try:
		os.remove(os.path.join(os.environ.get("LOCALAPPDATA", ""), "LineMR", "session.running"))
	except OSError:
		pass
	target = installedDllPath()
	for _attempt in range(20):
		try:
			os.makedirs(os.path.dirname(target), exist_ok=True)
			shutil.copyfile(BUNDLED_DLL, target)
			break
		except OSError:
			time.sleep(0.25)  # the DLL can stay locked for a moment after LINE exits
	else:
		log.error("LINE MR: could not install the bridge to %s", target, exc_info=True)
		# Translators: Reported when copying the bridge into LINE failed.
		wx.CallAfter(ui.message, _("Could not install the LINE MR bridge."))
		return
	log.info("LINE MR: installed bridge to %s", target)
	launcher = os.path.join(lineBinDir(), "LineLauncher.exe")
	subprocess.Popen([launcher], cwd=lineBinDir(), close_fds=True)
	# Translators: Reported while LINE restarts after the bridge was installed.
	wx.CallAfter(ui.message, _("LINE MR bridge installed. LINE is restarting."))


_offered = False


def offerInstall(pid, st):
	"""Asks the user whether to install the bridge and restart LINE."""
	global _offered
	if st in (Status.RUNNING, Status.NO_LINE, Status.NOT_BUNDLED):
		return
	_offered = True
	if st == Status.NOT_LOADED:
		# Translators: Question shown when LINE has to be restarted to load the bridge.
		question = _("LINE has to be restarted to use LINE MR. Restart LINE now?")
	else:
		# Translators: Question shown when the bridge must be installed (LINE will restart).
		question = _(
			"{status}\nInstall the LINE MR bridge now? LINE will be closed and started again."
		).format(status=describeStatus(st))

	def ask():
		answer = gui.messageBox(
			question,
			# Translators: Title of the dialog offering to install the bridge.
			_("LINE MR"),
			wx.YES_NO | wx.ICON_QUESTION,
		)
		if answer == wx.YES:
			threading.Thread(target=installAndRestart, name="lineMR.install", daemon=True).start()

	wx.CallAfter(ask)


def checkOnStart(pid):
	try:
		st = status(pid)
	except Exception:
		log.error("LINE MR: bridge status check failed", exc_info=True)
		return
	log.info("LINE MR: bridge status %s (LINE pid %s)", st.value, pid)
	if not _offered:
		offerInstall(pid, st)
