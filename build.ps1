# build.ps1 - build Terminal OS in Docker, then run the UEFI USB image in QEMU.
#
# Every build makes two images in dist\x86_64:
#   terminalos.img   the USB stick image (write it with -Usb)
#   kernel.iso       a CD image for virtual machines (VirtualBox, VMware, QEMU,
#                    Hyper-V...), BIOS or UEFI: give the machine 2 GB of memory
#                    or more for the browser
#
# Usage (from the project folder):
#   .\build.ps1             build + run
#   .\build.ps1 -NoRun       build only
#   .\build.ps1 -Clean       clean, build + run
#   .\build.ps1 -NewDisk     recreate disk.img (wipes it)
#   .\build.ps1 -Gdb         debug symbols; QEMU waits for gdb on localhost:1234
#   .\build.ps1 -Serial      mirror kernel output to this console
#   .\build.ps1 -Log         run QEMU with -no-reboot and CPU reset logging
#   .\build.ps1 -Usb         build, then write the USB stick (instead of QEMU)
#   .\build.ps1 -UsbOnly     write the last build to the USB stick, no build
#       with -Usb/-UsbOnly:  -Disk <n>  that USB disk if more than one is plugged in
#                            -Yes       no "type the disk number" question
#   (scripts\write-usb.ps1 does the writing: USB disks only, at most 64 GB,
#    never the Windows disk, and the stick is read back and checked. It needs
#    administrator rights: this script asks Windows for them when it lacks them.)
#
# The web browser (Ladybird, /ladybird/bin/Ladybird on the OS):
#   .\build.ps1 -Browser     rebuild Ladybird first (minutes after a small change;
#                            hours the first time), then the OS with it
#   .\build.ps1 -NoBrowser   leave Ladybird out (a smaller, faster image)
#   Without either, the OS includes the Ladybird built last (build\ladybird), if any.
#   Ladybird needs the myos-ladybird image and the ladybird-src and lb-build
#   Docker volumes (toolchain\ladybird). With myos-ladybird present the OS is
#   built in it (the full toolchain: the ports too), else in myos-buildenv.
#   On the OS:  windowserver /ladybird/bin/Ladybird:https://example.com/
#
# First run only: if PowerShell blocks the script, run:
#   Unblock-File .\build.ps1

param(
    [switch]$NoRun,
    [switch]$Clean,
    [switch]$NewDisk,
    [switch]$Gdb,
    [switch]$Serial,
    [switch]$Log,
    [switch]$Usb,
    [switch]$UsbOnly,
    [int]$Disk = -1,
    [switch]$Yes,
    [switch]$Browser,
    [switch]$NoBrowser
)

# Every step checks its exit code ($LASTEXITCODE). "Continue", not "Stop": with
# the output redirected, Windows PowerShell takes anything a tool prints on
# stderr (CMake's warnings, the ISO tool's banner) for an error and would stop.
$ErrorActionPreference = "Continue"
Set-Location $PSScriptRoot

function Fail($msg) { Write-Host $msg -ForegroundColor Red; exit 1 }
function Step($msg) { Write-Host $msg -ForegroundColor Cyan }

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Write-Usb {
    $script = Join-Path $PSScriptRoot "scripts\write-usb.ps1"
    if (Test-Admin) {
        $usbArgs = @{ Disk = $Disk }
        if ($Yes) { $usbArgs.Yes = $true }
        & $script @usbArgs
        exit $LASTEXITCODE
    }
    # Writing a disk needs administrator rights: run the writer in an elevated
    # PowerShell (Windows asks first) and wait for it.
    Step "Writing the USB stick needs administrator rights: Windows will ask."
    $argList = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-NoExit", "-File", "`"$script`"", "-Disk", $Disk)
    if ($Yes) { $argList += "-Yes" }
    try {
        $p = Start-Process powershell -Verb RunAs -ArgumentList $argList -Wait -PassThru
    } catch {
        Fail "Administrator rights were refused: the USB stick was not written."
    }
    exit $p.ExitCode
}

if ($UsbOnly) { Write-Usb }

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

# ---- The web browser (Ladybird) -------------------------------------------
# The full toolchain image: Ladybird is built in it, and so is the OS when it
# exists (the ports need it).
if ($Browser -and $NoBrowser) { Fail "-Browser and -NoBrowser: pick one." }
docker image inspect myos-ladybird *> $null
$haveLadybirdImage = $LASTEXITCODE -eq 0
if ($Browser) {
    docker image inspect myos-skia *> $null
    if ($LASTEXITCODE -ne 0) {
        Fail "-Browser needs the myos-skia image (toolchain\skia\Dockerfile and the images before it)."
    }
    foreach ($volume in "ladybird-src", "lb-build") {
        docker volume inspect $volume *> $null
        if ($LASTEXITCODE -ne 0) { Fail "-Browser needs the Docker volume $volume (Ladybird's source and build folder)." }
    }
    Step "Updating the myos-ladybird image..."
    docker build -q -t myos-ladybird -f toolchain/ladybird/Dockerfile toolchain
    if ($LASTEXITCODE -ne 0) { Fail "Building the myos-ladybird image failed." }
    $haveLadybirdImage = $true

    Step "Building Ladybird (incremental; the first build takes hours)..."
    docker run --rm -e JOBS=3 -v ladybird-src:/lb -v lb-build:/build `
        -v "${PWD}/toolchain/ladybird:/port" myos-ladybird sh /port/build.sh
    if ($LASTEXITCODE -ne 0) { Fail "Ladybird build failed." }

    Step "Laying out Ladybird in build\ladybird..."
    New-Item -ItemType Directory -Force "build\ladybird" | Out-Null
    docker run --rm -v lb-build:/build -v "${PWD}/toolchain/ladybird:/port" `
        -v "${PWD}/build/ladybird:/out" myos-ladybird sh /port/install.sh
    if ($LASTEXITCODE -ne 0) { Fail "Laying out Ladybird failed." }
}
$withBrowser = -not $NoBrowser -and (Test-Path "build\ladybird\bin\Ladybird")
if ($Browser -and -not $withBrowser) { Fail "Ladybird was built but build\ladybird\bin\Ladybird is missing." }

# ---- Build -----------------------------------------------------------------
# DEBUG changes compiler flags, so clean before a debug build.
$make = @()
if ($Clean -or $Gdb) { $make += "clean" }
if ($Gdb) { $make += "DEBUG=1" }
$make += "build-x86_64", "iso"
if ($withBrowser) { $make += "LADYBIRD=build/ladybird" }
$makeCommand = "make " + ($make -join " ")
$buildImage = if ($haveLadybirdImage) { "myos-ladybird" } else { "myos-buildenv" }
Step "Building Terminal OS ($makeCommand, in $buildImage)..."
docker run --rm -v "${PWD}:/root/env" -w /root/env $buildImage sh -c $makeCommand
if ($LASTEXITCODE -ne 0) { Fail "Build failed." }
$what = if ($withBrowser) { " (with the Ladybird browser)" } else { "" }
Write-Host "Build OK: dist\x86_64\terminalos.img (USB stick), dist\x86_64\kernel.iso (virtual machines)$what" -ForegroundColor Green

if ($Usb) { Write-Usb }
if ($NoRun) { exit 0 }

# ---- Run in QEMU (UEFI) ----------------------------------------------------
$qemuCommand = Get-Command "qemu-system-x86_64" -ErrorAction SilentlyContinue
if (-not $qemuCommand) {
    Fail "qemu-system-x86_64 not on PATH. Add C:\Program Files\qemu to PATH and reopen the terminal."
}

$qemuDir = Split-Path $qemuCommand.Source
$ovmfCandidates = @(
    (Join-Path $qemuDir "share\edk2-x86_64-code.fd"),
    (Join-Path $qemuDir "share\OVMF\OVMF_CODE.fd")
)
$ovmf = $ovmfCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $ovmf) {
    Fail "UEFI firmware not found. Expected edk2-x86_64-code.fd under $qemuDir\share."
}

$qemuArgs = @(
    "-cpu", "max",
    "-m", $(if ($withBrowser) { "2G" } else { "1G" }),   # (Ladybird runs several processes)
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
