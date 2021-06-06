project(
	'coordinatool',
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
	'coordinatool_common',
	files(
		'common/protocol.c',
		'common/protocol_lustre.c',
		'common/lustre.c',
	),
	# need lustre for logs
	dependencies: [lustre, jansson],
)

client_common = static_library(
	'coordinatool_client_common',
	files(
		'client_common/client.c',
		'client_common/tcp.c',
		'client_common/protocol.c',
	),
	include_directories: include_directories('common'),
	link_with: [common],
)

subdir('tests')


executable(
	'coordinatool',
	sources: files(
		'copytool/queue.c',
		'copytool/tcp.c',
		'copytool/lhsm.c',
		'copytool/protocol.c',
		'copytool/coordinatool.c',
	),
	dependencies: [urcu],
	include_directories: include_directories('common'),
	link_with: [common],
	install: true,
)

executable(
	'client',
	sources: files(
		'client/protocol.c',
		'client/client.c',
	),
	include_directories: include_directories(['common', 'client_common']),
	link_with: [client_common],
	install: true,
)

library(
	'coordinatool_client',
	sources: files(
		'preload/protocol.c',
		'preload/preload.c',
	),
	include_directories: include_directories(['common', 'client_common']),
	link_with: [client_common],
	install: true,
)
