#!/usr/bin/env python3
"""System tests: boot Terminal OS in QEMU, type commands, check what it prints.

    python tests/qemu/run_tests.py              all tests
    python tests/qemu/run_tests.py boot files   only these
    python tests/qemu/run_tests.py --list       what there is
    python tests/qemu/run_tests.py --offline    skip the ones that need the internet

Before: `make build-x86_64 iso test-assets` (in Docker). Needs QEMU with OVMF
(UEFI firmware) on this machine; QEMU and OVMF can be pointed to with the
QEMU and OVMF environment variables. Each test boots its own machine and waits
for the text it expects on the serial port (everything the console prints
goes there too), so nothing depends on fixed delays. Logs and screenshots go
to build/test/qemu/.
"""
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = os.path.join(ROOT, "build", "test", "qemu")
ASSETS = os.path.join(ROOT, "build", "test")
USB_IMG = os.path.join(ROOT, "dist", "x86_64", "terminalos.img")
ISO = os.path.join(ROOT, "dist", "x86_64", "kernel.iso")
PASSWORD = "pass1"
PROMPT = re.compile(r"\w+@terminal-os:[^\n]*[#$] ")


def find_qemu():
    for p in (os.environ.get("QEMU"), r"C:\Program Files\qemu\qemu-system-x86_64.exe"):
        if p and os.path.exists(p):
            return p
    return shutil.which("qemu-system-x86_64") or "qemu-system-x86_64"


def find_ovmf(name):
    """OVMF's code (read-only) or variable template file."""
    want = {"code": ["edk2-x86_64-code.fd", "OVMF_CODE.fd", "OVMF_CODE_4M.fd"],
            "vars": ["edk2-i386-vars.fd", "OVMF_VARS.fd", "OVMF_VARS_4M.fd"]}[name]
    dirs = [os.path.dirname(os.environ["OVMF"])] if os.environ.get("OVMF") else []
    dirs += [os.path.join(os.path.dirname(find_qemu()), "share"), "/usr/share/OVMF", "/usr/share/qemu"]
    for d in dirs:
        for f in want:
            if os.path.exists(os.path.join(d, f)):
                return os.path.join(d, f)
    raise TestError("no OVMF %s file found (set OVMF to the code file)" % name)


class TestError(Exception):
    pass


KEYS = {' ': 'spc', '/': 'slash', ':': 'shift-semicolon', '.': 'dot', '-': 'minus', '_': 'shift-minus',
        '&': 'shift-7', '$': 'shift-4', '?': 'shift-slash', ';': 'semicolon', '"': 'shift-apostrophe',
        '~': 'shift-grave_accent', '>': 'shift-dot', '<': 'shift-comma', '=': 'equal', '|': 'shift-backslash',
        "'": 'apostrophe', '*': 'shift-8', ',': 'comma', '!': 'shift-1', '@': 'shift-2', '#': 'shift-3',
        '(': 'shift-9', ')': 'shift-0', '+': 'shift-equal'}


_symbols = None


def kernel_symbols():
    """address -> name for the kernel's functions (nm in the build image)."""
    global _symbols
    if _symbols is None:
        _symbols = {}
        nm = subprocess.run(["docker", "run", "--rm", "-v", ROOT + ":/root/env", "myos-buildenv",
                             "sh", "-c", "x86_64-elf-nm -n /root/env/dist/x86_64/kernel.bin"],
                            capture_output=True, text=True).stdout
        for line in nm.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[1] in "tT":
                _symbols[int(parts[0], 16)] = parts[2]
    return _symbols


class Machine:
    """One QEMU: the monitor on stdin, the serial port in a file."""

    def __init__(self, name, args, mem="1G", cpus=4, uefi=True, nic=True):
        self.name = name
        self.serial = os.path.join(OUT, name + ".serial.txt")
        if os.path.exists(self.serial):
            os.remove(self.serial)
        cmd = [find_qemu(), "-cpu", "max", "-m", mem, "-smp", str(cpus), "-display", "none",
               "-monitor", "stdio", "-serial", "file:" + self.serial]
        if nic:
            cmd += ["-netdev", "user,id=n0", "-device", "rtl8139,netdev=n0"]
        if uefi:
            cmd += ["-drive", "if=pflash,format=raw,readonly=on,file=" + find_ovmf("code")]
        self.mon_path = os.path.join(OUT, name + ".monitor.txt")   # what the monitor answers
        self.mon_out = open(self.mon_path, "wb")
        self.proc = subprocess.Popen(cmd + args, cwd=ROOT, stdin=subprocess.PIPE,
                                     stdout=self.mon_out, stderr=subprocess.PIPE)
        self.pos = 0                         # serial text already looked at

    def monitor(self, command):
        self.proc.stdin.write((command + "\n").encode())
        self.proc.stdin.flush()
        time.sleep(0.03)

    def where(self):
        """Where each CPU is (for a hang): its instruction pointer and the
        kernel function there (from the symbols of kernel.bin)."""
        try:
            size = os.path.getsize(self.mon_path)
            self.monitor("info registers -a")
            time.sleep(1.5)
            with open(self.mon_path, "rb") as f:
                f.seek(size)
                regs = f.read().decode("latin-1")
            rips = [int(r, 16) for r in re.findall(r"RIP=([0-9a-f]{16})", regs)]
            syms = kernel_symbols()
            lines = []
            for i, rip in enumerate(rips):
                name = max((a for a in syms if a <= rip), default=None)
                lines.append("    cpu %d: %016x %s" % (i, rip, syms[name] + "+0x%x" % (rip - name) if name else "(user)"))
            return "\n  where the CPUs are:\n" + "\n".join(lines)
        except Exception as e:
            return "\n  (could not read the CPUs: %s)" % e

    def keys(self, *names):
        for k in names:
            self.monitor("sendkey " + k)

    def type(self, text, enter=True):
        for ch in text:
            self.keys('shift-' + ch.lower() if ch.isupper() else KEYS.get(ch, ch))
        if enter:
            self.keys("ret")

    def text(self):
        try:
            with open(self.serial, "rb") as f:
                return f.read().decode("latin-1").replace("\r", "")
        except FileNotFoundError:
            return ""

    def expect(self, pattern, timeout=60, since=None):
        """Wait for `pattern` (regex) in the output after `since` (default: after
        the last thing found). Returns the match."""
        rx = re.compile(pattern) if isinstance(pattern, str) else pattern
        start = self.pos if since is None else since
        end = time.time() + timeout
        while True:
            out = self.text()
            m = rx.search(out, start)
            if m:
                self.pos = m.end()
                return m
            for bad in ("System halted", "KERNEL PANIC"):
                if bad in out[start:] and bad not in pattern:
                    for _ in range(20):                    # the rest of the panic screen
                        if "System halted" in out[start:]:
                            break
                        time.sleep(0.25)
                        out = self.text()
                    raise TestError("the kernel crashed:\n" + tail(out[start:], 40))
            if self.proc.poll() is not None:
                raise TestError("QEMU ended (%s)" % self.proc.stderr.read().decode(errors="replace").strip())
            if time.time() > end:
                raise TestError("no %r after %d s; the end of the output:\n%s" % (rx.pattern, timeout, tail(out[start:])) + self.where())
            time.sleep(0.25)

    def run(self, command, timeout=60):
        """Type a shell command, wait for the next prompt; returns what it printed."""
        start = len(self.text())
        self.type(command)
        echo = self.expect(re.escape(command), timeout, since=start)
        m = self.expect(PROMPT, timeout, since=echo.end())
        return self.text()[echo.end():m.start()]

    def screenshot(self, name):
        self.monitor("screendump " + os.path.join(OUT, name + ".png").replace("\\", "/") + " -f png")
        time.sleep(1)

    def stop(self):
        try:
            if self.proc.poll() is None:              # (not powered off already)
                self.monitor("quit")
                self.proc.wait(5)
        except Exception:
            self.proc.kill()
        try:
            self.proc.stdin.close()
        except OSError:
            pass


def tail(text, lines=25):
    return "\n".join("    " + l for l in text.strip().splitlines()[-lines:])


def check(cond, what, output=""):
    if not cond:
        raise TestError(what + ("\n" + tail(output) if output else ""))


def text_login_prompt(m, timeout=120, user="", password=None):
    """To the text login's prompt: the graphical login asks for it (as on a
    Mac, the user name >console); a first start asks root's password and an
    everyday account (`user`, none if "") on the way."""
    while True:
        seen = m.expect(r"loginwindow: ready|login: |New password: ", timeout).group(0)
        if seen.startswith("loginwindow"):
            time.sleep(0.5)
            m.type(">console")
        elif seen.startswith("New"):
            m.type(PASSWORD)
            m.expect(r"Retype new password: ")
            m.type(PASSWORD)
            m.expect(r"User name: ")
            m.type(user)
            if user:
                m.expect(r"New password: ")
                m.type(password)
                m.expect(r"Retype new password: ")
                m.type(password)
        else:
            return


def login(m, timeout=120):
    """From power-on to the shell prompt as root (creating the accounts on a
    first start)."""
    text_login_prompt(m, timeout)
    m.type("root")
    m.expect(r"Password: ")
    m.type(PASSWORD)
    m.expect(PROMPT, 60)


def login_user(m, name, password, timeout=120):
    """First start: root's password, then an everyday account `name`; log in as it."""
    text_login_prompt(m, timeout, name, password)
    m.type(name)
    m.expect(r"Password: ")
    m.type(password)
    m.expect(PROMPT, 60)


def usb_stick():
    if not os.path.exists(USB_IMG):
        raise TestError("no %s: run `make build-x86_64` first" % USB_IMG)
    return ["-device", "qemu-xhci,id=xhci",
            "-drive", "if=none,id=stick,file=%s,format=raw,snapshot=on" % USB_IMG,
            "-device", "usb-storage,bus=xhci.0,drive=stick"]


def asset(name):
    p = os.path.join(ASSETS, name)
    if not os.path.exists(p):
        raise TestError("no %s: run `make test-assets` first" % p)
    return p


def nvme(image, snapshot=True):
    return ["-drive", "file=%s,if=none,id=nv0,format=raw%s" % (image, ",snapshot=on" if snapshot else ""),
            "-device", "nvme,serial=TOS123,drive=nv0"]


def sata(image, snapshot=True, port=0):
    """A disk on an AHCI controller (QEMU's ICH9 AHCI), on SATA port `port`."""
    return ["-device", "ahci,id=ahci",
            "-drive", "file=%s,if=none,id=sd0,format=raw%s" % (image, ",snapshot=on" if snapshot else ""),
            "-device", "ide-hd,drive=sd0,bus=ahci.%d" % port]


# ---- The tests ---------------------------------------------------------------------------

def test_boot():
    """UEFI boot from the USB stick, 4 cores, a program in its own address space."""
    m = Machine("boot", usb_stick())
    try:
        boot = m.expect(r"CPU: \d+ cores? running", 120)
        log = m.text()
        check("[ FAIL ]" not in log, "a boot step failed", log)
        check("4 cores running" in boot.group(0), "not all 4 cores started: " + boot.group(0))
        login(m)
        out = m.run("cpus")
        check("4 cores running" in out, "cpus", out)
        out = m.run("hello")
        check("from user space" in out, "hello did not run", out)
        code = re.search(r"code is at 0x([0-9a-f]+) and its stack at 0x([0-9a-f]+)", out)
        check(code and int(code.group(1), 16) < 0x10000000 and int(code.group(2), 16) > 0x7f0000000000,
              "the program is not in the lower half (code low, stack near 128 TiB)", out)
        out = m.run("meminfo")
        check(re.search(r"RAM:\s+\d+ MiB usable", out), "meminfo", out)
    finally:
        m.stop()


def test_files():
    """The file commands, now user programs: make, write, list, read and remove."""
    m = Machine("files", usb_stick(), cpus=2)
    try:
        login(m)
        check("Created directory" in m.run('mkdir "My Folder"'), "mkdir")
        m.run('cd "My Folder"')
        check("My Folder" in m.run("pwd"), "cd into a folder with a space")
        out = m.run("write note.txt hello from a test")
        check("Wrote 17 bytes" in out, "write", out)
        out = m.run("cat note.txt")
        check("hello from a test" in out, "cat", out)
        out = m.run("touch empty.txt")
        check("Created file" in out, "touch", out)
        out = m.run("ls")
        check("note.txt" in out and "empty.txt" in out and "2 files" in out, "ls", out)
        out = m.run("fileinfo note.txt")
        check("Size: 17 bytes" in out, "fileinfo", out)
        out = m.run("hexdump note.txt")
        check("68 65 6C 6C 6F" in out, "hexdump (hello = 68 65 6C 6C 6F)", out)
        out = m.run("rm empty.txt")
        check("Deleted" in out, "rm", out)
        check("empty.txt" not in m.run("ls"), "the file is still there after rm")
        m.run("cd /")
        out = m.run('tree "My Folder"')
        check("note.txt" in out and "1 file" in out, "tree", out)
        out = m.run("echo $USER is here")
        check("root is here" in out, "echo with a variable", out)
        out = m.run("date")
        check(re.search(r"\d{4}-\d\d-\d\d \d\d:\d\d:\d\d", out), "date", out)
        out = m.run("date -u")
        check("UTC" in out, "date -u", out)
        check("Up " in m.run("uptime"), "uptime")
        t = time.time()
        m.run("sleep 2")
        check(time.time() - t >= 1.5, "sleep 2 returned at once")
        out = m.run("cat nothing.txt")
        check("not found" in out, "cat of a missing file", out)
    finally:
        m.stop()


def test_fault():
    """A program that crashes is ended; the system and the shell go on."""
    m = Machine("fault", usb_stick(), cpus=2)
    try:
        login(m)
        out = m.run("fault")
        check("Segmentation fault" in out or "fault" in out.lower(), "fault", out)
        check("still here" in m.run("echo still here"), "the shell after a crashing program")
    finally:
        m.stop()


def test_nvme_video():
    """Files on an NVMe TERMINALOS partition; a video with sound from it."""
    wav = os.path.join(OUT, "video.wav")
    if os.path.exists(wav):
        os.remove(wav)
    m = Machine("video", usb_stick() + nvme(asset("nvme-files.img")) +
                ["-audiodev", "wav,id=snd0,path=" + wav, "-device", "intel-hda",
                 "-device", "hda-output,audiodev=snd0"])
    try:
        m.expect(r"Files\s+on the NVMe TERMINALOS partition", 120)
        login(m)
        check("test.mpg" in m.run("ls"), "test.mpg on the partition")
        start = len(m.text())
        m.type("video test.mpg")
        m.expect(r"test\.mpg: 640x360, 30 fps", 60, since=start)
        time.sleep(8)
        m.screenshot("video")
        m.keys("q")
        m.expect(PROMPT, 60)
        size = os.path.getsize(wav) if os.path.exists(wav) else 0
        data = open(wav, "rb").read()[44:] if size > 44 else b""
        check(len(data) > 48000 and any(b not in (0, 255) for b in data[::97]),
              "no sound was played (%d bytes recorded)" % len(data))
    finally:
        m.stop()


def test_user():
    """An everyday user on the NVMe partition: programs work in their home folder
    (the current folder belongs to each program, not to the kernel)."""
    m = Machine("user", usb_stick() + nvme(asset("nvme-files.img")))
    try:
        m.expect(r"Files\s+on the NVMe TERMINALOS partition", 120)
        login_user(m, "boss", "pass2")
        check("/HOME/BOSS" in m.run("pwd"), "boss does not start in /HOME/BOSS")
        out = m.run("ls")
        check("(empty)" in out, "ls in an empty home folder", out)
        out = m.run("write notes.txt mine")
        check("Wrote 4 bytes" in out, "write in the home folder", out)
        for _ in range(3):
            out = m.run("ls")
            check("notes.txt" in out and "PASSWD" not in out, "ls lists the home folder", out)
            out = m.run("cat notes.txt")
            check("mine" in out, "cat in the home folder", out)
        m.run("mkdir sub")
        m.run("cd sub")
        out = m.run("ls ..")
        check("notes.txt" in out and "sub/" in out, "ls ..", out)
        out = m.run("cat /PASSWD")
        check("permission denied" in out, "an everyday user read the password file", out)
        out = m.run("attack port", 60)                # echod (root) has "echo": not boss's to take
        check("attack port: refused" in out, "an everyday user took over a root service", out)
        out = m.run("attack sandbox", 60)             # stage 10: an app's sandbox holds
        check(out.count(": refused") == 3 and "NOT REFUSED" not in out and "folder works" in out,
              "the sandbox", out)
    finally:
        m.stop()


def test_bundle():
    """Stage 8: a .tapp package installed with tapp is an app bundle in
    ~/Apps/ID/; the window server lists it and opens it by its id."""
    m = Machine("bundle", usb_stick() + nvme(asset("nvme-files.img")))
    try:
        m.expect(r"Files\s+on the NVMe TERMINALOS partition", 120)
        login_user(m, "boss", "pass2")
        out = m.run("tapp install /hello.tapp", 60)
        check("FAILED" not in out.upper() and "error" not in out.lower(), "tapp install", out)
        out = m.run("tapp list")
        check("hello" in out, "tapp list does not show hello", out)
        m.type("windowserver hello")
        m.expect(r"windowserver: installed app Hello \(/HOME/BOSS/Apps/hello/hello\)", 30)
        m.expect(r"windowserver: open /HOME/BOSS/Apps/hello/hello: started", 30)
        time.sleep(2)
        m.screenshot("bundle")
        check("quit unexpectedly" not in m.text(), "the installed app crashed", m.text()[-600:])
    finally:
        m.stop()


def stick_with_room(path, mb):
    """A copy of the USB image on a stick of `mb` MiB (room for the data partition)."""
    shutil.copy(USB_IMG, path)
    with open(path, "r+b") as f:
        f.truncate(mb << 20)
    return ["-device", "qemu-xhci,id=xhci",
            "-drive", "if=none,id=stick,file=%s,format=raw" % path,
            "-device", "usb-storage,bus=xhci.0,drive=stick"]


def test_storage():
    """Stage 9: the first start makes the stick's data partition (ext3) and
    it becomes "/": files have owners and modes, they outlive a reboot, a
    change cut off after its journal commit is finished at the next start,
    and e2fsck finds nothing wrong."""
    stick = os.path.join(OUT, "storage-stick.img")
    args = stick_with_room(stick, 1280)
    m = Machine("storage", args)
    try:
        m.expect(r"making room for your files on the USB stick", 120)
        m.expect(r"Files\s+on the USB stick \(ext3", 120)
        login_user(m, "boss", "pass2")
        check("/HOME/BOSS" in m.run("pwd"), "boss does not start in /HOME/BOSS")
        out = m.run("write notes.txt kept")
        check("Wrote 4 bytes" in out, "write in the home folder", out)
        out = m.run("ls -l")
        check(re.search(r"-rw-r--r-- boss", out), "a new file is boss's, rw-r--r--", out)
        m.run("chmod 600 notes.txt")
        out = m.run("ls -l")
        check(re.search(r"-rw------- boss", out), "chmod 600", out)
        out = m.run("ls -l /HOME")
        check(re.search(r"drwx------ boss", out), "the home folder is boss's and private", out)
        out = m.run("ls -l /")
        check(re.search(r"drwxrwxrwt root .*TMP", out), "/TMP is everyone's, sticky", out)
        out = m.run("cat /PASSWD")
        check("permission denied" in out, "an everyday user read the password file", out)
        out = m.run("mkdir /mine")
        check("permission denied" in out, "an everyday user made a folder in /", out)
        out = m.run("mv notes.txt kept.txt")
        check("mv:" not in out, "mv", out)
        out = m.run("cat kept.txt")
        check("kept" in out, "the renamed file", out)
        m.type("shutdown")
        m.proc.wait(60)
    finally:
        m.stop()

    m = Machine("storage2", args)                     # the accounts and files are still there
    try:
        m.expect(r"Files\s+on the USB stick \(ext3", 120)
        check("making room" not in m.text(), "the data partition was made again")
        login(m)
        out = m.run("cat /HOME/BOSS/kept.txt")
        check("kept" in out, "boss's file after a reboot", out)
        m.run("write /TMP/before.txt replayed")
        m.run("crash journal")
        m.run("mv /TMP/before.txt /TMP/after.txt")
        m.proc.kill()                                  # the power goes
        m.proc.wait(10)
    finally:
        m.stop()

    m = Machine("storage3", args)
    try:
        m.expect(r"finished 1 change a crash cut off", 120)
        login(m)
        out = m.run("cat /TMP/after.txt")
        check("replayed" in out, "the change cut off after its commit", out)
        out = m.run("cat /TMP/before.txt")
        check("replayed" not in out, "the old name is still there", out)
        m.type("shutdown")
        m.proc.wait(60)
    finally:
        m.stop()
    fsck = subprocess.run(["docker", "run", "--rm", "-v", ROOT + ":/root/env", "myos-buildenv", "sh", "-c",
                           "dd if=/root/env/build/test/qemu/storage-stick.img of=/tmp/p.img bs=1M skip=1024 "
                           "status=none && e2fsck -fn /tmp/p.img"], capture_output=True, text=True)
    check(fsck.returncode == 0, "e2fsck on the data partition", fsck.stdout + fsck.stderr)


def test_sleep():
    """Sleep and wake: `suspend` and a key, then the power button twice; the
    programs carry on afterwards."""
    m = Machine("sleep", usb_stick())
    try:
        login(m)
        m.type("suspend")
        m.expect(r"Sleeping: ", 30)
        time.sleep(2)
        m.keys("spc")
        m.expect(r"Awake after \d+ s", 30)
        time.sleep(1)
        m.monitor("system_powerdown")                # the power button
        m.expect(r"Sleeping: ", 30)
        time.sleep(1)
        m.monitor("system_powerdown")
        m.expect(r"Awake after \d+ s", 30)
        time.sleep(1)
        m.type("")                                   # a fresh prompt
        out = m.run("echo still here")
        check("still here" in out, "the shell after sleeping", out)
    finally:
        m.stop()


def test_dynamic():
    """Dynamic linking: dyncheck starts through ld.so, uses libc.so and dlopens
    a library (needs the toolchain image's build: build.ps1)."""
    m = Machine("dynamic", usb_stick())
    try:
        login(m)
        out = m.run("dyncheck", 60)
        check("dynamic linking works" in out, "dyncheck", out)
        out = m.run("echo back")                      # the shell after a dynamic program
        check("back" in out, "the shell after dyncheck", out)
    finally:
        m.stop()


def test_ports():
    """The ports (toolchain/ports): Lua, SQLite, nano, Vim and bash run, dynamically linked."""
    m = Machine("ports", usb_stick())
    try:
        login(m)
        out = m.run("lua -e \"print('lua ' .. 6 * 7)\"", 60)   # (no %: the typing loses it)
        check("lua 42" in out, "lua", out)
        out = m.run("sqlite3 :memory: \"select 'sqlite ' || (6 * 7);\"", 60)
        check("sqlite 42" in out, "sqlite3", out)
        out = m.run("nano --version", 60)
        check("GNU nano" in out, "nano", out)
        out = m.run("vim --version", 60)
        check("VIM - Vi IMproved 9.1" in out, "vim", out)
        m.run("vim -es -c \"normal ivim-edit\" -c wq /TMP/v.txt", 60)   # Ex mode: no screen
        out = m.run("cat /TMP/v.txt")
        check("vim-edit" in out, "vim edits and saves a file", out)
        out = m.run("bash -c 'echo bash $((6 * 7))'", 60)
        check("bash 42" in out, "bash", out)
        out = m.run("bash -c 'echo from-bash > /TMP/b.txt; cat /TMP/b.txt'", 60)   # PATH, fork, exec
        check("from-bash" in out.replace("echo from-bash", ""), "bash runs the OS's programs", out)
        # Job control: a stopped job, then going on in the background (process groups, SIGSTOP, SIGCONT).
        out = m.run("bash -c 'set -m; sleep 20 & sleep 1; kill -STOP $!; sleep 1; jobs; bg; sleep 1; jobs; "
                    "kill $!; echo jc-done'", 90)
        check(re.search(r"Stopped\s+sleep 20", out) and re.search(r"Running\s+sleep 20", out) and
              "jc-done" in out.replace("echo jc-done", ""), "bash job control (jobs, bg)", out)
    finally:
        m.stop()


def test_install():
    """`install` on an empty partition, then the restart boots from the SSD."""
    disk = os.path.join(OUT, "install-nvme.img")
    vars_fd = os.path.join(OUT, "install-vars.fd")
    shutil.copy(asset("nvme-install.img"), disk)
    shutil.copy(find_ovmf("vars"), vars_fd)
    m = Machine("install", ["-drive", "if=pflash,format=raw,file=" + vars_fd] + usb_stick() + nvme(disk, False))
    try:
        login(m)
        m.type("install")
        m.expect(r"Install on partition number", 60)
        m.type("2")
        m.expect(r"Type yes to install: ")
        m.type("yes")
        m.expect(r"Installed\.", 180)
        m.expect(r"Restart into it now\? \(yes/no\) ")
        m.type("yes")
        m.expect(r"Boot\s+installed, from the SSD", 180)
    finally:
        m.stop()


def test_sata():
    """A SATA disk (AHCI): found at boot with its partitions; `install` puts
    Terminal OS on it (40 MB written and read back) and the firmware starts it
    from there, its partition mounted as /."""
    disk = os.path.join(OUT, "sata-install.img")
    vars_fd = os.path.join(OUT, "sata-vars.fd")
    shutil.copy(asset("nvme-install.img"), disk)
    shutil.copy(find_ovmf("vars"), vars_fd)
    m = Machine("sata", ["-drive", "if=pflash,format=raw,file=" + vars_fd] + usb_stick() + sata(disk, False, 2))
    try:
        m.expect(r"SATA\s+1 disk: QEMU HARDDISK, 0\.\d GB", 120)
        login(m)
        out = m.run("sata")
        check(re.search(r"AHCI 1\.\d controller .*MSI", out) and re.search(r"port 2: disk: QEMU HARDDISK", out),
              "`sata` shows the controller (with MSI) and the disk on port 2", out)
        out = m.run("disks")
        check(re.search(r"1\. SATA drive: QEMU HARDDISK", out) and re.search(r"2: \S.*\s66 MB", out),
              "`disks` lists the SATA drive and its two partitions", out)
        m.type("install")
        m.expect(r"Install on partition number", 60)
        m.type("2")
        m.expect(r"Type yes to install: ")
        m.type("yes")
        m.expect(r"RAMDISK\.GZ\s+\d+ KiB - written and checked", 240)
        m.expect(r"Installed\.", 60)
        m.expect(r"Restart into it now\? \(yes/no\) ")
        m.type("yes")
        m.expect(r"Boot\s+installed, from the SSD", 180)
        m.expect(r"Files\s+on the SATA TERMINALOS partition", 120)
    finally:
        m.stop()


def test_hub():
    """USB hubs: a keyboard, a mouse and a second stick behind a hub, and a
    hub behind that hub; a keyboard plugged into it and out again later."""
    stick2 = os.path.join(OUT, "hub-stick2.img")
    shutil.copy(asset("nvme-files.img"), stick2)
    m = Machine("hub", usb_stick() + [
        "-device", "usb-hub,bus=xhci.0,port=2,id=hub1",
        "-device", "usb-mouse,bus=xhci.0,port=2.1",
        "-device", "usb-kbd,bus=xhci.0,port=2.2",
        "-drive", "if=none,id=stick2,file=%s,format=raw" % stick2,
        "-device", "usb-storage,bus=xhci.0,port=2.3,drive=stick2",
        "-device", "usb-hub,bus=xhci.0,port=2.4,id=hub2",
        "-device", "usb-tablet,bus=xhci.0,port=2.4.1"])
    try:
        login(m)
        out = m.run("lsusb")
        root = re.search(r"port (\d+) +\S+ +12 Mbit/s +hub \(\d+ ports\)", out)   # (QEMU's port 2: a USB 2 port)
        check(root, "lsusb: the hub", out)
        h = root.group(1)
        check(re.search(r"port %s\.1 .* mouse \(in use\)" % h, out), "lsusb: the mouse behind the hub", out)
        check(re.search(r"port %s\.2 .* keyboard \(in use\)" % h, out), "lsusb: the keyboard behind the hub", out)
        check(re.search(r"port %s\.3 .* storage \(in use\)" % h, out), "lsusb: the stick behind the hub", out)
        check(re.search(r"port %s\.4 .* hub \(\d+ ports\)" % h, out), "lsusb: the hub behind the hub", out)
        check(re.search(r"port %s\.4\.1 .* HID device" % h, out), "lsusb: the tablet two hubs down", out)
        m.monitor("device_add usb-kbd,bus=xhci.0,port=2.5,id=kbd2")
        time.sleep(3)
        out = m.run("lsusb")
        check(re.search(r"port %s\.5 .* keyboard \(in use\)" % h, out), "a keyboard plugged into the hub", out)
        m.monitor("device_del kbd2")
        time.sleep(3)
        out = m.run("lsusb")
        check(not re.search(r"port %s\.5 " % h, out) and re.search(r"port %s\.2 .* keyboard" % h, out),
              "the keyboard unplugged from the hub", out)
        check("still here" in m.run("echo still here"), "typing after the hubs")
    finally:
        m.stop()


def test_usbnet():
    """A USB network adapter (CDC-ECM: QEMU's usb-net) behind a hub, and no
    other network card: it is the card; DHCP and a ping over it."""
    m = Machine("usbnet", usb_stick() + [
        "-netdev", "user,id=n1",
        "-device", "usb-hub,bus=xhci.0,port=2",
        "-device", "usb-net,bus=xhci.0,port=2.1,netdev=n1,id=net1"], nic=False)
    try:
        m.expect(r"Network\s+USB Ethernet [0-9a-f]{4}:[0-9a-f]{4}, MAC 52:54:00:12:34:\w\w", 120)
        m.expect(r"DHCP\s+10\.0\.2\.15, gateway 10\.0\.2\.2", 60)
        login(m)
        out = m.run("lsusb")
        check(re.search(r"port \d+\.1 .* network adapter \(in use\)", out), "lsusb: the adapter", out)
        out = m.run("ping 10.0.2.2 3", 30)
        check(re.search(r"3 packets transmitted, [123] received", out), "ping over the USB adapter", out)
        m.monitor("device_del net1")                           # out, and into another port of the hub
        time.sleep(2)
        m.monitor("device_add usb-net,bus=xhci.0,port=2.3,netdev=n1,id=net2")
        m.expect(r"USB Ethernet [0-9a-f]{4}:[0-9a-f]{4}: DHCP gave 10\.0\.2\.15", 60)
        out = m.run("ping 10.0.2.2 2", 30)
        check(re.search(r"2 packets transmitted, [12] received", out), "ping after plugging the adapter in again", out)
    finally:
        m.stop()


def test_network():
    """DHCP, DNS, TCP and TLS: an HTTPS download."""
    m = Machine("network", usb_stick(), cpus=2)
    try:
        login(m)
        out = m.run("download https://example.com/ ex.html", 120)
        check(re.search(r"Downloaded \d+ bytes", out), "download", out)
        check("Example Domain" in m.run("cat ex.html"), "the downloaded page")
    finally:
        m.stop()


def test_legacy_bios():
    """The GRUB ISO on a BIOS PC (the 32-bit way into the kernel)."""
    if not os.path.exists(ISO):
        raise TestError("no %s: run `make iso` first" % ISO)
    m = Machine("bios", ["-cdrom", ISO, "-boot", "d"], cpus=2, uefi=False)
    try:
        login(m, 180)                           # GRUB's menu starts it after 2 s
        check("from user space" in m.run("hello"), "hello")
    finally:
        m.stop()


def test_panic():
    """A kernel page fault shows the panic screen instead of resetting."""
    m = Machine("panic", usb_stick(), cpus=2)
    try:
        login(m)
        m.type("crash pf")
        m.expect(r"KERNEL PANIC", 30)
        m.expect(r"Page Fault", 30)
        m.expect(r"System halted", 30)
    finally:
        m.stop()


def test_kernel():
    """Stages 1-6: virtual memory, threads, ports, fork/exec/pipes/signals, the
    device registry, and launchd bringing a service back."""
    m = Machine("kernel", usb_stick())
    try:
        login(m)
        out = m.run("vmtest", 60)
        check("FAILED" not in out and "copy-on-write" in out.lower(), "vmtest", out)
        out = m.run("vmtest overflow", 60)
        check("Stack overflow" in out, "a runaway stack is not reported", out)
        out = m.run("threads", 60)
        check(re.search(r"threads: .*: ok", out), "threads", out)
        out = m.run("porttest", 60)
        check("FAILED" not in out and "1 MiB message: ok" in out, "porttest", out)
        out = m.run("forktest", 60)
        check("FAILED" not in out and " ok" in out, "forktest", out)
        out = m.run("ptytest", 60)                    # pseudo-terminals: cooked, raw, Ctrl+C/D
        check("ptytest: all ok" in out, "ptytest", out)
        for kind in ("elf", "tramp"):                 # what the kernel must refuse (attack.c)
            out = m.run("attack " + kind, 60)
            check("attack %s: refused" % kind in out, "attack " + kind, out)
        out = m.run("attack sigreturn", 60)           # ends only the program, not the kernel
        check("NOT REFUSED" not in out and "KERNEL PANIC" not in m.text(), "attack sigreturn", out)
        check("still here" in m.run("echo still here"), "the shell after attack sigreturn")
        out = m.run("echo hello world | wc")
        check(re.search(r"1\s+2\s+12", out), "a pipe", out)
        m.run("echo hi > o.txt")
        m.run("echo more >> o.txt")
        out = m.run("wc < o.txt")
        check(re.search(r"2\s+2\s+8", out), "redirection", out)
        out = m.run("ioreg")
        check("Storage" in out or "nvme" in out.lower() or "usb" in out.lower(), "ioreg", out)
        out = m.run("cpu")
        check(re.search(r"CPU: .*, 4 cores running", out) and re.search(r"idle", out), "cpu", out)
        out = m.run("ps")
        pid = re.search(r"^\s*(\d+)\s+\d+\s+\w+\s+\w+\s+\S+\s+echod", out, re.M)
        check(pid and "launchd" in out, "launchd and echod are not running", out)
        m.run("kill " + pid.group(1))
        m.run("sleep 1")
        out = m.run("ps")
        again = re.search(r"^\s*(\d+)\s+\d+\s+\w+\s+\w+\s+\S+\s+echod", out, re.M)
        check(again and again.group(1) != pid.group(1), "launchd did not restart echod", out)
    finally:
        m.stop()


def move_mouse(m, dx, dy, step=50):
    """In small steps: QEMU drops what does not fit in the PS/2 queue. Slowly
    (step=4) the pointer moves one point per count: no acceleration."""
    while dx or dy:
        sx, sy = max(-step, min(step, dx)), max(-step, min(step, dy))
        m.monitor("mouse_move %d %d" % (sx, sy))
        dx, dy = dx - sx, dy - sy
        time.sleep(0.02)


def test_desktop():
    """Stage 7: the window server starts, an app opens a window, a click reaches it."""
    m = Machine("desktop", usb_stick())
    try:
        login(m)
        m.type("windowserver about")
        ws = m.expect(r"windowserver: (\d+)x(\d+), scale (\d), cells (\d+)x(\d+)", 30)
        button = m.expect(r"about: OK button at (\d+),(\d+)", 30)
        time.sleep(1)
        m.screenshot("desktop")
        move_mouse(m, -4000, -4000)                  # the pointer to the top left corner
        time.sleep(0.5)
        m.monitor("mouse_button 1")
        time.sleep(0.3)
        m.monitor("mouse_button 0")
        corner = m.expect(r"click at (\d+),(\d+)", 10)
        m.keys("esc")                                # that click opened the Terminal OS menu
        time.sleep(0.3)
        cw, ch = int(ws.group(4)), int(ws.group(5))
        at = (int(corner.group(1)), int(corner.group(2)))
        for _ in range(4):                           # the pointer accelerates: aim, click, correct
            dx = (int(button.group(1)) - at[0]) * 8 // cw
            dy = (int(button.group(2)) - at[1]) * 16 // ch
            move_mouse(m, dx, dy, step=4)
            time.sleep(0.5)
            m.monitor("mouse_button 1")
            time.sleep(0.3)
            m.monitor("mouse_button 0")
            time.sleep(0.5)
            if "about: OK clicked" in m.text():
                break
            clicks = re.findall(r"click at (\d+),(\d+)", m.text())
            at = (int(clicks[-1][0]), int(clicks[-1][1]))
        m.expect(r"about: OK clicked", 10)
        m.keys("ctrl-q")                             # Ctrl+Q leaves the desktop
        m.expect(PROMPT, 10)
    finally:
        m.stop()


def test_polish():
    """The desktop's polish: a notification's banner, a screenshot (Print
    Screen), Calculator and Calendar open, the Launcher opens (Ctrl+L)."""
    m = Machine("polish", usb_stick())
    try:
        login(m)
        out = m.run("cat /DOCK.CFG")                 # the Dock's apps (once empty: a CRLF mkdock.sh)
        check("Calculator" in out and "Settings" in out, "the Dock's apps (DOCK.CFG)", out)
        m.type("windowserver notify:Hello calculator calendar")
        m.expect(r"windowserver: \d+x\d+", 30)
        m.expect(r"notification from notify: Hello", 30)
        time.sleep(3)
        m.screenshot("polish")
        m.keys("print")                              # Print Screen: a PNG in ~/Pictures
        m.expect(r"notification from DOOR OS: Screenshot", 30)
        m.keys("ctrl-l")                             # the Launcher (its glass once overflowed a GPU batch)
        time.sleep(2)
        m.keys("print")                              # still drawing, still answering
        m.expect(r"notification from DOOR OS: Screenshot", 30)
        check("Segmentation fault" not in m.text(), "the Launcher crashed the window server", m.text()[-600:])
        m.keys("ctrl-q")
        m.expect(PROMPT, 10)
    finally:
        m.stop()


def test_gui():
    """All graphical: the boot screen, the Setup Assistant makes an account
    (a name only: no password until one is turned on in Settings) and opens
    the desktop; Log Out shows the profiles; Return signs in again."""
    m = Machine("gui", usb_stick())
    try:
        m.expect(r"\] Timer", 120)
        time.sleep(0.3)
        m.screenshot("boot")                         # the boot screen, not the boot text
        m.expect(r"loginwindow: setup", 120)
        m.expect(r"loginwindow: ready", 30)
        time.sleep(1)
        m.screenshot("setup")
        m.type("anna")
        m.expect(r"loginwindow: created anna", 60)      # (the door, then two password hashes)
        m.expect(r"loginwindow: anna logged in", 60)
        m.expect(r"windowserver: \d+x\d+", 30)
        time.sleep(2)
        m.screenshot("gui")
        start = len(m.text())
        m.keys("ctrl-q")                             # Log Out: the profiles
        m.expect(r"loginwindow: ready", 30, since=start)
        time.sleep(1)
        m.screenshot("profiles")
        check(m.text().count("anna logged in") == 1, "Log Out signed straight back in")
        m.keys("ret")                                # no password: Return signs in
        m.expect(r"loginwindow: anna logged in", 20, since=start)
        m.expect(r"windowserver: \d+x\d+", 30, since=start)   # the desktop again, not the text console
        time.sleep(2)
        m.screenshot("gui2")
        start = len(m.text())                        # root has no password yet: no way in as root
        m.keys("ctrl-q")
        m.expect(r"loginwindow: ready", 30, since=start)
        time.sleep(1)
        m.type(">console")
        m.expect(r"login: ", 30, since=start)
        m.type("root")
        m.expect(r"Password: ", 10)
        m.type("")
        m.expect(r"Login incorrect", 10)
    finally:
        m.stop()


def test_apps():
    """Every app in apps/ starts without crashing, and ends at Log Out."""
    m = Machine("apps", usb_stick())
    try:
        login(m)
        start = len(m.text())
        # Every app in apps/ (Terminal last: the front window for the keys below).
        apps = sorted(n for n in os.listdir(os.path.join(ROOT, "apps"))
                      if os.path.isfile(os.path.join(ROOT, "apps", n, "app.cfg")) and n != "terminal")
        m.type("windowserver " + " ".join(apps + ["terminal"]))
        m.expect(r"windowserver: \d+x\d+", 30)
        time.sleep(6)
        m.screenshot("apps")
        m.type("ls")                                 # into Terminal, the front window: a kernel command
        time.sleep(2)
        m.type("passwd")                             # asks in the window (input hidden), then Ctrl+C
        time.sleep(1)
        m.keys("ctrl-c")
        time.sleep(1)
        m.screenshot("terminal")
        out = m.text()[start:]
        check("[ERROR]" not in out and "KERNEL PANIC" not in out, "an app crashed", out)
        m.keys("ctrl-q")
        m.expect(PROMPT, 20)
        out = m.run("ps")
        for name in apps:
            alive = [l for l in out.splitlines() if l.endswith(name) and " done " not in l]
            check(not alive, name + " kept running after Log Out", out)
    finally:
        m.stop()


def test_zoom():
    """The maximise button fills the screen with Terminal; Up recalls a command."""
    m = Machine("zoom", usb_stick())
    try:
        login(m)
        m.type("windowserver terminal")
        ws = m.expect(r"windowserver: (\d+)x(\d+), scale (\d), cells (\d+)x(\d+)", 30)
        time.sleep(3)
        S, cw, ch = int(ws.group(3)), int(ws.group(4)), int(ws.group(5))
        btn = m.expect(r"windowserver: Terminal zoom button at (\d+),(\d+)", 10, since=0)
        target = (int(btn.group(1)), int(btn.group(2)))   # its maximise button (title bar, right)
        move_mouse(m, -4000, -4000)
        time.sleep(0.5)
        at = (0, 0)
        for _ in range(4):                           # aim, click, correct
            move_mouse(m, (target[0] - at[0]) * 8 // cw, (target[1] - at[1]) * 16 // ch, step=4)
            time.sleep(0.5)
            m.monitor("mouse_button 1")
            time.sleep(0.3)
            m.monitor("mouse_button 0")
            time.sleep(0.8)
            if "windowserver: zoom" in m.text():
                break
            clicks = re.findall(r"click at (\d+),(\d+)", m.text())
            if clicks:
                at = (int(clicks[-1][0]), int(clicks[-1][1]))
        m.expect(r"windowserver: zoom Terminal to \d+x\d+", 10, since=0)
        time.sleep(1)
        start = len(m.text())
        m.type("whoami")                             # a kernel command: it shows on serial too
        time.sleep(1)
        m.keys("up")
        m.keys("ret")
        time.sleep(1.5)
        m.screenshot("zoom")
        check(len(re.findall(r"^root\r?$", m.text()[start:], re.M)) >= 2,
              "Up did not bring the command back", m.text()[-600:])
    finally:
        m.stop()


def test_logs():
    """The boot stick is at /USB; logd saves the log there; an app that crashes
    on the desktop gets a "quit unexpectedly" report."""
    m = Machine("logs", usb_stick())
    try:
        login(m)
        time.sleep(4)                                # logd saves every 3 s
        out = m.run("ls /USB/LOGS")
        check("BOOT0001.LOG" in out.upper(), "no log on the stick", out)
        out = m.run("cat /USB/LOGS/BOOT0001.LOG")
        check("USB stick" in out and "logd: saving" in out, "the saved log is not the boot's", out)
        start = len(m.text())
        m.type("windowserver fault:null")
        m.expect(r"crashrep: .* quit unexpectedly", 30, since=start)
        time.sleep(2)
        m.screenshot("crash")
        m.keys("ctrl-q")
        m.expect(PROMPT, 20)
    finally:
        m.stop()


TESTS = [("boot", test_boot), ("files", test_files), ("fault", test_fault), ("panic", test_panic),
         ("kernel", test_kernel), ("desktop", test_desktop), ("polish", test_polish), ("gui", test_gui), ("apps", test_apps), ("zoom", test_zoom), ("logs", test_logs), ("user", test_user), ("video", test_nvme_video), ("bundle", test_bundle), ("storage", test_storage), ("sleep", test_sleep), ("dynamic", test_dynamic), ("ports", test_ports), ("install", test_install), ("sata", test_sata), ("hub", test_hub), ("usbnet", test_usbnet), ("network", test_network),
         ("bios", test_legacy_bios)]
NEEDS_INTERNET = {"network"}
# Tests of programs only the full toolchain builds (myos-ladybird: build.ps1).
NEEDS_TOOLCHAIN = {"dynamic": os.path.join(ROOT, "build", "user", "dyncheck.elf"),
                   "ports": os.path.join(ROOT, "build", "ports", "out", "lua", "bin", "lua")}


def main(argv):
    if "--list" in argv:
        for name, fn in TESTS:
            print("%-8s %s" % (name, fn.__doc__))
        return 0
    offline = "--offline" in argv
    names = [a for a in argv if not a.startswith("--")]
    unknown = [n for n in names if n not in dict(TESTS)]
    if unknown:
        print("no such test: " + ", ".join(unknown))
        return 2
    os.makedirs(OUT, exist_ok=True)
    results = []
    for name, fn in TESTS:
        if names and name not in names:
            continue
        if offline and name in NEEDS_INTERNET:
            results.append((name, "SKIP", 0, "needs the internet"))
            continue
        if name in NEEDS_TOOLCHAIN and not os.path.exists(NEEDS_TOOLCHAIN[name]):
            results.append((name, "SKIP", 0, "needs the toolchain's build"))
            print("%-8s SKIP  (needs the toolchain's build)" % name)
            continue
        print("%-8s ..." % name, end="", flush=True)
        t = time.time()
        try:
            fn()
            status, why = "PASS", ""
        except TestError as e:
            status, why = "FAIL", str(e)
        except Exception as e:                   # a bug in the test itself
            status, why = "ERROR", "%s: %s" % (type(e).__name__, e)
        took = time.time() - t
        print("\r%-8s %s  (%d s)" % (name, status, took))
        if why:
            print("    " + why.replace("\n", "\n    "))
        results.append((name, status, took, why))
    failed = [r for r in results if r[1] in ("FAIL", "ERROR")]
    print("\n%d passed, %d failed, %d skipped. Logs: %s" % (
        sum(r[1] == "PASS" for r in results), len(failed), sum(r[1] == "SKIP" for r in results), OUT))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
