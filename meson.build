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
urcu = dependency('liburcu')

common = static_library(
	'master_ct_common',
	files(
		'common/protocol_lustre.c',
		'common/lustre.c',
	),
	# need lustre for logs
	dependencies: [lustre, jansson],
)

subdir('tests')


executable(
	'master_ct',
	sources: files(
                'master/queue.c',
		'master/tcp.c',
		'master/lhsm.c',
		'master/master_ct.c',
	),
        dependencies: [urcu],
	include_directories: include_directories('common'),
	link_with: [common],
	install: true,
)

executable(
	'client',
	sources: files(
		'client/client.c',
	),
	include_directories: include_directories('common'),
	link_with: [common],
	install: true,
)
