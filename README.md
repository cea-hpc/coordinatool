# Coordinatool

Lustre "coordinator in userspace" implemented as a copytool which takes
in all requests from coordinator and redispatches them to real lhsm agent,
wrapped around with a `LD_PRELOAD` lib

# Usage

## coordinatool configuration

## arguments

The server takes command line options, and must be given a mount point
to register as agent last.

Options:
 - `--host` / `--port` : host and port to bind to
 - `--archive` archive id to listen to, additive. defaults to any.
 - `--verbose` / `--quiet`


### systemd service

A systemd unit is provided, and should be started/enabled with, for
example, `coordinatool@mnt-lustre` (the argument is an absolute path
with the first slash removed, see systemd-escape)

Arguments can then be specified in either /etc/sysconfig/coordinatool
or /etc/sysconfig/coordinatool.mnt-lustre

### pitfalls

The server is stateless, that is, it won't remember requests it has
already accepted if the service is restarted, but lustre will consider
these to have been started and refuse to do anything with the files for
a long time.

To work around this, the standalone client should be used to feed the
server with 'leftovers' requests from the `hsm/active_requests` file
whenever the server is restarted. For example:

```sh
cat /sys/kernel/debug/lustre/mdt/lustre0-MDT*/hsm/active_requests |
    coordinatool-client -Q
```

Also, if many requests are queued and a request times out the original
request will be lost and clients can see errors, for this reason it is
recommended to increase the timeout and usual loop period settings:
```sh
lctl set_param -P mdt.lustre0-MDT*.hsm.active_request_timeout=$((3600*24*31))
lctl set_param -P mdt.lustre0-MDT*.hsm.loop_period=1
lctl set_param -P mdt.lustre0-MDT*.hsm.max_requests=1000
```


## client configuration

Clients read configuration from /etc/coordinatool.conf, or environment
variables (priority over config file)

See `client_common/coordinatool.conf` for config file example, which is
a simple "key <space> value" format separated by new lines.
Comments are accepted on their own lines with a sharp (`#`).

Environment variables use the same name in full caps prefixed by
`COORDINATOOL_` e.g. where `host` is used in the config file,
`COORDINATOOL_HOST` will have the same effect.

knobs:
 - `host`: define what dns name to connect to
 - `port`: define ports to connect to
 - `max_restore`, `max_archive`, `max_remove`: maximum number of
    simultaneous requests accepted for each type
 - `hal_size` buffer size used for internal receive buffer, defaults
   to 1MB like lustre. Accepts optional K/M/G suffix.
 - `archive_id`: archive id to request if set, default to any
 - `verbose`: loglevel, can be one of DEBUG, INFO, NORMAL, WARN, ERROR.


## standalone client

The standalone client also abides by config file and environment
variables, then:

- default just prints status and exist
- `--queue`/`-Q` parse stdin for `active_requests` and send these to
coordinatool
- other options for debug are listed in `--help`


# Tests

see [tests' readme](./tests/README.md)


# TODO

- systemd post-start action to ssh to mds and get a client to feed server?
- TLS + some kind of auth
- priority/fair queueing (stat file to get uid etc)
