# Using Terminal OS

The prompt is `root@terminal-os:<directory>#`. Up/Down arrows browse command history;
Shift+Up/Down scroll the screen.

The shell is an ordinary user-mode program (`SHELL.ELF`) that the kernel starts after you
log in; see [USERSPACE.md](USERSPACE.md#the-shell-is-a-user-program). Besides the commands
below it understands `a ; b`, `a && b`, "quoted arguments", `$?` (the last exit code),
`$USER`, `$HOME`, `$PWD` and `history`. Ctrl+C clears the line being typed.

The top row is a status bar: OS version, IP address, kernel heap in use, the disk the files
are on, uptime, and the date and time (IST). It updates every second while the shell or the
editor waits for a key.

At boot the screen shows the logo and one `[  OK  ]` / `[ WARN ]` / `[ FAIL ]` line per
subsystem (CPU, memory, display, interrupts, keyboard, network, storage, clock, DHCP). The
detailed driver messages go to the serial port and to the boot log: `dmesg` prints it.

## Users and login

After the boot screen the OS asks you to log in, like Linux:

```
terminal-os login: alice
Password:                      (not shown while you type)
alice@terminal-os:/HOME/ALICE$
```

The first time a disk is used (no `/PASSWD` yet) it asks for a root password and offers to
create your own account. The prompt ends in `#` for root and `$` for everyone else.

**Permissions.** FAT32 has no file owners, so the rule goes by location: root may change any
file; other users may read everything but create, change or delete only inside their home
folder (`/HOME/<NAME>`) and `/TMP`. This is enforced in the kernel for every command, the
editor, `download`, scripts and user programs. `mount <disk>`, `reboot`, `dhcp`,
`netdebug`, `crash`, `useradd` and `userdel` need root.

| Command            | Description                                                      |
| ------------------ | ---------------------------------------------------------------- |
| `whoami`, `id`     | Your name, or uid and home folder                                |
| `users`            | All accounts                                                     |
| `su [user]`        | Start a shell as another user (default root); asks their password unless you are root. `exit` goes back |
| `passwd [user]`    | Change your password; root can change anyone's                   |
| `useradd <name>`   | Create an account and its home folder (root; 1-8 lower-case letters/digits) |
| `userdel <name>`   | Delete an account; its files stay (root)                         |
| `logout` / `exit`  | End the session and return to the login prompt                  |
| `cd` / `cd ~`      | Go to your home folder                                           |

Accounts are stored in `/PASSWD` as `name:uid:salt:hash:home`; the hash is SHA-256 over the
salt and password, repeated 2000 times, so the file does not contain passwords. On the RAM
disk the accounts last until reboot.

## Commands

### Files
| Command                    | Description                                              |
| -------------------------- | -------------------------------------------------------- |
| `ls`                       | List the current directory                               |
| `cat <file>`               | Print a file (max 1 MB; Shift+Up/Down scrolls back)       |
| `write <file> <text>`      | Write `<text>` to a file (creates or overwrites)         |
| `touch <file>`             | Create an empty file                                     |
| `rm <file>`                | Delete a file                                            |
| `mkdir <dir>`              | Create a directory                                       |
| `cd <dir>` / `pwd`         | Change / print the working directory                     |
| `tree`                     | List the current directory (one level, no recursion yet) |
| `hexdump <file>`           | Hex dump of a file                                       |
| `fileinfo <file>`          | File name and size                                       |
| `edit <file>`              | Open the text editor                                     |

### Programs and scripts
| Command                    | Description                                              |
| -------------------------- | -------------------------------------------------------- |
| `<program> [args]`         | Run a user-mode program, e.g. `hello`, `snake`, `primes` (see [USERSPACE.md](USERSPACE.md)) |
| `run <file> [args]`        | Run a program by file name                               |
| `programs`                 | List the installed programs                              |
| `<program> ... &`          | Run it in the background                                 |
| `jobs` / `ps`              | Running programs / every task with its CPU time          |
| `fg [pid]`                 | Bring a background program to the foreground             |
| `kill <pid>`               | Stop a program (your own; root may stop any)             |
| `sh <script>`              | Run a script file                                        |
| `compile <file>[.c]`       | Compile and run a C file (tiny subset, see below)        |

### System and debugging
| Command                    | Description                                              |
| -------------------------- | -------------------------------------------------------- |
| `help`                     | List every command                                       |
| `sysinfo` / `neofetch`     | OS, boot mode (UEFI/BIOS), CPU model, memory, display, network, disk, theme, time |
| `dmesg`                    | The full boot log, including the driver messages hidden at boot |
| `lspci`                    | Every device on the PCI bus: address, IDs, kind, maker, and the driver of this OS that runs it |
| `lsusb`                    | The USB controller and what is plugged into each port (keyboards and mice are used; plug-in and removal are noticed) |
| `touchpad`                 | Find the laptop's I2C touchpad: controllers, addresses that answer, its report layout, then 8 seconds of raw reports |
| `clock [local\|utc]`       | Whether the hardware clock holds local time (Windows) or UTC (Linux, VMs); real PCs default to local |
| `clear`                    | Clear the screen                                         |
| `echo <text>`              | Print text                                               |
| `uptime`                   | Seconds since boot                                       |
| `sleep <seconds>`          | Wait                                                     |
| `status`                   | Uptime and test-allocation count                         |
| `meminfo`                  | Heap statistics                                          |
| `alloc`                    | Allocate one physical frame and print its address (test) |
| `malloc <bytes>`           | Allocate heap memory in a test slot                      |
| `free` / `freeidx <n>`     | Free the last / the n-th test allocation                 |
| `listptr`                  | List test allocations                                    |
| `mount [ata\|ram]`         | Show which disk the files are on, or switch to the ATA disk / RAM disk |
| `diskinfo`                 | Boot sector of the current disk                          |
| `readsector <lba>`         | Dump a raw sector                                        |
| `fat32info`                | FAT32 volume parameters                                  |
| `crash <kind>`             | Trigger a CPU exception on purpose: `div0 ud gp pf null stack int3 irq panic` |
| `reboot`                   | Reset the machine                                        |

### Network
| Command                    | Description                                              |
| -------------------------- | -------------------------------------------------------- |
| `ifconfig`                 | MAC address, IP settings, RX/TX packet counters          |
| `wifi`                     | The Wi-Fi adapter in this machine: chip, Wi-Fi generation, PCI IDs, and what a driver for it needs |
| `wifi scan`                | (root) Numbered list of the networks in range: name, channel, signal, security |
| `wifi connect [n\|name]`   | (root) Join network number `n` of the last scan, or by name; no argument: scan and ask for a number. Asks for the password unless the network is saved, then gets an address over DHCP |
| `wifi status`              | The joined network, access point, channel, signal, address, packet counts |
| `wifi disconnect`          | (root) Leave the network and stop reconnecting |
| `wifi forget`              | (root) Delete the saved network (`/WIFI.CFG`) |
| `wifi start`               | (root) Start the card step by step, printing each stage (for diagnosing) |
| `nettest`                  | Send an ARP request to the gateway and wait for the reply (proves TX + RX work) |
| `ping <ip> [count]`        | ICMP echo to a dotted IPv4 address (default 4, max 1000), one per second; any key stops it. Prints per-reply RTT and a loss/min/avg/max summary. |
| `download <url> [file]`    | Download an `http://` or `https://` URL and save it to FAT32 (HTTPS uses BearSSL TLS 1.2). |
| `netdebug <on\|off>`       | Print one line per received frame                        |

### Appearance
`themes` lists them, `theme <name>` switches: `default`, `dracula`, `nord`,
`monokai`, `gruvbox`, `solarized`, `matrix`, `cyberpunk`. `demo` shows the
message styles.

`resolution` shows the screen mode and text grid; `resolution 1280x800` changes it on
QEMU's standard VGA and VirtualBox (the console gets more or fewer rows and columns, and
keeps its scrollback). Under VirtualBox the screen follows the VM window by itself.
`font 1`..`font 4` sets the text size (`font auto`: x1, larger only past 160 columns).

> Files live on the ATA disk when there is one (QEMU's `disk.img`), otherwise on the
> **RAM disk** GRUB loads from the boot medium. RAM disk changes are lost at reboot.
>
> Filenames are FAT32 short names (8.3). The disk stores them upper-case, so
> `test.txt` is listed as `TEST.TXT`.

## Mouse

A PS/2 mouse (QEMU, VirtualBox, most PCs with a legacy mouse or touchpad) moves a
pointer, shown as an inverted cell, over the console:

| Action                     | What happens                                             |
| -------------------------- | -------------------------------------------------------- |
| Wheel                      | Scrolls through earlier output (like Shift+Up/Down); typing or new output jumps back |
| Drag with the left button  | Selects text, highlighted; releasing copies it            |
| Right or middle click      | Pastes the copied text as if it were typed               |

A program that reads the mouse (`getmouse`, e.g. `paint`) takes it over while it is
in the foreground. Under VirtualBox the pointer follows the host's pointer directly
(mouse integration), so the VM does not capture the mouse. The pointer needs the
framebuffer console; in VGA text mode only the wheel and paste work.

## Editor

`edit <file>` opens a line-based editor (up to 8000 lines; the view scrolls with the cursor and
long lines are clipped on screen, not in the file). Files with CRLF (`\r\n`) line endings are
saved back with CRLF. Files the editor cannot save without losing data (NUL bytes, too many lines) open
read-only, and Ctrl+S then refuses.

| Key         | Action                  |
| ----------- | ----------------------- |
| Ctrl+S      | Save                    |
| Ctrl+Q      | Quit                    |
| Ctrl+N      | New line                |
| Ctrl+D      | Delete line             |
| Ctrl+E      | Append to the current line (lines up to 255 chars) |
| Up / Down   | Move between lines      |

## Scripts

A script is a text file of shell commands (max 4096 bytes, lines up to 256 chars).

```sh
# comment
NAME=world
echo Hello $NAME
sleep 1
ls
exit
```

* `VAR=value` sets a variable (up to 16), `$VAR` expands it.
* `echo`, `sleep <seconds>` and `exit` are handled by the script runner; every
  other line is passed to the shell.
* Note: any line containing `=` is treated as an assignment, even
  `echo a=b` after variable expansion.

## The C subset

`compile hello.c` tokenizes the file and runs a very small subset on a stack VM.
Today that means a `main` function whose body consists of `printf("literal");`
and `return <number>;`:

```c
int main() {
    printf("Hello from my own OS!\n");
    return 0;
}
```

Variables, arithmetic, `if`/`while`/`for` and function calls are tokenized but
not yet compiled.
