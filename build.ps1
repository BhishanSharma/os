# build.ps1 - build Terminal OS in Docker, then run the UEFI USB image in QEMU.
#
# Usage (from the project folder):
#   .\build.ps1             build + run
#   .\build.ps1 -NoRun       build only
#   .\build.ps1 -Clean       clean, build + run
#   .\build.ps1 -NewDisk     recreate disk.img (wipes it)
#   .\build.ps1 -Gdb         debug symbols; QEMU waits for gdb on localhost:1234
#   .\build.ps1 -Serial      mirror kernel output to this console
#   .\build.ps1 -Log         run QEMU with -no-reboot and CPU reset logging
#
# First run only: if PowerShell blocks the script, run:
#   Unblock-File .\build.ps1

param(
    [switch]$NoRun,
    [switch]$Clean,
    [switch]$NewDisk,
    [switch]$Gdb,
    [switch]$Serial,
    [switch]$Log
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

function Fail($msg) { Write-Host $msg -ForegroundColor Red; exit 1 }
function Step($msg) { Write-Host $msg -ForegroundColor Cyan }

# ---- Prerequisites ---------------------------------------------------------
if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    Fail "docker not found. Install Docker Desktop."
}
docker info *> $null
if ($LASTEXITCODE -ne 0) { Fail "Docker isn't running. Start Docker Desktop and retry." }

# ---- Build image -----------------------------------------------------------
docker image inspect myos-buildenv *> $null
if ($LASTEXITCODE -ne 0) {
    Step "Building Docker image (first time only, takes a few minutes)..."
}
docker build -q buildenv -t myos-buildenv
if ($LASTEXITCODE -ne 0) { Fail "docker build failed." }

# ---- Optional ATA test disk ------------------------------------------------
# The current primary boot artifact is terminalos.img. disk.img is an extra
# writable ATA test disk, useful for filesystem testing.
if ($NewDisk -or -not (Test-Path "disk.img")) {
    Step "Creating 32 MB FAT32 disk.img..."
    Remove-Item "disk.img" -ErrorAction SilentlyContinue
    $cmd = "apt-get update -qq && apt-get install -y -qq dosfstools mtools >/dev/null && cd /w && sh scripts/mkdisk.sh disk.img"
    docker run --rm -v "${PWD}:/w" debian:stable-slim sh -c $cmd
    if ($LASTEXITCODE -ne 0) { Fail "Creating disk.img failed." }
}

# ---- Build -----------------------------------------------------------------
# DEBUG changes compiler flags, so clean before a debug build.
$make = @()
if ($Clean -or $Gdb) { $make += "clean" }
if ($Gdb) { $make += "DEBUG=1" }
$make += "build-x86_64"
$makeCommand = "make " + ($make -join " ")
Step "Building Terminal OS ($makeCommand)..."
docker run --rm -v "${PWD}:/root/env" myos-buildenv sh -c $makeCommand
if ($LASTEXITCODE -ne 0) { Fail "Build failed." }
Write-Host "Build OK: dist\x86_64\terminalos.img" -ForegroundColor Green

if ($NoRun) { exit 0 }

# ---- Run in QEMU (UEFI) ----------------------------------------------------
$qemuCommand = Get-Command "qemu-system-x86_64" -ErrorAction SilentlyContinue
if (-not $qemuCommand) {
    Fail "qemu-system-x86_64 not on PATH. Add C:\Program Files\qemu to PATH and reopen the terminal."
}

$qemuDir = Split-Path $qemuCommand.Source
$ovmfCandidates = @(
    (Join-Path $qemuDir "share\edk2-x86_64-code.fd"),
    (Join-Path $qemuDir "share\edk2-x86_64-code.fd"),
    (Join-Path $qemuDir "share\OVMF\OVMF_CODE.fd")
)
$ovmf = $ovmfCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $ovmf) {
    Fail "UEFI firmware not found. Expected edk2-x86_64-code.fd under $qemuDir\share."
}

$qemuArgs = @(
    "-cpu", "max",
    "-m", "1G",
    "-drive", "if=pflash,format=raw,readonly=on,file=$ovmf",
    "-drive", "if=virtio,file=dist/x86_64/terminalos.img,format=raw,snapshot=on",
    "-drive", "file=disk.img,format=raw,index=0,media=disk",
    "-device", "rtl8139,netdev=n0",
    "-netdev", "user,id=n0"
)
if ($Gdb) {
    $qemuArgs += @("-s", "-S")
    Step "QEMU is waiting for gdb on localhost:1234"
}
if ($Serial) { $qemuArgs += @("-serial", "stdio") }
if ($Log) {
    $qemuArgs += @("-no-reboot", "-d", "int,cpu_reset", "-D", "qemu.log")
    Step "Logging CPU exceptions and resets to qemu.log"
}

Step "Starting QEMU with UEFI..."
& $qemuCommand.Source @qemuArgs
if ($LASTEXITCODE -ne 0) { Fail "QEMU exited with code $LASTEXITCODE." }
