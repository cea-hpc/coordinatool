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
     due to how the backend is expected to work this directory must be shared.
   * TESTDIR junk directory inside lustre, use MNTPATH if multiple mountpoints
 - no other agent running for that lustre

Given that, they start a coordinatool and lhsmtool agents and run basic tests

Can set ONLY to a regex identifying test number (e.g. ONLY=0[01]) to only run
specified tests.

### Interactive tests

The test framework is also designed to be useable interactively as long as
requirements are met.

- source `run_tests.sh` and tab completion away...
- `do_*` will run simple actions on predefined clients
  * `do_{client,mds} idx command` runs arbitrary command on indexed client/mds
  * `do_{coordinatool,lhsmtoolcmd}_{start,service} idx [action]` starts or
    manage long running services
  * `do_coordinatool_client idx args` runs the coordinatool client
- `client_*` will run more complex predetermined scenarii, e.g.
  * `client_{archive,restore,remove}_n idx count` will archive, restore, or
    remove count files on client idx (must have been archived to restore...)
  * `client_archive_n_req idx count` will just send archive requests
    without waiting,
  * `client_archive_n_wait idx count` is its pending friend
  * `client_reset` cleans client dir
- similarly, `mds_*` defines snippets running on mds server
  * `mds_requeue_active_requests idx` will run coordinatool client to requeue
    active requests


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
