# Keeps NVDA at its own volume during LINE calls.
#
# LINE's call engine (LineCall.exe, ampkit_windows.dll: "Ducking started. ducking on vol_dB ...") lowers
# the volume of every other application's audio session itself (ISimpleAudioVolume, the Volume Mixer's
# slider) while a call rings or runs, and sets it back afterwards. So while LineCall.exe shows a window,
# NVDA's sessions are set back to the volume they had before the call: at once when Windows reports a
# volume change of the session (IAudioSessionEvents), and every 100 ms in case an event is missed.
#
# Windows itself can do the same: when a call opens an audio stream on the communications device, Windows lowers every other sound
# ("Reduce the volume of other sounds by 80%", the default on the Communications tab of the Sound
# control panel), NVDA's speech included, until the call ends. An application whose sound is lowered
# can opt out per audio session with IAudioSessionControl2::SetDuckingPreference(TRUE); the change
# applies from the next start of the session's stream (NVDA stops its stream whenever it falls
# silent). NVDA can create sessions at any time (output device or synthesizer change), so they are
# looked for regularly and when a call comes in. This applies to calls in other applications too.

import ctypes
import ctypes.wintypes as wintypes
import os
import threading
import time

import comtypes
from comtypes import COMMETHOD, COMObject, GUID, HRESULT, IUnknown
from logHandler import log

_CLSID_MMDeviceEnumerator = GUID("{BCDE0395-E52F-467C-8E3D-C4579291692E}")
_eRender = 0
_DEVICE_STATE_ACTIVE = 1
_CLSCTX_ALL = 23


class IMMDevice(IUnknown):
	_iid_ = GUID("{D666063F-1587-4E43-81F1-B948E807363F}")
	_methods_ = (
		COMMETHOD(
			[],
			HRESULT,
			"Activate",
			(["in"], ctypes.POINTER(GUID), "iid"),
			(["in"], wintypes.DWORD, "dwClsCtx"),
			(["in"], ctypes.c_void_p, "pActivationParams"),
			(["out"], ctypes.POINTER(ctypes.POINTER(IUnknown)), "ppInterface"),
		),
	)


class IMMDeviceCollection(IUnknown):
	_iid_ = GUID("{0BD7A1BE-7A1A-44DB-8397-CC5392387B5E}")
	_methods_ = (
		COMMETHOD([], HRESULT, "GetCount", (["out"], ctypes.POINTER(wintypes.UINT), "pcDevices")),
		COMMETHOD(
			[],
			HRESULT,
			"Item",
			(["in"], wintypes.UINT, "nDevice"),
			(["out"], ctypes.POINTER(ctypes.POINTER(IMMDevice)), "ppDevice"),
		),
	)


class IMMDeviceEnumerator(IUnknown):
	_iid_ = GUID("{A95664D2-9614-4F35-A746-DE8DB63617E6}")
	_methods_ = (
		COMMETHOD(
			[],
			HRESULT,
			"EnumAudioEndpoints",
			(["in"], ctypes.c_int, "dataFlow"),
			(["in"], wintypes.DWORD, "dwStateMask"),
			(["out"], ctypes.POINTER(ctypes.POINTER(IMMDeviceCollection)), "ppDevices"),
		),
	)


class IAudioSessionEvents(IUnknown):
	_iid_ = GUID("{24918ACC-64B3-37C1-8CA9-74A66E9957A8}")
	_methods_ = (
		COMMETHOD([], HRESULT, "OnDisplayNameChanged", (["in"], wintypes.LPCWSTR, "NewDisplayName"), (["in"], ctypes.POINTER(GUID), "EventContext")),
		COMMETHOD([], HRESULT, "OnIconPathChanged", (["in"], wintypes.LPCWSTR, "NewIconPath"), (["in"], ctypes.POINTER(GUID), "EventContext")),
		COMMETHOD([], HRESULT, "OnSimpleVolumeChanged", (["in"], ctypes.c_float, "NewVolume"), (["in"], wintypes.BOOL, "NewMute"), (["in"], ctypes.POINTER(GUID), "EventContext")),
		COMMETHOD([], HRESULT, "OnChannelVolumeChanged", (["in"], wintypes.DWORD, "ChannelCount"), (["in"], ctypes.POINTER(ctypes.c_float), "NewChannelVolumeArray"), (["in"], wintypes.DWORD, "ChangedChannel"), (["in"], ctypes.POINTER(GUID), "EventContext")),
		COMMETHOD([], HRESULT, "OnGroupingParamChanged", (["in"], ctypes.POINTER(GUID), "NewGroupingParam"), (["in"], ctypes.POINTER(GUID), "EventContext")),
		COMMETHOD([], HRESULT, "OnStateChanged", (["in"], ctypes.c_int, "NewState")),
		COMMETHOD([], HRESULT, "OnSessionDisconnected", (["in"], ctypes.c_int, "DisconnectReason")),
	)


class IAudioSessionControl(IUnknown):
	_iid_ = GUID("{F4B1A599-7266-4319-A8CA-E70ACB11E8CD}")
	_methods_ = (
		COMMETHOD([], HRESULT, "GetState", (["out"], ctypes.POINTER(ctypes.c_int), "pRetVal")),
		COMMETHOD([], HRESULT, "GetDisplayName", (["out"], ctypes.POINTER(ctypes.c_void_p), "pRetVal")),
		COMMETHOD([], HRESULT, "SetDisplayName", (["in"], wintypes.LPCWSTR, "Value"), (["in"], ctypes.c_void_p, "EventContext")),
		COMMETHOD([], HRESULT, "GetIconPath", (["out"], ctypes.POINTER(ctypes.c_void_p), "pRetVal")),
		COMMETHOD([], HRESULT, "SetIconPath", (["in"], wintypes.LPCWSTR, "Value"), (["in"], ctypes.c_void_p, "EventContext")),
		COMMETHOD([], HRESULT, "GetGroupingParam", (["out"], ctypes.POINTER(GUID), "pRetVal")),
		COMMETHOD([], HRESULT, "SetGroupingParam", (["in"], ctypes.POINTER(GUID), "Override"), (["in"], ctypes.c_void_p, "EventContext")),
		COMMETHOD([], HRESULT, "RegisterAudioSessionNotification", (["in"], ctypes.POINTER(IAudioSessionEvents), "NewNotifications")),
		COMMETHOD([], HRESULT, "UnregisterAudioSessionNotification", (["in"], ctypes.POINTER(IAudioSessionEvents), "NewNotifications")),
	)


class IAudioSessionControl2(IAudioSessionControl):
	_iid_ = GUID("{BFB7FF88-7239-4FC9-8FA2-07C950BE9C6D}")
	_methods_ = (
		COMMETHOD([], HRESULT, "GetSessionIdentifier", (["out"], ctypes.POINTER(ctypes.c_void_p), "pRetVal")),
		COMMETHOD([], HRESULT, "GetSessionInstanceIdentifier", (["out"], ctypes.POINTER(ctypes.c_void_p), "pRetVal")),
		COMMETHOD([], HRESULT, "GetProcessId", (["out"], ctypes.POINTER(wintypes.DWORD), "pRetVal")),
		COMMETHOD([], HRESULT, "IsSystemSoundsSession"),
		COMMETHOD([], HRESULT, "SetDuckingPreference", (["in"], wintypes.BOOL, "optOut")),
	)


class ISimpleAudioVolume(IUnknown):
	_iid_ = GUID("{87CE5498-68D6-44E5-9215-6DA47EF883D8}")
	_methods_ = (
		COMMETHOD([], HRESULT, "SetMasterVolume", (["in"], ctypes.c_float, "fLevel"), (["in"], ctypes.POINTER(GUID), "EventContext")),
		COMMETHOD([], HRESULT, "GetMasterVolume", (["out"], ctypes.POINTER(ctypes.c_float), "pfLevel")),
	)


class IAudioSessionEnumerator(IUnknown):
	_iid_ = GUID("{E2F5BB11-0570-40CA-ACDD-3AA01277DEE8}")
	_methods_ = (
		COMMETHOD([], HRESULT, "GetCount", (["out"], ctypes.POINTER(ctypes.c_int), "SessionCount")),
		COMMETHOD(
			[],
			HRESULT,
			"GetSession",
			(["in"], ctypes.c_int, "SessionCount"),
			(["out"], ctypes.POINTER(ctypes.POINTER(IAudioSessionControl)), "Session"),
		),
	)


class IAudioSessionManager2(IUnknown):
	_iid_ = GUID("{77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F}")
	_methods_ = (
		# IAudioSessionManager
		COMMETHOD([], HRESULT, "GetAudioSessionControl", (["in"], ctypes.c_void_p, "AudioSessionGuid"), (["in"], wintypes.DWORD, "StreamFlags"), (["out"], ctypes.POINTER(ctypes.c_void_p), "SessionControl")),
		COMMETHOD([], HRESULT, "GetSimpleAudioVolume", (["in"], ctypes.c_void_p, "AudioSessionGuid"), (["in"], wintypes.DWORD, "StreamFlags"), (["out"], ctypes.POINTER(ctypes.c_void_p), "AudioVolume")),
		# IAudioSessionManager2
		COMMETHOD([], HRESULT, "GetSessionEnumerator", (["out"], ctypes.POINTER(ctypes.POINTER(IAudioSessionEnumerator)), "SessionEnum")),
	)


class _ProcessEntry(ctypes.Structure):
	_fields_ = (
		("dwSize", wintypes.DWORD),
		("cntUsage", wintypes.DWORD),
		("th32ProcessID", wintypes.DWORD),
		("th32DefaultHeapID", ctypes.c_size_t),
		("th32ModuleID", wintypes.DWORD),
		("cntThreads", wintypes.DWORD),
		("th32ParentProcessID", wintypes.DWORD),
		("pcPriClassBase", wintypes.LONG),
		("dwFlags", wintypes.DWORD),
		("szExeFile", wintypes.WCHAR * 260),
	)


_kernel32 = ctypes.WinDLL("kernel32")
_kernel32.CreateToolhelp32Snapshot.argtypes = (wintypes.DWORD, wintypes.DWORD)
_kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
_kernel32.Process32FirstW.argtypes = (wintypes.HANDLE, ctypes.POINTER(_ProcessEntry))
_kernel32.Process32FirstW.restype = wintypes.BOOL
_kernel32.Process32NextW.argtypes = (wintypes.HANDLE, ctypes.POINTER(_ProcessEntry))
_kernel32.Process32NextW.restype = wintypes.BOOL
_kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
_kernel32.CloseHandle.restype = wintypes.BOOL
_TH32CS_SNAPPROCESS = 2
_INVALID_HANDLE = wintypes.HANDLE(-1).value


def _nvdaPids():
	"""NVDA's process and its child processes (32-bit synthesizer hosts play their own sound)."""
	own = os.getpid()
	pids = {own}
	snapshot = _kernel32.CreateToolhelp32Snapshot(_TH32CS_SNAPPROCESS, 0)
	if not snapshot or snapshot == _INVALID_HANDLE:
		return pids
	try:
		entry = _ProcessEntry()
		entry.dwSize = ctypes.sizeof(entry)
		ok = _kernel32.Process32FirstW(snapshot, ctypes.byref(entry))
		while ok:
			if entry.th32ParentProcessID == own:
				pids.add(entry.th32ProcessID)
			ok = _kernel32.Process32NextW(snapshot, ctypes.byref(entry))
	finally:
		_kernel32.CloseHandle(snapshot)
	return pids


def _sessions():
	"""IAudioSessionControl2 of every audio session on every active output device."""
	enumerator = comtypes.CoCreateInstance(_CLSID_MMDeviceEnumerator, IMMDeviceEnumerator, comtypes.CLSCTX_INPROC_SERVER)
	devices = enumerator.EnumAudioEndpoints(_eRender, _DEVICE_STATE_ACTIVE)
	for d in range(devices.GetCount()):
		try:
			manager = devices.Item(d).Activate(IAudioSessionManager2._iid_, _CLSCTX_ALL, None)
			sessions = manager.QueryInterface(IAudioSessionManager2).GetSessionEnumerator()
			for s in range(sessions.GetCount()):
				yield sessions.GetSession(s).QueryInterface(IAudioSessionControl2)
		except comtypes.COMError:
			log.debugWarning("LINE MR: cannot list the audio sessions of an output device", exc_info=True)


def _nvdaSessions():
	"""{session instance id: IAudioSessionControl2} of NVDA's audio sessions."""
	pids = _nvdaPids()
	found = {}
	for session in _sessions():
		try:
			if session.GetProcessId() in pids:
				pointer = session.GetSessionInstanceIdentifier()
				try:
					found[ctypes.wstring_at(pointer)] = session
				finally:
					ctypes.windll.ole32.CoTaskMemFree(ctypes.c_void_p(pointer))
		except comtypes.COMError:
			# A session that has just ended, or one on a device that went away.
			log.debugWarning("LINE MR: cannot read an audio session", exc_info=True)
	return found


#: Event context of the volume changes made here (their events are not reacted to).
_OWN_CHANGE = GUID("{6C1E4D2B-7A0F-4C55-9E3B-1F8D2A6B0C71}")


class _VolumeEvents(COMObject):
	"""Wakes the keeper when something else changes the volume of one of NVDA's sessions."""

	_com_interfaces_ = (IAudioSessionEvents,)

	def __init__(self, keeper):
		super().__init__()
		self._keeper = keeper

	def OnSimpleVolumeChanged(self, newVolume, newMute, context):
		if not (context and context[0] == _OWN_CHANGE):
			self._keeper.volumeChanged()

	def OnDisplayNameChanged(self, name, context):
		pass

	def OnIconPathChanged(self, path, context):
		pass

	def OnChannelVolumeChanged(self, count, volumes, changed, context):
		pass

	def OnGroupingParamChanged(self, param, context):
		pass

	def OnStateChanged(self, state):
		pass

	def OnSessionDisconnected(self, reason):
		pass


def _callShown():
	from .calls import callWindowShown
	return callWindowShown()


class Keeper(threading.Thread):
	"""Keeps NVDA's audio sessions at their volume while LINE shows a call (see the top of this file).

	Outside calls, every `interval` seconds: opts NVDA's sessions out of Windows' call ducking and
	remembers their volume. Looks for a LINE call window every second and whenever `poke()` is called.
	"""

	def __init__(self, interval=15.0):
		super().__init__(name="lineMR.keepNvdaVolume", daemon=True)
		self._interval = interval
		self._wake = threading.Event()
		self._stopping = False
		self._sessions = {}
		#: {session instance id: (IAudioSessionControl2, _VolumeEvents)} registered for volume changes
		self._watched = {}
		#: {session instance id: volume before the call}
		self._volumes = {}
		#: The volumes of the scan before, with the time of the last scan that remembered volumes.
		self._older = {}
		self._remembered = 0.0
		self._lastScan = 0.0
		self._inCall = False
		self._restored = False
		#: Look for a call window every 100 ms until then (LINE may lower the volume just before it shows one).
		self._watchUntil = 0.0

	def run(self):
		comtypes.CoInitializeEx(comtypes.COINIT_MULTITHREADED)
		try:
			while not self._stopping:
				try:
					self._step()
				except Exception:
					log.debugWarning("LINE MR: keeping NVDA's volume during calls failed", exc_info=True)
				fast = self._inCall or time.monotonic() < self._watchUntil
				self._wake.wait(0.1 if fast else 1.0)
				self._wake.clear()
		finally:
			self._unwatch(set(self._watched))
			self._sessions = {}
			comtypes.CoUninitialize()

	def _step(self):
		inCall = _callShown()
		if inCall != self._inCall:
			self._inCall = inCall
			self._restored = False
			log.debug(f"LINE MR: LINE call window {'shown' if inCall else 'gone'}")
			if inCall and time.monotonic() - self._remembered < 2.0:
				# The last scan may have seen a volume LINE had already lowered.
				for key, older in self._older.items():
					if key in self._volumes:
						self._volumes[key] = max(self._volumes[key], older)
		now = time.monotonic()
		# Sessions are looked for again during calls too (NVDA opens one when it starts to speak).
		if now - self._lastScan >= (2.0 if inCall else self._interval):
			self._lastScan = now
			self._scan(remember=not inCall)
		if inCall:
			self._restore()

	def _scan(self, remember):
		self._sessions = _nvdaSessions()
		if remember:
			self._older = dict(self._volumes)
			self._remembered = time.monotonic()
		for key, session in self._sessions.items():
			try:
				session.SetDuckingPreference(True)
				if remember:
					self._volumes[key] = session.QueryInterface(ISimpleAudioVolume).GetMasterVolume()
			except comtypes.COMError:
				log.debugWarning("LINE MR: cannot set up an audio session", exc_info=True)
		for key in set(self._volumes) - set(self._sessions):
			del self._volumes[key]
		self._unwatch(set(self._watched) - set(self._sessions))
		for key in set(self._sessions) - set(self._watched):
			session = self._sessions[key]
			sink = _VolumeEvents(self)
			try:
				session.RegisterAudioSessionNotification(sink)
				self._watched[key] = (session, sink)
			except comtypes.COMError:
				log.debugWarning("LINE MR: cannot watch the volume of an audio session", exc_info=True)

	def _unwatch(self, keys):
		for key in keys:
			session, sink = self._watched.pop(key)
			try:
				session.UnregisterAudioSessionNotification(sink)
			except comtypes.COMError:
				pass

	def _restore(self):
		for key, session in self._sessions.items():
			wanted = self._volumes.get(key)
			if wanted is None:
				continue
			try:
				volume = session.QueryInterface(ISimpleAudioVolume)
				level = volume.GetMasterVolume()
				if level < wanted - 0.01:
					volume.SetMasterVolume(wanted, ctypes.byref(_OWN_CHANGE))
					if not self._restored:
						self._restored = True
						log.info(f"LINE MR: LINE lowered NVDA's volume to {level:.2f} during a call; set back to {wanted:.2f}")
			except comtypes.COMError:
				log.debugWarning("LINE MR: cannot set the volume of an audio session", exc_info=True)

	def poke(self):
		self._wake.set()

	def volumeChanged(self):
		"""Something else changed the volume of one of NVDA's sessions (any thread)."""
		self._watchUntil = time.monotonic() + 3.0
		self._wake.set()

	def stop(self):
		self._stopping = True
		self._wake.set()
