param(
    [Parameter(Mandatory = $true)]
    [string]$Binary
)

$ErrorActionPreference = "Stop"
$root = Join-Path ([System.IO.Path]::GetTempPath()) ("syseba-integration-" + [guid]::NewGuid().ToString("N"))
$port = Get-Random -Minimum 19000 -Maximum 19999
$process = $null

function Wait-Until {
    param(
        [string]$Description,
        [scriptblock]$Condition
    )

    for ($attempt = 0; $attempt -lt 100; $attempt++) {
        if (& $Condition) {
            return
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Timeout: $Description"
}

function Read-SharedText {
    param([string]$Path)

    $stream = [System.IO.File]::Open(
        $Path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete
    )
    try {
        $reader = New-Object System.IO.StreamReader($stream)
        try {
            return $reader.ReadToEnd()
        }
        finally {
            $reader.Dispose()
        }
    }
    finally {
        $stream.Dispose()
    }
}

try {
    $source = Join-Path $root "source"
    $backup = Join-Path $root "backup"
    $restore = Join-Path $root "restore"
    $config = Join-Path $root "syseba.conf"
    $database = Join-Path $root "legacy.db"
    $log = Join-Path $root "syseba.log"
    $lock = Join-Path $root "syseba.lock"
    $token = Join-Path $root "web.token"
    $stdout = Join-Path $root "stdout.log"
    $stderr = Join-Path $root "stderr.log"

    New-Item -ItemType Directory -Force (Join-Path $source "docs"), $backup, $restore | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $source "docs\alpha.txt"), "alpha`n")
    [System.IO.File]::WriteAllText(
        $config,
        @"
[SETTINGS]
source = $source
backup = $backup
restore = $restore
log = $log
threads = 2
"@
    )
    [System.IO.File]::WriteAllText($token, "integration-token`n")

    $databaseScript = @'
import sqlite3
import sys
connection = sqlite3.connect(sys.argv[1])
connection.execute("""
CREATE TABLE logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp TEXT,
    operation TEXT,
    source_path TEXT,
    target_path TEXT,
    additional_info TEXT
)
""")
connection.commit()
connection.close()
'@
    $databaseScript | python - $database
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to create legacy SQLite fixture"
    }

    $arguments = @(
        "run",
        "--silent",
        "--web",
        "--web-host", "127.0.0.1",
        "--web-port", "$port",
        "--config", $config,
        "--lockfile", $lock,
        "--db-path", $database,
        "--web-token-file", $token
    )
    $process = Start-Process `
        -FilePath $Binary `
        -ArgumentList $arguments `
        -PassThru `
        -NoNewWindow `
        -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr

    Wait-Until "Web API" {
        try {
            $response = Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:$port/api/auth"
            return $response.StatusCode -eq 200
        }
        catch {
            return $false
        }
    }
    Wait-Until "initial backup" {
        Test-Path (Join-Path $backup "docs\alpha.txt")
    }
    $initial = [System.IO.File]::ReadAllText((Join-Path $backup "docs\alpha.txt"))
    if ($initial -ne "alpha`n") {
        throw "Initial backup content mismatch"
    }

    try {
        Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:$port/api/status" | Out-Null
        throw "Unauthenticated status unexpectedly succeeded"
    }
    catch {
        if ($_.Exception.Response.StatusCode.value__ -ne 401) {
            throw
        }
    }

    $headers = @{"X-SySeBa-Token" = "integration-token"}
    $status = Invoke-RestMethod -Uri "http://127.0.0.1:$port/api/status" -Headers $headers
    if ($status.version -ne "2.0.0") {
        throw "Unexpected Web API version"
    }

    [System.IO.File]::WriteAllText((Join-Path $source "docs\alpha.txt"), "beta`n")
    Wait-Until "modified backup" {
        (Test-Path (Join-Path $backup "docs\alpha.txt")) -and
        ([System.IO.File]::ReadAllText((Join-Path $backup "docs\alpha.txt")) -eq "beta`n")
    }

    Remove-Item -LiteralPath (Join-Path $source "docs\alpha.txt")
    Wait-Until "soft delete" {
        Test-Path (Join-Path $restore "docs\alpha.txt")
    }

    & $Binary restore-copy `
        --config $config `
        --db-path $database `
        --lockfile (Join-Path $root "maintenance.lock") `
        --path "docs/alpha.txt" | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "CLI restore failed"
    }
    Wait-Until "CLI restore" {
        Test-Path (Join-Path $source "docs\alpha.txt")
    }

    $checkScript = @'
import sqlite3
import sys
connection = sqlite3.connect(sys.argv[1])
columns = {row[1] for row in connection.execute("PRAGMA table_info(logs)")}
assert "level" in columns, columns
assert connection.execute("SELECT COUNT(*) FROM logs").fetchone()[0] > 0
connection.close()
'@
    $checkScript | python - $database
    if ($LASTEXITCODE -ne 0) {
        throw "SQLite migration check failed"
    }

    Wait-Until "text log" {
        try {
            return (Test-Path $log) -and
                ((Read-SharedText -Path $log) -match "\[INFO\]")
        }
        catch {
            return $false
        }
    }
    if ((Test-Path $stderr) -and (Get-Item $stderr).Length -ne 0) {
        throw "SySeBa wrote unexpected data to stderr: $([System.IO.File]::ReadAllText($stderr))"
    }
    Write-Output "windows integration: OK"
}
finally {
    if ($null -ne $process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        $process.WaitForExit()
    }
    if (Test-Path $root) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
