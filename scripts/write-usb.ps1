# write-usb.ps1 - write dist\x86_64\terminalos.img to a USB stick, then check it.
#
# Usage (from the project folder; asks for administrator rights itself):
#   .\scripts\write-usb.ps1                  the only USB stick plugged in
#   .\scripts\write-usb.ps1 -Disk 2          that disk (see the list it prints)
#   .\scripts\write-usb.ps1 -Yes             no question (the stick must still be the only one)
#   .\scripts\write-usb.ps1 -AllowLarge      a disk bigger than 64 GB (careful: external SSDs)
#
# Safety: only USB disks, never the disk Windows booted from, never one larger
# than 64 GB unless -AllowLarge, and the disk number is typed to confirm.
# Everything on the chosen disk is erased.
param(
    [int]$Disk = -1,
    [switch]$Yes,
    [switch]$AllowLarge,
    [string]$Image = "dist\x86_64\terminalos.img",
    [string]$LogFile = ""                      # (set when it runs itself elevated)
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not [IO.Path]::IsPathRooted($Image)) { $Image = Join-Path $root $Image }

function Fail($msg) { Write-Host $msg -ForegroundColor Red; exit 1 }
function Step($msg) { Write-Host $msg -ForegroundColor Cyan }

if (-not (Test-Path $Image)) { Fail "No $Image yet: build first (.\build.ps1 -NoRun)." }

# ---- Administrator rights: run again elevated, show what it printed ---------
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) {
    $log = Join-Path $env:TEMP "terminalos-usb.log"
    Remove-Item $log -ErrorAction SilentlyContinue
    $argList = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"",
              "-Image", "`"$Image`"", "-LogFile", "`"$log`"", "-Disk", $Disk)
    if ($Yes) { $argList += "-Yes" }
    if ($AllowLarge) { $argList += "-AllowLarge" }
    Step "Asking for administrator rights to write the disk..."
    try {
        $p = Start-Process powershell -Verb RunAs -ArgumentList $argList -Wait -PassThru
    } catch {
        Fail "Administrator rights were not given."
    }
    if (Test-Path $log) { Get-Content $log | Where-Object { $_ -notmatch '^\*{5,}|^(Windows PowerShell transcript|Start time|End time|Username|RunAs User|Configuration Name|Machine|Host Application|Process ID|PSVersion|PSEdition|PSCompatibleVersions|BuildVersion|CLRVersion|WSManStackVersion|PSRemotingProtocolVersion|SerializationVersion|Transcript started)' } }
    exit $p.ExitCode
}
if ($LogFile) { Start-Transcript -Path $LogFile -Force | Out-Null }

$code = 0
try {
    # ---- Which disk ----------------------------------------------------------
    $usb = @(Get-Disk | Where-Object { $_.BusType -eq "USB" -and -not $_.IsBoot -and -not $_.IsSystem })
    if ($usb.Count -eq 0) { throw "No USB disk found. Plug in the stick and try again." }
    Write-Host "USB disks:"
    foreach ($d in $usb) {
        Write-Host ("  Disk {0}: {1}, {2:N1} GB" -f $d.Number, $d.FriendlyName, ($d.Size / 1GB))
    }
    if ($Disk -ge 0) {
        $target = $usb | Where-Object { $_.Number -eq $Disk }
        if (-not $target) { throw "Disk $Disk is not one of the USB disks above." }
    } elseif ($usb.Count -eq 1) {
        $target = $usb[0]
    } else {
        throw "More than one USB disk: choose one with -Disk <number>."
    }
    if ($target.Size -gt 64GB -and -not $AllowLarge) {
        throw ("Disk {0} is {1:N0} GB: too big for a test stick (an external drive?). Use -AllowLarge if you are sure." -f $target.Number, ($target.Size / 1GB))
    }
    $bytes = (Get-Item $Image).Length
    if ($target.Size -lt $bytes) { throw "Disk $($target.Number) is smaller than the image." }

    $name = "Disk {0}: {1}, {2:N1} GB" -f $target.Number, $target.FriendlyName, ($target.Size / 1GB)
    if (-not $Yes) {
        Write-Host ""
        Write-Host "Everything on $name will be ERASED." -ForegroundColor Yellow
        $answer = Read-Host "Type the disk number ($($target.Number)) to go on"
        if ($answer -ne "$($target.Number)") { throw "Cancelled: nothing was written." }
    }

    # ---- Write ---------------------------------------------------------------
    $num = $target.Number
    Step "Erasing $name..."
    if ($target.IsOffline) { Set-Disk -Number $num -IsOffline $false }
    if ($target.IsReadOnly) { Set-Disk -Number $num -IsReadOnly $false }
    if ($target.PartitionStyle -ne "RAW") { Clear-Disk -Number $num -RemoveData -RemoveOEM -Confirm:$false }
    Start-Sleep -Seconds 2

    $data = [IO.File]::ReadAllBytes($Image)
    $len = [int]([math]::Ceiling($data.Length / 512) * 512)          # whole sectors
    $buf = New-Object byte[] $len
    [Array]::Copy($data, $buf, $data.Length)

    Step ("Writing {0:N1} MiB..." -f ($len / 1MB))
    $fs = New-Object IO.FileStream("\\.\PhysicalDrive$num", [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::ReadWrite)
    $chunk = 1MB
    for ($off = 0; $off -lt $len; $off += $chunk) {
        $fs.Write($buf, $off, [math]::Min($chunk, $len - $off))
        Write-Progress -Activity "Writing the USB stick" -PercentComplete ([int](100 * $off / $len))
    }
    $fs.Flush()
    $fs.Close()
    Write-Progress -Activity "Writing the USB stick" -Completed

    # ---- Check: read it back -------------------------------------------------
    Step "Checking..."
    $fs = New-Object IO.FileStream("\\.\PhysicalDrive$num", [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $back = New-Object byte[] $len
    $got = 0
    while ($got -lt $len) {
        $r = $fs.Read($back, $got, [math]::Min($chunk, $len - $got))
        if ($r -le 0) { break }
        $got += $r
    }
    $fs.Close()
    $sha = [Security.Cryptography.SHA256]::Create()
    if ([BitConverter]::ToString($sha.ComputeHash($buf)) -ne [BitConverter]::ToString($sha.ComputeHash($back))) {
        throw "The stick does not read back what was written (a bad stick?)."
    }
    Update-Disk -Number $num -ErrorAction SilentlyContinue
    Write-Host "USB stick ready: $name. Boot the laptop from it (F12 boot menu on most Acer laptops)." -ForegroundColor Green
} catch {
    Write-Host "ERROR: $_" -ForegroundColor Red
    $code = 1
}
if ($LogFile) { Stop-Transcript | Out-Null }
exit $code
