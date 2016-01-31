/*
 * dvpn, a multipoint vpn implementation
 * Copyright (C) 2016 Lennert Buytenhek
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version
 * 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License version 2.1 for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License version 2.1 along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street - Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <gnutls/x509.h>
#include <iv.h>
#include <iv_signal.h>
#include <string.h>
#include "adj_rib_in.h"
#include "conf.h"
#include "dgp_connect.h"
#include "dgp_listen.h"
#include "loc_rib.h"
#include "lsa.h"
#include "lsa_deserialise.h"
#include "lsa_diff.h"
#include "lsa_type.h"
#include "rib_listener_debug.h"
#include "rib_listener_to_loc.h"
#include "x509.h"

struct dgp_peer {
	struct iv_avl_node	an;
	uint8_t			id[NODE_ID_LEN];
	uint8_t			addr[16];
	struct dgp_listen_entry		dle;
	struct dgp_connect	*dc;
	struct iv_task		kill_task;
};

static uint8_t local_id[NODE_ID_LEN];

static struct loc_rib loc_rib;
static struct rib_listener_debug loc_rib_debug_listener;

static struct iv_fd local_query_fd;
static struct sockaddr_in6 local_query_addr;
static struct iv_timer local_query_timer;
static struct adj_rib_in local_adj_rib_in;
static struct rib_listener_to_loc local_to_loc_listener;

static struct rib_listener peer_listener;
static struct iv_avl_tree peers;

static struct dgp_listen_socket dls;

static struct iv_signal sigint;

static void read_local_id(const char *config)
{
	struct conf *conf;
	gnutls_x509_privkey_t key;

	conf = parse_config(config);
	if (conf == NULL)
		abort();

	if (x509_read_privkey(&key, conf->private_key) < 0)
		abort();

	free_config(conf);

	x509_get_key_id(local_id, key);

	gnutls_x509_privkey_deinit(key);
}

static int compare_peers(struct iv_avl_node *_a, struct iv_avl_node *_b)
{
	struct dgp_peer *a = iv_container_of(_a, struct dgp_peer, an);
	struct dgp_peer *b = iv_container_of(_b, struct dgp_peer, an);

	return memcmp(a->id, b->id, NODE_ID_LEN);
}

static void peer_del(struct dgp_peer *peer)
{
	if (peer->dc == NULL) {
		dgp_listen_entry_unregister(&peer->dle);
	} else {
		dgp_connect_stop(peer->dc);
		free(peer->dc);
	}

	if (iv_task_registered(&peer->kill_task))
		iv_task_unregister(&peer->kill_task);

	free(peer);
}

static void kill_peer(void *_peer)
{
	struct dgp_peer *peer = _peer;

	peer_del(peer);
}

static void peer_attr_add(void *_dummy, struct lsa_attr *attr)
{
	struct dgp_peer *peer;

	if (attr->type != LSA_ATTR_TYPE_PEER)
		return;

	peer = malloc(sizeof(*peer));
	if (peer == NULL)
		abort();

	memcpy(peer->id, lsa_attr_key(attr), NODE_ID_LEN);

	v6_global_addr_from_key_id(peer->addr, peer->id, NODE_ID_LEN);

	if (memcmp(local_id, peer->id, NODE_ID_LEN) < 0) {
		peer->dle.dls = &dls;
		peer->dle.remoteid = peer->id;
		dgp_listen_entry_register(&peer->dle);

		peer->dc = NULL;
	} else {
		struct dgp_connect *dc;

		dc = malloc(sizeof(*dc));
		if (dc == NULL)
			abort();

		dc->myid = local_id;
		dc->remoteid = peer->id;
		dc->loc_rib = &loc_rib;
		dgp_connect_start(dc);

		peer->dc = dc;
	}

	IV_TASK_INIT(&peer->kill_task);
	peer->kill_task.cookie = peer;
	peer->kill_task.handler = kill_peer;

	iv_avl_tree_insert(&peers, &peer->an);
}

static struct dgp_peer *peer_find_by_id(uint8_t *id)
{
	struct iv_avl_node *an;

	an = peers.root;
	while (an != NULL) {
		struct dgp_peer *peer;
		int ret;

		peer = iv_container_of(an, struct dgp_peer, an);

		ret = memcmp(id, peer->id, NODE_ID_LEN);
		if (ret == 0)
			return peer;

		if (ret < 0)
			an = an->left;
		else
			an = an->right;
	}

	return NULL;
}

static void peer_attr_del(void *_dummy, struct lsa_attr *attr)
{
	struct dgp_peer *peer;

	if (attr->type != LSA_ATTR_TYPE_PEER)
		return;

	peer = peer_find_by_id(lsa_attr_key(attr));
	if (peer == NULL)
		abort();

	iv_avl_tree_delete(&peers, &peer->an);

	iv_task_register(&peer->kill_task);
}

static void peer_lsa_add(void *_dummy, struct lsa *lsa)
{
	lsa_diff(NULL, lsa, NULL, peer_attr_add, NULL, peer_attr_del);
}

static void peer_lsa_mod(void *_dummy, struct lsa *old, struct lsa *new)
{
	lsa_diff(old, new, NULL, peer_attr_add, NULL, peer_attr_del);
}

static void peer_lsa_del(void *_dummy, struct lsa *lsa)
{
	lsa_diff(lsa, NULL, NULL, peer_attr_add, NULL, peer_attr_del);
}

static void got_response(void *_dummy)
{
	uint8_t buf[65536];
	struct sockaddr_storage recvaddr;
	socklen_t addrlen;
	int ret;
	struct lsa *lsa;

	addrlen = sizeof(recvaddr);

	ret = recvfrom(local_query_fd.fd, buf, sizeof(buf), 0,
			(struct sockaddr *)&recvaddr, &addrlen);
	if (ret < 0) {
		perror("recvfrom");
		return;
	}

	lsa = lsa_deserialise(buf, ret);
	if (lsa == NULL) {
		fprintf(stderr, "error deserialising LSA\n");
		adj_rib_in_flush(&local_adj_rib_in);
		return;
	}

	if (memcmp(lsa->id, local_id, NODE_ID_LEN)) {
		fprintf(stderr, "node ID mismatch\n");
		lsa_put(lsa);
		return;
	}

	adj_rib_in_add_lsa(&local_adj_rib_in, lsa);

	lsa_put(lsa);
}

static void query_timer_expiry(void *_dummy)
{
	uint8_t buf[1];

	local_query_timer.expires.tv_nsec += 100000000;
	if (local_query_timer.expires.tv_nsec >= 1000000000) {
		local_query_timer.expires.tv_sec++;
		local_query_timer.expires.tv_nsec -= 1000000000;
	}
	iv_timer_register(&local_query_timer);

	if (sendto(local_query_fd.fd, buf, 0, 0,
		   (struct sockaddr *)&local_query_addr,
		   sizeof(local_query_addr)) < 0) {
		perror("sendto");
		return;
	}
}

static void query_start(void)
{
	int fd;
	uint8_t addr[16];

	fd = socket(AF_INET6, SOCK_DGRAM, 0);
	if (fd < 0) {
		perror("socket");
		exit(1);
	}

	IV_FD_INIT(&local_query_fd);
	local_query_fd.fd = fd;
	local_query_fd.handler_in = got_response;
	iv_fd_register(&local_query_fd);

	v6_global_addr_from_key_id(addr, local_id, NODE_ID_LEN);

	local_query_addr.sin6_family = AF_INET6;
	local_query_addr.sin6_port = htons(19275);
	local_query_addr.sin6_flowinfo = 0;
	memcpy(&local_query_addr.sin6_addr, addr, 16);
	local_query_addr.sin6_scope_id = 0;

	IV_TIMER_INIT(&local_query_timer);
	iv_validate_now();
	local_query_timer.expires = iv_now;
	local_query_timer.handler = query_timer_expiry;
	iv_timer_register(&local_query_timer);

	local_adj_rib_in.myid = NULL;
	local_adj_rib_in.remoteid = local_id;
	adj_rib_in_init(&local_adj_rib_in);

	local_to_loc_listener.dest = &loc_rib;
	rib_listener_to_loc_init(&local_to_loc_listener);
	adj_rib_in_listener_register(&local_adj_rib_in,
				     &local_to_loc_listener.rl);
}

static void got_sigint(void *_dummy)
{
	fprintf(stderr, "SIGINT received, shutting down\n");

	iv_fd_unregister(&local_query_fd);
	iv_timer_unregister(&local_query_timer);
	adj_rib_in_flush(&local_adj_rib_in);
	rib_listener_to_loc_deinit(&local_to_loc_listener);

	dgp_listen_socket_unregister(&dls);

	iv_signal_unregister(&sigint);
}

int main(int argc, char *argv[])
{
	static struct option long_options[] = {
		{ "config-file", required_argument, 0, 'c' },
		{ 0, 0, 0, 0, },
	};
	const char *config = "/etc/dvpn.ini";

	while (1) {
		int c;

		c = getopt_long(argc, argv, "c:", long_options, NULL);
		if (c == -1)
			break;

		switch (c) {
		case 'c':
			config = optarg;
			break;

		case '?':
			fprintf(stderr, "syntax: %s [-c <config.ini>]\n",
				argv[0]);
			return 1;

		default:
			abort();
		}
	}

	iv_init();

	gnutls_global_init();
	read_local_id(config);
	gnutls_global_deinit();

	loc_rib_init(&loc_rib);

	loc_rib_debug_listener.name = "loc-rib";
	rib_listener_debug_init(&loc_rib_debug_listener);
	loc_rib_listener_register(&loc_rib, &loc_rib_debug_listener.rl);

	query_start();

	peer_listener.lsa_add = peer_lsa_add;
	peer_listener.lsa_mod = peer_lsa_mod;
	peer_listener.lsa_del = peer_lsa_del;
	loc_rib_listener_register(&loc_rib, &peer_listener);

	INIT_IV_AVL_TREE(&peers, compare_peers);

	dls.myid = local_id;
	dls.loc_rib = &loc_rib;
	dgp_listen_socket_register(&dls);

	IV_SIGNAL_INIT(&sigint);
	sigint.signum = SIGINT;
	sigint.flags = 0;
	sigint.cookie = NULL;
	sigint.handler = got_sigint;
	iv_signal_register(&sigint);

	iv_main();

	loc_rib_deinit(&loc_rib);

	rib_listener_debug_deinit(&loc_rib_debug_listener);

	iv_deinit();

	return 0;
}
