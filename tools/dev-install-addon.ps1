<#
.SYNOPSIS
  Development install: copies the staged add-on (build\addon) into NVDA's user add-ons
  folder and restarts NVDA, skipping the .nvda-addon install dialog.

.PARAMETER NoRestart
  Copy only; restart NVDA yourself (NVDA+Q, or NVDA+Control+F3 does not reload add-on folders).
#>
param([switch]$NoRestart)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
python "$root\tools\build_addon.py" --stage-only
if ($LASTEXITCODE -ne 0) { throw "staging failed" }

$dest = Join-Path $env:APPDATA "nvda\addons\lineMR"
if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
Copy-Item (Join-Path $root "build\addon") $dest -Recurse
Write-Host "Copied add-on to $dest"

if (-not $NoRestart) {
	$nvda = @("${env:ProgramFiles}\NVDA\nvda.exe", "${env:ProgramFiles(x86)}\NVDA\nvda.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
	if (-not $nvda) { throw "NVDA not found" }
	Start-Process $nvda -ArgumentList "-r"
	Write-Host "Restarting NVDA"
}
