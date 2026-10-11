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
PROMPT = re.compile(r"root@terminal-os:[^\n]*# ")


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


class Machine:
    """One QEMU: the monitor on stdin, the serial port in a file."""

    def __init__(self, name, args, mem="1G", cpus=4, uefi=True):
        self.name = name
        self.serial = os.path.join(OUT, name + ".serial.txt")
        if os.path.exists(self.serial):
            os.remove(self.serial)
        cmd = [find_qemu(), "-cpu", "max", "-m", mem, "-smp", str(cpus), "-display", "none",
               "-monitor", "stdio", "-serial", "file:" + self.serial,
               "-netdev", "user,id=n0", "-device", "rtl8139,netdev=n0"]
        if uefi:
            cmd += ["-drive", "if=pflash,format=raw,readonly=on,file=" + find_ovmf("code")]
        self.proc = subprocess.Popen(cmd + args, cwd=ROOT, stdin=subprocess.PIPE,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.pos = 0                         # serial text already looked at

    def monitor(self, command):
        self.proc.stdin.write((command + "\n").encode())
        self.proc.stdin.flush()
        time.sleep(0.03)

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
                    raise TestError("the kernel crashed:\n" + tail(out[start:]))
            if self.proc.poll() is not None:
                raise TestError("QEMU ended (%s)" % self.proc.stderr.read().decode(errors="replace").strip())
            if time.time() > end:
                raise TestError("no %r after %d s; the end of the output:\n%s" % (rx.pattern, timeout, tail(out[start:])))
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
            self.monitor("quit")
            self.proc.wait(5)
        except Exception:
            self.proc.kill()


def tail(text, lines=25):
    return "\n".join("    " + l for l in text.strip().splitlines()[-lines:])


def check(cond, what, output=""):
    if not cond:
        raise TestError(what + ("\n" + tail(output) if output else ""))


def login(m, timeout=120):
    """From power-on to the shell prompt as root (creating the accounts on a
    first start)."""
    first = m.expect(r"New password: |login: ", timeout)
    if first.group(0).startswith("New"):
        m.type(PASSWORD)
        m.expect(r"Retype new password: ")
        m.type(PASSWORD)
        m.expect(r"User name: ")
        m.type("")                              # no everyday account
        m.expect(r"login: ")
    m.type("root")
    m.expect(r"Password: ")
    m.type(PASSWORD)
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


TESTS = [("boot", test_boot), ("files", test_files), ("fault", test_fault), ("panic", test_panic),
         ("video", test_nvme_video), ("install", test_install), ("network", test_network),
         ("bios", test_legacy_bios)]
NEEDS_INTERNET = {"network"}


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
