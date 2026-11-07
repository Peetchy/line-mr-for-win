# LINE MR: checks GitHub for a newer release of the add-on and installs it on request.
#
# When NVDA starts, the latest release of github.com/Peetchy/line-mr-for-win is compared with the
# installed version; if it is newer, a dialog offers to update now, later or to skip that version.
# The download is handed to NVDA's own installer (gui.addonGui.handleRemoteAddonInstall, as when an
# .nvda-addon file is opened): NVDA asks for confirmation, installs and offers to restart.
# Also holds the add-on's category in NVDA's settings (updates, the focus highlight in LINE, What's new).

import hashlib
import json
import os
import re
import tempfile
import threading
import urllib.request

import addonHandler
import config
import globalPluginHandler
import gui
import ui
import wx
from gui import guiHelper, nvdaControls, settingsDialogs
from logHandler import log
from scriptHandler import script

addonHandler.initTranslation()

SCRIPT_CATEGORY = "LINE MR"
LATEST_RELEASE_URL = "https://api.github.com/repos/Peetchy/line-mr-for-win/releases/latest"
#: Asset the release workflow uploads under a fixed name (also linked from the README).
ASSET_NAME = "lineMR.nvda-addon"
#: Seconds after NVDA starts before the first check (start-up stays quick).
STARTUP_DELAY_MS = 15000
TIMEOUT = 30

CONFIG_SECTION = "lineMR"
config.conf.spec[CONFIG_SECTION] = {
	"checkForUpdates": "boolean(default=true)",
	"skippedVersion": "string(default='')",
	"highlightFocus": "boolean(default=true)",
	"directChatSenderNames": "boolean(default=false)",
	"senderOnlyWhenChanged": "boolean(default=false)",
	"highlightColor": "string(default='yellow')",
	"highlightWidth": "integer(default=4, min=1, max=12)",
	# Voice message transcription (appModules/line/transcribe.py): off until the user turns it on,
	# which downloads the speech model (speechModel.py).
	"transcribeVoiceMessages": "boolean(default=false)",
	"transcribeLanguage": "string(default='auto')",
	# Ask for the language before each transcription (the dialog's "Don't show this again" clears it).
	"transcribeAskLanguage": "boolean(default=true)",
}


def _applyHighlightSettings():
	"""Passes the focus highlight and message presentation settings to the bridge."""
	try:
		from appModules.line import highlight, transcribe

		conf = config.conf[CONFIG_SECTION]
		highlight.writeSettings(conf["highlightFocus"], conf["highlightColor"], conf["highlightWidth"])
		# Also the message settings; downloads the speech model if transcription is on and it is missing.
		transcribe.applySettings()
	except Exception:
		log.error("LINE MR: cannot write the bridge presentation settings", exc_info=True)


def _versionKey(version):
	return tuple(int(n) for n in re.findall(r"\d+", version or ""))


def _installedVersion():
	try:
		return addonHandler.getCodeAddon().version
	except Exception:
		return ""


def _installedFromStore():
	"""True when NVDA's Add-on Store manages this add-on (it then offers the updates itself)."""
	try:
		return getattr(addonHandler.getCodeAddon(), "_addonStoreData", None) is not None
	except Exception:
		return False


def _request(url):
	return urllib.request.Request(url, headers={
		"User-Agent": "lineMR-nvda-addon",
		"Accept": "application/vnd.github+json",
	})


def _latestRelease():
	"""(version, release page URL, asset URL, sha256 or None) of the latest release, or None."""
	with urllib.request.urlopen(_request(LATEST_RELEASE_URL), timeout=TIMEOUT) as response:
		release = json.load(response)
	assets = [a for a in release.get("assets", []) if a.get("name", "").endswith(".nvda-addon")]
	if not assets:
		return None
	asset = next((a for a in assets if a.get("name") == ASSET_NAME), assets[0])
	digest = asset.get("digest") or ""
	sha256 = digest[len("sha256:"):] if digest.startswith("sha256:") else None
	version = release.get("tag_name", "").lstrip("vV")
	return version, release.get("html_url", ""), asset["browser_download_url"], sha256


def _download(url, sha256):
	path = os.path.join(tempfile.gettempdir(), ASSET_NAME)
	digest = hashlib.sha256()
	with urllib.request.urlopen(_request(url), timeout=TIMEOUT) as response, open(path, "wb") as f:
		while chunk := response.read(65536):
			digest.update(chunk)
			f.write(chunk)
	if sha256 and digest.hexdigest().lower() != sha256.lower():
		os.remove(path)
		raise ValueError("checksum mismatch")
	return path


class _Checker:
	def __init__(self):
		self._busy = False

	def check(self, manual):
		"""Looks for a newer release in the background; `manual` also reports "up to date" and errors."""
		if self._busy:
			return
		if _installedFromStore():
			if manual:
				wx.CallAfter(ui.message, _storeMessage())
			return
		self._busy = True
		threading.Thread(target=self._check, args=(manual,), name="lineMR.updateCheck", daemon=True).start()

	def _check(self, manual):
		try:
			latest = _latestRelease()
		except Exception:
			log.debugWarning("LINE MR: update check failed", exc_info=True)
			self._busy = False
			if manual:
				# Translators: Reported when the update check cannot reach GitHub.
				wx.CallAfter(ui.message, _("Could not check for LINE MR updates."))
			return
		self._busy = False
		installed = _installedVersion()
		if not latest or _versionKey(latest[0]) <= _versionKey(installed):
			if manual:
				# Translators: Reported when no newer version of the add-on exists. {version} is the installed one.
				wx.CallAfter(ui.message, _("LINE MR {version} is up to date.").format(version=installed))
			return
		if not manual and latest[0] == config.conf[CONFIG_SECTION]["skippedVersion"]:
			return
		wx.CallAfter(self._offer, installed, *latest)

	def _offer(self, installed, version, pageUrl, assetUrl, sha256):
		dialog = wx.MessageDialog(
			gui.mainFrame,
			# Translators: Text of the dialog offering an add-on update.
			_(
				"LINE MR {new} is available (installed: {old}).\n\n"
				"Update now? NVDA downloads and installs it, then asks to restart."
			).format(new=version, old=installed),
			# Translators: Title of the dialog offering an add-on update.
			_("LINE MR update"),
			wx.YES_NO | wx.CANCEL | wx.ICON_INFORMATION,
		)
		dialog.SetYesNoCancelLabels(
			# Translators: Button of the update dialog.
			_("&Update now"),
			# Translators: Button of the update dialog.
			_("&Later"),
			# Translators: Button of the update dialog: do not offer this version again.
			_("&Skip this version"),
		)
		gui.mainFrame.prePopup()
		try:
			answer = dialog.ShowModal()
		finally:
			dialog.Destroy()
			gui.mainFrame.postPopup()
		if answer == wx.ID_YES:
			# Translators: Reported while the add-on update downloads.
			ui.message(_("Downloading LINE MR {version}").format(version=version))
			threading.Thread(target=self._install, args=(assetUrl, sha256), name="lineMR.updateDownload",
				daemon=True).start()
		elif answer == wx.ID_CANCEL:
			config.conf[CONFIG_SECTION]["skippedVersion"] = version

	def _install(self, assetUrl, sha256):
		try:
			path = _download(assetUrl, sha256)
		except Exception:
			log.error("LINE MR: downloading the update failed", exc_info=True)
			# Translators: Reported when the add-on update cannot be downloaded.
			wx.CallAfter(ui.message, _("Could not download the LINE MR update."))
			return
		# NVDA's own add-on installation: confirmation, install, restart prompt.
		from gui import addonGui
		wx.CallAfter(addonGui.handleRemoteAddonInstall, path)


_checker = _Checker()


def _storeMessage():
	# Translators: Reported instead of an update check when NVDA's Add-on Store manages the add-on.
	return _("LINE MR was installed from the NVDA Add-on Store, which offers its updates.")


def _changesPath():
	"""The add-on's "What's new" page in NVDA's language (zh_TW, zh, ...), else English."""
	try:
		import languageHandler
		lang = languageHandler.getLanguage()
	except Exception:
		lang = "en"
	docs = os.path.join(addonHandler.getCodeAddon().path, "doc")
	for candidate in (lang, lang.split("_")[0], "en"):
		path = os.path.join(docs, candidate, "changes.html")
		if os.path.isfile(path):
			return path
	return None


def showWhatsNew():
	"""Shows the changes of each version in a browseable window (headings per version)."""
	path = _changesPath()
	if not path:
		# Translators: Reported when the add-on's list of changes is missing.
		ui.message(_("The list of changes was not found."))
		return
	with open(path, encoding="utf-8") as f:
		html = f.read()
	body = re.search(r"<body>(.*)</body>", html, re.DOTALL)
	# Translators: Title of the window that lists what changed in each version of the add-on.
	ui.browseableMessage(body.group(1) if body else html, _("What's new in LINE MR"), isHtml=True)


class LineMRSettingsPanel(settingsDialogs.SettingsPanel):
	# Translators: Title of the add-on's category in NVDA's settings.
	title = _("LINE MR")

	def makeSettings(self, settingsSizer):
		helper = guiHelper.BoxSizerHelper(self, sizer=settingsSizer)
		self.checkForUpdates = None
		if _installedFromStore():
			helper.addItem(wx.StaticText(self, label=_storeMessage()))
		else:
			self.checkForUpdates = helper.addItem(
				# Translators: Check box in the add-on's settings.
				wx.CheckBox(self, label=_("Check for LINE MR &updates when NVDA starts"))
			)
			self.checkForUpdates.SetValue(config.conf[CONFIG_SECTION]["checkForUpdates"])
			# Translators: Button in the add-on's settings.
			checkNow = helper.addItem(wx.Button(self, label=_("&Check for updates now")))
			checkNow.Bind(wx.EVT_BUTTON, lambda evt: _checker.check(manual=True))
		from appModules.line import highlight

		conf = config.conf[CONFIG_SECTION]
		self.directChatSenderNames = helper.addItem(
			# Translators: Includes the other person's name before received messages in one-to-one chats.
			wx.CheckBox(self, label=_("Show the sender's &name in one-to-one chats"))
		)
		self.directChatSenderNames.SetValue(conf["directChatSenderNames"])
		self.senderOnlyWhenChanged = helper.addItem(
			# Translators: Check box in the add-on's settings: a message's sender is read only when it is
			# not the sender of the message above.
			wx.CheckBox(self, label=_("Read the sender's name only when the &sender changes"))
		)
		self.senderOnlyWhenChanged.SetValue(conf["senderOnlyWhenChanged"])
		self.highlightFocus = helper.addItem(
			# Translators: Check box in the add-on's settings: a coloured frame shows which control in
			# LINE has the focus (for low-vision users).
			wx.CheckBox(self, label=_("&Highlight the focused control in LINE"))
		)
		self.highlightFocus.SetValue(conf["highlightFocus"])
		self._highlightColors = [key for key, _label, _value in highlight.COLORS]
		self.highlightColor = helper.addLabeledControl(
			# Translators: Label of the list of colours of the focus highlight in the add-on's settings.
			_("Highlight &colour:"),
			wx.Choice,
			choices=[label for _key, label, _value in highlight.COLORS],
		)
		color = conf["highlightColor"]
		self.highlightColor.SetSelection(
			self._highlightColors.index(color) if color in self._highlightColors else 0
		)
		self.highlightWidth = helper.addLabeledControl(
			# Translators: Label of the width of the focus highlight frame in the add-on's settings.
			_("Highlight &width:"),
			nvdaControls.SelectOnFocusSpinCtrl,
			min=highlight.MIN_WIDTH,
			max=highlight.MAX_WIDTH,
			initial=conf["highlightWidth"],
		)
		self._makeTranscriptionSettings(helper, conf)
		# Translators: Label of the bridge's state in the add-on's settings (read-only text).
		self.bridgeState = helper.addLabeledControl(_("&Bridge:"), wx.TextCtrl, style=wx.TE_READONLY)
		self.bridgeButton = helper.addItem(wx.Button(self))
		self.bridgeButton.Bind(wx.EVT_BUTTON, self._onBridgeButton)
		self._refreshBridge()
		# Translators: Button in the add-on's settings: shows what changed in each version.
		whatsNew = helper.addItem(wx.Button(self, label=_("&What's new")))
		whatsNew.Bind(wx.EVT_BUTTON, lambda evt: showWhatsNew())

	def _makeTranscriptionSettings(self, helper, conf):
		from appModules.line import speechModel, transcribe

		# Translators: Group of settings for turning voice messages into text.
		group = guiHelper.BoxSizerHelper(self, sizer=wx.StaticBoxSizer(wx.VERTICAL, self, label=_("Voice message transcription")))
		helper.addItem(group)
		box = group.sizer.GetStaticBox()
		self.transcribeVoiceMessages = group.addItem(
			wx.CheckBox(
				box,
				# Translators: Check box in the add-on's settings ({size} is in megabytes). Turning it on
				# downloads the speech model.
				label=_("&Transcribe voice messages with Alt+T (downloads a speech model of {size} MB once)").format(
					size=speechModel.DOWNLOAD_MB
				),
			)
		)
		self.transcribeVoiceMessages.SetValue(conf["transcribeVoiceMessages"])
		# Translators: Label of the state of the speech model in the add-on's settings (read-only text).
		self.speechModelState = group.addLabeledControl(_("Speech model:"), wx.TextCtrl, style=wx.TE_READONLY)
		self.transcribeLanguage = group.addLabeledControl(
			# Translators: Label of the language list for voice messages in the add-on's settings.
			_("Voice message &language:"), wx.Choice, choices=transcribe.languageLabels()
		)
		language = conf["transcribeLanguage"]
		self.transcribeLanguage.SetSelection(transcribe.WHISPER_LANGUAGES.index(language) if language in transcribe.WHISPER_LANGUAGES else 0)
		self.transcribeAskLanguage = group.addItem(
			# Translators: Check box in the add-on's settings: a dialog asks for the language before each
			# voice message is transcribed.
			wx.CheckBox(box, label=_("&Ask for the language before each transcription"))
		)
		self.transcribeAskLanguage.SetValue(conf["transcribeAskLanguage"])
		self.transcribeVoiceMessages.Bind(wx.EVT_CHECKBOX, lambda evt: self._showTranscriptionDetails())
		self._showTranscriptionDetails()
		self._refreshSpeechModel()
		self._speechModelTimer = wx.Timer(self)
		self.Bind(wx.EVT_TIMER, lambda evt: self._refreshSpeechModel(), self._speechModelTimer)
		self._speechModelTimer.Start(1000)

	def _showTranscriptionDetails(self):
		"""The speech model's state and the language show only while the check box is ticked."""
		on = self.transcribeVoiceMessages.GetValue()
		for control in (self.speechModelState, self.transcribeLanguage):
			# addLabeledControl puts the label and the control in a sizer of their own.
			control.GetContainingSizer().ShowItems(on)
		self.transcribeAskLanguage.Show(on)
		self.Layout()

	def _refreshSpeechModel(self):
		if not self:
			return
		from appModules.line import speechModel

		download = speechModel.downloading()
		if download is not None:
			# Translators: State of the speech model in the add-on's settings ({percent} = how much is done).
			text = _("Downloading, {percent}%").format(percent=download.done * 100 // max(1, download.total))
		elif speechModel.installed():
			# Translators: State of the speech model in the add-on's settings.
			text = _("Downloaded")
		else:
			# Translators: State of the speech model in the add-on's settings.
			text = _("Not downloaded")
		if self.speechModelState.GetValue() != text:
			self.speechModelState.SetValue(text)

	def onDiscard(self):
		self._speechModelTimer.Stop()

	def _refreshBridge(self):
		"""Shows the bridge's state and offers the opposite: Disconnect while it is installed in LINE,
		Connect otherwise."""
		if not self:
			return  # the settings dialog was closed meanwhile
		from appModules.line import bridge

		pids = bridge._linePids()
		pid = pids[0] if pids else None
		try:
			st = bridge.status(pid)
			self._bridgeConnected = bridge.bridgeInstalled(pid)
		except Exception:
			log.error("LINE MR: bridge status check failed", exc_info=True)
			return
		self.bridgeState.SetValue(bridge.describeStatus(st))
		self.bridgeButton.SetLabel(
			# Translators: Button in the add-on's settings: removes the bridge from LINE (LINE restarts).
			_("&Disconnect bridge")
			if self._bridgeConnected
			# Translators: Button in the add-on's settings: installs the bridge in LINE (LINE restarts).
			else _("C&onnect bridge")
		)
		self.bridgeButton.Enable(
			st not in (bridge.Status.NO_LINE, bridge.Status.INCOMPATIBLE, bridge.Status.NOT_BUNDLED)
		)

	def _onBridgeButton(self, evt):
		from appModules.line import bridge

		if self._bridgeConnected:
			# Translators: Question before the bridge is removed from LINE.
			question = _(
				"Disconnect the LINE MR bridge? LINE will be closed and started again without it "
				"(a call in progress ends). LINE MR then stops naming LINE's controls and reading messages, "
				"until you connect the bridge again here."
			)
			work = bridge.disconnectAndRestart
		else:
			# Translators: Question before the bridge is installed in LINE.
			question = _("Connect the LINE MR bridge? LINE will be closed and started again (a call in progress ends).")
			work = bridge.connectAndRestart
		# Translators: Title of LINE MR's dialogs.
		if gui.messageBox(question, _("LINE MR"), wx.YES_NO | wx.ICON_QUESTION, self) != wx.YES:
			return
		self.bridgeButton.Disable()
		# Translators: Shown in the add-on's settings while LINE restarts.
		self.bridgeState.SetValue(_("Restarting LINE..."))

		def run():
			try:
				work()
			except Exception:
				log.error("LINE MR: connecting / disconnecting the bridge failed", exc_info=True)
			wx.CallAfter(self._refreshBridge)

		threading.Thread(target=run, name="lineMR.bridgeConnection", daemon=True).start()

	def onSave(self):
		conf = config.conf[CONFIG_SECTION]
		if self.checkForUpdates:
			conf["checkForUpdates"] = self.checkForUpdates.GetValue()
		conf["highlightFocus"] = self.highlightFocus.GetValue()
		conf["directChatSenderNames"] = self.directChatSenderNames.GetValue()
		conf["senderOnlyWhenChanged"] = self.senderOnlyWhenChanged.GetValue()
		conf["highlightColor"] = self._highlightColors[max(0, self.highlightColor.GetSelection())]
		conf["highlightWidth"] = self.highlightWidth.GetValue()
		from appModules.line import speechModel, transcribe

		self._speechModelTimer.Stop()
		conf["transcribeLanguage"] = transcribe.WHISPER_LANGUAGES[max(0, self.transcribeLanguage.GetSelection())]
		conf["transcribeAskLanguage"] = self.transcribeAskLanguage.GetValue()
		wasOn, on = conf["transcribeVoiceMessages"], self.transcribeVoiceMessages.GetValue()
		conf["transcribeVoiceMessages"] = on
		if wasOn and not on and (speechModel.installed() or speechModel.downloading() is not None):
			if gui.messageBox(
				# Translators: Question when voice message transcription is turned off ({size} is in megabytes).
				_("Delete the downloaded speech model ({size} MB)? Turning transcription on again downloads it again.").format(
					size=speechModel.DOWNLOAD_MB
				),
				# Translators: Title of LINE MR's dialogs.
				_("LINE MR"),
				wx.YES_NO | wx.ICON_QUESTION,
				self,
			) == wx.YES:
				speechModel.remove()
			else:
				speechModel.cancelDownload()
		_applyHighlightSettings()


class GlobalPlugin(globalPluginHandler.GlobalPlugin):
	def __init__(self):
		super().__init__()
		settingsDialogs.NVDASettingsDialog.categoryClasses.append(LineMRSettingsPanel)
		# The bridge's defaults are the add-on's; a changed setting (or another NVDA profile) is passed on.
		_applyHighlightSettings()
		self._timer = None
		if config.conf[CONFIG_SECTION]["checkForUpdates"] and not _installedFromStore():
			self._timer = wx.CallLater(STARTUP_DELAY_MS, _checker.check, False)

	def terminate(self):
		if self._timer:
			self._timer.Stop()
		try:
			settingsDialogs.NVDASettingsDialog.categoryClasses.remove(LineMRSettingsPanel)
		except ValueError:
			pass
		super().terminate()

	@script(
		# Translators: Describes the command that checks for a newer version of the add-on.
		description=_("Checks for a newer version of LINE MR"),
		category=SCRIPT_CATEGORY,
	)
	def script_checkForUpdates(self, gesture):
		# Translators: Reported when the update check starts.
		ui.message(_("Checking for LINE MR updates"))
		_checker.check(manual=True)
