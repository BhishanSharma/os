# Vim 9.1: the editor, in the Terminal window (no GUI). Needs ncurses.
# Without the runtime folder (30 MB: help, syntax files): its settings come
# from /SYS/vimrc.def (in place of defaults.vim), and :help has no pages.
URL=https://github.com/vim/vim/archive/refs/tags/v9.1.0000.tar.gz
SHA256=8d2a74e358be9cf260174e15e2e29dba4773f2670718b5bf728db6f54b90e25d
NEEDS=ncurses

build() {
    nc=$PORTS_OUT/ncurses/root/usr
    here=$(pwd)
    cd src &&
    vim_cv_toupper_broken=no vim_cv_terminfo=yes vim_cv_tgetent=zero vim_cv_getcwd_broken=no \
    vim_cv_stat_ignores_slash=no vim_cv_memmove_handles_overlap=yes vim_cv_bcopy_handles_overlap=yes \
    vim_cv_memcpy_handles_overlap=no ac_cv_small_wchar_t=no vim_cv_tty_group=world vim_cv_tty_mode=0620 \
    vim_cv_timer_create=no vim_cv_timer_create_with_lrt=no \
    ./configure --host=$HOST --prefix=/usr --with-features=normal --enable-gui=no --without-x \
        --disable-nls --disable-netbeans --disable-channel --disable-gpm --disable-sysmouse \
        --disable-xsmp --disable-selinux --disable-libsodium --disable-canberra --disable-acl \
        --disable-sound --with-tlib=ncursesw --enable-multibyte \
        CC="$CC" CPPFLAGS="-I$nc/include -I$nc/include/ncursesw -D_GNU_SOURCE" LDFLAGS="-L$nc/lib" \
        CFLAGS="-O2 -std=gnu17 -DVIM_DEFAULTS_FILE='\"/SYS/vimrc.def\"'" &&
    make -j3 && cp vim "$DEST/bin/" &&
    cat > "$DEST/bin/vimrc.def" <<'EOF'
" Terminal OS: Vim's settings when you have no ~/.vimrc of your own.
set nocompatible
set backspace=indent,eol,start
set encoding=utf-8
set ruler showcmd showmode
set incsearch hlsearch
set wildmenu
set scrolloff=3
set nrformats-=octal
set history=200
EOF
}
