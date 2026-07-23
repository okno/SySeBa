[CmdletBinding()]
param([switch]$Purge)

$ErrorActionPreference = "Stop"
$principal = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent()
)
if (-not $principal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this script from an elevated PowerShell terminal."
}

& sc.exe stop SySeBa 2>$null | Out-Null
Start-Sleep -Seconds 2
& sc.exe delete SySeBa | Out-Null
if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 1060) {
    throw "Unable to remove the SySeBa Windows service."
}
if ($Purge) {
    Remove-Item -LiteralPath (Join-Path $env:ProgramData "SySeBa") `
        -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host "SySeBa service removed."
