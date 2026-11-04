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
	"highlightColor": "string(default='yellow')",
	"highlightWidth": "integer(default=4, min=1, max=12)",
}


def _applyHighlightSettings():
	"""Passes the focus highlight and message presentation settings to the bridge."""
	try:
		from appModules.line import highlight, messageSettings

		conf = config.conf[CONFIG_SECTION]
		highlight.writeSettings(conf["highlightFocus"], conf["highlightColor"], conf["highlightWidth"])
		messageSettings.writeSettings(conf["directChatSenderNames"])
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
		# Translators: Button in the add-on's settings: shows what changed in each version.
		whatsNew = helper.addItem(wx.Button(self, label=_("&What's new")))
		whatsNew.Bind(wx.EVT_BUTTON, lambda evt: showWhatsNew())

	def onSave(self):
		conf = config.conf[CONFIG_SECTION]
		if self.checkForUpdates:
			conf["checkForUpdates"] = self.checkForUpdates.GetValue()
		conf["highlightFocus"] = self.highlightFocus.GetValue()
		conf["directChatSenderNames"] = self.directChatSenderNames.GetValue()
		conf["highlightColor"] = self._highlightColors[max(0, self.highlightColor.GetSelection())]
		conf["highlightWidth"] = self.highlightWidth.GetValue()
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
