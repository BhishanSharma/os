# usb-logs.ps1 - copy the logs Terminal OS saved on its USB stick (LOGS\BOOTnnnn.LOG,
# one per boot, by logd) into .\logs, to read them in Windows.
#
# Usage (from the project folder; asks for administrator rights to read the stick):
#   .\scripts\usb-logs.ps1            the only USB stick plugged in
#   .\scripts\usb-logs.ps1 -Disk 2    that disk
#
# Windows gives the stick's partition no drive letter (it is an EFI system
# partition), so the partition is copied raw (read only) and its files are
# read by scripts\fatlogs.py (Python 3). Nothing is written to the stick.
param(
    [int]$Disk = -1,
    [string]$Out = "",                         # (set when it runs itself elevated)
    [string]$LogFile = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
function Fail($msg) { Write-Host $msg -ForegroundColor Red; exit 1 }
function Step($msg) { Write-Host $msg -ForegroundColor Cyan }

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $Out) {
    # ---- Step 1 (as administrator): the stick's partition into a file -------
    $image = Join-Path $env:TEMP "terminalos-usb-part.img"
    $log = Join-Path $env:TEMP "terminalos-usb-logs.log"
    Remove-Item $image, $log -ErrorAction SilentlyContinue
    $argList = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"",
                 "-Disk", $Disk, "-Out", "`"$image`"", "-LogFile", "`"$log`"")
    Step "Asking for administrator rights to read the stick..."
    try {
        $p = Start-Process powershell -Verb RunAs -ArgumentList $argList -Wait -PassThru
    } catch {
        Fail "Administrator rights were not given."
    }
    if (Test-Path $log) { Get-Content $log | Where-Object { $_ -match '^(USB|  Disk|Reading|ERROR)' } }
    if ($p.ExitCode -ne 0 -or -not (Test-Path $image)) { exit 1 }

    # ---- Step 2: its LOGS folder out (scripts\fatlogs.py reads FAT32) --------
    $dest = Join-Path $root "logs"
    python (Join-Path $PSScriptRoot "fatlogs.py") $image LOGS $dest
    $code = $LASTEXITCODE
    Remove-Item $image -ErrorAction SilentlyContinue
    if ($code -eq 2) { Fail "No LOGS folder on the stick yet (boot Terminal OS from it first)." }
    if ($code -ne 0) { Fail "Could not read the stick's files (is Python 3 on PATH?)." }
    $files = Get-ChildItem $dest -Filter "BOOT*.LOG" | Sort-Object Name
    Write-Host ("Copied {0} log(s) to {1}; the newest is {2}." -f $files.Count, $dest, $files[-1].Name) -ForegroundColor Green
    exit 0
}

# ---- Elevated: find the stick, copy its "TERMOS USB" partition (read only) ----
Start-Transcript -Path $LogFile -Force | Out-Null
$code = 0
try {
    $usb = @(Get-Disk | Where-Object { $_.BusType -eq "USB" -and -not $_.IsBoot -and -not $_.IsSystem })
    if ($Disk -ge 0) { $usb = @($usb | Where-Object { $_.Number -eq $Disk }) }
    # The stick Terminal OS was written to: GPT, first partition an EFI system one of about 62 MiB.
    $found = @()
    foreach ($d in $usb) {
        $part = Get-Partition -DiskNumber $d.Number -ErrorAction SilentlyContinue |
                Where-Object { $_.GptType -eq "{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}" -and $_.Size -le 256MB } |
                Select-Object -First 1
        if ($part) { $found += [pscustomobject]@{ Disk = $d; Part = $part } }
    }
    if ($found.Count -eq 0) { throw "No Terminal OS USB stick found." }
    if ($found.Count -gt 1) { throw "More than one Terminal OS stick: choose one with -Disk <number>." }
    $d = $found[0].Disk
    $part = $found[0].Part
    Write-Output ("USB stick: Disk {0}, {1}" -f $d.Number, $d.FriendlyName)
    Write-Output ("Reading its partition ({0:N0} MiB)..." -f ($part.Size / 1MB))
    $fs = New-Object IO.FileStream("\\.\PhysicalDrive$($d.Number)", [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $fs.Seek([int64]$part.Offset, [IO.SeekOrigin]::Begin) | Out-Null
    $outFile = [IO.File]::Create($Out)
    $buf = New-Object byte[] (1MB)
    $left = [int64]$part.Size
    while ($left -gt 0) {
        $n = $fs.Read($buf, 0, [int][math]::Min($buf.Length, $left))
        if ($n -le 0) { break }
        $outFile.Write($buf, 0, $n)
        $left -= $n
    }
    $outFile.Close()
    $fs.Close()
} catch {
    Write-Output "ERROR: $_"
    $code = 1
}
Stop-Transcript | Out-Null
exit $code
