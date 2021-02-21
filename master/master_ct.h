/* SPDX-License-Identifier: LGPL-3.0-or-later */

#ifndef MASTER_CT_H
#define MASTER_CT_H

// for urcu slight optimization
#define _LGPL_SOURCE

#include <errno.h>
#include <linux/lustre/lustre_idl.h>
#include <lustre/lustreapi.h>
#include <sys/socket.h>
#include <urcu/wfcqueue.h>

#include "logs.h"
#include "protocol.h"


/* queue types */

struct hsm_action_node {
	struct cds_wfcq_node node;
	/* hsm_action_item is variable length and MUST be last */
	struct hsm_action_item hai;
};

struct hsm_action_queues {
	struct cds_wfcq_head restore_head;
	struct cds_wfcq_tail restore_tail;
	struct cds_wfcq_head archive_head;
	struct cds_wfcq_tail archive_tail;
	struct cds_wfcq_head remove_head;
	struct cds_wfcq_tail remove_tail;
	struct cds_wfcq_head running_head;
	struct cds_wfcq_tail running_tail;
};

/* common types */

struct state {
	// options
	const char *mntpath;
	int archive_cnt;
	int archive_id[LL_HSM_MAX_ARCHIVES_PER_AGENT];
	const char *host;
	const char *port;
	// states value
	struct hsm_copytool_private *ctdata;
	int epoll_fd;
	int hsm_fd;
	int listen_fd;
	struct hsm_action_queues queues;
	struct ct_stats stats;
};



/* lhsm */

int handle_ct_event(struct state *state);
int ct_register(struct state *state);
int ct_start(struct state *state);

/* protocol */
extern protocol_read_cb protocol_cbs[];

/* tcp */

int tcp_listen(struct state *state);
char *sockaddr2str(struct sockaddr_storage *addr, socklen_t len);
int handle_client_connect(struct state *state);

/* queue */

void hsm_action_queues_init(struct hsm_action_queues *queues);
int hsm_action_enqueue(struct state *state,
		       struct hsm_action_item *hai);
struct hsm_action_item *hsm_action_dequeue(struct state *state,
					   enum hsm_copytool_action action);

/* common */

int epoll_addfd(int epoll_fd, int fd);
int epoll_delfd(int epoll_fd, int fd);

#endif
