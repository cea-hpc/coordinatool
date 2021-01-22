project(
	'master_ct',
	'c',
	version: '0.0.0',
	license: 'LGPL3.0-or-later',
	default_options: [
		'warning_level=3',
#		'werror=true',
	],
)

add_project_arguments(['-D_GNU_SOURCE',], language: 'c')

glib2 = dependency('glib-2.0')
cc = meson.get_compiler('c')
lustre = cc.find_library('lustreapi')
resolv = cc.find_library('resolv')

executable(
	'master_ct',
	sources: [
		'lhsmtool_cmd.c',
		],
	dependencies: [glib2, lustre, resolv],
	install: true,
)
