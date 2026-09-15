pixels = executable('pixels',
	[
		'test-pixels.c',
		'../src/pixels.c',
	],
	include_directories: inc,
	dependencies: [
		pixman,
		libdrm_inc,
		libm,
	],
)
test('pixels', pixels)

base64 = executable('base64',
	[
		'test-base64.c',
		'../src/base64.c',
	],
	include_directories: inc,
)
test('base64', base64)

bandwidth = executable('bandwidth',
	[
		'test-bandwidth.c',
		'../src/bandwidth.c',
	],
	include_directories: inc,
	dependencies: [
		libm,
	],
)
test('bandwidth', bandwidth)
