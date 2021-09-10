project(
	'coordinatool',
	'c',
	version: '0.1',
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
phobos = cc.find_library('phobos_store')
urcu = dependency('liburcu')
libdl = cc.find_library('dl')
glib = dependency('glib-2.0')
systemd = dependency('systemd', required: false)
systemd_system_unit_dir = systemd.get_pkgconfig_variable('systemdsystemunitdir')

if systemd.found()
	configure_file(
		copy: true,
		input: 'systemd/coordinatool@.service',
		install: true,
		install_dir: systemd_system_unit_dir,
		output: 'coordinatool@.service',
	)
	configure_file(
		copy: true,
		input: 'systemd/coordinatool.sysconfig',
		install: true,
		install_dir: '/etc/sysconfig',
		output: 'coordinatool',
	)
endif

version_h = vcs_tag(
	command: [ 'git', 'describe', '--always', ],
	input: 'common/version.h.in',
	output: 'version.h',
	replace_string: '@VERSION@'
)

common = static_library(
	'coordinatool_common',
	files(
		'common/config.c',
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
	'lhsmd_coordinatool',
	sources: files(
		'copytool/config.c',
		'copytool/queue.c',
		'copytool/tcp.c',
		'copytool/lhsm.c',
		'copytool/protocol.c',
		'copytool/scheduler.c',
		'copytool/coordinatool.c',
	) + [ version_h ],
	dependencies: [urcu, glib, phobos],
	include_directories: include_directories('common'),
	link_with: [common],
	install: true,
)

executable(
	'coordinatool-client',
	sources: files(
		'client/protocol.c',
		'client/client.c',
	) + [ version_h ],
	include_directories: include_directories(['common', 'client_common']),
	link_with: [client_common],
	install: true,
)

library(
	'coordinatool_client',
	sources: files(
		'preload/protocol.c',
		'preload/preload.c',
		'preload/state.c',
	),
	include_directories: include_directories(['common', 'client_common']),
	link_with: [client_common],
	dependencies: [libdl],
	install: true,
)
