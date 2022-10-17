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

hiredis = dependency('hiredis')
jansson = dependency('jansson')
libdl = cc.find_library('dl')
lustre = cc.find_library('lustreapi')
systemd = dependency('systemd', required: false)
systemd_system_unit_dir = systemd.get_pkgconfig_variable('systemdsystemunitdir')
urcu = dependency('liburcu')

use_phobos = get_option('phobos')

phobos = cc.find_library('phobos_store', required: use_phobos)
glib = dependency('glib-2.0', required: use_phobos)

conf_data = configuration_data()
conf_data.set10('HAVE_PHOBOS', phobos.found() and glib.found())
configure_file(output: 'config.h', configuration: conf_data)

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
		'common/lustre.c',
		'common/protocol.c',
		'common/protocol_lustre.c',
	),
	# need lustre for logs
	dependencies: [lustre, jansson],
)

client_common = static_library(
	'coordinatool_client_common',
	files(
		'client_common/client.c',
		'client_common/protocol.c',
		'client_common/tcp.c',
	),
	include_directories: include_directories('common'),
	link_with: [common],
)

subdir('tests')


executable(
	'lhsmd_coordinatool',
	sources: files(
		'copytool/config.c',
		'copytool/coordinatool.c',
		'copytool/lhsm.c',
		'copytool/protocol.c',
		'copytool/queue.c',
		'copytool/redis.c',
		'copytool/scheduler.c',
		'copytool/tcp.c',
		'copytool/timer.c',
	) + [ version_h ],
	dependencies: [hiredis, urcu, glib, phobos],
	include_directories: include_directories(['common', '.']),
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
		'preload/preload.c',
		'preload/protocol.c',
		'preload/tree.c',
	),
	include_directories: include_directories(['common', 'client_common']),
	link_with: [client_common],
	dependencies: [libdl],
	install: true,
)
