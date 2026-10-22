# LINE MR: NVDA app module for LineCall.exe, LINE's call process (incoming-call notification and
# call windows, Qt Quick). The bridge (installed with LINE's, see line/bridge.py) runs there too;
# this module presents its windows like LINE's own.

import threading

import addonHandler
import appModuleHandler
import queueHandler
import ui
from logHandler import log

from .line import bridge, chooseOverlayClasses

addonHandler.initTranslation()


def _warnNoBridge():
	# Translators: Spoken when a LINE call window appears but LINE MR is not active in it yet.
	ui.message(_("LINE MR is not active for calls yet. Restart LINE after this call."))


class AppModule(appModuleHandler.AppModule):
	def __init__(self, *args, **kwargs):
		super().__init__(*args, **kwargs)
		# LINE may have downloaded or updated its call process after the bridge was installed: then
		# this LineCall.exe runs without it. Install it for the next start and tell the user.
		threading.Thread(target=self._checkBridge, name="lineMR.callBridgeCheck", daemon=True).start()

	def _checkBridge(self):
		try:
			if bridge.DLL_NAME in bridge._processModules(self.processID):
				return
			bridge.repairCallBridges()
		except Exception:
			log.error("LINE MR: checking LineCall's bridge failed", exc_info=True)
			return
		log.info("LINE MR: LineCall.exe (pid %s) runs without the bridge", self.processID)
		queueHandler.queueFunction(queueHandler.eventQueue, _warnNoBridge)

	def chooseNVDAObjectOverlayClasses(self, obj, clsList):
		chooseOverlayClasses(obj, clsList)
