# SQLite 3.47: the database and its shell (`sqlite3`). One program, no
# threads, no file locking (one user at a time: the "unix-none" VFS), no WAL.
URL=https://www.sqlite.org/2024/sqlite-autoconf-3470200.tar.gz
SHA256=f1b2ee412c28d7472bc95ba996368d6f0cdcf00362affdadb27ed286c179540b

build() {
    $CC -O2 -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_WAL -DSQLITE_OMIT_LOAD_EXTENSION \
        -DSQLITE_DEFAULT_VFS='"unix-none"' -DHAVE_USLEEP=1 \
        shell.c sqlite3.c -o "$DEST/bin/sqlite3"
}
