/* GCC target description for Terminal OS (x86_64-pc-terminalos): ELF, the
 * System V ABI, mlibc as the C library. Programs are linked statically (there
 * is no dynamic loader yet). Copied to gcc/config/terminalos.h by patch-gcc.sh. */

#undef TARGET_TERMINALOS
#define TARGET_TERMINALOS 1

#undef LIB_SPEC
#define LIB_SPEC "%{pthread:} -lc"

/* crt1.o, crti.o, crtn.o come from mlibc; crtbegin.o, crtend.o from libgcc. */
#undef STARTFILE_SPEC
#define STARTFILE_SPEC "%{!shared:crt1.o%s} crti.o%s crtbegin.o%s"

#undef ENDFILE_SPEC
#define ENDFILE_SPEC "crtend.o%s crtn.o%s"

#undef LINK_SPEC
#define LINK_SPEC "%{!shared:-static} -z max-page-size=4096 --eh-frame-hdr"

/* C++ sees the GNU extensions of the C library, as on Linux (libstdc++ is
 * configured as for glibc, and mlibc's own C++ needs them). */
#undef CPLUSPLUS_CPP_SPEC
#define CPLUSPLUS_CPP_SPEC "-D_GNU_SOURCE %(cpp)"

#undef TARGET_OS_CPP_BUILTINS
#define TARGET_OS_CPP_BUILTINS()          \
  do {                                    \
    builtin_define ("__terminalos__");    \
    builtin_define ("__unix__");          \
    builtin_assert ("system=terminalos"); \
    builtin_assert ("system=unix");       \
    builtin_assert ("system=posix");      \
  } while (0)
