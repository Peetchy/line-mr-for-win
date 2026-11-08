# LINE MR: NVDA app module for LineMediaPlayer.exe, LINE's photo and video viewer (opened by Enter
# or Space on a photo or video message). The bridge (installed with LINE's, see line/bridge.py) runs
# there too; this module presents its windows like LINE's own.

import threading

import addonHandler
import appModuleHandler
import keyboardHandler
import queueHandler
import ui
from logHandler import log
from scriptHandler import script

from .line import SCRIPT_CATEGORY, bridge, chooseOverlayClasses, transcribe
from .line.calls import CallCommandsAppModule

addonHandler.initTranslation()


def _warnNoBridge():
	# Translators: Spoken when LINE's photo / video viewer appears but LINE MR is not active in it yet.
	ui.message(_("LINE MR is not active in LINE's photo and video viewer yet. Restart LINE to use it there."))


class AppModule(CallCommandsAppModule):
	def __init__(self, *args, **kwargs):
		super().__init__(*args, **kwargs)
		# LINE may have downloaded or updated its viewer after the bridge was installed: then this
		# LineMediaPlayer.exe runs without it. Install it for the next start and tell the user.
		threading.Thread(target=self._checkBridge, name="lineMR.mediaBridgeCheck", daemon=True).start()

	def _checkBridge(self):
		try:
			if bridge.disconnected() or bridge.bridgeLoaded(self.processID):
				return
			bridge.repairHelperBridges()
		except Exception:
			log.error("LINE MR: checking LineMediaPlayer's bridge failed", exc_info=True)
			return
		log.info("LINE MR: LineMediaPlayer.exe (pid %s) runs without the bridge", self.processID)
		queueHandler.queueFunction(queueHandler.eventQueue, _warnNoBridge)

	def chooseNVDAObjectOverlayClasses(self, obj, clsList):
		chooseOverlayClasses(obj, clsList)

	@script(
		# Translators: Describes the command that turns the video shown in LINE's viewer into text (pressed
		# again while it works, it cancels).
		description=_("Transcribes the video shown into text; cancels a running transcription"),
		category=SCRIPT_CATEGORY,
		gesture="kb:alt+shift+t",
	)
	def script_transcribeVideo(self, gesture):
		if transcribe.cancel():
			return
		problem = transcribe.unavailableMessage()
		if problem:
			if not transcribe.retryDownload():
				ui.message(problem)
			return
		# The viewer's bridge reads the video's length and pauses it (a photo: "Not a voice message or video").
		keyboardHandler.KeyboardInputGesture.fromName(transcribe.COMMAND_KEY).send()
