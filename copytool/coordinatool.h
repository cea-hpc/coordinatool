/* SPDX-License-Identifier: LGPL-3.0-or-later */

#ifndef COORDINATOOL_H
#define COORDINATOOL_H

// for urcu slight optimization
#define _LGPL_SOURCE

#include <errno.h>
#include <linux/lustre/lustre_idl.h>
#include <lustre/lustreapi.h>
#include <sys/socket.h>
#include <urcu/compiler.h>
#include <urcu/list.h>
#include <hiredis/async.h>

#include "config.h"
#include "logs.h"
#include "protocol.h"
#include "utils.h"


/* uncomment to add magic checks for han and list sanity checks */
// #define DEBUG_ACTION_NODE 0x12349876abcd1234ULL
#ifdef DEBUG_ACTION_NODE
static inline int check_list(struct cds_list_head *h) {
	struct cds_list_head *n;
	int cnt = 0;
	cds_list_for_each(n, h) {
		if (cnt++ > 100000)
			return 1;
	}
	return 0;
}
#define cds_list_splice(a, h) do { \
	if (check_list(h)) { \
		LOG_ERROR(-EIO, "BAD LIST SPLICE (head)"); \
		abort(); \
	} \
	if (check_list(a)) { \
		LOG_ERROR(-EIO, "BAD LIST SPLICE (add)"); \
		abort(); \
	} \
	cds_list_splice(a, h); \
	/* a is invalid at this point */ \
	if (check_list(h)) { \
		LOG_ERROR(-EIO, "BAD LIST SPLICE post (head)"); \
		abort(); \
	} \
} while (0)
#define cds_list_add(n, h) do { \
	if (check_list(h)) { \
		LOG_ERROR(-EIO, "BAD LIST ADD (head)"); \
		abort(); \
	} \
	if (!cds_list_empty(n)) { \
		LOG_ERROR(-EIO, "BAD LIST ADD"); \
		abort(); \
	}	\
	cds_list_add(n, h); \
} while (0)
#define cds_list_add_tail(n, h) do { \
	if (check_list(h)) { \
		LOG_ERROR(-EIO, "BAD LIST ADD TAIL (head)"); \
		abort(); \
	} \
	if (!cds_list_empty(n)) { \
		LOG_ERROR(-EIO, "BAD LIST ADD TAIL"); \
		abort(); \
	}	\
	cds_list_add_tail(n, h); \
} while (0)
#define cds_list_del(n) do { \
	struct cds_list_head *__next = (n)->next; \
	if (check_list(n)) { \
		LOG_ERROR(-EIO, "BAD LIST DEL"); \
		abort(); \
	} \
	cds_list_del_init(n); \
	if (check_list(__next)) { \
		LOG_ERROR(-EIO, "BAD LIST DEL (post)"); \
		abort(); \
	} \
} while (0)
#endif

/* queue types */

struct hsm_action_node {
#ifdef DEBUG_ACTION_NODE
	int64_t magic;
#endif
	/* list is used to track order of requests in waiting list,
	 * or dump requests assigned to a client */
	struct cds_list_head node;
	/* enriched infos to take scheduling decisions */
	struct item_info {
		uint64_t cookie;
		struct lu_fid dfid;
		uint64_t timestamp;
		size_t hai_len;
		enum hsm_copytool_action action;
		uint32_t archive_id;
		uint64_t hal_flags;
		const char *data; /* unlike lustre's, nul-terminated */
#if HAVE_PHOBOS
		char *hsm_fuid;
#endif
	} info;
	/* queue han will be enqueued to */
	struct hsm_action_queues *queues;
	/* if sent to a client, remember who for eventual cancel */
	struct client *client;
	/* json representation of hai */
	json_t *hai;
};

#define ARCHIVE_ID_UNINIT ((unsigned int)-1)
struct hsm_action_queues {
	struct cds_list_head waiting_restore;
	struct cds_list_head waiting_archive;
	struct cds_list_head waiting_remove;
	void *actions_tree;
	struct state *state;
};

struct host_mapping {
	struct cds_list_head node;
	const char *tag;
	int count;
	const char *hosts[];
};

/* common types */
struct client {
	const char *id; /* id sent by the client during EHLO, or addr */
	bool id_set; /* set if clients introduce themselves */
	int fd;
	struct cds_list_head node_clients;
	unsigned int done_restore;
	unsigned int done_archive;
	unsigned int done_remove;
	int current_restore;
	int current_archive;
	int current_remove;
	size_t max_bytes;
	int max_restore;
	int max_archive;
	int max_remove;
	int *archives;
	enum client_status {
		CLIENT_INIT,         /* new connection */
		CLIENT_READY,        /* connected, post ehlo */
		CLIENT_DISCONNECTED, /* recovery or temporary disconnect */
		CLIENT_WAITING,      /* recv in progress */
	} status;
	struct cds_list_head active_requests;
	/* per client queues */
	struct hsm_action_queues queues;
	union { /* status-dependant fields */
		int64_t disconnected_timestamp;
		struct cds_list_head waiting_node;
	};
};

struct ct_stats {
	unsigned int running_restore;
	unsigned int running_archive;
	unsigned int running_remove;
	unsigned int pending_restore;
	unsigned int pending_archive;
	unsigned int pending_remove;
	long unsigned int done_restore;
	long unsigned int done_archive;
	long unsigned int done_remove;
	unsigned int clients_connected;
	struct cds_list_head clients;
	struct cds_list_head disconnected_clients;
};

struct state {
	/* config: option, config file or env var */
	struct state_config {
		const char *confpath;
		const char *host;
		const char *port;
		const char *redis_host;
		int redis_port;
		enum llapi_message_level verbose;
		int client_grace_ms;
		int archive_cnt;
		int archives[LL_HSM_MAX_ARCHIVES_PER_AGENT];
		struct cds_list_head archive_mappings;
	} config;
	/* options: command line switches only */
	const char *mntpath;
	/* state values */
	struct hsm_copytool_private *ctdata;
	const char *fsname;
	redisAsyncContext *redis_ac;
	int epoll_fd;
	int hsm_fd;
	int listen_fd;
	int timer_fd;
	int signal_fd;
	bool terminating;
	struct hsm_action_queues queues;
	struct cds_list_head waiting_clients;
	struct ct_stats stats;
};


/* config */

int config_init(struct state_config *config);
void config_free(struct state_config *config);


/* coordinatool */

int epoll_addfd(int epoll_fd, int fd, void *data);
int epoll_delfd(int epoll_fd, int fd);


/* lhsm */

int handle_ct_event(struct state *state);
int ct_register(struct state *state);


/* protocol */

/* stop enqueuing new hsm action items if we cannot enqueue at least
 * HAI_SIZE_MARGIN more.
 * That is because item is variable size depending on its data.
 * This is mere optimisation, if element didn't fit it is just put back
 * in waiting list -- at the end, so needs avoiding in general.
 */
#define HAI_SIZE_MARGIN (sizeof(struct hsm_action_item) + 100)

extern protocol_read_cb protocol_cbs[];

/**
 * send status reply
 *
 * @param fd socket to write on
 * @param ct_stats stats to get stats to send from
 * @param status error code
 * @param error nul-terminated error string, can be NULL
 * @return 0 on success, -errno on error
 */
int protocol_reply_status(struct client *client, struct ct_stats *ct_stats,
			  int status, char *error);
int protocol_reply_recv(struct client *client,
			const char *fsname, uint32_t archive_id,
			uint64_t hal_flags, json_t *hai_list,
			int status, char *error);
int protocol_reply_done(struct client *client, int status, char *error);
int protocol_reply_queue(struct client *client, int enqueued, int skipped,
			 int status, char *error);
int protocol_reply_ehlo(struct client *client, int status, char *error);


/* queue */

void queue_node_free(struct hsm_action_node *han);
void hsm_action_free_all(struct state *state);
void hsm_action_queues_init(struct state *state,
			    struct hsm_action_queues *queues);
int hsm_action_requeue(struct hsm_action_node *han, bool start);
void hsm_action_move(struct hsm_action_queues *queues,
		     struct hsm_action_node *han,
		     bool start);
int hsm_action_enqueue_json(struct state *state, json_t *json_hai,
			    int64_t timestamp,
			    struct hsm_action_node **han_out,
			    const char *requestor);
int hsm_action_enqueue(struct state *state,
		       struct hsm_action_item *hai,
		       uint32_t archive_id, uint64_t hal_flags,
		       int64_t timestamp);
void hsm_action_dequeue(struct hsm_action_queues *queues,
			struct hsm_action_node *han);
struct hsm_action_node *hsm_action_search_queue(struct hsm_action_queues *queues,
                                                unsigned long cookie,
						struct lu_fid *dfid);


/* redis */

int redis_connect(struct state *state);
int redis_store_request(struct state *state, struct hsm_action_node *han);
int redis_assign_request(struct state *state, struct client *client,
			 struct hsm_action_node *han);
int redis_delete_request(struct state *state, uint64_t cookie,
			 struct lu_fid *dfid);
int redis_recovery(struct state *state);


/* scheduler */

int schedule_on_client(struct state *state,
               struct hsm_action_node *han,
               const char *hostname);
void hsm_action_node_enrich(struct state *state, struct hsm_action_node *han);
void ct_schedule(struct state *state);
void ct_schedule_client(struct state *state,
			struct client *client);


/* tcp */

int tcp_listen(struct state *state);
char *sockaddr2str(struct sockaddr_storage *addr, socklen_t len);
int handle_client_connect(struct state *state);
struct client *client_new_disconnected(struct state *state, const char *id);
void client_free(struct client *client);
void client_disconnect(struct client *client);

/* timer */
int timer_init(struct state *state);
int timer_rearm(struct state *state);
void handle_expired_timers(struct state *state);

#if HAVE_PHOBOS
int phobos_enrich(struct state *state,
		  struct hsm_action_node *han);
bool phobos_can_send(struct client *client,
		     struct hsm_action_node *han);
#endif

#endif
