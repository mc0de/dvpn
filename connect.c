/*
 * dvpn, a multipoint vpn implementation
 * Copyright (C) 2015 Lennert Buytenhek
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
#include <errno.h>
#include <fcntl.h>
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#include <iv.h>
#include <netdb.h>
#include <string.h>
#include "conf.h"
#include "itf.h"
#include "iv_getaddrinfo.h"
#include "pconn.h"
#include "tun.h"
#include "x509.h"

struct server_peer
{
	struct conf_connect_entry	*ce;
	gnutls_x509_privkey_t		key;

	int			state;
	struct tun_interface	tun;
	struct iv_timer		rx_timeout;
	union {
		struct {
			struct addrinfo		hints;
			struct iv_getaddrinfo	addrinfo;
		};
		struct {
			struct addrinfo		*res;
			struct addrinfo		*rp;
			struct iv_fd		connectfd;
		};
		struct {
			struct pconn		pconn;
			struct iv_timer		keepalive_timer;
		};
	};
};

#define STATE_RESOLVE		1
#define STATE_CONNECT		2
#define STATE_TLS_HANDSHAKE	3
#define STATE_CONNECTED		4
#define STATE_WAITING_RETRY	5

#define RESOLVE_TIMEOUT		10
#define CONNECT_TIMEOUT		10
#define HANDSHAKE_TIMEOUT	10
#define KEEPALIVE_INTERVAL	30
#define RETRY_WAIT_TIME		10

static void printhex(const uint8_t *a, int len)
{
	int i;

	for (i = 0; i < len; i++) {
		printf("%.2x", a[i]);
		if (i < len - 1)
			printf(":");
	}
}

static int verify_key_id(void *_sp, const uint8_t *id, int len)
{
	struct server_peer *sp = _sp;
	struct conf_connect_entry *ce = sp->ce;

	printf("key id: ");
	printhex(id, len);
	printf("\n");

	return memcmp(ce->fingerprint, id, 20);
}

static void send_keepalive(void *_sp)
{
	static uint8_t keepalive[] = { 0x00, 0x00 };
	struct server_peer *sp = _sp;

	fprintf(stderr, "sending keepalive\n");

	if (sp->state != STATE_CONNECTED)
		abort();

	if (pconn_record_send(&sp->pconn, keepalive, 2)) {
		pconn_destroy(&sp->pconn);
		close(sp->pconn.fd);

		sp->state = STATE_WAITING_RETRY;

		iv_validate_now();

		iv_timer_unregister(&sp->rx_timeout);
		sp->rx_timeout.expires = iv_now;
		sp->rx_timeout.expires.tv_sec += RETRY_WAIT_TIME;
		iv_timer_register(&sp->rx_timeout);

		return;
	}

	sp->keepalive_timer.expires.tv_sec += KEEPALIVE_INTERVAL;
	iv_timer_register(&sp->keepalive_timer);
}

static void handshake_done(void *_sp)
{
	struct server_peer *sp = _sp;
	uint8_t id[64];

	fprintf(stderr, "handshake done\n");

	sp->state = STATE_CONNECTED;

	iv_validate_now();

	iv_timer_unregister(&sp->rx_timeout);
	sp->rx_timeout.expires = iv_now;
	sp->rx_timeout.expires.tv_sec += 1.5 * KEEPALIVE_INTERVAL;
	iv_timer_register(&sp->rx_timeout);

	IV_TIMER_INIT(&sp->keepalive_timer);
	sp->keepalive_timer.expires = iv_now;
	sp->keepalive_timer.expires.tv_sec += KEEPALIVE_INTERVAL;
	sp->keepalive_timer.cookie = sp;
	sp->keepalive_timer.handler = send_keepalive;
	iv_timer_register(&sp->keepalive_timer);

	x509_get_key_id(id + 2, sizeof(id) - 2, sp->key);

	id[0] = 0xfe;
	id[1] = 0x80;
	itf_add_v6(tun_interface_get_name(&sp->tun), id, 10);

	itf_set_state(tun_interface_get_name(&sp->tun), 1);
}

static void record_received(void *_sp, const uint8_t *rec, int len)
{
	struct server_peer *sp = _sp;
	int rlen;

	iv_validate_now();

	iv_timer_unregister(&sp->rx_timeout);
	sp->rx_timeout.expires = iv_now;

	if (len <= 2)
		goto out;

	rlen = (rec[0] << 8) | rec[1];
	if (rlen + 2 != len)
		goto out;

	if (tun_interface_send_packet(&sp->tun, rec + 2, rlen) < 0) {
		pconn_destroy(&sp->pconn);
		close(sp->pconn.fd);

		iv_timer_unregister(&sp->keepalive_timer);

		sp->state = STATE_WAITING_RETRY;

		sp->rx_timeout.expires.tv_sec += RETRY_WAIT_TIME;
		iv_timer_register(&sp->rx_timeout);

		return;
	}

out:
	sp->rx_timeout.expires.tv_sec += 1.5 * KEEPALIVE_INTERVAL;
	iv_timer_register(&sp->rx_timeout);
}

static void connection_lost(void *_sp)
{
	struct server_peer *sp = _sp;

	fprintf(stderr, "connection lost\n");

	pconn_destroy(&sp->pconn);
	close(sp->pconn.fd);

	if (sp->state == STATE_CONNECTED &&
	    iv_timer_registered(&sp->keepalive_timer)) {
		iv_timer_unregister(&sp->keepalive_timer);
	}

	itf_set_state(tun_interface_get_name(&sp->tun), 0);

	sp->state = STATE_WAITING_RETRY;

	iv_validate_now();

	iv_timer_unregister(&sp->rx_timeout);
	sp->rx_timeout.expires = iv_now;
	sp->rx_timeout.expires.tv_sec += RETRY_WAIT_TIME;
	iv_timer_register(&sp->rx_timeout);
}

static void connect_success(struct server_peer *sp, int fd)
{
	freeaddrinfo(sp->res);

	sp->state = STATE_TLS_HANDSHAKE;

	iv_validate_now();

	if (iv_timer_registered(&sp->rx_timeout))
		iv_timer_unregister(&sp->rx_timeout);
	sp->rx_timeout.expires = iv_now;
	sp->rx_timeout.expires.tv_sec += HANDSHAKE_TIMEOUT;
	iv_timer_register(&sp->rx_timeout);

	sp->pconn.fd = fd;
	sp->pconn.role = PCONN_ROLE_CLIENT;
	sp->pconn.key = sp->key;
	sp->pconn.cookie = sp;
	sp->pconn.verify_key_id = verify_key_id;
	sp->pconn.handshake_done = handshake_done;
	sp->pconn.record_received = record_received;
	sp->pconn.connection_lost = connection_lost;
	pconn_start(&sp->pconn);
}

static void try_connect(struct server_peer *sp)
{
	int fd;
	int ret;

	while (sp->rp != NULL) {
		fd = socket(sp->rp->ai_family, sp->rp->ai_socktype,
			    sp->rp->ai_protocol);

		if (fd >= 0) {
			fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

			ret = connect(fd, sp->rp->ai_addr, sp->rp->ai_addrlen);
			if (ret == 0 || errno == EINPROGRESS)
				break;

			perror("connect");
			close(fd);
		}

		sp->rp = sp->rp->ai_next;
	}

	if (sp->rp == NULL) {
		fprintf(stderr, "error connecting\n");

		freeaddrinfo(sp->res);

		fprintf(stderr, "retrying in %d seconds\n", RETRY_WAIT_TIME);

		sp->state = STATE_WAITING_RETRY;

		iv_validate_now();

		if (iv_timer_registered(&sp->rx_timeout))
			iv_timer_unregister(&sp->rx_timeout);
		sp->rx_timeout.expires = iv_now;
		sp->rx_timeout.expires.tv_sec += RETRY_WAIT_TIME;
		iv_timer_register(&sp->rx_timeout);

		return;
	}

	if (ret == 0) {
		connect_success(sp, fd);
	} else {
		iv_validate_now();

		if (iv_timer_registered(&sp->rx_timeout))
			iv_timer_unregister(&sp->rx_timeout);
		sp->rx_timeout.expires = iv_now;
		sp->rx_timeout.expires.tv_sec += CONNECT_TIMEOUT;
		iv_timer_register(&sp->rx_timeout);

		sp->connectfd.fd = fd;
		iv_fd_register(&sp->connectfd);
	}
}

static void connect_pollout(void *_sp)
{
	struct server_peer *sp = _sp;
	int fd;
	socklen_t len;
	int ret;

	fd = sp->connectfd.fd;

	len = sizeof(ret);
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &ret, &len) < 0) {
		perror("connect_pollout,getsockopt(SO_ERROR)");
		return;
	}

	if (ret == EINPROGRESS)
		return;

	iv_fd_unregister(&sp->connectfd);

	if (ret == 0) {
		connect_success(sp, fd);
	} else {
		fprintf(stderr, "connect: %s\n", strerror(ret));
		close(fd);

		sp->rp = sp->rp->ai_next;
		try_connect(sp);
	}
}

static void resolve_complete(void *_sp, int rc, struct addrinfo *res)
{
	struct server_peer *sp = _sp;

	fprintf(stderr, "resolve complete\n");

	if (rc == 0) {
		sp->state = STATE_CONNECT;

		sp->res = res;
		sp->rp = res;

		IV_FD_INIT(&sp->connectfd);
		sp->connectfd.cookie = sp;
		sp->connectfd.handler_out = connect_pollout;

		try_connect(sp);
	} else {
		fprintf(stderr, "resolving: %s\n", gai_strerror(rc));

		if (res != NULL)
			freeaddrinfo(res);

		fprintf(stderr, "retrying in %d seconds\n", RETRY_WAIT_TIME);

		sp->state = STATE_WAITING_RETRY;

		iv_validate_now();

		iv_timer_unregister(&sp->rx_timeout);
		sp->rx_timeout.expires = iv_now;
		sp->rx_timeout.expires.tv_sec += RETRY_WAIT_TIME;
		iv_timer_register(&sp->rx_timeout);
	}
}

static int start_resolve(struct server_peer *sp)
{
	if (sp->state != STATE_RESOLVE)
		abort();

	sp->hints.ai_family = PF_UNSPEC;
	sp->hints.ai_socktype = SOCK_STREAM;
	sp->hints.ai_protocol = 0;
	sp->hints.ai_flags = AI_ADDRCONFIG | AI_V4MAPPED | AI_NUMERICSERV;

	sp->addrinfo.node = sp->ce->hostname;
	sp->addrinfo.service = sp->ce->port;
	sp->addrinfo.hints = &sp->hints;
	sp->addrinfo.cookie = sp;
	sp->addrinfo.handler = resolve_complete;

	return iv_getaddrinfo_submit(&sp->addrinfo);
}

static void got_packet(void *_sp, uint8_t *buf, int len)
{
	struct server_peer *sp = _sp;
	uint8_t sndbuf[len + 2];

	if (sp->state != STATE_CONNECTED)
		return;

	iv_timer_unregister(&sp->keepalive_timer);

	sndbuf[0] = len >> 8;
	sndbuf[1] = len & 0xff;
	memcpy(sndbuf + 2, buf, len);

	iv_validate_now();

	if (pconn_record_send(&sp->pconn, sndbuf, len + 2)) {
		pconn_destroy(&sp->pconn);
		close(sp->pconn.fd);

		sp->state = STATE_WAITING_RETRY;

		iv_timer_unregister(&sp->rx_timeout);
		sp->rx_timeout.expires = iv_now;
		sp->rx_timeout.expires.tv_sec += RETRY_WAIT_TIME;
		iv_timer_register(&sp->rx_timeout);

		return;
	}

	sp->keepalive_timer.expires = iv_now;
	sp->keepalive_timer.expires.tv_sec += KEEPALIVE_INTERVAL;
	iv_timer_register(&sp->keepalive_timer);
}

static void rx_timeout_expired(void *_sp)
{
	struct server_peer *sp = _sp;

	iv_validate_now();

	sp->rx_timeout.expires = iv_now;

	if (sp->state == STATE_CONNECT) {
		iv_fd_unregister(&sp->connectfd);
		close(sp->connectfd.fd);

		sp->rp = sp->rp->ai_next;
		try_connect(sp);
	} else if (sp->state == STATE_WAITING_RETRY) {
		sp->state = STATE_RESOLVE;
		if (start_resolve(sp) == 0) {
			sp->rx_timeout.expires.tv_sec += RESOLVE_TIMEOUT;
		} else {
			sp->state = STATE_WAITING_RETRY;
			sp->rx_timeout.expires.tv_sec += RETRY_WAIT_TIME;
		}
		iv_timer_register(&sp->rx_timeout);
	} else {
		if (sp->state == STATE_RESOLVE) {
			iv_getaddrinfo_cancel(&sp->addrinfo);
		} else if (sp->state == STATE_TLS_HANDSHAKE) {
			pconn_destroy(&sp->pconn);
			close(sp->pconn.fd);
		} else if (sp->state == STATE_CONNECTED) {
			pconn_destroy(&sp->pconn);
			close(sp->pconn.fd);
		} else {
			abort();
		}

		sp->state = STATE_WAITING_RETRY;
		sp->rx_timeout.expires.tv_sec += RETRY_WAIT_TIME;
		iv_timer_register(&sp->rx_timeout);
	}
}

void *server_peer_add(struct conf_connect_entry *ce, gnutls_x509_privkey_t key)
{
	struct server_peer *sp;

	sp = malloc(sizeof(*sp));
	if (sp == NULL)
		return NULL;

	sp->ce = ce;
	sp->key = key;

	sp->state = STATE_RESOLVE;

	sp->tun.itfname = sp->ce->tunitf;
	sp->tun.cookie = sp;
	sp->tun.got_packet = got_packet;
	if (tun_interface_register(&sp->tun) < 0) {
		free(sp);
		return NULL;
	}

	IV_TIMER_INIT(&sp->rx_timeout);
	iv_validate_now();
	sp->rx_timeout.expires = iv_now;
	sp->rx_timeout.cookie = sp;
	sp->rx_timeout.handler = rx_timeout_expired;

	if (start_resolve(sp) < 0) {
		sp->rx_timeout.expires.tv_sec += RESOLVE_TIMEOUT;
	} else {
		sp->state = STATE_WAITING_RETRY;
		sp->rx_timeout.expires.tv_sec += RETRY_WAIT_TIME;
	}

	iv_timer_register(&sp->rx_timeout);

	return (void *)sp;
}

void server_peer_del(void *_sp)
{
	struct server_peer *sp = _sp;

	tun_interface_unregister(&sp->tun);

	iv_timer_unregister(&sp->rx_timeout);

	if (sp->state == STATE_RESOLVE) {
		iv_getaddrinfo_cancel(&sp->addrinfo);
	} else if (sp->state == STATE_CONNECT) {
		freeaddrinfo(sp->res);
		iv_fd_unregister(&sp->connectfd);
		close(sp->connectfd.fd);
	} else if (sp->state == STATE_TLS_HANDSHAKE) {
		pconn_destroy(&sp->pconn);
		close(sp->pconn.fd);
	} else if (sp->state == STATE_CONNECTED) {
		pconn_destroy(&sp->pconn);
		close(sp->pconn.fd);
		iv_timer_unregister(&sp->keepalive_timer);
	} else if (sp->state == STATE_WAITING_RETRY) {
	} else {
		abort();
	}

	free(sp);
}