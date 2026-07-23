[CmdletBinding()]
param(
    [int]$WebPort = 8765,
    [string]$WebHost = "0.0.0.0"
)

$ErrorActionPreference = "Stop"
$principal = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent()
)
if (-not $principal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this script from an elevated PowerShell terminal."
}
if ($WebPort -lt 1 -or $WebPort -gt 65535) {
    throw "WebPort must be between 1 and 65535."
}

$binary = Join-Path $PSScriptRoot "bin\SySeBa.exe"
$template = Join-Path $PSScriptRoot "etc\syseba\syseba.conf"
$state = Join-Path $env:ProgramData "SySeBa"
$config = Join-Path $state "syseba.conf"
$token = Join-Path $state "syseba_web.token"

if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) {
    throw "SySeBa.exe was not found at $binary"
}
New-Item -ItemType Directory -Force -Path $state | Out-Null
foreach ($directory in @("source", "backup", "restore")) {
    New-Item -ItemType Directory -Force -Path (Join-Path $state $directory) |
        Out-Null
}
& icacls.exe $state `
    /inheritance:r `
    /grant:r `
    "*S-1-5-18:(OI)(CI)(F)" `
    "*S-1-5-32-544:(OI)(CI)(F)" | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "Unable to protect $state with the SYSTEM/Administrators ACL."
}
if (-not (Test-Path -LiteralPath $config -PathType Leaf)) {
    Copy-Item -LiteralPath $template -Destination $config
}

& $binary service-install `
    --config $config `
    --web-host $WebHost `
    --web-port $WebPort `
    --web-token-file $token
if ($LASTEXITCODE -ne 0) {
    throw "SySeBa service installation failed with exit code $LASTEXITCODE."
}

Write-Host "SySeBa service installed and configured for automatic startup."
Write-Host "Web UI: http://<server-ip>:$WebPort"
Write-Host "Token:  $token"
