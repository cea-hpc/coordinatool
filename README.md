# Coordinatool

Lustre "coordinator in userspace" implemented as a copytool which takes
in all requests from coordinator and re-dispatches them to real lhsm agent,
wrapped around a `LD_PRELOAD` lib

# Motivation

The coordinatool allows more flexibility in writing custom request
scheduling, e.g. regroup similar files on same mover to optimize
later reads.

# Usage

## coordinatool configuration

### arguments

The server takes command line options, and must be given a mount point
to register as agent last.

Options:
 - `--host` / `--port` : host and port to bind to
 - `--archive` archive id to listen to, additive. defaults to any.
 - `--verbose` / `--quiet`

### config file

See `coordinatool.conf` comment for detailed led features, some are
described below:
- `archive_on_hosts data mover1 [mover2 ...]`:
force archive requests with 'data' in hsm data to be sent to one of the
mover listed afterwards.
If none are online the request will not be sent and wait for movers to
connect.
- `batch_archives_slice_sec <idletime> <maxtime>` /
  `batch_archives_slots_per_client <count>`:
Limit archives to only be sent to movers if the lustre hsm data is
exactly the same, e.g. if two archives are sent with 'tag=foo' and
'tag=bar' and movers only have a client with a single slot then only
the first one will be scheduled immediately, and the second one will
be scheduled after the mover has been idle for target time or after
the maximum time elapsed.
- `reporting_dir .dir` /
  `reporting_hint [hint]` /
  `reporting_schedule_interval_ms <time>`:
If a request comes in with `[hint]=something` then .dir/something
will be created within the lustre filesystem and report on the
following events:
- `new <fid>`: a request for fid has been received
- `assign <fid> <mover>`: request for fid has selected a mover
- `progress <fid> <mover> <pos>/<total>`: every schedule interval,
queued requests will report their positions in waiting queues.
If no mover has been selected yet then `global_queue` is printed
as mover name.
- `sent <fid> <mover>`: request has been sent to mover
- `done <fid> <status>`: mover was done with request, with status (int)

The file will then be deleted after at least one schedule interval if no
requests are active.
Note that if the server restarts just after the last request involved
was done, the file will never be deleted, so an additional crontab such
as `find /mnt/lustre/.dir -mtime 1 -delete` is recommended.

### systemd service

A systemd unit is provided, and should be started/enabled with, for
example, `coordinatool@mnt-lustre` (the argument is an absolute path
with the first slash removed, see systemd-escape)

Arguments can then be specified in either /etc/sysconfig/coordinatool
or /etc/sysconfig/coordinatool.mnt-lustre

### pitfalls

If many requests are queued and a request times out the original
request will be lost and clients can see errors, for this reason it is
recommended to increase the timeout and usual loop period settings:
```sh
lctl set_param -P mdt.lustre0-MDT*.hsm.active_request_timeout=$((3600*24*31))
lctl set_param -P mdt.lustre0-MDT*.hsm.loop_period=1
lctl set_param -P mdt.lustre0-MDT*.hsm.max_requests=1000
```


The server will remember requests in a redis database so restarts should
be transparent (if redis is available);
if some requests have been dropped and need to be re-queued from lustre
then they can be re-added by parsing `active_requests` as follow:

```sh
cat /sys/kernel/debug/lustre/mdt/lustre0-MDT*/hsm/active_requests |
    coordinatool-client -Q
```

Note `active_requests` does not contain the full user data, so this is
not appropriate if more than 12 bytes of user data are sent.

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
 - `verbose`: loglevel, can be one of DEBUG, INFO, NORMAL, WARN, ERROR, OFF.


## standalone client

The standalone client also abides by config file and environment
variables, then:

- default just prints status and exist, if verbose it will dump all requests
in coordinatool queues for debugging.
- `--queue`/`-Q` parse stdin for `active_requests` and send these to
coordinatool
- other options for debug are listed in `--help`


# Tests

see [tests' readme](./tests/README.md)


# TODO

- TLS + some kind of auth?
- priority/fair queueing (stat file to get uid etc)
