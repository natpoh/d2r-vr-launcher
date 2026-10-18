# Builds D2R_VR_Launcher_Setup_<version>.exe with Inno Setup 6 from a build folder holding D2RVRLauncher.exe and vrmodlauncher.exe.
#
#   powershell -ExecutionPolicy Bypass -File build_launcher_installer.ps1 -BuildDir <cmake build>\Release [-Version 0.3.0] [-OutputDirectory <dir>]
#
# The installer is per user (PrivilegesRequired=lowest): no UAC prompt. A SHA-256 file is written beside it.
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [string]$Version = '0.3.2',
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'Output'),
    [string]$ISCC = 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'
)
$ErrorActionPreference = 'Stop'
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
foreach ($f in 'D2RVRLauncher.exe', 'vrmodlauncher.exe') {
    if (-not (Test-Path -LiteralPath (Join-Path $BuildDir $f))) { throw "$f is not in $BuildDir" }
}
if ($Version -notmatch '^\d+\.\d+\.\d+([-.][0-9A-Za-z.]+)?$') { throw "Use a version like 0.3.0 (got '$Version')" }
if (-not (Test-Path -LiteralPath $ISCC)) { throw "Inno Setup 6 is needed: $ISCC" }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
& $ISCC /Q "/DBuildDir=$BuildDir" "/DLauncherVersion=$Version" "/O$OutputDirectory" (Join-Path $PSScriptRoot 'D2R_VR_Launcher_Setup.iss')
if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)" }
$setup = Join-Path $OutputDirectory "D2R_VR_Launcher_Setup_$Version.exe"
$hash = (Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash
Set-Content -Path "$setup.sha256" -Value "$hash  $(Split-Path $setup -Leaf)" -Encoding ascii
Write-Host "$setup  SHA-256 $hash"
