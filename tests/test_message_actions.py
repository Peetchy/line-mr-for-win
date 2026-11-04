"""Offline dispatcher tests; no real LINE messages are modified."""
import importlib.util
from pathlib import Path
import sys
import types
import unittest

class Obj:
    def __init__(self, parent=None, tag="", pid=42):
        self.parent, self.UIAAutomationId, self.processID = parent, tag, pid

api = types.ModuleType("api")
api.getFocusObject = lambda: api.focus
keys = []
class Key:
    def __init__(self, name): self.name = name
    def send(self): keys.append(self.name)
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
for name, module in {"api": api, "keyboardHandler": keyboard, "NVDAObjects": types.ModuleType("NVDAObjects"),
    "NVDAObjects.UIA": uia, "logHandler": logging, "actions_test": package,
    "actions_test.elements": elements}.items(): sys.modules[name] = module
source = Path(__file__).resolve().parents[1] / "addon/appModules/line/messageActions.py"
spec = importlib.util.spec_from_file_location("actions_test.messageActions", source)
actions = importlib.util.module_from_spec(spec)
spec.loader.exec_module(actions)

class DispatchTests(unittest.TestCase):
    def setUp(self):
        keys.clear()
        self.app = types.SimpleNamespace(processID=42)
        api.focus = Obj(parent=Obj(tag="lineMR_list_messages"))
    def test_three_distinct_commands(self):
        for command in ("reply", "delete", "unsend"):
            actions.run(self.app, command, Key("original"))
        self.assertEqual(keys, ["control+alt+shift+f9", "control+alt+shift+f10", "control+alt+shift+f11"])
    def test_editor_keeps_original_key(self):
        api.focus = Obj(tag="lineMR_input_message")
        actions.run(self.app, "delete", Key("original"))
        self.assertEqual(keys, ["original"])
    def test_other_process_keeps_original_key(self):
        api.focus.processID = 99
        actions.run(self.app, "unsend", Key("original"))
        self.assertEqual(keys, ["original"])
    def test_held_key_dispatches_once(self):
        actions.run(self.app, "delete", Key("original"))
        actions.run(self.app, "delete", Key("original"))
        self.assertEqual(keys, ["control+alt+shift+f10"])
    def test_prefixed_list_id(self):
        api.focus = Obj(tag="parent.lineMR_list_messages")
        actions.run(self.app, "reply", Key("original"))
        self.assertEqual(keys, ["control+alt+shift+f9"])
    def test_unknown_focus_does_not_dispatch(self):
        api.focus = Obj()
        actions.run(self.app, "reply", Key("original"))
        self.assertEqual(keys, ["original"])

if __name__ == "__main__": unittest.main()
