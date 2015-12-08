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

#ifndef __CSPF_H
#define __CSPF_H

#include "spf.h"

struct cspf_node
{
	void			*cookie;

	struct spf_node		a;
	struct spf_node		b;
	struct spf_edge		ab;
};

struct cspf_edge
{
	struct spf_edge		e0;
	struct spf_edge		e1;
};

enum cspf_edge_type {
	EDGE_TYPE_EPEER = 0,
	EDGE_TYPE_CUSTOMER = 1,
	EDGE_TYPE_TRANSIT = 2,
	EDGE_TYPE_IPEER = 3,
};

void cspf_node_add(struct spf_context *ctx, struct cspf_node *node);
void cspf_edge_add(struct spf_context *ctx, struct cspf_edge *edge,
		   struct cspf_node *from, struct cspf_node *to,
		   enum cspf_edge_type type, int cost);
const char *cspf_edge_type_name(enum cspf_edge_type type);
void cspf_run(struct spf_context *ctx, struct cspf_node *source);
void *cspf_node_parent(struct cspf_node *node);
int cspf_node_cost(struct cspf_node *node);


#endif
