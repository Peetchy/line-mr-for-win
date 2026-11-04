"""Offline tests of the Reply / Delete / Unsend dispatch (addon/appModules/line/messageActions.py).

NVDA's modules are replaced by stubs; no LINE message is touched.

Usage: python tests/test_message_actions.py
"""

import importlib.util
import sys
import types
import unittest
from pathlib import Path


class Obj:
	def __init__(self, parent=None, tag="", pid=42):
		self.parent = parent
		self.UIAAutomationId = tag
		self.processID = pid


sent = []


class Key:
	def __init__(self, name):
		self.name = name

	def send(self):
		sent.append(self.name)


def _loadModule():
	api = types.ModuleType("api")
	api.getFocusObject = lambda: api.focus
	keyboard = types.ModuleType("keyboardHandler")
	keyboard.KeyboardInputGesture = types.SimpleNamespace(fromName=Key)
	uia = types.ModuleType("NVDAObjects.UIA")
	uia.UIA = Obj
	logging = types.ModuleType("logHandler")
	logging.log = types.SimpleNamespace(info=lambda *a, **kw: None, debugWarning=lambda *a, **kw: None)
	elements = types.ModuleType("actions_test.elements")
	elements.TAG_PREFIX = "lineMR_"
	package = types.ModuleType("actions_test")
	package.__path__ = []
	sys.modules.update({
		"api": api,
		"keyboardHandler": keyboard,
		"NVDAObjects": types.ModuleType("NVDAObjects"),
		"NVDAObjects.UIA": uia,
		"logHandler": logging,
		"actions_test": package,
		"actions_test.elements": elements,
	})
	source = Path(__file__).resolve().parents[1] / "addon" / "appModules" / "line" / "messageActions.py"
	spec = importlib.util.spec_from_file_location("actions_test.messageActions", source)
	module = importlib.util.module_from_spec(spec)
	spec.loader.exec_module(module)
	return api, module


api, actions = _loadModule()


class DispatchTests(unittest.TestCase):
	def setUp(self):
		sent.clear()
		self.app = types.SimpleNamespace(processID=42)
		api.focus = Obj(parent=Obj(tag="lineMR_list_messages"))

	def test_three_distinct_commands(self):
		for command in ("reply", "delete", "unsend"):
			actions.run(self.app, command, Key("original"))
		self.assertEqual(sent, ["control+alt+shift+f9", "control+alt+shift+f10", "control+alt+shift+f11"])

	def test_message_input_keeps_original_key(self):
		api.focus = Obj(tag="lineMR_input_message")
		actions.run(self.app, "delete", Key("original"))
		self.assertEqual(sent, ["original"])

	def test_other_process_keeps_original_key(self):
		api.focus.processID = 99
		actions.run(self.app, "unsend", Key("original"))
		self.assertEqual(sent, ["original"])

	def test_held_key_dispatches_once(self):
		actions.run(self.app, "delete", Key("original"))
		actions.run(self.app, "delete", Key("original"))
		self.assertEqual(sent, ["control+alt+shift+f10"])

	def test_prefixed_list_id(self):
		api.focus = Obj(tag="parent.lineMR_list_messages")
		actions.run(self.app, "reply", Key("original"))
		self.assertEqual(sent, ["control+alt+shift+f9"])

	def test_unknown_focus_keeps_original_key(self):
		api.focus = Obj()
		actions.run(self.app, "reply", Key("original"))
		self.assertEqual(sent, ["original"])


if __name__ == "__main__":
	unittest.main()
