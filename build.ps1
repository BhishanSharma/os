# build.ps1 - build the OS ISO in Docker, then run it in QEMU.
#
# Usage (from the project folder):
#   .\build.ps1             build + run
#   .\build.ps1 -NoRun      build only
#   .\build.ps1 -Clean      make clean first, then build + run
#   .\build.ps1 -NewDisk    recreate disk.img (wipes it)
#   .\build.ps1 -Gdb        debug symbols; QEMU waits for gdb on localhost:1234
#   .\build.ps1 -Serial     mirror kernel output (boot log, panic reports) to this console
#   .\build.ps1 -Log        run QEMU with -no-reboot -d int,cpu_reset -D qemu.log
#
# First run only: if PowerShell blocks the script, run
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
if (-not (Get-Command docker -ErrorAction SilentlyContinue)) { Fail "docker not found. Install Docker Desktop." }
docker info *> $null
if ($LASTEXITCODE -ne 0) { Fail "Docker isn't running. Start Docker Desktop and retry." }

# ---- Build image (once) ----------------------------------------------------
docker image inspect myos-buildenv *> $null
if ($LASTEXITCODE -ne 0) {
    Step "Building Docker image (first time only, takes a few minutes)..."
    docker build buildenv -t myos-buildenv
    if ($LASTEXITCODE -ne 0) { Fail "docker build failed." }
}

# ---- Disk image ------------------------------------------------------------
if ($NewDisk -or -not (Test-Path disk.img)) {
    Step "Creating 32 MB FAT32 disk.img..."
    Remove-Item disk.img -ErrorAction SilentlyContinue
    $cmd = "apt-get update -qq && apt-get install -y -qq dosfstools mtools >/dev/null && cd /w && sh scripts/mkdisk.sh disk.img"
    docker run --rm -v "${PWD}:/w" debian:stable-slim sh -c $cmd
    if ($LASTEXITCODE -ne 0) { Fail "Creating disk.img failed." }
}

# ---- Build -----------------------------------------------------------------
$make = "make"
if ($Clean -or $Gdb) { $make += " clean" }   # flag changes (DEBUG) are not tracked, so start clean
if ($Gdb) { $make += " DEBUG=1" }
$make += " build-x86_64"
Step "Building kernel ($make)..."
docker run --rm -v "${PWD}:/root/env" myos-buildenv sh -c $make
if ($LASTEXITCODE -ne 0) { Fail "Build failed." }
Write-Host "Build OK: dist\x86_64\kernel.iso" -ForegroundColor Green

if ($NoRun) { exit 0 }

# ---- Run -------------------------------------------------------------------
if (-not (Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue)) {
    Fail "qemu-system-x86_64 not on PATH. Add C:\Program Files\qemu to PATH and reopen the terminal."
}

$qemuArgs = @(
    "-cpu", "max",
    "-cdrom", "dist\x86_64\kernel.iso",
    "-drive", "file=disk.img,format=raw,index=0,media=disk",
    "-boot", "d",
    "-device", "rtl8139,netdev=n0",
    "-netdev", "user,id=n0"
)
if ($Gdb) { $qemuArgs += @("-s", "-S"); Step "QEMU is waiting for gdb on localhost:1234" }
if ($Serial) { $qemuArgs += @("-serial", "stdio") }
if ($Log)   { $qemuArgs += @("-no-reboot", "-d", "int,cpu_reset", "-D", "qemu.log"); Step "Logging to qemu.log" }

Step "Starting QEMU..."
& qemu-system-x86_64 @qemuArgs
