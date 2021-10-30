# Tests

There are three categories of tests:
- Unit tests, ran through meson with `ninja -C <builddir> test`
- Integration tests, ran manually here through `./tests/run_tests.sh`
- Fuzzing

## Unit tests

- `parse_active_requests`: checks basic parsing works
- XXX add protocol primitives tests

## Integration tests

tests assume:
 - sudo available
 - `tests_config.sh` has been filled, in particular:
   * CLIENT/MNTPATH are arrays with available clients (node, mount point)
     if CLIENT is set to something other than localhost, ssh key must be configured
   * MDS/MDT are set to mds node and mdt name (e.g. testfs0-MDT0000)
   * SOURCEDIR/BUILDDIR must be accessible from all nodes (shared mount)
   * ARCHIVEDIR junk directory that can be used on clients for `lhsmtool_cmd` backend
   * TESTDIR junk directory inside lustre, use MNTPATH if multiple mountpoints
 - no other agent running for that lustre

Given that, they start a coordinatool and lhsmtool agents and run basic tests

Can set ONLY to a regex identifying test number (e.g. ONLY=0[01]) to only run
specified tests.

## Fuzzing

Fuzzing has been performed with afl for:
 - config parsing
 - `active_requests` parsing

In both case this was done through the simple client as follow:
```sh
# install afl (available on fedora)
$ CC=afl-fuzz meson build-afl
$ ninja -C build-afl
$ mkdir input output

## for config
$ cp client_common/coordinatool.conf input/
$ afl-fuzz -i input -o output -- ./build-afl/coordinatool-client --config @@

## for active_requests, note we start a fake server
$ cp tests/active_requests input/
$ ncat -k -l -p 5123 >/dev/null </dev/null &
$ COORDINATOOL_HOST=localhost afl-fuzz -i input -o output -- ./build-afl/coordinatool-client -Q -i 0
```
