# Installs and checks LINE MR's in-process accessibility bridge (line_a11y.dll).
#
# LINE's Qt loads plugins only from its own directory, so the bridge is copied to
# <LINE's folder>\platformthemes\line_a11y.dll, where Qt picks it up as the "windows" platform theme
# at start-up. LINE's folder is that of the running LINE.exe (usually
# %LOCALAPPDATA%\LINE\bin\current; Program Files when LINE was installed for all users). LINE's
# helper processes get a copy in LINE\Data\plugin\<name>\<version>\platformthemes: LineCall.exe
# (incoming-call notification, call windows) and LineMediaPlayer.exe (photo and video viewer).
# LINE updates can replace these folders, and
# the DLL is locked while LINE runs, so installing means: quit LINE, copy, start LINE again, and
# check that LINE loaded it (Smart App Control or antivirus software can block an unsigned DLL).

import ctypes
import ctypes.wintypes as wintypes
import enum
import hashlib
import os
import shutil
import subprocess
import threading
import time
import winreg

import addonHandler
import core
import gui
import ui
import wx
from logHandler import log

addonHandler.initTranslation()

DLL_NAME = "line_a11y.dll"
_ADDON_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BUNDLED_DLL = os.path.join(_ADDON_ROOT, "bridge", DLL_NAME)
#: The bridge is built against Qt 6.6; Qt loads plugins built for the same major and an older or equal minor version.
MIN_QT = (6, 6)


class Status(enum.Enum):
	RUNNING = "running"  # installed, current, and loaded in the running LINE
	NOT_LOADED = "notLoaded"  # installed and current, but LINE was started before it was installed
	BLOCKED = "blocked"  # installed, but LINE started without it after an install (Smart App Control, antivirus)
	OUTDATED = "outdated"  # an older bridge is installed
	MISSING = "missing"  # no bridge in LINE's folder (fresh install or LINE update)
	INCOMPATIBLE = "incompatible"  # LINE uses a Qt version the bridge cannot be loaded into
	NO_LINE = "noLine"  # LINE for Windows not found
	NOT_BUNDLED = "notBundled"  # the add-on package lacks the DLL (development build)


def _localAppData():
	return os.environ.get("LOCALAPPDATA", "")


def bridgeDataDir():
	"""Where the bridge keeps its logs and crash guard (%LOCALAPPDATA%\\LineMR)."""
	return os.path.join(_localAppData(), "LineMR")


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
_version = ctypes.WinDLL("version", use_last_error=True)
_shell32 = ctypes.WinDLL("shell32", use_last_error=True)
_user32 = ctypes.WinDLL("user32", use_last_error=True)
_kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
_kernel32.OpenProcess.restype = wintypes.HANDLE
_kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
_kernel32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
_kernel32.WaitForSingleObject.restype = wintypes.DWORD
_kernel32.QueryFullProcessImageNameW.argtypes = (
	wintypes.HANDLE,
	wintypes.DWORD,
	wintypes.LPWSTR,
	ctypes.POINTER(wintypes.DWORD),
)
_kernel32.QueryFullProcessImageNameW.restype = wintypes.BOOL
_psapi.EnumProcessModulesEx.argtypes = (
	wintypes.HANDLE,
	ctypes.POINTER(wintypes.HMODULE),
	wintypes.DWORD,
	ctypes.POINTER(wintypes.DWORD),
	wintypes.DWORD,
)
_psapi.GetModuleBaseNameW.argtypes = (wintypes.HANDLE, wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD)
_psapi.GetModuleBaseNameW.restype = wintypes.DWORD
_version.GetFileVersionInfoSizeW.argtypes = (wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD))
_version.GetFileVersionInfoSizeW.restype = wintypes.DWORD
_version.GetFileVersionInfoW.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p)
_version.GetFileVersionInfoW.restype = wintypes.BOOL
_version.VerQueryValueW.argtypes = (
	ctypes.c_void_p,
	wintypes.LPCWSTR,
	ctypes.POINTER(ctypes.c_void_p),
	ctypes.POINTER(wintypes.UINT),
)
_version.VerQueryValueW.restype = wintypes.BOOL
_user32.GetDpiForSystem.argtypes = ()
_user32.GetDpiForSystem.restype = wintypes.UINT


class _ShellExecuteInfo(ctypes.Structure):
	_fields_ = (
		("cbSize", wintypes.DWORD),
		("fMask", wintypes.ULONG),
		("hwnd", wintypes.HWND),
		("lpVerb", wintypes.LPCWSTR),
		("lpFile", wintypes.LPCWSTR),
		("lpParameters", wintypes.LPCWSTR),
		("lpDirectory", wintypes.LPCWSTR),
		("nShow", ctypes.c_int),
		("hInstApp", wintypes.HINSTANCE),
		("lpIDList", ctypes.c_void_p),
		("lpClass", wintypes.LPCWSTR),
		("hkeyClass", wintypes.HKEY),
		("dwHotKey", wintypes.DWORD),
		("hIconOrMonitor", wintypes.HANDLE),
		("hProcess", wintypes.HANDLE),
	)


_shell32.ShellExecuteExW.argtypes = (ctypes.POINTER(_ShellExecuteInfo),)
_shell32.ShellExecuteExW.restype = wintypes.BOOL


class _FixedFileInfo(ctypes.Structure):
	_fields_ = [
		(name, wintypes.DWORD)
		for name in (
			"dwSignature",
			"dwStrucVersion",
			"dwFileVersionMS",
			"dwFileVersionLS",
			"dwProductVersionMS",
			"dwProductVersionLS",
			"dwFileFlagsMask",
			"dwFileFlags",
			"dwFileOS",
			"dwFileType",
			"dwFileSubtype",
			"dwFileDateMS",
			"dwFileDateLS",
		)
	]


#: LINE's helper processes: Data\plugin\<name>\<version>\<name>.exe.
HELPERS = ("LineCall", "LineMediaPlayer")
LINE_PROCESSES = ("line.exe",) + tuple(h.lower() + ".exe" for h in HELPERS)

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PROCESS_VM_READ = 0x0010
SYNCHRONIZE = 0x00100000
LIST_MODULES_ALL = 0x03
SEE_MASK_NOCLOSEPROCESS = 0x00000040
SW_HIDE = 0


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


def _processImagePath(pid):
	"""Full path of the program process `pid` runs, or None."""
	handle = _kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
	if not handle:
		return None
	try:
		buf = ctypes.create_unicode_buffer(1024)
		size = wintypes.DWORD(len(buf))
		if not _kernel32.QueryFullProcessImageNameW(handle, 0, buf, ctypes.byref(size)):
			return None
		return buf.value
	finally:
		_kernel32.CloseHandle(handle)


def fileVersion(path):
	"""(major, minor, patch, build) from a file's version resource, or None."""
	size = _version.GetFileVersionInfoSizeW(path, None)
	if not size:
		return None
	data = ctypes.create_string_buffer(size)
	if not _version.GetFileVersionInfoW(path, 0, size, data):
		return None
	pointer = ctypes.c_void_p()
	length = wintypes.UINT()
	if not _version.VerQueryValueW(data, "\\", ctypes.byref(pointer), ctypes.byref(length)) or not pointer.value:
		return None
	info = _FixedFileInfo.from_address(pointer.value)
	ms, ls = info.dwProductVersionMS, info.dwProductVersionLS
	return (ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF)


def _lineProcesses():
	"""(lower-case image name, pid) of LINE.exe and its helper processes (started by LINE)."""
	out = subprocess.run(
		["tasklist", "/FO", "CSV", "/NH"],
		capture_output=True,
		text=True,
		creationflags=subprocess.CREATE_NO_WINDOW,
	).stdout
	found = []
	for line in out.splitlines():
		parts = [p.strip('"') for p in line.split('","')]
		if len(parts) > 1 and parts[0].lower() in LINE_PROCESSES and parts[1].isdigit():
			found.append((parts[0].lower(), int(parts[1])))
	return found


def _linePids():
	"""LINE.exe and its helper processes (they lock their copies of the bridge)."""
	return [pid for _name, pid in _lineProcesses()]


# ---- Where LINE is ------------------------------------------------------------------------------

_lineDir = None


def _isLineDir(directory):
	return bool(directory) and os.path.isfile(os.path.join(directory, "LINE.exe"))


def _standardLineDirs():
	dirs = [os.path.join(_localAppData(), "LINE", "bin", "current")]
	for var in ("ProgramFiles", "ProgramFiles(x86)", "ProgramW6432"):
		base = os.environ.get(var)
		if base:
			dirs.append(os.path.join(base, "LINE", "bin", "current"))
	return dirs


def lineDir(pid=None):
	"""Folder of the LINE.exe in use: the running one's (`pid`, else any running LINE.exe), else
	the last one seen, else a standard install. None when there is no LINE."""
	global _lineDir
	pids = [pid] if pid else [p for name, p in _lineProcesses() if name == "line.exe"]
	for p in pids:
		path = _processImagePath(p)
		if path and os.path.basename(path).lower() == "line.exe":
			if _lineDir is None or os.path.normcase(_lineDir) != os.path.normcase(os.path.dirname(path)):
				log.info("LINE MR: LINE runs from %s", os.path.dirname(path))
			_lineDir = os.path.dirname(path)
			return _lineDir
	if _isLineDir(_lineDir):
		return _lineDir
	for directory in _standardLineDirs():
		if _isLineDir(directory):
			_lineDir = directory
			return directory
	return None


def installedDllPath(directory=None):
	directory = directory or lineDir()
	return os.path.join(directory, "platformthemes", DLL_NAME) if directory else None


def _helperRoots(helper, directory=None):
	"""Data\\plugin\\<helper> next to LINE's bin folder, and in %LOCALAPPDATA%\\LINE (where a LINE
	installed for all users keeps its data)."""
	directory = directory or lineDir()
	roots = []
	if directory:
		roots.append(os.path.join(os.path.dirname(os.path.dirname(directory)), "Data", "plugin", helper))
	roots.append(os.path.join(_localAppData(), "LINE", "Data", "plugin", helper))
	unique = []
	for root in roots:
		if os.path.normcase(os.path.abspath(root)) not in (os.path.normcase(os.path.abspath(r)) for r in unique):
			unique.append(root)
	return unique


def _helperDllPaths(helper, directory=None):
	"""Bridge locations for each installed version of a helper process."""
	paths = []
	for root in _helperRoots(helper, directory):
		try:
			versions = os.listdir(root)
		except OSError:
			continue
		paths.extend(
			os.path.join(root, v, "platformthemes", DLL_NAME)
			for v in versions
			if os.path.isfile(os.path.join(root, v, helper + ".exe"))
		)
	return paths


def callDllPaths(directory=None):
	"""Bridge locations for each installed LineCall.exe version."""
	return _helperDllPaths("LineCall", directory)


def helperDllPaths(directory=None):
	"""Bridge locations for every helper process (calls, photo and video viewer)."""
	return [p for helper in HELPERS for p in _helperDllPaths(helper, directory)]


def lineQtVersion(directory):
	return fileVersion(os.path.join(directory, "Qt6Core.dll"))


def qtSupported(directory):
	"""Can LINE's Qt load the bridge? (Qt 6, at least the version it was built against.)"""
	if not os.path.isfile(os.path.join(directory, "Qt6Core.dll")):
		return False  # Qt 5 (old LINE) or a layout this add-on does not know
	version = lineQtVersion(directory)
	return version is None or (version[0] == MIN_QT[0] and version[1] >= MIN_QT[1])


# ---- Status -------------------------------------------------------------------------------------

#: Set when LINE started without the bridge right after an install (path of the installed DLL).
_blockedPath = None


def status(pid=None):
	global _blockedPath
	directory = lineDir(pid)
	if not directory:
		return Status.NO_LINE
	if not os.path.isfile(BUNDLED_DLL):
		return Status.NOT_BUNDLED
	if not qtSupported(directory):
		return Status.INCOMPATIBLE
	# The helpers' copies are repaired without restarting LINE (repairHelperBridges()).
	target = installedDllPath(directory)
	if not os.path.isfile(target):
		return Status.MISSING
	if not _sameFile(BUNDLED_DLL, target):
		return Status.OUTDATED
	if pid and DLL_NAME not in _processModules(pid):
		return Status.BLOCKED if _blockedPath else Status.NOT_LOADED
	_blockedPath = None
	return Status.RUNNING


def smartAppControlOn():
	"""Is Windows 11 Smart App Control enforcing (it blocks unsigned DLLs such as the bridge)?"""
	try:
		with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SYSTEM\CurrentControlSet\Control\CI\Policy") as key:
			value, _type = winreg.QueryValueEx(key, "VerifiedAndReputablePolicyState")
	except OSError:
		return False
	return value == 1  # 0 off, 1 on, 2 evaluation (does not block)


def inSafeMode():
	"""Has the bridge's crash guard switched message reading off in the running LINE?"""
	directory = bridgeDataDir()
	try:
		with open(os.path.join(directory, "crash-count"), encoding="ascii") as f:
			crashes = int(f.read().strip() or 0)
	except (OSError, ValueError):
		return False
	return crashes >= 2 and os.path.isfile(os.path.join(directory, "session.running"))


class CallStatus(enum.Enum):
	RUNNING = "running"  # LineCall.exe runs with the bridge
	READY = "ready"  # installed and current; LineCall.exe is not running
	NOT_LOADED = "notLoaded"  # LineCall.exe runs without the bridge (LINE must be restarted)
	MISSING = "missing"  # could not be installed (see NVDA's log)
	NO_CALL = "noCall"  # LINE has not downloaded its call process yet


def _replaceFile(source, target):
	"""Copies source over target, also when a running helper process has target loaded."""
	os.makedirs(os.path.dirname(target), exist_ok=True)
	temp = target + ".new"
	shutil.copyfile(source, temp)
	if os.path.exists(target):
		try:
			os.remove(target)
		except OSError:
			# Older Windows keeps a loaded DLL from being deleted, but lets it be renamed.
			os.replace(target, "%s.old%d" % (target, int(time.time() * 1000)))
	os.replace(temp, target)


def _removeOldCopies(directory):
	try:
		names = os.listdir(directory)
	except OSError:
		return
	for name in names:
		if name.startswith(DLL_NAME + ".old"):
			try:
				os.remove(os.path.join(directory, name))
			except OSError:
				pass  # still loaded by a running helper process


def repairHelperBridges():
	"""Installs the bundled bridge next to every helper process (LineCall.exe, LineMediaPlayer.exe)
	that lacks it or has another build.

	LINE downloads its helpers after it was installed and updates them into new version folders, so
	they can be missing when the bridge was installed. No restart is needed: a running helper picks
	the new bridge up when it next starts. Returns the paths that were (re)installed.
	"""
	if not os.path.isfile(BUNDLED_DLL):
		return []
	done = []
	for target in helperDllPaths():
		_removeOldCopies(os.path.dirname(target))
		try:
			if os.path.isfile(target) and _sameFile(BUNDLED_DLL, target):
				continue
			_replaceFile(BUNDLED_DLL, target)
		except OSError:
			log.error("LINE MR: could not install the bridge to %s", target, exc_info=True)
			continue
		log.info("LINE MR: installed bridge to %s", target)
		done.append(target)
	return done


def _callPids():
	return [pid for name, pid in _lineProcesses() if name == "linecall.exe"]


def callStatus():
	paths = callDllPaths()
	if not paths:
		return CallStatus.NO_CALL
	pids = _callPids()
	if pids:
		return CallStatus.RUNNING if all(DLL_NAME in _processModules(pid) for pid in pids) else CallStatus.NOT_LOADED
	if os.path.isfile(BUNDLED_DLL) and all(os.path.isfile(p) and _sameFile(BUNDLED_DLL, p) for p in paths):
		return CallStatus.READY
	return CallStatus.MISSING


def describeCallStatus(st):
	return {
		# Translators: Reported when the bridge works in LINE's call process.
		CallStatus.RUNNING: _("Calls: the bridge is running."),
		# Translators: Reported when the bridge is ready for LINE's call process, which is not running.
		CallStatus.READY: _("Calls: the bridge is installed."),
		# Translators: Reported when LINE's call process runs without the bridge.
		CallStatus.NOT_LOADED: _("Calls: the bridge is not loaded yet. Restart LINE to use LINE MR with calls."),
		# Translators: Reported when the bridge could not be installed for LINE's call process.
		CallStatus.MISSING: _("Calls: the bridge could not be installed."),
		# Translators: Reported when LINE has not downloaded its call component yet.
		CallStatus.NO_CALL: _("Calls: LINE has not set up calls yet."),
	}[st]


def describeStatus(st):
	return {
		# Translators: Reported when the LINE accessibility bridge works.
		Status.RUNNING: _("LINE MR bridge is running."),
		# Translators: Reported when the bridge is installed but LINE has to be restarted to load it.
		Status.NOT_LOADED: _("LINE MR bridge is installed, but LINE must be restarted to use it."),
		# Translators: Reported when LINE started without the bridge right after it was installed.
		Status.BLOCKED: _(
			"LINE started without the LINE MR bridge. Windows or antivirus software is probably blocking it."
		),
		# Translators: Reported when an older version of the bridge is installed in LINE.
		Status.OUTDATED: _("An older LINE MR bridge is installed in LINE."),
		# Translators: Reported when the bridge is not installed in LINE (for example after a LINE update).
		Status.MISSING: _("LINE MR bridge is not installed in LINE."),
		# Translators: Reported when the installed LINE is too old (or too different) for the bridge.
		Status.INCOMPATIBLE: _("This version of LINE is not supported by LINE MR. Update LINE to the latest version."),
		# Translators: Reported when LINE for Windows cannot be found.
		Status.NO_LINE: _("LINE for Windows was not found."),
		# Translators: Reported when the add-on package does not contain the bridge.
		Status.NOT_BUNDLED: _("This LINE MR package does not contain the bridge."),
	}[st]


def describeSafeMode():
	# Translators: Reported when LINE crashed twice in a row and the bridge stopped reading message contents.
	return _(
		"LINE closed unexpectedly twice in a row, so LINE MR does not read message contents for now. "
		"Quit LINE normally and start it again to turn message reading back on."
	)


def describeBlocked(target):
	"""Why LINE probably started without the bridge, and what the user can do."""
	if not os.path.isfile(target):
		# Translators: Shown when the installed bridge disappeared from LINE's folder. {path} is the file.
		return _(
			"The LINE MR bridge was removed from LINE's folder, probably by antivirus software.\n"
			"Allow {path} in your antivirus software, then press NVDA+Shift+Control+L in LINE to install it again."
		).format(path=target)
	if smartAppControlOn():
		# Translators: Shown when Windows Smart App Control keeps LINE from loading the (unsigned) bridge.
		return _(
			"Windows Smart App Control is on. It blocks the LINE MR bridge ({path}), so LINE MR cannot work.\n"
			"Smart App Control can be turned off in Windows Security, App & browser control "
			"(Windows cannot turn it on again without a reinstall)."
		).format(path=target)
	# Translators: Shown when LINE started without the bridge for an unknown reason. {path} is the bridge file.
	return _(
		"LINE started without the LINE MR bridge ({path}). Antivirus software or a security policy may be blocking it: "
		"allow the file, then restart LINE.\n"
		"To send a report to the developer, press NVDA+Shift+Control+L twice in LINE: it copies the report to the clipboard."
	).format(path=target)


# ---- Installing ---------------------------------------------------------------------------------


def _waitForExit(pid, timeout=10.0):
	handle = _kernel32.OpenProcess(SYNCHRONIZE, False, pid)
	if not handle:
		return True
	try:
		return _kernel32.WaitForSingleObject(handle, int(timeout * 1000)) == 0
	finally:
		_kernel32.CloseHandle(handle)


def _writable(directory):
	probe = os.path.join(directory, ".lineMR-write-test")
	try:
		os.makedirs(directory, exist_ok=True)
		with open(probe, "w"):
			pass
		os.remove(probe)
		return True
	except OSError:
		return False


def _elevatedCopy(source, targets):
	"""Copies with administrator rights (LINE installed for all users); Windows asks the user first."""
	commands = [
		'(if not exist "{d}" mkdir "{d}") & copy /y "{s}" "{t}"'.format(d=os.path.dirname(t), s=source, t=t)
		for t in targets
	]
	info = _ShellExecuteInfo()
	info.cbSize = ctypes.sizeof(info)
	info.fMask = SEE_MASK_NOCLOSEPROCESS
	info.lpVerb = "runas"
	info.lpFile = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "cmd.exe")
	info.lpParameters = "/c " + " & ".join(commands)
	info.nShow = SW_HIDE
	if not _shell32.ShellExecuteExW(ctypes.byref(info)):
		log.warning("LINE MR: administrator copy not started (error %d)", ctypes.get_last_error())
		return False
	if info.hProcess:
		_kernel32.WaitForSingleObject(info.hProcess, 120000)
		_kernel32.CloseHandle(info.hProcess)
	return all(os.path.isfile(t) and _sameFile(source, t) for t in targets)


def _copyBridge(target):
	"""Copies the bundled bridge to target; returns the error (OSError) or None."""
	error = None
	for _attempt in range(20):
		try:
			os.makedirs(os.path.dirname(target), exist_ok=True)
			shutil.copyfile(BUNDLED_DLL, target)
			return None
		except OSError as e:
			error = e
			time.sleep(0.25)  # the DLL can stay locked for a moment after LINE exits
	return error


def _launcher(directory):
	for path in (
		os.path.join(os.path.dirname(directory), "LineLauncher.exe"),
		os.path.join(directory, "LineLauncher.exe"),
		os.path.join(directory, "LINE.exe"),
	):
		if os.path.isfile(path):
			return path
	return None


def _waitForBridge(timeout=90.0):
	"""After starting LINE: True if it loaded the bridge, False if Qt started without it, None if
	LINE did not get that far."""
	deadline = time.monotonic() + timeout
	while time.monotonic() < deadline:
		time.sleep(1)
		for name, pid in _lineProcesses():
			if name != "line.exe":
				continue
			modules = _processModules(pid)
			if DLL_NAME in modules:
				return True
			if "qwindows.dll" in modules:
				# Qt has loaded its platform plugin; it asks for platform themes right after.
				time.sleep(3)
				return DLL_NAME in _processModules(pid)
	return None


def _showMessage(message, error=True):
	def show():
		gui.messageBox(
			message,
			# Translators: Title of LINE MR's dialogs.
			_("LINE MR"),
			wx.OK | (wx.ICON_ERROR if error else wx.ICON_INFORMATION),
		)

	wx.CallAfter(show)


def _quitLine():
	"""Ends LINE.exe and its helper processes (they lock their copies of the bridge)."""
	for pid in _linePids():
		subprocess.run(
			["taskkill", "/PID", str(pid), "/F"],
			capture_output=True,
			creationflags=subprocess.CREATE_NO_WINDOW,
		)
		_waitForExit(pid)


#: True while installAndRestart() runs (no offers meanwhile).
_installing = False


def installAndRestart():
	"""Quits LINE, installs the bundled bridge, starts LINE again and checks that LINE loaded it.
	Runs off the main thread."""
	global _installing
	_installing = True
	try:
		_installAndRestart()
	finally:
		_installing = False


def _installAndRestart():
	global _blockedPath
	directory = lineDir()  # before LINE quits: the running LINE.exe tells where it is
	if not directory:
		wx.CallAfter(ui.message, describeStatus(Status.NO_LINE))
		return
	_quitLine()
	# Not a crash: keep the bridge's crash guard (%LOCALAPPDATA%\LineMR\session.running) from counting it.
	try:
		os.remove(os.path.join(bridgeDataDir(), "session.running"))
	except OSError:
		pass
	target = installedDllPath(directory)
	failure = None
	error = _copyBridge(target)
	if error is not None and _writable(os.path.dirname(target)):
		# Still locked: a LINE process may have started again meanwhile (or lingered). Once more.
		log.info("LINE MR: %s is locked (%s); quitting LINE again", target, error)
		_quitLine()
		error = _copyBridge(target)
	if error is not None and not _writable(os.path.dirname(target)):
		# LINE installed for all users (Program Files): only an administrator can add the file.
		log.info("LINE MR: %s needs administrator rights", directory)
		# Translators: Spoken before Windows asks for administrator rights to install the bridge.
		wx.CallAfter(ui.message, _("LINE is installed for all users. Windows will ask for permission to install the bridge."))
		if _elevatedCopy(BUNDLED_DLL, [target]):
			error = None
	if error is not None:
		log.error("LINE MR: could not install the bridge to %s: %s", target, error)
		# Translators: Shown when copying the bridge into LINE's folder failed. {path}: the file, {error}: the reason.
		failure = _("Could not install the LINE MR bridge to {path}.\n{error}").format(path=target, error=error)
	else:
		log.info("LINE MR: installed bridge to %s", target)
	for helperTarget in helperDllPaths(directory):
		helperError = _copyBridge(helperTarget)
		if helperError is None:
			log.info("LINE MR: installed bridge to %s", helperTarget)
		else:
			log.error("LINE MR: could not install the bridge to %s: %s", helperTarget, helperError)
	launcher = _launcher(directory)
	if launcher:
		subprocess.Popen([launcher], cwd=os.path.dirname(launcher), close_fds=True)
	if failure:
		_showMessage(failure)
		return
	# Translators: Reported while LINE restarts after the bridge was installed.
	wx.CallAfter(ui.message, _("LINE MR bridge installed. LINE is restarting."))
	loaded = _waitForBridge()
	log.info("LINE MR: bridge loaded after restart: %s", loaded)
	if loaded is False:
		_blockedPath = target
		_showMessage(describeBlocked(target))


# Offers come back while the bridge is not running: when LINE starts, and when the user switches to
# LINE again (closing LINE's window only hides it in the notification area, so a LINE started before
# the install keeps running without the bridge). Not more often than this, longer after a "No".
OFFER_INTERVAL = 60.0
DECLINED_INTERVAL = 600.0
_lastOffer = None  # time.monotonic() of the last offer
_declined = False
_dialogOpen = False
#: Statuses LINE MR can do nothing about (no LINE, unsupported LINE): told once per NVDA session.
_told = set()


def offerInstall(pid, st, force=False):
	"""Asks the user whether to install the bridge and restart LINE, or says why LINE MR cannot work.
	`force` (the status command) asks even if it asked a moment ago."""
	global _lastOffer, _declined, _dialogOpen
	if st in (Status.RUNNING, Status.NOT_BUNDLED) or _installing or _dialogOpen:
		return
	if st in (Status.NO_LINE, Status.INCOMPATIBLE):
		if st in _told and not force:
			return
		_told.add(st)
		if st == Status.NO_LINE:
			wx.CallAfter(ui.message, describeStatus(st))
			return
		directory = lineDir(pid)
		version = lineQtVersion(directory) if directory else None
		log.warning("LINE MR: LINE in %s uses Qt %s, the bridge needs Qt %d.%d or later", directory, version, *MIN_QT)
		_showMessage(describeStatus(st), error=False)
		return
	now = time.monotonic()
	if not force and _lastOffer is not None:
		if now - _lastOffer < (DECLINED_INTERVAL if _declined else OFFER_INTERVAL):
			return
	_lastOffer = now
	if st == Status.NOT_LOADED:
		# Translators: Question shown when LINE has to be restarted to load the bridge.
		question = _(
			"LINE has to be restarted to use LINE MR. Closing LINE's window does not quit LINE "
			"(it keeps running in the notification area).\nRestart LINE now?"
		)
	elif st == Status.BLOCKED:
		# Translators: Question shown after LINE started without the bridge; {reason} explains why.
		question = _("{reason}\n\nInstall the bridge again and restart LINE?").format(
			reason=describeBlocked(_blockedPath or installedDllPath(lineDir(pid)))
		)
	else:
		# Translators: Question shown when the bridge must be installed (LINE will restart).
		question = _(
			"{status}\nInstall the LINE MR bridge now? LINE will be closed and started again."
		).format(status=describeStatus(st))
	log.info("LINE MR: offering to install / restart (status %s)", st.value)

	def ask():
		global _declined, _dialogOpen
		_dialogOpen = True
		try:
			answer = gui.messageBox(
				question,
				# Translators: Title of the dialog offering to install the bridge.
				_("LINE MR"),
				wx.YES_NO | wx.ICON_QUESTION,
			)
		finally:
			_dialogOpen = False
		_declined = answer != wx.YES
		if answer == wx.YES:
			threading.Thread(target=installAndRestart, name="lineMR.install", daemon=True).start()

	_dialogOpen = True  # until ask() runs
	wx.CallAfter(ask)


def checkOnStart(pid):
	try:
		repairHelperBridges()
		st = status(pid)
	except Exception:
		log.error("LINE MR: bridge status check failed", exc_info=True)
		return
	log.info("LINE MR: bridge status %s (LINE pid %s, folder %s)", st.value, pid, lineDir(pid))
	if st == Status.RUNNING and inSafeMode():
		log.warning("LINE MR: the bridge runs in safe mode (LINE crashed twice in a row)")
		core.callLater(4000, ui.message, describeSafeMode())
	offerInstall(pid, st)


_lastFocusCheck = None


def checkOnFocus(pid):
	"""The user switched to LINE: offer again if the bridge is still not running (see OFFER_INTERVAL)."""
	global _lastFocusCheck
	now = time.monotonic()
	if _installing or _dialogOpen or (_lastFocusCheck is not None and now - _lastFocusCheck < OFFER_INTERVAL):
		return
	_lastFocusCheck = now
	try:
		st = status(pid)
	except Exception:
		log.error("LINE MR: bridge status check failed", exc_info=True)
		return
	if st != Status.RUNNING:
		log.info("LINE MR: LINE in front, bridge status %s", st.value)
		offerInstall(pid, st)


# ---- Diagnostics --------------------------------------------------------------------------------

#: Bridge log lines worth reporting (no chat contents: start-up, language, guard, calls, handler).
_REPORT_LOG_TAGS = ("[plugin]", "[bridge]", "[guard]", "[introspect]", "[handler]", "[call]", "[crash]")


def _private(text):
	"""Replaces the user's folders (which hold the Windows user name) with variables."""
	for var in ("LOCALAPPDATA", "APPDATA", "USERPROFILE"):
		value = os.environ.get(var)
		if value:
			for form in (value, value.replace("\\", "/")):
				text = text.replace(form, "%" + var + "%")
	return text


def _versionText(version):
	return ".".join(str(n) for n in version) if version else "unknown"


def _lineLanguage():
	try:
		with open(os.path.join(_localAppData(), "LINE", "Data", "installLang.ini"), encoding="utf-8-sig") as f:
			for line in f:
				if line.strip().lower().startswith("installlang="):
					return line.split("=", 1)[1].strip()
	except OSError:
		pass
	return "unknown"


def _displayScale():
	"""Windows' display scale in percent (as applied at sign-in; not affected by DPI awareness)."""
	try:
		with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Control Panel\Desktop\WindowMetrics") as key:
			dpi, _type = winreg.QueryValueEx(key, "AppliedDPI")
	except OSError:
		dpi = _user32.GetDpiForSystem()
	return round(dpi * 100 / 96)


def _bridgeLogLines(limit=40):
	path = os.path.join(bridgeDataDir(), "logs", "bridge.log")
	try:
		with open(path, encoding="utf-8", errors="replace") as f:
			lines = [line.rstrip() for line in f if any(tag in line for tag in _REPORT_LOG_TAGS)]
	except OSError:
		return ["(no bridge log: the bridge has never run in LINE)"]
	return lines[:limit]


def diagnostics(pid=None):
	"""A plain-text report for the developer (no chat contents, user folders replaced)."""
	import buildVersion
	import winVersion

	addon = addonHandler.getCodeAddon()
	directory = lineDir(pid)
	lines = [
		"LINE MR %s" % addon.version,
		"NVDA %s" % buildVersion.version,
		"Windows %s" % winVersion.getWinVer(),
		"Display scale %d%%" % _displayScale(),
		"Smart App Control: %s" % ("on" if smartAppControlOn() else "off or evaluation"),
		"LINE folder: %s" % (directory or "not found"),
	]
	if directory:
		lines += [
			"LINE version: %s" % _versionText(fileVersion(os.path.join(directory, "LINE.exe"))),
			"Qt version: %s" % _versionText(lineQtVersion(directory)),
			"Bridge file: %s" % (
				"same as the add-on's" if os.path.isfile(installedDllPath(directory)) and os.path.isfile(BUNDLED_DLL)
				and _sameFile(BUNDLED_DLL, installedDllPath(directory))
				else ("present, different" if os.path.isfile(installedDllPath(directory)) else "missing")
			),
		]
	lines += [
		"LINE language: %s" % _lineLanguage(),
		"Status: %s" % status(pid).value,
		"Calls: %s (%s)" % (callStatus().value, ", ".join(os.path.basename(os.path.dirname(os.path.dirname(p))) for p in callDllPaths()) or "none"),
		"Media player: %s" % (
			", ".join(
				"%s %s" % (
					os.path.basename(os.path.dirname(os.path.dirname(p))),
					"current" if os.path.isfile(p) and os.path.isfile(BUNDLED_DLL) and _sameFile(BUNDLED_DLL, p) else "missing or different",
				)
				for p in _helperDllPaths("LineMediaPlayer")
			) or "none"
		),
		"Safe mode: %s" % ("yes" if inSafeMode() else "no"),
		"LINE processes: %s" % (
			", ".join(
				"%s %d%s" % (name, p, " (bridge loaded)" if DLL_NAME in _processModules(p) else "")
				for name, p in _lineProcesses()
			) or "none"
		),
		"",
		"Bridge log:",
	]
	lines += _bridgeLogLines()
	return _private("\n".join(lines))
