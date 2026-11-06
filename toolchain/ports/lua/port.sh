# Lua 5.4: the language and its interpreter (`lua`, `luac`). Built as on
# Linux, so `require` loads C modules (.so) with dlopen.
URL=https://www.lua.org/ftp/lua-5.4.7.tar.gz
SHA256=9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30

build() {
    make -C src lua luac CC="$CC -std=gnu99" AR="$AR rcu" RANLIB="$RANLIB" \
        SYSCFLAGS="-DLUA_USE_LINUX" SYSLIBS="-Wl,-E -ldl" MYCFLAGS=-O2
    cp src/lua src/luac "$DEST/bin/"
}
