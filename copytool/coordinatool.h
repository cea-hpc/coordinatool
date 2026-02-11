/* SPDX-License-Identifier: LGPL-3.0-or-later */

#ifndef COORDINATOOL_H
#define COORDINATOOL_H

// for urcu slight optimization
#define _LGPL_SOURCE

#include <errno.h>
#include <linux/lustre/lustre_idl.h>
#include <lustre/lustreapi.h>
#include <sys/socket.h>
#include <hiredis/async.h>

#ifndef NO_CONFIG_H
#include "config.h"
#endif
#include "logs.h"
#include "protocol.h"
#include "utils.h"
#include "list_utils.h"

/* uncomment to add magic checks for han and list sanity checks */
// #define DEBUG_ACTION_NODE 0x12349876abcd1234ULL
#ifdef DEBUG_ACTION_NODE
static inline int check_list(struct cds_list_head *h)
{
	struct cds_list_head *n;
	int cnt = 0;
	cds_list_for_each(n, h)
	{
		if (cnt++ > 100000)
			return 1;
	}
	return 0;
}
#define cds_list_splice(a, h)                                           \
	do {                                                            \
		if (check_list(h)) {                                    \
			LOG_ERROR(-EIO, "BAD LIST SPLICE (head)");      \
			abort();                                        \
		}                                                       \
		if (check_list(a)) {                                    \
			LOG_ERROR(-EIO, "BAD LIST SPLICE (add)");       \
			abort();                                        \
		}                                                       \
		cds_list_splice(a, h);                                  \
		/* a is invalid at this point */                        \
		if (check_list(h)) {                                    \
			LOG_ERROR(-EIO, "BAD LIST SPLICE post (head)"); \
			abort();                                        \
		}                                                       \
	} while (0)
#define cds_list_add(n, h)                                      \
	do {                                                    \
		if (check_list(h)) {                            \
			LOG_ERROR(-EIO, "BAD LIST ADD (head)"); \
			abort();                                \
		}                                               \
		if (!cds_list_empty(n)) {                       \
			LOG_ERROR(-EIO, "BAD LIST ADD");        \
			abort();                                \
		}                                               \
		cds_list_add(n, h);                             \
	} while (0)
#define cds_list_add_tail(n, h)                                      \
	do {                                                         \
		if (check_list(h)) {                                 \
			LOG_ERROR(-EIO, "BAD LIST ADD TAIL (head)"); \
			abort();                                     \
		}                                                    \
		if (!cds_list_empty(n)) {                            \
			LOG_ERROR(-EIO, "BAD LIST ADD TAIL");        \
			abort();                                     \
		}                                                    \
		cds_list_add_tail(n, h);                             \
	} while (0)
#define cds_list_del(n)                                         \
	do {                                                    \
		struct cds_list_head *__next = (n)->next;       \
		if (check_list(n)) {                            \
			LOG_ERROR(-EIO, "BAD LIST DEL");        \
			abort();                                \
		}                                               \
		cds_list_del_init(n);                           \
		if (check_list(__next)) {                       \
			LOG_ERROR(-EIO, "BAD LIST DEL (post)"); \
			abort();                                \
		}                                               \
	} while (0)
#endif

struct reporting {
	/* name given in hint */
	const char *hint;
	/* hint length, needed to compare without modifying hai value
	 * that might not have a trailing nul byte. Actual hint has a
	 * nul byte at hint[hint_len] for safe filesystem use. */
	int hint_len;
	/* refcount to cleanup reporting file when no request left */
	int refcount;
	/* for unlink when refcount hits zero... */
	struct cds_list_head node;
};

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
	/* if sent to a client, remember who for eventual cancel (not implemented) */
	struct client *client;
	/* counter to decrease on done -- used for queues current count */
	int *current_count;
	/* reporting info if any */
	struct reporting *reporting;
	/* json representation of hai */
	json_t *hai;
};

#define ARCHIVE_ID_UNINIT ((unsigned int)-1)
struct hsm_action_queues {
	struct cds_list_head waiting_restore;
	struct cds_list_head waiting_archive;
	struct cds_list_head waiting_remove;
};

struct host_mapping {
	struct cds_list_head node;
	const char *tag;
	int count;
	bool consistent_hash;
	int hash_count;
	const char *hosts[];
};

/* common types */
struct client_batch {
	uint64_t expire_max_ns;
	uint64_t expire_idle_ns;
	char *hint;
	int current_count;
	struct cds_list_head waiting_archive;
};

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
		CLIENT_INIT, /* new connection */
		CLIENT_READY, /* connected, post ehlo */
		CLIENT_DISCONNECTED, /* recovery or temporary disconnect */
		CLIENT_WAITING, /* recv in progress */
	} status;
	struct cds_list_head active_requests;
	/* per client queues */
	struct hsm_action_queues queues;
	union { /* status-dependant fields */
		int64_t disconnected_timestamp;
		struct cds_list_head waiting_node;
	};
	struct client_batch batch[];
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
		const char *reporting_hint;
		const char *reporting_dir;
		int64_t reporting_schedule_interval_ns;
		const char *redis_host;
		int redis_port;
		enum llapi_message_level verbose;
		int client_grace_ms;
		int archive_cnt;
		int archives[LL_HSM_MAX_ARCHIVES_PER_AGENT];
		struct cds_list_head archive_mappings;
		int64_t batch_slice_idle;
		int64_t batch_slice_max;
		int batch_slots;
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
	int reporting_dir_fd;
	int timer_fd;
	int signal_fd;
	bool terminating;
	enum protocol_lock locked;
	struct hsm_action_queues queues;
	void *hsm_actions_tree;
	void *reporting_tree;
	struct cds_list_head reporting_cleanup_list;
	struct cds_list_head waiting_clients;
	struct ct_stats stats;
};

/* config */

void initiate_termination(void);
int config_init(struct state_config *config);
void config_free(struct state_config *config);

/* coordinatool */

extern struct state *state;

int epoll_addfd(int epoll_fd, int fd, void *data);
int epoll_delfd(int epoll_fd, int fd);

/* lhsm */

static inline int han_data_len(struct hsm_action_node *han)
{
	return han->info.hai_len - sizeof(struct hsm_action_item);
}

int handle_ct_event(void);
int ct_register(void);

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
int protocol_reply_status(struct client *client, int verbose, int status,
			  char *error);
int protocol_reply_recv(struct client *client, const char *fsname,
			uint32_t archive_id, uint64_t hal_flags,
			json_t *hai_list, int status, char *error);
int protocol_reply_queue(struct client *client, int enqueued, int skipped,
			 int status, char *error);
int protocol_reply_simple(struct client *client, const char *cmd, int status,
			  char *error);

/* queue */

// create new actions (foom json or lustre)
int hsm_action_new_json(json_t *json_hai, int64_t timestamp,
			struct hsm_action_node **han_out,
			const char *requestor);
int hsm_action_new_lustre(struct hsm_action_item *hai, uint32_t archive_id,
			  uint64_t hal_flags, int64_t timestamp);
// free one action
void hsm_action_free(struct hsm_action_node *han);
// free all actions (cleanup on shutdown)
void hsm_action_free_all(void);
// enqueue action on specific list
// (remove from current list it's in and update stats)
// if list is empty, try to schedule action
int hsm_action_enqueue(struct hsm_action_node *han, struct cds_list_head *list);
// same as above but remove han from old list first
static inline int hsm_action_requeue(struct hsm_action_node *han,
				     struct cds_list_head *list)
{
	cds_list_del(&han->node);
	return hsm_action_enqueue(han, list);
}
// same as above but for iterating through a list
static inline int hsm_action_requeue_all(struct cds_list_head *list)
{
	struct cds_list_head *n, *next;
	int rc, total = 0;

	cds_list_for_each_safe(n, next, list)
	{
		struct hsm_action_node *han =
			caa_container_of(n, struct hsm_action_node, node);
		rc = hsm_action_requeue(han, NULL);
		if (rc < 0)
			total = rc;
		else if (total >= 0)
			total += rc;
	}
	return total;
}
// start action on given client
// (add to active_requests and update stats)
void hsm_action_start(struct hsm_action_node *han, struct client *client);
// find node by cookie
struct hsm_action_node *hsm_action_search(unsigned long cookie,
					  struct lu_fid *dfid);

// init queue lists
void hsm_action_queues_init(struct hsm_action_queues *queues);
// get list in queue (by action type)
struct cds_list_head *get_queue_list(struct hsm_action_queues *queues,
				     struct hsm_action_node *han);

/* redis */

int redis_connect(void);
int redis_store_request(struct hsm_action_node *han);
int redis_assign_request(struct client *client, struct hsm_action_node *han);
int redis_deassign_request(struct hsm_action_node *han);
int redis_delete_request(uint64_t cookie, struct lu_fid *dfid);
int redis_recovery(void);

/* batch */
struct cds_list_head *schedule_batch_slot_active(struct hsm_action_node *han);
struct cds_list_head *schedule_batch_slot_new(struct hsm_action_node *han);
struct cds_list_head *
schedule_batch_slot_on_client(struct client *client,
			      struct hsm_action_node *han);
void batch_reschedule_client(struct client *client);
bool batch_slot_can_send(struct client *client, struct hsm_action_node *han);
uint64_t batch_next_expiry(void);
void batch_clear_expired(uint64_t now_ns);

/* reporting */
int reporting_init(void);
void reporting_cleanup(void);
int report_new_action(struct hsm_action_node *han);
int report_free_action(struct hsm_action_node *han);
int report_action(struct hsm_action_node *han, const char *format, ...)
	__attribute__((format(printf, 2, 3)));
int64_t report_next_schedule(void);
void report_pending_receives(int64_t now_ns);

/* scheduler */

struct client *find_client(struct cds_list_head *clients, const char *hostname);
struct cds_list_head *schedule_on_client(struct client *client,
					 struct hsm_action_node *han);
struct cds_list_head *hsm_action_node_schedule(struct hsm_action_node *han);
void ct_schedule(bool rearm_timers);
void ct_schedule_client(struct client *client);

/* tcp */

int tcp_listen(void);
char *sockaddr2str(struct sockaddr_storage *addr, socklen_t len);
int handle_client_connect(void);
struct client *client_new_disconnected(const char *id);
void client_free(struct client *client);
void client_disconnect(struct client *client);

/* timer */
int timer_init(void);
int timer_rearm(void);
void handle_expired_timers(void);

/* utils */
char *parse_hint(struct hsm_action_node *han, const char *hint_needle,
		 size_t *hint_len);
size_t dbj2(const char *buf, size_t size);
/**
 * Replace a substring
 *
 * It will replace the string "old_value" with "new_value" inside "orig".
 *
 * "old_value" must be a valid pointer inside "orig".
 */
char *replace_string(const char *orig, size_t orig_len, const char *new_value,
		     size_t new_len, const char *old_value, size_t old_len);

/* phobos */
#if HAVE_PHOBOS
int phobos_enrich(struct hsm_action_node *han);
struct cds_list_head *phobos_schedule(struct hsm_action_node *han);
bool phobos_can_send(struct client *client, struct hsm_action_node *han);
#endif

#endif
