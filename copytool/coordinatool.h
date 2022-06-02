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

#include "logs.h"
#include "protocol.h"
#include "utils.h"


/* queue types */

struct hsm_action_node {
	/* list is used to track order of requests in waiting list,
	 * or dump requests assigned to a client */
	struct cds_list_head node;
	struct hsm_action_queues *queues;
	/* if sent to a client, remember who for eventual cancel */
	struct client *client;
	/* enriched infos to take scheduling decisions */
	struct item_info {
		uint64_t cookie;
		enum hsm_copytool_action action;
		struct lu_fid dfid;
		size_t hai_len;
#ifdef PHOBOS
		char *hsm_fuid;
#endif
	} info;
	/* json representation of hai */
	json_t *hai;
};

#define ARCHIVE_ID_UNINIT ((unsigned int)-1)
struct hsm_action_queues {
	struct cds_list_head waiting_restore;
	struct cds_list_head waiting_archive;
	struct cds_list_head waiting_remove;
	void *actions_tree;
	char *fsname;
	unsigned long long hal_flags;
	unsigned int archive_id;
	struct state *state;
};

/* common types */
struct client {
	const char *id; /* id sent by the client during EHLO, or addr */
	int fd;
	struct cds_list_head node_clients;
	unsigned int done_restore;
	unsigned int done_archive;
	unsigned int done_remove;
	int current_restore;
	int current_archive;
	int current_remove;
	int max_restore;
	int max_archive;
	int max_remove;
	size_t max_bytes;
	struct cds_list_head active_requests;
	enum client_state {
		CLIENT_CONNECTED = 0, /* default state */
		CLIENT_WAITING,
	} state;
	/* per client queues */
	struct hsm_action_queues queues;
	union { /* state-dependant fields */
		struct cds_list_head node_waiting;
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
};

struct state {
	/* config: option, config file or env var */
	struct state_config {
		const char *confpath;
		const char *host;
		const char *port;
		const char *state_dir_prefix;
		enum llapi_message_level verbose;
		const char *redis_host;
		int redis_port;
	} config;
	/* options: command line switches only */
	int archive_cnt;
	int archive_id[LL_HSM_MAX_ARCHIVES_PER_AGENT];
	const char *mntpath;
	/* state values */
	struct hsm_copytool_private *ctdata;
	redisAsyncContext *redis_ac;
	int epoll_fd;
	int hsm_fd;
	int listen_fd;
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
int protocol_reply_recv_single(struct client *client,
			       struct hsm_action_queues *queues,
			       struct hsm_action_node *han);
int protocol_reply_recv(struct client *client, struct hsm_action_queues *queues,
			json_t *hal, int status, char *error);
int protocol_reply_done(struct client *client, int status, char *error);
int protocol_reply_queue(struct client *client, int enqueued,
			 int status, char *error);
int protocol_reply_ehlo(struct client *client, int status, char *error);


/* queue */

void queue_node_free(struct hsm_action_node *han);
struct hsm_action_queues *hsm_action_queues_get(struct state *state,
						unsigned int archive_id,
						unsigned long long flags,
						const char *fsname);
void hsm_action_queues_init(struct state *state,
			    struct hsm_action_queues *queues);
int hsm_action_requeue(struct hsm_action_node *han, bool start);
void hsm_action_move(struct hsm_action_queues *queues,
		     struct hsm_action_node *han,
		     bool start);
int hsm_action_enqueue(struct hsm_action_queues *queues,
		       struct hsm_action_item *hai);
void hsm_action_dequeue(struct hsm_action_queues *queues,
			struct hsm_action_node *han);
struct hsm_action_node *hsm_action_search_queue(struct hsm_action_queues *queues,
                                                unsigned long cookie);


/* redis */

int redis_connect(struct state *state);
int redis_insert(struct state *state, struct hsm_action_node *han);
int redis_delete(struct state *state, uint64_t cookie);


/* scheduler */

void hsm_action_node_enrich(struct state *state, struct hsm_action_node *han);
void ct_schedule(struct state *state);
void ct_schedule_client(struct state *state,
			struct client *client);


/* tcp */

int tcp_listen(struct state *state);
char *sockaddr2str(struct sockaddr_storage *addr, socklen_t len);
int handle_client_connect(struct state *state);
void free_client(struct state *state, struct client *client);

#endif
