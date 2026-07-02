# Run TwoClientsOpen.TwoConcurrentClients in a loop until it fails.
# Sleeps 10s between iterations. Exits on first non-zero exit code.
#
# Env:
#   SEEKDB_BIN   path to the seekdb binary (required — read by the test)
#   TEST_BIN     path to the test executable (default: ..\build\Debug\test_two_clients_threads.exe
#                relative to this script)

$ErrorActionPreference = 'Stop'

if (-not $env:SEEKDB_BIN) {
    Write-Error "set SEEKDB_BIN to the seekdb binary"
    exit 2
}

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$TestBin   = if ($env:TEST_BIN) { $env:TEST_BIN }
             else { Join-Path $ScriptDir '..\build\Debug\test_two_clients_threads.exe' }
$Filter    = 'TwoClientsOpen.TwoConcurrentClients'

if (-not (Test-Path $TestBin)) {
    Write-Error "test binary not found: $TestBin"
    exit 2
}

$i = 1
while ($true) {
    Write-Host "=== iteration $i ==="
    & $TestBin "--gtest_filter=$Filter"
    if ($LASTEXITCODE -ne 0) {
        Write-Error "FAILED on iteration $i (exit=$LASTEXITCODE)"
        exit 1
    }
    Start-Sleep -Seconds 10
    $i++
}
