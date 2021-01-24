project(
	'master_ct',
	'c',
	version: '0.0.0',
	license: 'LGPL3.0-or-later',
	default_options: [
		'warning_level=3',
		'werror=true',
	],
)

cc = meson.get_compiler('c')

global_arguments = cc.get_supported_arguments([
		'-D_GNU_SOURCE',
		'-Wno-gnu-zero-variadic-macro-arguments',
	])

add_project_arguments(global_arguments, language: 'c')

jansson = dependency('jansson')
lustre = cc.find_library('lustreapi')

subdir('tests')

executable(
	'master_ct',
	sources: [
		'master_ct.c',
	],
	dependencies: [lustre, jansson],
	install: true,
)

executable(
	'client',
	sources: [
		'client.c',
	],
	# needs lustre for logs
	dependencies: [lustre, jansson],
	install: true,
)
