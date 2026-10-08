# build.ps1 - build the OS ISO in Docker, then run it in QEMU.
# Usage (from the project folder):
#   .\build.ps1            build + run
#   .\build.ps1 -NoRun     build only
#   .\build.ps1 -Clean     make clean first, then build + run
#   .\build.ps1 -NewDisk   recreate disk.img (wipes it)
param(
    [switch]$NoRun,
    [switch]$Clean,
    [switch]$NewDisk
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

function Fail($msg) { Write-Host $msg -ForegroundColor Red; exit 1 }

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) { Fail "docker not found. Start Docker Desktop." }
docker info *> $null
if ($LASTEXITCODE -ne 0) { Fail "Docker isn't running. Start Docker Desktop and retry." }

# Build image once if it doesn't exist
docker image inspect myos-buildenv *> $null
if ($LASTEXITCODE -ne 0) {
    Write-Host "Building Docker image (first time only)..." -ForegroundColor Cyan
    docker build buildenv -t myos-buildenv
    if ($LASTEXITCODE -ne 0) { Fail "docker build failed." }
}

# Create disk.img if missing (or requested)
if ($NewDisk -or -not (Test-Path disk.img)) {
    Write-Host "Creating 32 MB FAT32 disk.img..." -ForegroundColor Cyan
    Remove-Item disk.img -ErrorAction SilentlyContinue
    $cmd = "apt-get update -qq && apt-get install -y -qq dosfstools mtools >/dev/null && cd /w && " +
           "dd if=/dev/zero of=disk.img bs=1M count=32 2>/dev/null && mkfs.fat -F 32 disk.img >/dev/null && " +
           "echo 'Hello from FAT32!' > /tmp/test.txt && echo 'Readme file content' > /tmp/readme.txt && " +
           "mcopy -i disk.img /tmp/test.txt ::test.txt && mcopy -i disk.img /tmp/readme.txt ::readme.txt"
    docker run --rm -v "${PWD}:/w" debian:stable-slim sh -c $cmd
    if ($LASTEXITCODE -ne 0) { Fail "Creating disk.img failed." }
}

# Build
$make = "make build-x86_64"
if ($Clean) { $make = "make clean && make build-x86_64" }
Write-Host "Building kernel..." -ForegroundColor Cyan
docker run --rm -v "${PWD}:/root/env" myos-buildenv sh -c $make
if ($LASTEXITCODE -ne 0) { Fail "Build failed." }
Write-Host "Build OK: dist\x86_64\kernel.iso" -ForegroundColor Green

if ($NoRun) { exit 0 }

# Run
if (-not (Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue)) {
    Fail "qemu-system-x86_64 not on PATH. Add C:\Program Files\qemu to PATH and reopen the terminal."
}
Write-Host "Starting QEMU..." -ForegroundColor Cyan
qemu-system-x86_64 `
    -cdrom dist\x86_64\kernel.iso `
    -drive file=disk.img,format=raw,index=0,media=disk `
    -boot d `
    -device rtl8139,netdev=n0 `
    -netdev user,id=n0
