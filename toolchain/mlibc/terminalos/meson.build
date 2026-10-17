# mlibc's Terminal OS port (copied to mlibc/sysdeps/terminalos by the
# toolchain's Dockerfile; include/abi-bits is made there from Linux's ABI).
sysdep_supported_options = {
	'posix': true,
	'linux': false,
	'glibc': true,
	'bsd': true,
}

if not headers_only
	rtld_sources += files(
		'generic/sysdeps.cpp',
		'generic/net.cpp',
	)

	libc_sources += files(
		'generic/entry.cpp',
		'generic/sysdeps.cpp',
		'generic/net.cpp',
		'threading/thread-entry.S',
	)
endif

if not no_headers
	install_subdir('include/abi-bits', install_dir: get_option('includedir'), follow_symlinks: true)
	if fs.is_dir('include/terminalos')       # (the headers-only step has just this file)
		install_subdir('include/terminalos', install_dir: get_option('includedir'))
	endif
endif

if not headers_only
	foreach crt : ['crt1', 'crti', 'crtn']
		custom_target(crt,
			build_by_default: true,
			command: c_compiler.cmd_array() + ['-c', '-o', '@OUTPUT@', '@INPUT@'],
			input: 'crt-x86_64/' + crt + '.S',
			output: crt + '.o',
			install: true,
			install_dir: get_option('libdir')
		)
	endforeach
endif
