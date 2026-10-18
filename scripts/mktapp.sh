#!/bin/sh
# mktapp.sh SRC USER_BUILD "CC CFLAGS" LD OUT - an app folder as a package for
# tapp (docs/PACKAGES.md); `make tapp SRC=folder` runs it.
#
# SRC has app.cfg (with id = the folder's name), its .c (and .h) files, and
# perhaps icon.png and other files the app reads (flat, no folders). The .c
# files become the program ID, linked like the built-in apps (crt0, libc.a:
# user/include is the SDK). OUT/ID.tapp is a ustar archive of app.cfg, ID,
# icon.png and the other files.
set -e
src=${1%/}
ub=$2
cc=$3
ld=$4
out=$5
id=$(basename "$src")

[ -f "$src/app.cfg" ] || { echo "mktapp: $src has no app.cfg"; exit 1; }
cfg_id=$(sed -n 's/^[[:space:]]*id[[:space:]]*=[[:space:]]*\([a-z0-9]*\).*/\1/p' "$src/app.cfg" | head -1)
[ "$cfg_id" = "$id" ] || { echo "mktapp: $src/app.cfg must say id = $id (the folder's name)"; exit 1; }
echo "$id" | grep -Eq '^[a-z][a-z0-9]{0,7}$' || { echo "mktapp: the id must be 1-8 lower-case letters or digits, a letter first"; exit 1; }
ls "$src"/*.c > /dev/null 2>&1 || { echo "mktapp: $src has no .c files"; exit 1; }

work=$out/$id
rm -rf "$work"
mkdir -p "$work/obj" "$work/files"
for c in "$src"/*.c; do
    $cc -I "$src" "$c" -o "$work/obj/$(basename "$c" .c).o"
done
$ld -T user/user.ld -z max-page-size=4096 -z noexecstack -o "$work/files/$id" "$ub/crt0.o" "$work"/obj/*.o "$ub/libc.a"

for f in "$src"/*; do                              # everything else but the sources
    case "$f" in *.c|*.h) continue ;; esac
    [ -f "$f" ] && cp "$f" "$work/files/"
done
rm -f "$out/$id.tapp"
(cd "$work/files" && tar --format=ustar --owner=0 --group=0 --numeric-owner -cf "../../$id.tapp" *)
rm -rf "$work"
echo "$out/$id.tapp ($(wc -c < "$out/$id.tapp") bytes): install it with tapp (open it in Files)"
