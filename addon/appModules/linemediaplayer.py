# LINE MR: NVDA app module for LineMediaPlayer.exe, LINE's photo and video viewer (opened by Enter
# or Space on a photo or video message). The bridge (installed with LINE's, see line/bridge.py) runs
# there too; this module presents its windows like LINE's own.

import threading

import addonHandler
import appModuleHandler
import queueHandler
import ui
from logHandler import log

from .line import bridge, chooseOverlayClasses

addonHandler.initTranslation()


def _warnNoBridge():
	# Translators: Spoken when LINE's photo / video viewer appears but LINE MR is not active in it yet.
	ui.message(_("LINE MR is not active in LINE's photo and video viewer yet. Restart LINE to use it there."))


class AppModule(appModuleHandler.AppModule):
	def __init__(self, *args, **kwargs):
		super().__init__(*args, **kwargs)
		# LINE may have downloaded or updated its viewer after the bridge was installed: then this
		# LineMediaPlayer.exe runs without it. Install it for the next start and tell the user.
		threading.Thread(target=self._checkBridge, name="lineMR.mediaBridgeCheck", daemon=True).start()

	def _checkBridge(self):
		try:
			if bridge.DLL_NAME in bridge._processModules(self.processID):
				return
			bridge.repairHelperBridges()
		except Exception:
			log.error("LINE MR: checking LineMediaPlayer's bridge failed", exc_info=True)
			return
		log.info("LINE MR: LineMediaPlayer.exe (pid %s) runs without the bridge", self.processID)
		queueHandler.queueFunction(queueHandler.eventQueue, _warnNoBridge)

	def chooseNVDAObjectOverlayClasses(self, obj, clsList):
		chooseOverlayClasses(obj, clsList)
