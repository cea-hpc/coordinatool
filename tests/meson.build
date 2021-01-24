glib2 = dependency('glib-2.0')
resolv = cc.find_library('resolv')

executable(
	'master_ct',
	sources: [
		'lhsmtool_cmd.c',
		],
	dependencies: [glib2, lustre, resolv],
	install: true,
)
executable(
	'json',
	sources: [
		'json.c',
		],
	dependencies: [lustre, jansson],
	install: true,
)
