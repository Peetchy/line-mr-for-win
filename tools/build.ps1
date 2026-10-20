<#
.SYNOPSIS
  Builds the bridge (bridge\build\line_a11y.dll) and the NVDA add-on (dist\lineMR-<ver>.nvda-addon).

.PARAMETER QtDir
  Qt 6.6.3 MSVC SDK (headers + import libs). Get it with:
  python -m aqt install-qt windows desktop 6.6.3 win64_msvc2019_64 --archives qtbase --outputdir $HOME\Qt
#>
param(
	[string]$QtDir = "$HOME\Qt\6.6.3\msvc2019_64",
	[ValidateSet("Release", "RelWithDebInfo", "Debug")] [string]$Config = "RelWithDebInfo",
	[switch]$BridgeOnly,
	[switch]$AddonOnly
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

if (-not $AddonOnly) {
	$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
	$vs = $null
	if (Test-Path $vswhere) { $vs = & $vswhere -latest -products * -property installationPath }
	if (-not $vs) { $vs = "C:\Program Files\Microsoft Visual Studio\2022\Community" }
	$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
	if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found under $vs" }
	if (-not (Test-Path "$QtDir\lib\cmake\Qt6")) { throw "Qt SDK not found at $QtDir" }

	$src = Join-Path $root "bridge"
	$out = Join-Path $src "build"
	$cmds = @(
		"`"$vcvars`" >nul",
		"cmake -S `"$src`" -B `"$out`" -G Ninja -DCMAKE_BUILD_TYPE=$Config -DCMAKE_PREFIX_PATH=`"$QtDir`"",
		"cmake --build `"$out`""
	) -join " && "
	# vcvars writes harmless warnings to stderr, which Windows PowerShell would turn into errors.
	$ErrorActionPreference = "Continue"
	cmd /c "$cmds 2>&1"
	$ErrorActionPreference = "Stop"
	if ($LASTEXITCODE -ne 0) { throw "bridge build failed" }
	Write-Host "Built $out\line_a11y.dll"
}

if (-not $BridgeOnly) {
	python "$root\tools\build_addon.py"
	if ($LASTEXITCODE -ne 0) { throw "add-on build failed" }
}
