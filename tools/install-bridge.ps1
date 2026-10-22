<#
.SYNOPSIS
  Installs (or removes) line_a11y.dll in LINE for Windows and optionally restarts LINE.

.DESCRIPTION
  LINE's Qt only searches its own directory for plugins (QT_PLUGIN_PATH is ignored), so the
  bridge goes to %LOCALAPPDATA%\LINE\bin\current\platformthemes\line_a11y.dll, where Qt loads
  it as the "windows" platform theme however LINE is started. LINE's helper processes get a copy
  in %LOCALAPPDATA%\LINE\Data\plugin\<name>\<version>\platformthemes: LineCall (incoming-call
  notification, call windows) and LineMediaPlayer (photo and video viewer). LINE updates may
  replace these folders; run this script again (the NVDA add-on also re-installs it).

.PARAMETER Restart
  Quit LINE first (the DLL is locked while LINE runs) and start it again afterwards.
.PARAMETER Uninstall
  Remove the bridge instead of installing it.
.PARAMETER Dll
  Bridge DLL to install (default: bridge\build\line_a11y.dll).
#>
param(
	[switch]$Restart,
	[switch]$Uninstall,
	[string]$Dll
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$lineBin = Join-Path $env:LOCALAPPDATA "LINE\bin"
if (-not (Test-Path (Join-Path $lineBin "current\LINE.exe"))) { throw "LINE for Windows is not installed under $lineBin" }
$targets = @(Join-Path $lineBin "current\platformthemes\line_a11y.dll")
$helpers = @("LineCall", "LineMediaPlayer")
foreach ($helper in $helpers) {
	$helperRoot = Join-Path $env:LOCALAPPDATA "LINE\Data\plugin\$helper"
	if (-not (Test-Path $helperRoot)) { continue }
	foreach ($dir in Get-ChildItem $helperRoot -Directory) {
		if (Test-Path (Join-Path $dir.FullName "$helper.exe")) {
			$targets += Join-Path $dir.FullName "platformthemes\line_a11y.dll"
		}
	}
}

# LINE starts its helper processes, which lock their copies of the bridge too.
function Test-LineRunning { [bool](Get-Process (@("LINE") + $helpers) -ErrorAction SilentlyContinue) }

$wasRunning = Test-LineRunning
if ($wasRunning) {
	if (-not $Restart) { throw "LINE is running and locks the bridge DLL; quit LINE or pass -Restart" }
	Get-Process (@("LINE") + $helpers) -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
	for ($i = 0; $i -lt 50 -and (Test-LineRunning); $i++) { Start-Sleep -Milliseconds 200 }
	if (Test-LineRunning) { throw "LINE did not quit" }
	# Not a crash: keep the bridge's crash guard from counting it.
	Remove-Item (Join-Path $env:LOCALAPPDATA "LineMR\session.running") -ErrorAction SilentlyContinue
}

if ($Uninstall) {
	foreach ($target in $targets) {
		if (Test-Path $target) { Remove-Item $target -Force }
		$dir = Split-Path $target
		if ((Test-Path $dir) -and -not (Get-ChildItem $dir)) { Remove-Item $dir }
		Write-Host "Removed $target"
	}
} else {
	if (-not $Dll) { $Dll = Join-Path $root "bridge\build\line_a11y.dll" }
	if (-not (Test-Path $Dll)) { throw "No bridge DLL at $Dll; build it with tools\build.ps1" }
	foreach ($target in $targets) {
		New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
		Copy-Item $Dll $target -Force
		Write-Host "Installed $target"
	}
	# Keep a dev-installed add-on's bundled copy identical, or it reports the bridge as outdated.
	$addonDll = Join-Path $env:APPDATA "nvda\addons\lineMR\bridge\line_a11y.dll"
	if (Test-Path $addonDll) {
		Copy-Item $Dll $addonDll -Force
		Write-Host "Updated $addonDll"
	}
}

if ($Restart) {
	Start-Process -FilePath (Join-Path $lineBin "LineLauncher.exe") -WorkingDirectory $lineBin
	Write-Host "Started LINE. Bridge log: $env:LOCALAPPDATA\LineMR\logs\bridge.log"
}
