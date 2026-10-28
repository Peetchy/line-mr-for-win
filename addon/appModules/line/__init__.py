# LINE MR (Line Mai Roo): NVDA app module for LINE for Windows (LINE.exe).
#
# LINE is a Qt 6 Widgets application whose custom Lc* widgets expose no names or text to UI
# Automation. The add-on ships an in-process accessibility bridge (bridge\line_a11y.dll, a Qt
# platform-theme plugin) that gives those widgets proper names, roles and actions; this module
# keeps the bridge installed and refines what NVDA presents.

import addonHandler
import api
import appModuleHandler
import controlTypes
import core
import scriptHandler
import ui
from logHandler import log
from NVDAObjects.behaviors import Dialog
from NVDAObjects.UIA import UIA
from scriptHandler import script

from . import bridge, chatSearch, elements, fullText
from .calls import CallCommandsAppModule

addonHandler.initTranslation()

#: Category for this add-on's commands in NVDA's Input Gestures dialog.
SCRIPT_CATEGORY = "LINE MR"

#: UIA class names of LINE's plain layout containers (nameless groups that only position children).
LAYOUT_CLASSES = frozenset({
	"LcWidget",
	"LcTitleBar",
	"LcLine",
	"LcImage",
	"LcScrollArea",
	"LcStackedWidget",
})


def _uiaClassName(obj):
	try:
		return obj.UIAElement.cachedClassName or ""
	except Exception:
		return ""


class ShadowWindow(UIA):
	"""The native windows LINE draws its window shadows with (class shadow_corner_* / shadow_side_*)."""

	presentationType = UIA.presType_unavailable
	isFocusable = False


class LayoutGroup(UIA):
	"""A nameless LINE container: skipped in simple review and not announced as an ancestor."""

	isPresentableFocusAncestor = False

	def _get_presentationType(self):
		if self.name:
			return super().presentationType
		return self.presType_layout


#: LINE windows that work like dialogs: their texts are read when focus enters them (and by NVDA+B).
DIALOG_WINDOW_CLASSES = frozenset({
	"LoginWindow",
})


#: LINE windows that are not dialogs.
MAIN_WINDOW_CLASSES = frozenset({
	"AllInOneWindow",
	"ChatWindow",
})


class DialogWindow(Dialog):
	"""A LINE window that is really a dialog (login, Qt Quick pages): NVDA reads its texts on entry."""

	role = controlTypes.Role.DIALOG


def chooseOverlayClasses(obj, clsList):
	"""Overlay classes for LINE's UIA objects (also used for LineCall.exe, see appModules/linecall.py)."""
	if not isinstance(obj, UIA):
		return
	className = _uiaClassName(obj)
	if className.startswith(("shadow_corner_", "shadow_side_")):
		clsList.insert(0, ShadowWindow)
	elif className in DIALOG_WINDOW_CLASSES or (
		# LINE's other windows (photos & files, settings, Qt Quick pages such as the member list
		# and profiles) work like dialogs; the main window and chat windows do not.
		obj.role == controlTypes.Role.WINDOW
		and ("_QMLTYPE_" in className or className.endswith("Window"))
		and className not in MAIN_WINDOW_CLASSES
	):
		clsList.insert(0, DialogWindow)
	elif className == "LcTextField" or className == "LcButton":
		chatSearch.chooseOverlayClasses(obj, clsList)
	elif className == "QPlainTextEdit":
		fullText.chooseOverlayClasses(obj, clsList)
	elif obj.role in (controlTypes.Role.GROUPING, controlTypes.Role.PANE) and (
		className in LAYOUT_CLASSES or className.endswith("Panel")
	):
		clsList.insert(0, LayoutGroup)


class AppModule(CallCommandsAppModule):
	def __init__(self, *args, **kwargs):
		super().__init__(*args, **kwargs)
		bridge.checkOnStart(self.processID)

	def event_appModule_gainFocus(self):
		# A LINE started before the bridge was installed keeps running (closing its window only hides
		# it): ask again when the user comes back to it.
		bridge.checkOnFocus(self.processID)

	def chooseNVDAObjectOverlayClasses(self, obj, clsList):
		chooseOverlayClasses(obj, clsList)

	@script(
		# Translators: Describes the command that reports whether the LINE accessibility bridge works.
		description=_(
			"Reports whether the LINE MR accessibility bridge is installed and running in LINE and its call windows; "
			"pressed twice, copies a report for the developer"
		),
		category=SCRIPT_CATEGORY,
		gesture="kb:NVDA+shift+control+l",
	)
	def script_bridgeStatus(self, gesture):
		if scriptHandler.getLastScriptRepeatCount() == 1:
			if self._pendingOffer is not None:
				self._pendingOffer.Stop()  # the report is wanted, not the install dialog
				self._pendingOffer = None
			api.copyToClip(bridge.diagnostics(self.processID))
			# Translators: Reported after the diagnostic report was copied to the clipboard.
			ui.message(_("LINE MR report copied to the clipboard."))
			return
		status = bridge.status(self.processID)
		ui.message(bridge.describeStatus(status))
		if status == bridge.Status.RUNNING:
			if bridge.inSafeMode():
				ui.message(bridge.describeSafeMode())
			bridge.repairHelperBridges()
			ui.message(bridge.describeCallStatus(bridge.callStatus()))
		if status != bridge.Status.RUNNING:
			# After the double-press time, so that a second press can copy the report instead.
			self._pendingOffer = core.callLater(800, self._offerInstall, status)

	_pendingOffer = None

	def _offerInstall(self, status):
		self._pendingOffer = None
		bridge.offerInstall(self.processID, status, force=True)

	# ---- Main window commands ------------------------------------------------------------------

	def _activate(self, tag, notFound):
		obj = elements.find(tag)
		if obj is None:
			ui.message(notFound)
			return None
		try:
			obj.doAction()
		except Exception:
			log.debugWarning("LINE MR: could not activate %s", tag, exc_info=True)
		return obj

	def _focus(self, tags, notFound):
		for tag in tags:
			obj = elements.find(tag)
			if obj is not None:
				obj.setFocus()
				return obj
		if notFound:
			ui.message(notFound)
		return None

	@script(
		# Translators: Describes the command that opens LINE's friends tab.
		description=_("Shows the friends list"),
		category=SCRIPT_CATEGORY,
		gesture="kb:alt+1",
	)
	def script_friendsTab(self, gesture):
		# Translators: Reported when a LINE command needs the main window.
		self._activate("gnb-friends", _("Not available here. Switch to LINE's main window."))

	@script(
		# Translators: Describes the command that opens LINE's chats tab.
		description=_("Shows the chat list"),
		category=SCRIPT_CATEGORY,
		gesture="kb:alt+2",
	)
	def script_chatsTab(self, gesture):
		self._activate("gnb-chat", _("Not available here. Switch to LINE's main window."))

	@script(
		# Translators: Describes the command that opens LINE's add-friends tab.
		description=_("Shows add friends"),
		category=SCRIPT_CATEGORY,
		gesture="kb:alt+3",
	)
	def script_addFriendsTab(self, gesture):
		self._activate("gnb-add-friend", _("Not available here. Switch to LINE's main window."))

	@script(
		# Translators: Describes the command that moves focus to LINE's chat or friend list.
		description=_("Moves to the chat list or friend list"),
		category=SCRIPT_CATEGORY,
		gesture="kb:alt+l",
	)
	def script_focusList(self, gesture):
		# Translators: Reported when no chat or friend list is shown.
		self._focus(("list_chats", "list_friends"), _("No list is shown."))

	@script(
		# Translators: Describes the command that moves focus to LINE's chat list.
		description=_("Moves to the chat list (switches to the chats tab first if needed)"),
		category=SCRIPT_CATEGORY,
		gesture="kb:alt+c",
	)
	def script_focusChats(self, gesture):
		if self._focus(("list_chats",), None) is not None:
			return
		# Another tab is shown: switch to the chats tab, then go to its list once LINE has shown it.
		if self._activate("gnb-chat", _("Not available here. Switch to LINE's main window.")) is None:
			return

		def focusList(triesLeft=5):
			if self._focus(("list_chats",), None) is None and triesLeft > 0:
				core.callLater(200, focusList, triesLeft - 1)

		core.callLater(200, focusList)

	@script(
		# Translators: Describes the command that moves focus to the search field of LINE's chat or friend list.
		description=_("Moves to the search field of the chat list or friend list"),
		category=SCRIPT_CATEGORY,
		gesture="kb:alt+f",
	)
	def script_focusSearch(self, gesture):
		# Translators: Reported when no chat or friend list (with its search field) is shown.
		self._focus(("search_chats", "search_friends"), _("No search field is shown."))

	@script(
		# Translators: Describes the command that moves focus to the messages of the open chat.
		description=_("Moves to the messages of the open chat"),
		category=SCRIPT_CATEGORY,
		gesture="kb:alt+m",
	)
	def script_focusMessages(self, gesture):
		# Translators: Reported when no chat is open.
		self._focus(("list_messages",), _("No chat is open."))

	@script(
		# Translators: Describes the command that moves focus to the message input of the open chat.
		description=_("Moves to the message input of the open chat"),
		category=SCRIPT_CATEGORY,
		gesture="kb:alt+i",
	)
	def script_focusInput(self, gesture):
		self._focus(("input_message",), _("No chat is open."))

	@script(
		# Translators: Describes the commands that read recent messages (1 is the newest).
		description=_("Reads a recent message of the open chat: 1 is the newest, 9 the ninth newest"),
		category=SCRIPT_CATEGORY,
		gestures=[f"kb:NVDA+alt+{n}" for n in range(1, 10)],
	)
	def script_readRecentMessage(self, gesture):
		n = int(gesture.mainKeyName[-1])
		messages = elements.find("list_messages")
		if messages is None:
			ui.message(_("No chat is open."))
			return
		rows = [row.name for row in elements.children(messages) if row.name]
		if len(rows) < n:
			# Translators: Reported when the open chat has fewer messages on screen than asked for.
			ui.message(_("No message {number}.").format(number=n))
			return
		ui.message(rows[-n])

	def terminate(self):
		log.debug("LINE MR: app module terminated")
		super().terminate()
