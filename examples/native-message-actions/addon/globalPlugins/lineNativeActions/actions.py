"""Exact native menu activation. No DLL, coordinate clicks, or confirmation handling."""
import time

import api
import controlTypes
import core
import keyboardHandler
import UIAHandler
import ui
import winUser
from logHandler import log
from NVDAObjects.UIA import UIA

LABELS = {
	"reply": frozenset(("回覆", "回复", "回復", "Reply", "返信", "ตอบกลับ")),
	"delete": frozenset(("刪除", "删除", "Delete", "削除", "ลบ")),
	"unsend": frozenset(("收回", "撤回", "Unsend", "送信取消", "送信を取り消す", "ยกเลิกข้อความ", "ยกเลิกการส่ง")),
}


def _className(obj):
	try:
		return obj.UIAElement.cachedClassName or ""
	except Exception:
		return ""


def messageContext(focus):
	"""Prove native-message-list ancestry. Never intercept a text editor or another app."""
	if not isinstance(focus, UIA) or getattr(getattr(focus, "appModule", None), "appName", "").lower() != "line":
		return False
	seenList = False
	obj = focus
	for _ in range(16):
		if obj is None or not isinstance(obj, UIA) or obj.processID != focus.processID:
			return False
		if obj.role == controlTypes.Role.EDITABLETEXT:
			return False
		if obj.role == controlTypes.Role.LIST:
			seenList = True
		name = _className(obj)
		if name == "SearchDetailPanel":
			return False
		if name == "ChatMessageView":
			return seenList
		obj = obj.parent
	return False


def chooseItem(items, action):
	"""One exact enabled native button; missing/ambiguous entries never substitute another."""
	found = []
	for item in items:
		name = (item.name or "").replace("&", "").strip().rstrip(".…").strip()
		if item.role == controlTypes.Role.BUTTON and name in LABELS[action]:
			if controlTypes.State.UNAVAILABLE not in item.states and controlTypes.State.INVISIBLE not in item.states:
				found.append(item)
	return found[0] if len(found) == 1 else None


def menuContext(focus):
	if not isinstance(focus, UIA) or getattr(getattr(focus, "appModule", None), "appName", "").lower() != "line":
		return False
	obj = focus
	for _ in range(10):
		if obj is None or not isinstance(obj, UIA) or obj.processID != focus.processID:
			return False
		if _className(obj) == "LcContextMenu":
			return True
		obj = obj.parent
	return False


def messageMenu(items):
	# A chat-list menu can also have Delete. Require message-only Reply plus a
	# copy/share entry before accepting a shortcut from an already-open popup.
	names = {(item.name or "").replace("&", "").strip().rstrip(".…").strip()
	         for item in items if item.role == controlTypes.Role.BUTTON}
	return bool(names & LABELS["reply"]) and bool(names & {
		"複製", "复制", "Copy", "コピー", "คัดลอก", "分享", "Share", "共有", "แชร์",
	})


def _sameItem(a, b):
	return bool(UIAHandler.handler.clientObject.CompareElements(a.UIAElement, b.UIAElement))


def _nativeMenus(pid):
	"""The popup is its own native window; inspect only LINE's LcContextMenu windows."""
	h = UIAHandler.handler
	cond = h.clientObject.CreateAndCondition(
		h.clientObject.CreatePropertyCondition(UIAHandler.UIA_ProcessIdPropertyId, pid),
		h.clientObject.CreatePropertyCondition(UIAHandler.UIA_ClassNamePropertyId, "LcContextMenu"),
	)
	root = h.clientObject.GetRootElement()
	menus = root.FindAllBuildCache(UIAHandler.UIA.TreeScope_Children, cond, h.baseCacheRequest)
	result = []
	for i in range(menus.Length):
		menu = UIA(UIAElement=menus.GetElement(i))
		# NVDA's baseCacheRequest does not necessarily include IsOffscreen. Read live;
		# querying cachedIsOffscreen on such an element raises E_INVALIDARG.
		if menu.UIAElement.CurrentIsOffscreen or controlTypes.State.INVISIBLE in menu.states:
			continue
		items = menu.UIAElement.FindAllBuildCache(
			UIAHandler.UIA.TreeScope_Descendants,
			h.clientObject.CreateTrueCondition(), h.baseCacheRequest,
		)
		result.append([UIA(UIAElement=items.GetElement(j)) for j in range(items.Length)])
	return result


class Controller:
	def __init__(self):
		self._request = None
		self._timer = None
		self._last = None

	def cancel(self):
		self._request = None
		if self._timer is not None:
			self._timer.Stop()
			self._timer = None

	def run(self, action, gesture):
		try:
			focus = api.getFocusObject()
			inMenu = menuContext(focus)
			if not inMenu and not messageContext(focus):
				log.debug("LINE Native Actions: %s passed through outside confirmed message context (class %s)", action, _className(focus))
				gesture.send()
				return
			if self._request is not None:
				return
			now = time.monotonic()
			if self._last and self._last[0] == action and now - self._last[1] < 0.5:
				return
			self._last = (action, now)
			menus = _nativeMenus(focus.processID)
			if inMenu and (len(menus) != 1 or not messageMenu(menus[0])):
				gesture.send()
				return
			if menus and not inMenu:
				ui.message("請先關閉選單，回到訊息再使用快捷鍵。")
				return
			request = (object(), action, focus.processID)
			self._request = request
			if inMenu:
				log.info("LINE Native Actions: requested %s from native menu", action)
				self._poll(request, 1)
				return
			if action == "reply":
				# KeyboardInputGesture.send does not release unrelated held modifiers.
				# Alt+Applications may be ignored by LINE; wait for the user's release.
				log.info("LINE Native Actions: reply waiting for modifier release")
				self._timer = core.callLater(40, self._openReplyMenu, request, 50)
				return
			log.info("LINE Native Actions: opening menu for %s from message (pid %d)", action, focus.processID)
			keyboardHandler.KeyboardInputGesture.fromName("applications").send()
			log.info("LINE Native Actions: requested %s", action)
			self._timer = core.callLater(80, self._poll, request, 20)
		except Exception:
			self.cancel()
			log.debugWarning("LINE Native Actions: command setup failed", exc_info=True)
			ui.message("無法開啟 LINE 原生訊息選單。")

	def _openReplyMenu(self, request, attempts):
		if self._request is not request:
			return
		self._timer = None
		try:
			pid = request[2]
			focus = api.getFocusObject()
			if api.getForegroundObject().processID != pid or focus.processID != pid or not messageContext(focus):
				log.info("LINE Native Actions: reply cancelled before menu because context changed")
				self.cancel()
				return
			if any(winUser.getKeyState(key) & 0x8000 for key in (0x10, 0x11, 0x12, 0x5B, 0x5C, 0x2D, 0x14)):
				if attempts <= 1:
					self.cancel()
					ui.message("請放開快捷鍵後再試一次。")
					return
				self._timer = core.callLater(40, self._openReplyMenu, request, attempts - 1)
				return
			keyboardHandler.KeyboardInputGesture.fromName("applications").send()
			log.info("LINE Native Actions: requested reply after modifier release")
			self._timer = core.callLater(80, self._poll, request, 20)
		except Exception:
			self.cancel()
			log.debugWarning("LINE Native Actions: delayed reply menu failed", exc_info=True)
			ui.message("無法開啟 LINE 原生回覆選單。")

	def _poll(self, request, attempts):
		if self._request is not request:
			return
		self._timer = None
		try:
			_token, action, pid = request
			foregroundPID = api.getForegroundObject().processID
			if foregroundPID != pid:
				log.info("LINE Native Actions: cancelled %s because foreground changed (source %d, foreground %d)", action, pid, foregroundPID)
				self.cancel()
				return
			menus = _nativeMenus(pid)
			if attempts == 20:
				log.info("LINE Native Actions: first %s poll sees %d native menus", action, len(menus))
			if len(menus) == 1:
				item = chooseItem(menus[0], action)
				if item is None:
					log.info("LINE Native Actions: %s unavailable or ambiguous in native menu", action)
					self.cancel()
					ui.message("這則訊息目前沒有可用的回覆、刪除或收回項目。")
					return
				# Consume before Invoke: never follow into or confirm another dialog.
				self.cancel()
				item.doAction()
				log.info("LINE Native Actions: invoked %s (native completion not yet verified)", action)
				if action == "reply":
					# The native Reply Invoke can return without dismissing the popup.
					# Verify the ORIGINAL button is still focused before one native Enter.
					followup = object()
					self._request = followup
					self._timer = core.callLater(150, self._finishReply, followup, item, pid)
				return
			if len(menus) > 1 or attempts <= 1:
				log.info("LINE Native Actions: %s stopped with %d menus (attempts left %d)", action, len(menus), attempts)
				self.cancel()
				ui.message("無法確認 LINE 訊息選單，請先關閉選單後重試。")
				return
			self._timer = core.callLater(80, self._poll, request, attempts - 1)
		except Exception:
			self.cancel()
			log.debugWarning("LINE Native Actions: native menu operation failed", exc_info=True)
			ui.message("無法操作 LINE 原生訊息選單。")

	def _finishReply(self, request, originalItem, pid):
		if self._request is not request:
			return
		self.cancel() # one check, at most one Enter; never inspect a later confirmation
		try:
			if api.getForegroundObject().processID != pid:
				return
			menus = _nativeMenus(pid)
			if not menus:
				log.info("LINE Native Actions: reply popup closed after Invoke")
				return
			if len(menus) != 1 or not messageMenu(menus[0]):
				return
			item = chooseItem(menus[0], "reply")
			focus = api.getFocusObject()
			if item is None or not menuContext(focus) or chooseItem([focus], "reply") is not focus:
				return
			if not _sameItem(item, originalItem) or not _sameItem(focus, originalItem):
				return
			if any(winUser.getKeyState(key) & 0x8000 for key in (0x10, 0x11, 0x12, 0x5B, 0x5C, 0x2D, 0x14)):
				return
			keyboardHandler.KeyboardInputGesture.fromName("enter").send()
			log.info("LINE Native Actions: sent one Enter to original focused Reply button")
		except Exception:
			log.debugWarning("LINE Native Actions: reply completion check failed", exc_info=True)
