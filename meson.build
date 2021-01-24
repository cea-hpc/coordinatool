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

add_project_arguments([
		'-D_GNU_SOURCE',
		'-Wno-gnu-zero-variadic-macro-arguments',
	], language: 'c')

jansson = dependency('jansson')
cc = meson.get_compiler('c')
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
