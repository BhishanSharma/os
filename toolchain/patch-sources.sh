#!/bin/sh
# Teach binutils and GCC the target x86_64-pc-terminalos (Terminal OS with mlibc).
# Usage: patch-sources.sh binutils DIR | gcc DIR
# Each edit is checked: a newer release that moved the text fails loudly.
set -eu
what=$1
here=$(cd "$(dirname "$0")" && pwd)
cd "$2"

# insert_before FILE PATTERN TEXT: TEXT on its own lines before the first line matching PATTERN
insert_before() {
    grep -qF -- "$2" "$1" || { echo "patch-sources: '$2' not found in $1" >&2; exit 1; }
    PAT="$2" TEXT="$3" awk 'done == 0 && index($0, ENVIRON["PAT"]) { print ENVIRON["TEXT"]; done = 1 } { print }' "$1" > "$1.new"
    mv "$1.new" "$1"
}

# config.sub: an operating system named terminalos
add_os() {
    if grep -q '| mlibc\* \\' "$1"; then
        insert_before "$1" '| mlibc* \' '	| terminalos* \'
    else
        insert_before "$1" '| chorusrdb* | cegcc* | glidix* | serenity* \' '	     | terminalos* \'
    fi
}

case $what in
binutils)
    add_os config.sub
    insert_before bfd/config.bfd '  x86_64-*-redox*)' '  x86_64-*-terminalos*)
    targ_defvec=x86_64_elf64_vec
    targ_selvecs=i386_elf32_vec
    want64=true
    ;;'
    insert_before gas/configure.tgt '  i386-*-redox*)' '  i386-*-terminalos*)			fmt=elf ;;'
    insert_before ld/configure.tgt 'x86_64-*-redox*)' 'x86_64-*-terminalos*)	targ_emul=elf_x86_64
			;;'
    ;;
gcc)
    add_os config.sub
    cp "$here/terminalos.h" gcc/config/terminalos.h
    insert_before gcc/config.gcc '*-*-fuchsia*)' '*-*-terminalos*)
  extra_options="$extra_options gnu-user.opt"
  gas=yes
  gnu_ld=yes
  default_use_cxa_atexit=yes
  use_gcc_stdint=provide
  native_system_header_dir=/usr/include
  case ${enable_threads} in
    "" | yes | posix) thread_file='"'"'posix'"'"' ;;
  esac
  ;;'
    insert_before gcc/config.gcc 'x86_64-*-fuchsia*)' 'x86_64-*-terminalos*)
	tm_file="${tm_file} i386/unix.h i386/att.h elfos.h glibc-stdint.h i386/i386elf.h i386/x86-64.h terminalos.h"
	;;'
    insert_before libgcc/config.host 'x86_64-*-fuchsia*)' 'x86_64-*-terminalos*)
	extra_parts="$extra_parts crtbegin.o crtbeginS.o crtend.o crtendS.o"
	tmake_file="$tmake_file i386/t-crtstuff t-crtstuff-pic t-libgcc-pic"
	;;'
    # No fixincludes: mlibc's headers need no fixing.
    insert_before fixincludes/mkfixinc.sh '    *-musl* )' '    *-terminalos* | \'
    # libstdc++: configure checks the C library as for glibc (it is POSIX and
    # glibc-compatible enough). Both the m4 source and the generated configure.
    for f in libstdc++-v3/crossconfig.m4 libstdc++-v3/configure; do
        grep -q -- '  \*-linux\* | \*-uclinux\* | \*-gnu\* | \*-kfreebsd\*-gnu | \*-cygwin\* | \*-solaris\*)' "$f" ||
            { echo "patch-sources: the libstdc++ host case moved in $f" >&2; exit 1; }
        sed -i 's/^  \*-linux\* | \*-uclinux\* | \*-gnu\* | \*-kfreebsd\*-gnu | \*-cygwin\* | \*-solaris\*)/  *-linux* | *-uclinux* | *-gnu* | *-kfreebsd*-gnu | *-cygwin* | *-solaris* | *-terminalos*)/' "$f"
    done
    ;;
*)
    echo "usage: patch-sources.sh binutils|gcc DIR" >&2
    exit 1
    ;;
esac
echo "patch-sources: $what patched for x86_64-pc-terminalos"
