project('microcnf', 'c',
	version : '0.1.0',
	default_options : ['warning_level=3', 'c_std=gnu2x']
)

microcnf_bin = executable('microcnf', 'microcnf.c',
	dependencies: [
		dependency('glib-2.0'),
		dependency('gio-2.0'),
		dependency('libsolv')
	],
	c_args: ['-DG_LOG_DOMAIN="microcnf"'],
	install : true
)
