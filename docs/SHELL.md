# Using Terminal OS

The prompt shows the current directory on one line and `>` on the next.
Up/Down arrows browse command history; Shift+Up/Down scroll the screen.

## Commands

### Files
| Command                    | Description                                              |
| -------------------------- | -------------------------------------------------------- |
| `ls`                       | List the current directory                               |
| `cat <file>`               | Print a file (max 4 KB)                                  |
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
| `sh <script>`              | Run a script file                                        |
| `compile <file>[.c]`       | Compile and run a C file (tiny subset, see below)        |

### System and debugging
| Command                    | Description                                              |
| -------------------------- | -------------------------------------------------------- |
| `help`                     | List every command                                       |
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
| `diskinfo`                 | ATA disk information                                     |
| `readsector <lba>`         | Dump a raw sector                                        |
| `fat32info`                | FAT32 volume parameters                                  |
| `crash <kind>`             | Trigger a CPU exception on purpose: `div0 ud gp pf null stack int3 irq panic` |
| `reboot`                   | Reset the machine                                        |

### Network
| Command                    | Description                                              |
| -------------------------- | -------------------------------------------------------- |
| `ifconfig`                 | MAC address, IP settings, RX/TX packet counters          |
| `nettest`                  | Send an ARP request to the gateway and wait for the reply (proves TX + RX work) |
| `ping <ip> [count]`        | ICMP echo to a dotted IPv4 address (default 4, max 1000), one per second; any key stops it. Prints per-reply RTT and a loss/min/avg/max summary. |
| `download <url> [file]`    | Download an `http://` or `https://` URL and save it to FAT32 (HTTPS uses BearSSL TLS 1.2). |
| `netdebug <on\|off>`       | Print one line per received frame                        |

### Appearance
`themes` lists them, `theme <name>` switches: `default`, `dracula`, `nord`,
`monokai`, `gruvbox`, `solarized`, `matrix`, `cyberpunk`. `demo` shows the
message styles.

> Filenames are FAT32 short names (8.3). The disk stores them upper-case, so
> `test.txt` is listed as `TEST.TXT`.

## Editor

`edit <file>` opens a line-based editor.

| Key         | Action                  |
| ----------- | ----------------------- |
| Ctrl+S      | Save                    |
| Ctrl+Q      | Quit                    |
| Ctrl+N      | New line                |
| Ctrl+D      | Delete line             |
| Ctrl+E      | Edit the current line   |
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
