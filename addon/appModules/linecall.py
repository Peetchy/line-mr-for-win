# LINE MR: NVDA app module for LineCall.exe, LINE's call process (incoming-call notification and
# call windows, Qt Quick). The bridge (installed with LINE's, see line/bridge.py) runs there too;
# this module presents its windows like LINE's own.

import appModuleHandler

from .line import chooseOverlayClasses


class AppModule(appModuleHandler.AppModule):
	def chooseNVDAObjectOverlayClasses(self, obj, clsList):
		chooseOverlayClasses(obj, clsList)
