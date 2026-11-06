# GNU bash 5.2: the shell, with its own readline (line editing, history:
# the Terminal window gives it a pseudo-terminal in raw mode). Job
# control (Ctrl+Z, fg, bg, jobs: the kernel's process groups), no bash malloc.
URL=https://ftp.gnu.org/gnu/bash/bash-5.2.37.tar.gz
SHA256=9599b22ecd1d5787ad7d3b7bf0c59f312b3396d1e281175dd1f8a4014da621ff

build() {
    cp /tc/config.sub support/config.sub
    # PATH when none is set: the system folder (the RAM disk's programs: /SYS
    # on an installed system, else /), then the current folder.
    sed -i 's|"/usr/local/bin:/usr/local/sbin:/usr/bin:/usr/sbin:/bin:/sbin:."|"/SYS:/:/usr/bin:/bin:."|; s|"/bin:/usr/bin:/sbin:/usr/sbin:/etc:/usr/etc"|"/SYS:/"|' config-top.h
    grep -q '"/SYS:/:/usr/bin:/bin:."' config-top.h
    # What configure cannot find out when it cannot run the OS's programs.
    export bash_cv_job_control_missing=present bash_cv_sys_named_pipes=missing \
        bash_cv_getcwd_malloc=yes bash_cv_func_sigsetjmp=present bash_cv_wexitstatus_offset=8 \
        bash_cv_dev_fd=absent bash_cv_dev_stdin=absent bash_cv_pgrp_pipe=no \
        bash_cv_func_strcoll_broken=no bash_cv_printf_a_format=yes bash_cv_ulimit_maxfds=no \
        bash_cv_under_sys_siglist=no bash_cv_sys_siglist=no bash_cv_unusable_rtsigs=no \
        bash_cv_func_ctype_nonascii=no bash_cv_must_reinstall_sighandlers=no \
        bash_cv_func_snprintf=yes bash_cv_func_vsnprintf=yes bash_cv_termcap_lib=gnutermcap \
        ac_cv_func_mmap_fixed_mapped=yes ac_cv_rl_version=8.2
    ./configure --host=$HOST --prefix=/usr --without-bash-malloc \
        --disable-nls --without-curses --enable-readline --enable-history \
        CC="$CC" AR="$AR" RANLIB="$RANLIB" CFLAGS="-O2 -std=gnu17 -fcommon -Wno-implicit-function-declaration"
    make -j3 && cp bash "$DEST/bin/"
}
