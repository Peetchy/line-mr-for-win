"""Offline tests. No LINE message is modified."""
import importlib.util
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import Mock, patch

class Obj:
	def __init__(self, role="text", cls="LcText", parent=None, pid=42, app="line", name="", states=()):
		self.role, self.parent, self.processID = role, parent, pid
		self.appModule = types.SimpleNamespace(appName=app)
		self.UIAElement = types.SimpleNamespace(cachedClassName=cls)
		self.name, self.states = name, set(states)
		self.doAction = Mock()

api = types.SimpleNamespace(getFocusObject=Mock(), getForegroundObject=Mock(return_value=Obj()))
keys, messages = [], []
keyboard = types.SimpleNamespace(KeyboardInputGesture=types.SimpleNamespace(
	fromName=lambda key: types.SimpleNamespace(send=lambda: keys.append(key))))
timer = Mock()
for name, module in {
	"api": api,
	"controlTypes": types.SimpleNamespace(Role=types.SimpleNamespace(EDITABLETEXT="edit", LIST="list", BUTTON="button"),
		State=types.SimpleNamespace(UNAVAILABLE="disabled", INVISIBLE="hidden")),
	"core": types.SimpleNamespace(callLater=Mock(return_value=timer)),
	"keyboardHandler": keyboard, "UIAHandler": types.SimpleNamespace(),
	"ui": types.SimpleNamespace(message=messages.append),
	"winUser": types.SimpleNamespace(getKeyState=Mock(return_value=0)),
	"logHandler": types.SimpleNamespace(log=Mock()),
	"NVDAObjects.UIA": types.SimpleNamespace(UIA=Obj),
}.items(): sys.modules[name] = module
path = Path(__file__).resolve().parents[1] / "addon/globalPlugins/lineNativeActions/actions.py"
spec = importlib.util.spec_from_file_location("actions", path)
actions = importlib.util.module_from_spec(spec)
spec.loader.exec_module(actions)
realNativeMenus = actions._nativeMenus

def message():
	return Obj(parent=Obj(role="list", cls="LcListView", parent=Obj(cls="ChatMessageView")))

class Tests(unittest.TestCase):
	def setUp(self):
		keys.clear(); messages.clear()
		api.getFocusObject.return_value = message()
		api.getForegroundObject.return_value = Obj()
		actions.winUser.getKeyState.return_value = 0
		actions._nativeMenus = Mock(return_value=[])
		self.c = actions.Controller()
		self.g = types.SimpleNamespace(send=Mock())

	def test_message_scope(self):
		self.assertTrue(actions.messageContext(message()))
		self.assertFalse(actions.messageContext(Obj(role="edit", parent=message())))
		self.assertFalse(actions.messageContext(Obj(app="notepad")))
		self.assertFalse(actions.messageContext(Obj(parent=Obj(role="list", cls="MainChatPanel"))))
		self.assertFalse(actions.messageContext(Obj(parent=Obj(role="list", cls="SearchDetailPanel", parent=Obj(cls="ChatMessageView")))))

	def test_editor_delete_passes_through(self):
		api.getFocusObject.return_value = Obj(role="edit")
		self.c.run("delete", self.g)
		self.g.send.assert_called_once()
		self.assertEqual(keys, [])

	def test_exact_enabled_unique_action(self):
		good = Obj(role="button", name="刪除")
		self.assertIs(actions.chooseItem([good, Obj(role="button", name="全部刪除")], "delete"), good)
		self.assertIsNone(actions.chooseItem([Obj(role="button", name="刪除", states=("disabled",))], "delete"))
		self.assertIsNone(actions.chooseItem([good, good], "delete"))
		self.assertIsNone(actions.chooseItem([good], "unsend"))

	def test_each_action_invokes_once_and_stops(self):
		for action, label in (("reply", "回覆"), ("delete", "刪除"), ("unsend", "收回")):
			self.setUp()
			self.c.run(action, self.g)
			request = self.c._request
			if action == "reply": self.c._openReplyMenu(request, 50)
			button = Obj(role="button", name=label)
			actions._nativeMenus.return_value = [[button]]
			self.c._poll(request, 20)
			self.c._poll(request, 19)
			button.doAction.assert_called_once()
			if action == "reply": self.c._finishReply(self.c._request, button, 42)
			self.assertIsNone(self.c._request)
			self.assertEqual(keys, ["applications"])

	def test_switch_application_cancels(self):
		self.c.run("delete", self.g)
		api.getForegroundObject.return_value = Obj(pid=99)
		button = Obj(role="button", name="刪除")
		actions._nativeMenus.return_value = [[button]]
		self.c._poll(self.c._request, 20)
		button.doAction.assert_not_called()
		self.assertIsNone(self.c._request)

	def test_missing_action_and_timeout_do_not_invoke(self):
		self.c.run("unsend", self.g)
		button = Obj(role="button", name="刪除")
		actions._nativeMenus.return_value = [[button]]
		self.c._poll(self.c._request, 20)
		button.doAction.assert_not_called()
		self.setUp()
		self.c.run("reply", self.g)
		self.c._poll(self.c._request, 1)
		self.assertIsNone(self.c._request)

	def test_repeats_and_cancelled_request(self):
		self.c.run("reply", self.g)
		request = self.c._request
		self.c.run("reply", self.g)
		self.assertEqual(len(keys), 0) # reply waits for modifiers before sending Applications
		self.c.cancel()
		button = Obj(role="button", name="回覆")
		actions._nativeMenus.return_value = [[button]]
		self.c._poll(request, 20)
		button.doAction.assert_not_called()

	def test_reply_waits_for_alt_release(self):
		actions.winUser.getKeyState.return_value = 0x8000
		self.c.run("reply", self.g)
		request = self.c._request
		self.c._openReplyMenu(request, 50)
		self.assertEqual(keys, [])
		actions.winUser.getKeyState.return_value = 0
		self.c._openReplyMenu(request, 49)
		self.assertEqual(keys, ["applications"])

	def test_reply_release_timeout_or_context_change(self):
		actions.winUser.getKeyState.return_value = 0x8000
		self.c.run("reply", self.g)
		self.c._openReplyMenu(self.c._request, 1)
		self.assertIsNone(self.c._request)
		self.assertEqual(keys, [])

	def test_reply_enter_only_on_original_focused_button(self):
		popup = Obj(cls="LcContextMenu")
		reply = Obj(role="button", name="回覆", parent=popup)
		copy = Obj(role="button", name="複製", parent=popup)
		api.getFocusObject.return_value = reply
		actions._nativeMenus.return_value = [[reply, copy]]
		request = self.c._request = object()
		with patch.object(actions, "_sameItem", side_effect=lambda a,b: a is b):
			self.c._finishReply(request, reply, 42)
			self.c._finishReply(request, reply, 42)
		self.assertEqual(keys, ["enter"])
		reply.doAction.assert_not_called()

	def test_reply_never_enters_confirmation_or_other_focus(self):
		for mode in ("closed", "confirmation", "copy", "different-reply"):
			self.setUp()
			popup = Obj(cls="LcContextMenu")
			reply = Obj(role="button", name="回覆", parent=popup)
			copy = Obj(role="button", name="複製", parent=popup)
			original = Obj(role="button", name="回覆", parent=popup) if mode == "different-reply" else reply
			api.getFocusObject.return_value = copy if mode == "copy" else reply
			if mode == "closed": actions._nativeMenus.return_value = []
			elif mode == "confirmation": actions._nativeMenus.return_value = [[Obj(role="button", name="確定")]]
			else: actions._nativeMenus.return_value = [[reply, copy]]
			request = self.c._request = object()
			with patch.object(actions, "_sameItem", side_effect=lambda a,b: a is b):
				self.c._finishReply(request, original, 42)
			self.assertEqual(keys, [], mode)
		self.setUp()
		self.c.run("reply", self.g)
		api.getFocusObject.return_value = Obj(role="edit")
		self.c._openReplyMenu(self.c._request, 50)
		self.assertIsNone(self.c._request)
		self.assertEqual(keys, [])

	def test_open_message_menu_and_other_menu(self):
		popup = Obj(cls="LcContextMenu")
		api.getFocusObject.return_value = Obj(role="button", parent=popup)
		reply = Obj(role="button", name="回覆")
		copy = Obj(role="button", name="複製")
		actions._nativeMenus.return_value = [[reply, copy]]
		self.c.run("reply", self.g)
		reply.doAction.assert_called_once()
		self.assertEqual(keys, [])
		self.setUp()
		api.getFocusObject.return_value = Obj(role="button", parent=popup)
		delete = Obj(role="button", name="刪除")
		actions._nativeMenus.return_value = [[delete]]
		self.c.run("delete", self.g)
		delete.doAction.assert_not_called()
		self.g.send.assert_called_once()

	def test_native_menu_without_cached_offscreen_property(self):
		class Raw:
			CurrentIsOffscreen = False
			@property
			def cachedIsOffscreen(self):
				raise ValueError("E_INVALIDARG: property absent from cache")
		class Collection:
			def __init__(self, *items): self.items, self.Length = items, len(items)
			def GetElement(self, i): return self.items[i]
		button = Obj(role="button", name="收回")
		rawButton = types.SimpleNamespace(obj=button)
		rawMenu = Raw()
		menu = Obj(cls="LcContextMenu")
		menu.UIAElement = rawMenu
		rawMenu.obj = menu
		rawMenu.FindAllBuildCache = Mock(return_value=Collection(rawButton))
		h = Mock()
		h.clientObject.GetRootElement.return_value.FindAllBuildCache.return_value = Collection(rawMenu)
		with patch.object(actions, "UIA", side_effect=lambda **kw: kw["UIAElement"].obj), patch.object(
			actions, "UIAHandler", types.SimpleNamespace(handler=h, UIA_ProcessIdPropertyId=1,
				UIA_ClassNamePropertyId=2, UIA=types.SimpleNamespace(TreeScope_Children=2, TreeScope_Descendants=4))):
			self.assertEqual(realNativeMenus(42), [[button]])

if __name__ == "__main__": unittest.main()
