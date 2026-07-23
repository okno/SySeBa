[CmdletBinding()]
param(
    [string]$Distribution = ""
)

$ErrorActionPreference = "Stop"
$script = (Resolve-Path -LiteralPath (
    Join-Path $PSScriptRoot "build-release.sh"
)).Path

if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
    throw "WSL is required for the cross-platform release builder."
}

$linuxScript = (& wsl.exe wslpath -a $script).Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($linuxScript)) {
    throw "Unable to translate the release script path into WSL."
}

$arguments = @()
if (-not [string]::IsNullOrWhiteSpace($Distribution)) {
    $arguments += @("--distribution", $Distribution)
}
$arguments += @("--exec", "bash", $linuxScript)

& wsl.exe @arguments
if ($LASTEXITCODE -ne 0) {
    throw "Release build failed with exit code $LASTEXITCODE."
}

Write-Host "Local release completed. No artifact was published."
