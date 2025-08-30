/* SPDX-License-Identifier: LGPL-3.0-or-later */

#include <assert.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <time.h>

#include "coordinatool.h"
#include "version.h"

#if HAVE_PHOBOS_INIT
#include "coordinatool_phobos_store.h"
#endif

struct state *state;

int epoll_addfd(int epoll_fd, int fd, void *data)
{
	struct epoll_event ev;
	int rc;

	ev.events = EPOLLIN;
	ev.data.ptr = data;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
		rc = -errno;
		LOG_ERROR(rc, "Could not add fd to epoll watches");
		return rc;
	}

	return 0;
}

int epoll_delfd(int epoll_fd, int fd)
{
	int rc = 0;

	if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL) < 0) {
		rc = -errno;
		LOG_ERROR(rc, "Could not remove fd from epoll watches");
	}
	return rc;
}

static void print_help(char *argv0)
{
	printf("Usage: %s [options] mountpoint\n", argv0);
	printf("\n");
	printf("Options:\n");
	printf("    -v, --verbose: increase verbosity (repeatable)\n");
	printf("    -q, --quiet: decrease verbosity\n");
	printf("    -c, --config: alternative config path\n");
	printf("    -A, --archive <id>: set which archive id to handle\n");
	printf("                      (default any, can be set multiple times)\n");
	printf("                      note option removes any id defined in config\n");
	printf("    -p, --port <port>: select port to listen to\n");
	printf("    -H, --host <host>: select address to listen to\n");
	printf("    --redis-host <host>: hostname for redis server (default: localhost)\n");
	printf("    --redis-port <port>: port for redis server (default 6397)\n");
	printf("    --client-grace <time_ms>: time before we forget a client (default 10s)\n");
	printf("    -V, --version: print version info\n");
	printf("    -h, --help: this help\n");
}

static void print_version(void)
{
	printf("Coordinatool version %s\n", VERSION);
}

static int signal_init(void)
{
	int rc;
	sigset_t ss;

	sigemptyset(&ss);
	sigaddset(&ss, SIGTERM);
	sigaddset(&ss, SIGINT);
	sigaddset(&ss, SIGQUIT);

	state->signal_fd = signalfd(-1, &ss, SFD_NONBLOCK | SFD_CLOEXEC);
	if (state->signal_fd < 0) {
		rc = -errno;
		LOG_ERROR(rc, "Could not setup signal fd");
		return rc;
	}

	/* we need to block signals for signal fd to get a chance */
	rc = sigprocmask(SIG_BLOCK, &ss, NULL);
	if (rc < 0) {
		rc = -errno;
		LOG_ERROR(rc, "Could not block signals");
		return rc;
	}
	return epoll_addfd(state->epoll_fd, state->signal_fd,
			   (void *)(uintptr_t)state->signal_fd);
}

static void signal_log(int signal_fd)
{
	int n;
	struct signalfd_siginfo siginfo;

	n = read(signal_fd, &siginfo, sizeof(siginfo));
	if (n < 0) {
		n = -errno;
		LOG_WARN(n, "Read from signal fd failed, exiting anyway");
		return;
	}
	if (n != sizeof(siginfo)) {
		LOG_WARN(
			-EIO,
			"Read %d bytes from signal fd instead of %zd?! Exiting anyway",
			n, sizeof(siginfo));
		return;
	}

	LOG_INFO("Got signal %d from %d, exiting", siginfo.ssi_signo,
		 siginfo.ssi_pid);
}

void initiate_termination(void)
{
	struct cds_list_head *n, *nnext;
	state->terminating = true;

	epoll_delfd(state->epoll_fd, state->hsm_fd);
	if (state->listen_fd >= 0)
		close(state->listen_fd);
	if (state->timer_fd >= 0)
		close(state->timer_fd);
	cds_list_for_each_safe(n, nnext, &state->stats.clients)
	{
		struct client *client =
			caa_container_of(n, struct client, node_clients);

		client_free(client);
	}
	cds_list_for_each_safe(n, nnext, &state->stats.disconnected_clients)
	{
		struct client *client =
			caa_container_of(n, struct client, node_clients);

		client_free(client);
	}

	/* stop redis */
	if (state->redis_ac) {
		bool connected = state->redis_ac->c.flags & REDIS_CONNECTED;
		redisAsyncDisconnect(state->redis_ac);
		/* if we just initiated connect with no IO, the disconnect
					 * callback won't be called yet state->redis_ac is freed:
					 * just clear it here. */
		if (!connected) {
			state->redis_ac = NULL;
		}
	}
}

static int random_init(void)
{
	struct timespec tp;

	/* we don't need good rng, just use time */
	if (clock_gettime(CLOCK_REALTIME, &tp) < 0) {
		int rc = -errno;
		LOG_ERROR(rc, "could not get time");
		return rc;
	}

	srand(tp.tv_sec + tp.tv_nsec);
	return 0;
}

static int lustre_get_fsname(void)
{
	char fsname[LUSTRE_MAXFSNAME + 1];
	int rc;

	rc = llapi_search_fsname(state->mntpath, fsname);
	if (rc < 0) {
		LOG_ERROR(rc, "cannot find a Lustre filesystem mounted at '%s'",
			  state->mntpath);
		return rc;
	}

	state->fsname = xstrdup(fsname);
	return 0;
}

#define MAX_EVENTS 10
static int ct_start(void)
{
	int rc;
	struct epoll_event events[MAX_EVENTS];
	int nfds;

	rc = lustre_get_fsname();
	if (rc)
		return rc;

	state->epoll_fd = epoll_create1(0);
	if (state->epoll_fd < 0) {
		rc = -errno;
		LOG_ERROR(rc, "could not create epoll fd");
		return rc;
	}

	rc = random_init();
	if (rc < 0)
		return rc;

	rc = timer_init();
	if (rc < 0)
		return rc;

	rc = signal_init();
	if (rc < 0)
		return rc;

	hsm_action_queues_init(&state->queues);

	rc = reporting_init();
	if (rc < 0)
		return rc;

	rc = redis_connect();
	if (rc < 0)
		return rc;

	/* we need to run this before other fds have been added to epoll */
	rc = redis_recovery();
	if (rc < 0)
		return rc;

	rc = tcp_listen();
	if (rc < 0)
		return rc;

	rc = ct_register();
	if (rc < 0)
		return rc;

	LOG_NORMAL("Starting main loop");
	while (1) {
		nfds = epoll_wait(state->epoll_fd, events, MAX_EVENTS, -1);
		if (nfds < 0 && errno == EINTR)
			continue;
		if (nfds < 0) {
			rc = -errno;
			LOG_ERROR(rc, "epoll_wait failed");
			return rc;
		}
		int n;
		for (n = 0; n < nfds; n++) {
			if (events[n].events & (EPOLLERR | EPOLLHUP)) {
				LOG_INFO("%d on error/hup", events[n].data.fd);
			}
			if (events[n].data.fd == state->hsm_fd) {
				handle_ct_event();
			} else if (events[n].data.fd == state->listen_fd) {
				handle_client_connect();
			} else if (events[n].data.ptr == state->redis_ac) {
				if (events[n].events & EPOLLIN) {
					redisAsyncHandleRead(state->redis_ac);
				}
				/* EPOLLOUT is only requested when we have something
				 * to send.
				 * When disconnecting redis_ac can be cleared while
				 * handling read, so check it's still here.
				 */
				if (state->redis_ac &&
				    events[n].events & EPOLLOUT) {
					redisAsyncHandleWrite(state->redis_ac);
				}
				if (!state->redis_ac) {
					/* done flushing redis requests,
					 * we can exit */
					if (state->terminating)
						return 0;
					LOG_INFO(
						"Connection to redis server failed, trying to reconnect");
					// XXX check rc
					// XXX if server isn't up we'll busy loop on this...
					redis_connect();
				}
			} else if (events[n].data.fd == state->timer_fd) {
				handle_expired_timers();
			} else if (events[n].data.fd == state->signal_fd) {
				signal_log(state->signal_fd);

				/* we got killed, close all clients and stop listening
				 * for lustre events and initiate redis disconnect. */
				if (state->terminating) {
					LOG_WARN(
						0,
						"Got killed twice, no longer waiting for redis");
					return 0;
				}
				initiate_termination();
			} else {
				struct client *client = events[n].data.ptr;
				if (protocol_read_command(
					    client->fd, client->id, client,
					    protocol_cbs, NULL) < 0) {
					client_disconnect(client);
				}
			}

			/* We exit this loop */
			if (state->terminating && !state->redis_ac) {
				return 0;
			}
		}
	}
}

#define OPT_REDIS_HOST 257
#define OPT_REDIS_PORT 258
#define OPT_CLIENT_GRACE 259
int main(int argc, char *argv[])
{
	const struct option long_opts[] = {
		{ "verbose", no_argument, NULL, 'v' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "config", required_argument, NULL, 'c' },
		{ "archive", required_argument, NULL, 'A' },
		{ "port", required_argument, NULL, 'p' },
		{ "host", required_argument, NULL, 'H' },
		{ "redis-host", required_argument, NULL, OPT_REDIS_HOST },
		{ "redis-port", required_argument, NULL, OPT_REDIS_PORT },
		{ "client-grace", required_argument, NULL, OPT_CLIENT_GRACE },
		{ "help", no_argument, NULL, 'h' },
		{ "version", no_argument, NULL, 'V' },
		{ 0 },
	};
	const char short_opts[] = "c:A:vqH:p:Vh";
	int rc;
	bool first_archive_id = true;

#if HAVE_PHOBOS_INIT
	phobos_init();
	atexit(phobos_fini);
#endif

	// state init
	struct state mstate = {
		.listen_fd = -1,
		.timer_fd = -1,
		.reporting_dir_fd = -1,
	};
	state = &mstate;
	CDS_INIT_LIST_HEAD(&mstate.config.archive_mappings);
	CDS_INIT_LIST_HEAD(&mstate.stats.clients);
	CDS_INIT_LIST_HEAD(&mstate.stats.disconnected_clients);
	CDS_INIT_LIST_HEAD(&mstate.waiting_clients);
	CDS_INIT_LIST_HEAD(&mstate.reporting_cleanup_list);

	/* parse arguments once first just for config */
	while ((rc = getopt_long(argc, argv, short_opts, long_opts, NULL)) !=
	       -1) {
		if (rc == 'c') {
			free((void *)mstate.config.confpath);
			mstate.config.confpath = xstrdup(optarg);
		}
	}

	rc = config_init(&mstate.config);
	if (rc) {
		rc = EXIT_FAILURE;
		goto out;
	}

	optind = 1;
	while ((rc = getopt_long(argc, argv, short_opts, long_opts, NULL)) !=
	       -1) {
		switch (rc) {
		case 'c': // parsed above
			break;
		case 'A':
			if (first_archive_id) {
				/* reset any value defined in config */
				mstate.config.archive_cnt = 0;
				first_archive_id = false;
			}
			if (mstate.config.archive_cnt >=
			    LL_HSM_MAX_ARCHIVES_PER_AGENT) {
				LOG_ERROR(-E2BIG, "too many archive id given");
				rc = EXIT_FAILURE;
				goto out;
			}
			mstate.config.archives[mstate.config.archive_cnt] =
				parse_int(optarg, INT_MAX, "Archive id");
			if (mstate.config.archives[mstate.config.archive_cnt] <=
			    0) {
				rc = EXIT_FAILURE;
				goto out;
			}
			mstate.config.archive_cnt++;
			break;
		case 'v':
			mstate.config.verbose++;
			llapi_msg_set_level(mstate.config.verbose);
			break;
		case 'q':
			mstate.config.verbose--;
			llapi_msg_set_level(mstate.config.verbose);
			break;
		case 'H':
			free((void *)mstate.config.host);
			mstate.config.host = xstrdup(optarg);
			break;
		case 'p':
			free((void *)mstate.config.port);
			mstate.config.port = xstrdup(optarg);
			break;
		case OPT_REDIS_HOST:
			free((void *)mstate.config.redis_host);
			mstate.config.redis_host = xstrdup(optarg);
			break;
		case OPT_REDIS_PORT:
			mstate.config.redis_port =
				parse_int(optarg, 65535, "Redis port");
			if (mstate.config.redis_port < 0) {
				rc = EXIT_FAILURE;
				goto out;
			}
			break;
		case OPT_CLIENT_GRACE:
			mstate.config.client_grace_ms =
				parse_int(optarg, INT_MAX, "client grace ms");
			if (mstate.config.client_grace_ms < 0) {
				rc = EXIT_FAILURE;
				goto out;
			}
			break;
		case 'V':
			print_version();
			rc = EXIT_SUCCESS;
			goto out;
		case 'h':
			print_help(argv[0]);
			rc = EXIT_SUCCESS;
			goto out;
		default:
			fprintf(stderr, "Unknown option %c, see --help\n", rc);
			rc = EXIT_FAILURE;
			goto out;
		}
	}
	if (argc != optind + 1) {
		LOG_ERROR(-EINVAL, "no mount point specified");
		rc = EXIT_FAILURE;
		goto out;
	}
	mstate.mntpath = argv[optind];

	rc = ct_start();
	rc = rc ? EXIT_FAILURE : EXIT_SUCCESS;

out:
	if (mstate.redis_ac) {
		LOG_WARN(
			EISCONN,
			"redis connection was not closed completely, some requests will likely not be remembered");
	}
	if (mstate.ctdata) {
		llapi_hsm_copytool_unregister(&mstate.ctdata);
	}
	hsm_action_free_all();
	reporting_cleanup();
	config_free(&mstate.config);
	free((void *)mstate.fsname);
	return rc;
}
