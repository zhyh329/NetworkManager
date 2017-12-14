#
# target: libnipv4ll.so
# We build both, a static and a shared library. We want our tests to get access
# to internals, so we link them statically.
#

libnipv4ll_private = static_library('nipv4ll-private',
                                   ['n-ipv4ll.c'],
                                   c_args: [
                                          '-fvisibility=hidden',
                                          '-fno-common'
                                   ],
                                   dependencies: [dep_clist, dep_nacd],
                                   pic: true)
install_headers('n-ipv4ll.h')
libnipv4ll_symfile = join_paths(meson.current_source_dir(), 'libnipv4ll.sym')
libnipv4ll_shared = shared_library('nipv4ll',
                                   dependencies: [dep_clist, dep_nacd],
                                   objects: libnipv4ll_private.extract_all_objects(),
                                   install: true,
                                   soversion: 0,
                                   link_depends: libnipv4ll_symfile,
                                   link_args: [
                                           '-Wl,--no-undefined',
                                           '-Wl,--version-script=@0@'.format(libnipv4ll_symfile)
                                   ])
mod_pkgconfig.generate(libraries: libnipv4ll_shared,
                       version: meson.project_version(),
                       name: 'libnipv4ll',
                       filebase: 'libnipv4ll',
                       description: 'IPv4 Link Local Address Configuration')

#
# target: test-api
# The test-api program explicitly links against the shared library, since it
# tests for symbol visibility.
#

test_api = executable('test-api',
                      ['test-api.c'],
                      link_with: libnipv4ll_shared)
test('API Symbol Visibility', test_api)

#
# target: test-*
# All other tests are listed here. They link against the static library, so
# they can access internals for verification.
#

test_basic = executable('test-basic',
                        ['test-basic.c'],
                        link_with: libnipv4ll_private)
test('Basic API Behavior', test_basic)
