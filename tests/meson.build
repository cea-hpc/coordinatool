glib2 = dependency('glib-2.0')
resolv = cc.find_library('resolv')

executable(
	'lhsmtool_cmd',
	sources: [
		'lhsmtool_cmd.c',
		],
	dependencies: [glib2, lustre, resolv],
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
executable(
	'json',
	sources: [
		'json.c',
		],
	dependencies: [lustre, jansson],
	include_directories: include_directories('../common'),
	link_with: [common],
)
