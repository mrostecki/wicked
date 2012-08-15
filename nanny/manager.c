/*
 * This daemon manages interface policies.
 *
 * Copyright (C) 2010-2012 Olaf Kirch <okir@suse.de>
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/poll.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <limits.h>
#include <errno.h>

#include <wicked/netinfo.h>
#include <wicked/addrconf.h>
#include <wicked/logging.h>
#include <wicked/wicked.h>
#include <wicked/socket.h>
#include <wicked/objectmodel.h>
#include <wicked/modem.h>
#include <wicked/dbus-service.h>
#include <wicked/dbus-errors.h>
#include <wicked/fsm.h>
#include "manager.h"


struct ni_manager_secret {
	ni_manager_secret_t *	next;

	char *			security_id;
	char *			path;
	char *			value;
};

static int		ni_manager_prompt(const ni_fsm_prompt_t *, xml_node_t *, void *);

/*
 * Initialize the manager objectmodel
 */
void
ni_objectmodel_manager_init(ni_manager_t *mgr)
{
	ni_dbus_object_t *root_object;

	ni_objectmodel_managed_policy_init(mgr->server);
	ni_objectmodel_managed_netif_init(mgr->server);
	ni_objectmodel_managed_modem_init(mgr->server);

	ni_objectmodel_register_service(&ni_objectmodel_manager_service);

	root_object = ni_dbus_server_get_root_object(mgr->server);
	root_object->handle = mgr;
	root_object->class = &ni_objectmodel_manager_class;
	ni_objectmodel_bind_compatible_interfaces(root_object);

	{
		unsigned int i;

		ni_trace("%s supports:", ni_dbus_object_get_path(root_object));
		for (i = 0; root_object->interfaces[i]; ++i) {
			const ni_dbus_service_t *service = root_object->interfaces[i];

			ni_trace("  %s", service->name);
		}
		ni_trace("(%u interfaces)", i);
	}
}

ni_manager_t *
ni_manager_new(void)
{
	ni_manager_t *mgr;

	mgr = calloc(1, sizeof(*mgr));

	mgr->server = ni_server_listen_dbus(NI_OBJECTMODEL_DBUS_BUS_NAME_MANAGER);
	if (!mgr->server)
		ni_fatal("Cannot create server, giving up.");

	mgr->fsm = ni_fsm_new();

	ni_fsm_set_user_prompt_fn(mgr->fsm, ni_manager_prompt, mgr);

	ni_objectmodel_manager_init(mgr);
	ni_objectmodel_register_all();

	return mgr;
}

/*
 * (Re-) check a device to see if the applicable policy changed.
 * This is a multi-stage process.
 * One, devices can be scheduled for a recheck explicitly (eg when they
 * appear via hotplug).
 *
 * Two, all enabled devices are checked when policies have been updated.
 *
 * Both checks happen once per mainloop iteration.
 */
void
ni_manager_schedule_recheck(ni_manager_t *mgr, ni_ifworker_t *w)
{
	if (ni_ifworker_array_index(&mgr->recheck, w) < 0)
		ni_ifworker_array_append(&mgr->recheck, w);
}

void
ni_manager_recheck_do(ni_manager_t *mgr)
{
	unsigned int i;

	if (ni_fsm_policies_changed_since(mgr->fsm, &mgr->last_policy_seq)) {
		ni_managed_device_t *mdev;

		for (mdev = mgr->netdev_list; mdev; mdev = mdev->next) {
			if (mdev->user_controlled)
				ni_manager_schedule_recheck(mgr, mdev->worker);
		}
		for (mdev = mgr->modem_list; mdev; mdev = mdev->next) {
			ni_manager_schedule_recheck(mgr, mdev->worker);
		}
	}

	if (mgr->recheck.count == 0)
		return;

	ni_fsm_refresh_state(mgr->fsm);

	for (i = 0; i < mgr->recheck.count; ++i)
		ni_manager_recheck(mgr, mgr->recheck.data[i]);
	ni_ifworker_array_destroy(&mgr->recheck);
}

/*
 * Check whether a given interface should be reconfigured
 */
void
ni_manager_recheck(ni_manager_t *mgr, ni_ifworker_t *w)
{
	static const unsigned int MAX_POLICIES = 20;
	const ni_fsm_policy_t *policies[MAX_POLICIES];
	const ni_fsm_policy_t *policy;
	ni_managed_policy_t *mpolicy;
	unsigned int count;

	/* Ignore devices that went away */
	if (w->dead)
		return;

	ni_trace("%s(%s)", __func__, w->name);
	w->use_default_policies = TRUE;
	if ((count = ni_fsm_policy_get_applicable_policies(mgr->fsm, w, policies, MAX_POLICIES)) == 0) {
		ni_trace("%s: no applicable policies", w->name);
		return;
	}

	policy = policies[count-1];
	mpolicy = ni_manager_get_policy(mgr, policy);

	ni_manager_apply_policy(mgr, mpolicy, w);
}

/*
 * Taking down an interface
 */
void
ni_manager_schedule_down(ni_manager_t *mgr, ni_ifworker_t *w)
{
	if (ni_ifworker_array_index(&mgr->down, w) < 0)
		ni_ifworker_array_append(&mgr->down, w);
}

void
ni_manager_down_do(ni_manager_t *mgr)
{
	unsigned int i;

	if (mgr->down.count == 0)
		return;

	for (i = 0; i < mgr->down.count; ++i)
		; // ni_manager_down(mgr, mgr->down.data[i]);
	ni_ifworker_array_destroy(&mgr->down);
}

ni_managed_policy_t *
ni_manager_get_policy(ni_manager_t *mgr, const ni_fsm_policy_t *policy)
{
	ni_managed_policy_t *mpolicy;

	for (mpolicy = mgr->policy_list; mpolicy; mpolicy = mpolicy->next) {
		if (mpolicy->fsm_policy == policy)
			return mpolicy;
	}
	return NULL;
}

/*
 * Register a device
 */
void
ni_manager_register_device(ni_manager_t *mgr, ni_ifworker_t *w)
{
	ni_managed_device_t *mdev;

	if (ni_manager_get_device(mgr, w) != NULL)
		return;

	if (w->type == NI_IFWORKER_TYPE_NETDEV) {
		mdev = ni_managed_device_new(mgr, w, &mgr->netdev_list);
		mdev->object = ni_objectmodel_register_managed_netdev(mgr->server, mdev);

	} else
	if (w->type == NI_IFWORKER_TYPE_MODEM) {
		mdev = ni_managed_device_new(mgr, w, &mgr->modem_list);
		mdev->object = ni_objectmodel_register_managed_modem(mgr->server, mdev);

		/* FIXME: for now, we allow users to control all modems */
		mdev->user_controlled = TRUE;
	}
}

/*
 * Unregister a device
 */
void
ni_manager_unregister_device(ni_manager_t *mgr, ni_ifworker_t *w)
{
	ni_managed_device_t *mdev = NULL;

	if ((mdev = ni_manager_get_device(mgr, w)) == NULL) {
		ni_error("%s: cannot unregister; device not known", w->name);
		return;
	}

	ni_manager_remove_device(mgr, mdev);
	ni_objectmodel_unregister_managed_device(mdev);
	ni_fsm_destroy_worker(mgr->fsm, w);
}

/*
 * Apply the selected policy to this worker
 */
void
ni_manager_apply_policy(ni_manager_t *mgr, ni_managed_policy_t *mpolicy, ni_ifworker_t *w)
{
	ni_managed_device_t *mdev;

	if ((mdev = ni_manager_get_device(mgr, w)) == NULL)
		return;

	ni_managed_device_apply_policy(mdev, mpolicy);
}

/*
 * Handle prompting
 */
static ni_ifworker_t *
ni_manager_identify_node_owner(ni_manager_t *mgr, xml_node_t *node, ni_stringbuf_t *path)
{
	ni_managed_device_t *mdev;
	ni_ifworker_t *w = NULL;

	for (mdev = mgr->netdev_list; mdev; mdev = mdev->next) {
		if (mdev->selected_config == node) {
			w = mdev->worker;
			goto found;
		}
	}
	for (mdev = mgr->modem_list; mdev; mdev = mdev->next) {
		if (mdev->selected_config == node) {
			w = mdev->worker;
			goto found;
		}
	}

	if (node != NULL)
		w = ni_manager_identify_node_owner(mgr, node->parent, path);

	if (w == NULL)
		return NULL;

found:
	ni_stringbuf_putc(path, '/');
	ni_stringbuf_puts(path, node->name);
	return w;
}

int
ni_manager_prompt(const ni_fsm_prompt_t *p, xml_node_t *node, void *user_data)
{
	ni_stringbuf_t path_buf;
	ni_manager_t *mgr = user_data;
	ni_ifworker_t *w = NULL;
	const char *value;
	int rv = -1;

	ni_trace("%s: type=%u string=%s id=%s", __func__, p->type, p->string, p->id);

	ni_stringbuf_init(&path_buf);

	w = ni_manager_identify_node_owner(mgr, node, &path_buf);
	if (w == NULL) {
		ni_error("%s: unable to identify device owning this config", __func__);
		goto done;
	}

	if (w->security_id == NULL) {
		ni_error("%s: no security id set, cannot handle prompt for \"%s\"",
				w->name, path_buf.string);
		goto done;
	}

	value = ni_manager_get_secret(mgr, w->security_id, path_buf.string);
	if (value == NULL) {
		/* FIXME: Send out event that we need this piece of information */
		ni_trace("%s: prompting for type=%u id=%s path=%s",
				w->name, p->type, w->security_id, path_buf.string);
		goto done;
	}

	xml_node_set_cdata(node, value);
	rv = 0;

done:
	ni_stringbuf_destroy(&path_buf);
	return rv;
}

/*
 * Handle nanny's security database
 */
ni_manager_secret_t **
__ni_manager_find_secret(ni_manager_t *mgr, const char *security_id, const char *path)
{
	ni_manager_secret_t *sec, **pos;

	for (pos = &mgr->secret_db; (sec = *pos) != NULL; pos = &sec->next) {
		if (ni_string_eq(sec->security_id, security_id)
		 && ni_string_eq(sec->path, path))
			break;
	}

	return pos;
}

static ni_manager_secret_t *
ni_manager_secret_new(const char *security_id, const char *path)
{
	ni_manager_secret_t *sec;

	sec = calloc(1, sizeof(*sec));
	ni_string_dup(&sec->security_id, security_id);
	ni_string_dup(&sec->path, path);
	return sec;
}

static void
ni_manager_secret_free(ni_manager_secret_t *sec)
{
	ni_string_free(&sec->security_id);
	ni_string_free(&sec->path);
	ni_string_free(&sec->value);
	free(sec);
}

void
ni_manager_add_secret(ni_manager_t *mgr, const char *security_id, const char *path, const char *value)
{
	ni_manager_secret_t *sec, **pos;
	ni_managed_device_t *mmod;

	pos = __ni_manager_find_secret(mgr, security_id, path);
	if ((sec = *pos) == NULL)
		*pos = sec = ni_manager_secret_new(security_id, path);

	ni_string_dup(&sec->value, value);

	ni_trace("%s: secret for %s updated", security_id, path);
	for (mmod = mgr->modem_list; mmod; mmod = mmod->next) {
		ni_ifworker_t *w = mmod->worker;

		ni_trace("%s: security-id=%s", w->name, w->security_id);
		if (w && ni_string_eq(w->security_id, security_id)
		 && !ni_ifworker_is_running(w)) {
			ni_trace("%s: secret for %s updated, rechecking", w->name, path);
			ni_manager_schedule_recheck(mgr, w);
		}
	}
}

void
ni_manager_clear_secrets(ni_manager_t *mgr, const char *security_id, const char *path)
{
	ni_manager_secret_t *sec, **pos;

	ni_trace("%s(%s, %s)", __func__, security_id, path);
	for (pos = &mgr->secret_db; (sec = *pos) != NULL; ) {
		if ((security_id == NULL || ni_string_eq(sec->security_id, security_id))
		 && (path == NULL || ni_string_eq(sec->path, path))) {
			*pos = sec->next;

			ni_manager_secret_free(sec);
		} else {
			pos = &sec->next;
		}
	}
}

const char *
ni_manager_get_secret(ni_manager_t *mgr, const char *security_id, const char *path)
{
	ni_manager_secret_t *sec, **pos;

	pos = __ni_manager_find_secret(mgr, security_id, path);
	if ((sec = *pos) == NULL)
		return NULL;

	return sec->value;
}

/*
 * Extract fsm handle from dbus object
 */
static ni_manager_t *
ni_objectmodel_manager_unwrap(const ni_dbus_object_t *object, DBusError *error)
{
	ni_manager_t *mgr = object->handle;

	if (ni_dbus_object_isa(object, &ni_objectmodel_manager_class))
		return mgr;

	if (error)
		dbus_set_error(error, DBUS_ERROR_FAILED,
			"method not compatible with object %s of class %s",
			object->path, object->class->name);
	return NULL;
}

/*
 * Manager.getDevice(devname)
 */
static dbus_bool_t
ni_objectmodel_manager_get_device(ni_dbus_object_t *object, const ni_dbus_method_t *method,
					unsigned int argc, const ni_dbus_variant_t *argv,
					ni_dbus_message_t *reply, DBusError *error)
{
	ni_manager_t *mgr;
	const char *ifname;
	ni_ifworker_t *w;
	ni_managed_device_t *mdev = NULL;

	if ((mgr = ni_objectmodel_manager_unwrap(object, error)) == NULL)
		return FALSE;

	if (argc != 1 || !ni_dbus_variant_get_string(&argv[0], &ifname))
		return ni_dbus_error_invalid_args(error, ni_dbus_object_get_path(object), method->name);

	/* XXX: scalability. Use ni_call_identify_device() */
	ni_fsm_refresh_state(mgr->fsm);
	w = ni_fsm_ifworker_by_name(mgr->fsm, NI_IFWORKER_TYPE_NETDEV, ifname);

	if (w)
		mdev = ni_manager_get_device(mgr, w);

	if (mdev == NULL) {
		dbus_set_error(error, NI_DBUS_ERROR_DEVICE_NOT_KNOWN, "No such device: %s", ifname);
		return FALSE;
	} else {
		ni_netdev_t *dev = ni_ifworker_get_netdev(w);
		char object_path[128];

		snprintf(object_path, sizeof(object_path),
				NI_OBJECTMODEL_MANAGED_NETIF_LIST_PATH "/%u",
				dev->link.ifindex);

		ni_dbus_message_append_object_path(reply, object_path);
	}
	return TRUE;
}


/*
 * Manager.createPolicy()
 */
static dbus_bool_t
ni_objectmodel_manager_create_policy(ni_dbus_object_t *object, const ni_dbus_method_t *method,
					unsigned int argc, const ni_dbus_variant_t *argv,
					ni_dbus_message_t *reply, DBusError *error)
{
	ni_dbus_object_t *policy_object;
	ni_manager_t *mgr;
	ni_fsm_policy_t *policy;
	const char *name;
	char namebuf[64];

	if ((mgr = ni_objectmodel_manager_unwrap(object, error)) == NULL)
		return FALSE;

	if (argc != 1 || !ni_dbus_variant_get_string(&argv[0], &name))
		return ni_dbus_error_invalid_args(error, ni_dbus_object_get_path(object), method->name);

	if (*name == '\0') {
		static unsigned int counter = 0;

		do {
			snprintf(namebuf, sizeof(namebuf), "policy%u", counter++);
		} while (ni_fsm_policy_by_name(mgr->fsm, namebuf) == NULL);
		name = namebuf;
	}

#ifdef notyet
	if (!ni_policy_name_valid(name)) {
		dbus_set_error(error, DBUS_ERROR_INVALID_ARGS,
				"Bad policy name \"%s\" in call to %s.%s",
				name, ni_dbus_object_get_path(object), method->name);
		return FALSE;
	}
#endif

	if (ni_fsm_policy_by_name(mgr->fsm, name) != NULL) {
		dbus_set_error(error, NI_DBUS_ERROR_POLICY_EXISTS,
				"Policy \"%s\" already exists in call to %s.%s",
				name, ni_dbus_object_get_path(object), method->name);
		return FALSE;
	}

	policy = ni_fsm_policy_new(mgr->fsm, name, NULL);

	policy_object = ni_objectmodel_register_managed_policy(ni_dbus_object_get_server(object),
					ni_managed_policy_new(mgr, policy, NULL));

	ni_dbus_message_append_object_path(reply, ni_dbus_object_get_path(policy_object));
	return TRUE;
}

/*
 * Manager.addSecret(security-id, path, value)
 * The security-id is an identifier that is derived from eg the modem's IMEI,
 * or the wireless ESSID.
 */
static dbus_bool_t
ni_objectmodel_manager_set_secret(ni_dbus_object_t *object, const ni_dbus_method_t *method,
					unsigned int argc, const ni_dbus_variant_t *argv,
					ni_dbus_message_t *reply, DBusError *error)
{
	const char *security_id, *element_path, *value;
	ni_manager_t *mgr;

	if ((mgr = ni_objectmodel_manager_unwrap(object, error)) == NULL)
		return FALSE;

	if (argc != 3
	 || !ni_dbus_variant_get_string(&argv[0], &security_id)
	 || !ni_dbus_variant_get_string(&argv[1], &element_path)
	 || !ni_dbus_variant_get_string(&argv[2], &value))
		return ni_dbus_error_invalid_args(error, ni_dbus_object_get_path(object), method->name);

	ni_manager_add_secret(mgr, security_id, element_path, value);
	return TRUE;
}


static ni_dbus_method_t		ni_objectmodel_manager_methods[] = {
	{ "createPolicy",	"s",		ni_objectmodel_manager_create_policy	},
	{ "getDevice",		"s",		ni_objectmodel_manager_get_device	},
	{ "addSecret",		"sss",		ni_objectmodel_manager_set_secret	},
	{ NULL }
};

ni_dbus_class_t			ni_objectmodel_manager_class = {
	.name		= "manager",
};

ni_dbus_service_t		ni_objectmodel_manager_service = {
	.name		= NI_OBJECTMODEL_MANAGER_INTERFACE,
	.compatible	= &ni_objectmodel_manager_class,
	.methods	= ni_objectmodel_manager_methods
};