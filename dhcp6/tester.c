/*
 *	wicked dhcp6 in test (request offer/lease) mode
 *
 *	Copyright (C) 2013-2014 SUSE LINUX Products GmbH, Nuernberg, Germany.
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License along
 *	with this program; if not, see <http://www.gnu.org/licenses/> or write
 *	to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *	Boston, MA 02110-1301 USA.
 *
 *	Authors:
 *		Marius Tomaschewski <mt@suse.de>
 *
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include <net/if_arp.h>

#include <wicked/types.h>
#include <wicked/logging.h>
#include <wicked/leaseinfo.h>
#include <wicked/xml.h>

#include "dhcp6/dhcp6.h"
#include "dhcp6/tester.h"
#include "duid.h"


static int			dhcp6_tester_status;


static void
dhcp6_tester_protocol_event(enum ni_dhcp6_event ev, const ni_dhcp6_device_t *dev,
				ni_addrconf_lease_t *lease)
{
	ni_debug_dhcp("%s(ev=%u, dev=%s[%u], config-uuid=%s)", __func__, ev,
			dev->ifname, dev->link.ifindex,
			dev->config ? ni_uuid_print(&dev->config->uuid) : "<none>");

	switch (ev) {
	case NI_DHCP6_EVENT_ACQUIRED:
		if (lease && lease->state == NI_ADDRCONF_STATE_GRANTED) {
			ni_leaseinfo_dump(stdout, lease, dev->ifname, NULL);
			dhcp6_tester_status = 0;
		}
		break;
	default:
		break;
	}
}

static ni_bool_t
dhcp6_tester_req_xml_init(ni_dhcp6_request_t *req, xml_document_t *doc)
{
	xml_node_t *xml, *child;
	const char *type;

	xml = xml_document_root(doc);
	if (xml && !xml->name && xml->children)
		xml = xml->children;

	/* TODO: parse using /ipv6:dhcp/request xml schema */
	if (!xml || !ni_string_eq(xml->name, "request")) {
		ni_error("Invalid dhcp6 request xml '%s'",
			xml ? xml_node_location(xml) : NULL);
		return FALSE;
	}

	type = xml_node_get_attr(xml, "type");
	if (ni_string_eq(type, "offer")) {
		req->dry_run = NI_DHCP6_RUN_OFFER;
	} else
	if (ni_string_eq(type, "lease")) {
		req->dry_run = NI_DHCP6_RUN_LEASE;
	}

	for (child = xml->children; child; child = child->next) {
		if (ni_string_eq(child->name, "uuid")) {
			if (ni_uuid_parse(&req->uuid, child->cdata) != 0)
				goto failure;
		} else
		if (ni_string_eq(child->name, "mode")) {
			if (ni_dhcp6_mode_name_to_type(child->cdata, &req->mode) != 0)
				goto failure;
		} else
		if (ni_string_eq(child->name, "acquire-timeout")) {
			if (ni_parse_uint(child->cdata, &req->acquire_timeout, 10) != 0)
				goto failure;
		} else
		if (ni_string_eq(child->name, "hostname")) {
			if (!ni_check_domain_name(child->cdata, ni_string_len(child->cdata), 0))
				goto failure;
			ni_string_dup(&req->hostname, child->cdata);
		} else
		if (ni_string_eq(child->name, "clientid")) {
			ni_opaque_t duid;

			ni_duid_clear(&duid);
			if (!ni_duid_parse_hex(&duid, child->cdata))
				goto failure;
			ni_string_dup(&req->clientid, child->cdata);
		}
	}

	return TRUE;
failure:
	if (child) {
		ni_error("Cannot parse dhcp6 request '%s' at %s: %s",
			child->name, xml_node_location(child), child->cdata);
	}
	return FALSE;
}

static ni_bool_t
dhcp6_tester_req_init(ni_dhcp6_request_t *req, const char *request)
{
	/* Apply some defaults */
	req->dry_run = NI_DHCP6_RUN_OFFER;
	req->acquire_timeout = 10;
	req->mode = NI_DHCP6_MODE_MANAGED;

	if (!ni_string_empty(request)) {
		xml_document_t *doc;

		if (!(doc = xml_document_read(request))) {
			ni_error("Cannot parse dhcp6 request xml '%s'", request);
			return FALSE;
		}

		if (!dhcp6_tester_req_xml_init(req, doc)) {
			xml_document_free(doc);
			return FALSE;
		}
		xml_document_free(doc);
	}

	/* Always enter dry run mode & disable rapid-commit */
	req->rapid_commit = FALSE;
	if (ni_uuid_is_null(&req->uuid))
		ni_uuid_generate(&req->uuid);

	return TRUE;
}

int
dhcp6_tester_run(const char *ifname, const char *request, unsigned int timeout)
{
	ni_netconfig_t *nc;
	ni_netdev_t *ifp;
	ni_dhcp6_device_t *dev;
	ni_dhcp6_request_t *req;
	char *errdetail = NULL;
	int rv;

	dhcp6_tester_status = 2;

	if (!(nc = ni_global_state_handle(1)))
		ni_fatal("Cannot refresh interface list!");

	if (!(ifp = ni_netdev_by_name(nc, ifname)))
		ni_fatal("Cannot find interface with name '%s'", ifname);

	switch (ifp->link.hwaddr.type) {
	case ARPHRD_ETHER:
		break;
	default:
		ni_fatal("Interface type not supported yet");
		break;
	}

	if (!(dev = ni_dhcp6_device_new(ifp->name, &ifp->link)))
		ni_fatal("Cannot allocate dhcp6 client for '%s'", ifname);

	ni_dhcp6_set_event_handler(dhcp6_tester_protocol_event);

	if (!(req = ni_dhcp6_request_new())) {
		ni_error("Cannot allocate dhcp6 request");
		goto failure;
	}

	if (!dhcp6_tester_req_init(req, request))
		goto failure;

	if (timeout != -1U)
		req->acquire_timeout = timeout;

	if ((rv = ni_dhcp6_acquire(dev, req, &errdetail)) < 0) {
		ni_error("%s: DHCPv6 acquire request %s failed: %s%s[%s]",
				dev->ifname, ni_uuid_print(&req->uuid),
				(errdetail ? errdetail : ""),
				(errdetail ? " " : ""),
				ni_strerror(rv));
		ni_string_free(&errdetail);
		goto failure;
	}

	while (!ni_caught_terminal_signal()) {
		long timeout;

		timeout = ni_timer_next_timeout();

		if (ni_socket_wait(timeout) != 0)
			break;
	}
	ni_server_deactivate_interface_events();
	ni_socket_deactivate_all();

failure:
	ni_dhcp6_device_put(dev);
	ni_dhcp6_request_free(req);
	return dhcp6_tester_status;
}
