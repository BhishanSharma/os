# GNU nano 8.2: a small, friendly text editor for the Terminal (Ctrl+O saves,
# Ctrl+X quits; the keys are listed at the bottom). Needs ncurses.
URL=https://www.nano-editor.org/dist/v8/nano-8.2.tar.gz
SHA256=51e57dcecb8a6bb64a1bf5a916615e247d9bcd8e951ac99d01139c897efba20e
NEEDS=ncurses

build() {
    nc=$PORTS_OUT/ncurses/root/usr
    cp /tc/config.sub config.sub
    ./configure --host=$HOST --prefix=/usr --disable-nls --disable-libmagic --disable-speller \
        --enable-utf8 CC="$CC" CPPFLAGS="-I$nc/include -I$nc/include/ncursesw" \
        LDFLAGS="-L$nc/lib" NCURSESW_CFLAGS="-I$nc/include/ncursesw" NCURSESW_LIBS="-lncursesw" \
        LIBS="-lncursesw" CFLAGS="-O2 -std=gnu17" &&
    make -j3 && cp src/nano "$DEST/bin/"
}
