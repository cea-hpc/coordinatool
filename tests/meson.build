glib2 = dependency('glib-2.0')
resolv = cc.find_library('resolv')

executable(
	'lhsmtool_cmd',
	sources: [
		'lhsmtool_cmd.c',
		],
	dependencies: [glib2, lustre, resolv],
	c_args: [
		'-Wno-gnu-zero-variadic-macro-arguments',
		'-Wno-unused-parameter',
		'-Wno-sign-compare',
		'-Wno-address-of-packed-member',
		'-Wno-incompatible-pointer-types',
		'-Wno-pointer-sign',
		'-Wno-zero-length-array',
	],
)
executable(
	'json',
	sources: [
		'json.c',
		],
	dependencies: [lustre, jansson],
)
