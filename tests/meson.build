glib2 = dependency('glib-2.0')
resolv = cc.find_library('resolv')

executable(
	'lhsmtool_cmd',
	sources: [
		'lhsmtool_cmd.c',
		],
	dependencies: [glib2, lustre, resolv],
	# for config.h and utils
	include_directories: include_directories(['..', '../common']),
	c_args: cc.get_supported_arguments([
		# gcc
		'-Wno-implicit-fallthrough',
		'-Wno-pedantic',
		'-Wno-cast-function-type',
		# clang
		'-Wno-unused-parameter',
		'-Wno-sign-compare',
		'-Wno-address-of-packed-member',
		'-Wno-incompatible-pointer-types',
		'-Wno-pointer-sign',
		'-Wno-zero-length-array',
	]),
)

lhsmcmd_conf_data = configuration_data()
lhsmcmd_conf_data.set('src_dir', meson.current_source_dir())
configure_file(
	input: 'lhsm_cmd.conf.in',
	output: 'lhsm_cmd.conf',
	configuration: lhsmcmd_conf_data,
)

parse_active_requests = executable(
	'parse_active_requests',
	sources: [
		'parse_active_requests.c',
		],
	include_directories: include_directories('../common'),
	link_with: [common],
)
test('parse_active_requests', parse_active_requests, args: [meson.current_source_dir() / 'active_requests'])

executable(
	'json',
	sources: [
		'json.c',
		],
	include_directories: include_directories('../common'),
	link_with: [common],
)

test_hiredis = executable(
	'hiredis',
	sources: [
		'hiredis.c',
		'../copytool/redis.c',
	],
	include_directories: include_directories('../common', '..'),
	dependencies: [hiredis],
	link_with: [common],
)
test('hiredis', test_hiredis)
