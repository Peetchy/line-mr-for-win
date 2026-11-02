"""User-triggered commands only; no app-module overlays or in-process bridge."""
import globalPluginHandler
from scriptHandler import script

from .actions import Controller

CATEGORY = "LINE 原生訊息快捷鍵"


class GlobalPlugin(globalPluginHandler.GlobalPlugin):
	def __init__(self):
		super().__init__()
		self._actions = Controller()

	@script(description="回覆目前的 LINE 訊息（使用原生選單）", category=CATEGORY, gesture="kb:alt+r")
	def script_reply(self, gesture):
		self._actions.run("reply", gesture)

	@script(description="刪除此裝置上的目前 LINE 訊息（保留確認視窗）", category=CATEGORY, gesture="kb:delete")
	def script_delete(self, gesture):
		self._actions.run("delete", gesture)

	@script(description="收回目前的 LINE 訊息（保留確認視窗）", category=CATEGORY, gesture="kb:shift+delete")
	def script_unsend(self, gesture):
		self._actions.run("unsend", gesture)

	def terminate(self):
		self._actions.cancel()
		super().terminate()
