# Commands with letters and digits work in any keyboard layout (user request 2026-11-08).
#
# NVDA names a key by the character the current layout gives it: with the Thai layout Alt+C is
# "alt+แ", so Alt+C, Alt+1 ... did nothing until the user switched to English. Layouts of other
# scripts (Thai, Russian, Greek ...) keep the English layout's virtual key codes on those keys, so a
# command whose key gives no English letter or digit is looked up again under the letter / digit of
# its virtual key code, which is the key at the same place on an English keyboard. Latin layouts
# (German, French ...) are left alone: their keys already name the letter printed on them.

import inputCore
import keyboardHandler
from logHandler import log


def _usKey(gesture):
	"""The English letter / digit of the key at the same place, or "" if NVDA's name already is one or
	the key is no letter / digit key."""
	if not isinstance(gesture, keyboardHandler.KeyboardInputGesture):
		return ""
	name = gesture.mainKeyName or ""
	if len(name) == 1 and name.isascii() and name.isalnum():
		return ""
	vk = gesture.vkCode
	if 0x41 <= vk <= 0x5A or 0x30 <= vk <= 0x39:
		return chr(vk).lower()
	return ""


def _identifier(gesture, key):
	return inputCore.normalizeGestureIdentifier("kb:" + "+".join(list(gesture.modifierNames) + [key]))


class _Renamed:
	"""The gesture under the English key's name (only what ScriptableObject.getScript() reads)."""

	def __init__(self, identifier):
		self.normalizedIdentifiers = [identifier]


def getScript(obj, gesture, default):
	"""For a ScriptableObject's getScript(): `default(gesture)`, else the script of the English key at
	the same place (the user's own Input gestures first, as NVDA does)."""
	script = default(gesture)
	if script:
		return script
	key = _usKey(gesture)
	if not key:
		return None
	identifier = _identifier(gesture, key)
	try:
		module = obj.__class__.__module__
		for cls in obj.__class__.__mro__:
			for mod, className, name in inputCore.manager.userGestureMap.getScriptsForGesture(identifier):
				if className == cls.__name__ and mod in (module, cls.__module__):
					# None: the user removed this gesture.
					return getattr(obj, "script_" + name, None) if name else None
	except Exception:
		log.debugWarning("LINE MR: user gesture lookup failed", exc_info=True)
	return default(_Renamed(identifier))
