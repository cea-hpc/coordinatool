# Tests

There are three categories of tests:
- Unit tests, ran through meson with ninja -C tests
- Integration tests, ran manually here somehow when it's done
- Fuzzing

## Unit tests

XXX describe briefly

## Integration tests

XXX ditto

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
