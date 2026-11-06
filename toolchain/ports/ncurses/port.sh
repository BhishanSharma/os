# ncurses 6.5: the terminal screen library (for nano and other full-screen
# programs). Static, wide characters; the Terminal window's descriptions
# (xterm, vt100) are built in, so no terminfo files are needed on the OS.
# A library only: nothing of it goes on the RAM disk.
URL=https://ftp.gnu.org/gnu/ncurses/ncurses-6.5.tar.gz
SHA256=136d91bc269a9a5785e5f9e980bc76ab57428f604ce3e5a5a90cebc767971cc6

build() {
    cp /tc/config.sub config.sub
    ./configure --host=$HOST --prefix=/usr --with-build-cc=gcc --without-shared --with-normal \
        --without-debug --without-ada --without-cxx-binding --without-manpages --without-progs \
        --without-tests --enable-widec --disable-database --with-fallbacks=xterm,vt100 \
        --disable-stripping --without-pkg-config CC="$CC" AR="$AR" RANLIB="$RANLIB" CPPFLAGS=-D_GNU_SOURCE CFLAGS="-O2 -std=gnu17" &&
    make -j3 libs && make install.libs install.includes DESTDIR="$DEST/root"
}
