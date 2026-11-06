#!/bin/sh
# Build the ports: programs from other people's sources, for Terminal OS.
# Runs in the myos-ladybird image (build.ps1 does it before the OS build),
# with the repository as the current folder:
#     sh toolchain/ports/build.sh [NAME...]
#
# Each toolchain/ports/NAME/port.sh sets URL and SHA256 (the source, checked
# before use) and defines build(), run in the unpacked source with CC, CXX,
# AR, RANLIB and HOST set for the OS (dynamically linked: /SYS/LIB/libc.so).
# What build() puts in $DEST/bin goes on the RAM disk as programs. A port is
# built again only when its port.sh changed; one that cannot be downloaded
# (offline) is left out with a warning, the OS still builds. NEEDS names the
# ports to build first (a library: $PORTS_OUT/NAME has what it installed).
set -eu
here=toolchain/ports
out=build/ports/out
cache=build/ports/cache
mkdir -p "$out" "$cache"
export CC="x86_64-pc-terminalos-gcc -specs=/opt/terminalos/dynamic.specs"
export CXX="x86_64-pc-terminalos-g++ -specs=/opt/terminalos/dynamic.specs"
export AR=x86_64-pc-terminalos-ar RANLIB=x86_64-pc-terminalos-ranlib HOST=x86_64-pc-terminalos
root=$(pwd)
export PORTS_OUT=$root/$out

names=${*:-$(cd "$here" && for d in */; do echo "${d%/}"; done)}
for name in $names; do
    recipe=$here/$name/port.sh
    [ -f "$recipe" ] || { echo "ports: no such port: $name" >&2; exit 1; }
    needs=$( . "./$recipe"; echo "${NEEDS:-}" )
    for need in $needs; do sh "$0" "$need"; done
    stamp=$out/$name/.built
    if [ -f "$stamp" ] && [ "$stamp" -nt "$recipe" ]; then continue; fi
    (
        . "./$recipe"
        file=$cache/${URL##*/}
        if [ ! -f "$file" ]; then
            curl -fsSL -o "$file.part" "$URL" && mv "$file.part" "$file" ||
                { rm -f "$file.part"; echo "ports: $name: cannot download $URL (left out)" >&2; exit 0; }
        fi
        echo "$SHA256  $file" | sha256sum -c --quiet - ||
            { echo "ports: $name: $file is not the expected source (checksum)" >&2; exit 1; }
        work=$(mktemp -d)
        tar xf "$file" -C "$work"
        DEST=$root/$out/$name
        rm -rf "$DEST"
        mkdir -p "$DEST/bin"
        log=$root/build/ports/$name.log
        echo "ports: building $name"
        (cd "$work"/*/ && build) > "$log" 2>&1 || { tail -n 40 "$log"; echo "ports: $name failed ($log)" >&2; exit 1; }
        for f in "$DEST"/bin/*; do
            if [ -f "$f" ]; then x86_64-pc-terminalos-strip "$f"; fi
        done
        rm -rf "$work"
        touch "$DEST/.built"
    )
done
