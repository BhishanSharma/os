#!/bin/sh
# Put the Terminal OS port into an mlibc source tree.
# Usage: setup-port.sh MLIBC_DIR
set -eu
here=$(cd "$(dirname "$0")" && pwd)
cd "$1"

rm -rf sysdeps/terminalos
cp -r "$here/terminalos" sysdeps/terminalos

# The ABI (errno numbers, struct stat, O_* flags...) is Linux's, like most
# mlibc ports: link the same headers Astral uses.
mkdir -p sysdeps/terminalos/include/abi-bits
for f in sysdeps/astral/include/abi-bits/*.h; do
    ln -sf "../../../../abis/linux/$(basename "$f")" "sysdeps/terminalos/include/abi-bits/$(basename "$f")"
done

# meson.build: build sysdeps/terminalos when the cross file says system = 'terminalos'.
if ! grep -q "'terminalos'" meson.build; then
    anchor="elif host_machine.system() == 'astral'"
    grep -qF "$anchor" meson.build || { echo "setup-port: the sysdeps list moved in meson.build" >&2; exit 1; }
    ANCHOR="$anchor" awk 'done == 0 && index($0, ENVIRON["ANCHOR"]) == 1 {
        print "elif host_machine.system() == '"'"'terminalos'"'"'"
        print "\trtld_include_dirs += include_directories('"'"'sysdeps/terminalos/include'"'"')"
        print "\tlibc_include_dirs += include_directories('"'"'sysdeps/terminalos/include'"'"')"
        print "\tsubdir('"'"'sysdeps/terminalos'"'"')"
        done = 1
    } { print }' meson.build > meson.build.new
    mv meson.build.new meson.build
fi
echo "setup-port: mlibc has sysdeps/terminalos"
