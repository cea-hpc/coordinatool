project(
	'coordinatool',
	'c',
	version: '0.4',
	license: 'LGPL3.0-or-later',
	default_options: [
		'warning_level=3',
		'werror=true',
	],
)

cc = meson.get_compiler('c')

global_arguments = cc.get_supported_arguments([
		'-D_GNU_SOURCE',
		'-Wno-pedantic',
		'-Wno-unknown-pragmas',
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
have_phobos = phobos.found() and glib.found()
conf_data.set10('HAVE_PHOBOS', have_phobos)

if have_phobos
	have_phobos_init = cc.has_function(
		'phobos_init',
		prefix: '#define _GNU_SOURCE\n#include <phobos_store.h>',
		dependencies: [ glib, phobos ],
	)
	conf_data.set10('HAVE_PHOBOS_INIT', have_phobos_init)

	have_phobos_1_95 = cc.compiles(
		'''
		#include <phobos_store.h>
		void func() {
			char *hostname;
			int nb_new_lock;
			phobos_locate("obj", NULL, 0, NULL, &hostname, &nb_new_lock);
		}
		''',
		name : 'phobos_locate 1.95 check',
		dependencies: [glib, phobos])

	have_phobos_3 = cc.compiles(
		'''
		#include <phobos_store.h>
		void func() {
			char *hostname;
			int nb_new_lock;
			phobos_locate("obj", NULL, 0, NULL, NULL, &hostname, &nb_new_lock);
		}
		''',
		name : 'phobos_locate 3 check',
		dependencies: [glib, phobos])

	if have_phobos_3
		conf_data.set('PHOBOS_VERSION', 300)
	elif have_phobos_1_95
		conf_data.set('PHOBOS_VERSION', 195)
	endif
endif

have_gettid = cc.has_function(
	'gettid',
	prefix: '#define _GNU_SOURCE\n#include <unistd.h>')
conf_data.set10('HAVE_GETTID', have_gettid)

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

lhsmd_coordinatool_sources = [
	'copytool/batch.c',
	'copytool/config.c',
	'copytool/coordinatool.c',
	'copytool/lhsm.c',
	'copytool/protocol.c',
	'copytool/queue.c',
	'copytool/redis.c',
	'copytool/reporting.c',
	'copytool/scheduler.c',
	'copytool/tcp.c',
	'copytool/timer.c',
]

if have_phobos
	lhsmd_coordinatool_sources += 'copytool/phobos.c'
endif

executable(
	'lhsmd_coordinatool',
	sources: files(lhsmd_coordinatool_sources) + [ version_h ],
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
