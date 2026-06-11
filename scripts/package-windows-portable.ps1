$ErrorActionPreference = "Stop"

$RootDir = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if ($PSScriptRoot -match 'scripts$') {
    $RootDir = Split-Path -Parent $PSScriptRoot
}

$metadata = @{}
Get-Content "$RootDir\APP_METADATA" | ForEach-Object {
    if ($_ -match '^(\w+)=(.*)$') {
        $metadata[$Matches[1]] = $Matches[2]
    }
}
$AppName = $metadata["APP_NAME"]
$BuildDir = "$RootDir\build_release"
$ExePath = "$BuildDir\Release\$AppName.exe"

if (-not (Test-Path $ExePath)) {
    Write-Error "Release exe not found at $ExePath. Run scripts/build-windows.ps1 release first."
    exit 1
}

$ZipName = "${AppName}-windows-x64-portable.zip"
$ZipPath = "$BuildDir\$ZipName"
$StageDir = "$BuildDir\portable_stage"

if (Test-Path $StageDir) { Remove-Item -Recurse -Force $StageDir }
New-Item -ItemType Directory -Path $StageDir | Out-Null
Copy-Item $ExePath "$StageDir\$AppName.exe"

if (Test-Path $ZipPath) { Remove-Item -Force $ZipPath }
Compress-Archive -Path "$StageDir\*" -DestinationPath $ZipPath

Write-Host "Portable Windows package: $ZipPath"
