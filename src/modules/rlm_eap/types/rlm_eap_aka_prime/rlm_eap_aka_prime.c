/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or (at
 *   your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 * @file rlm_eap_aka_prime.c
 * @brief Implements EAP-AKA'
 *
 * @author Arran Cudbard-Bell <a.cudbardb@freeradius.org>
 *
 * @copyright 2019 The FreeRADIUS server project
 * @copyright 2019 Network RADIUS SARL <sales@networkradius.com>
 */
RCSID("$Id$")

#include <freeradius-devel/eap_aka_sim/state_machine.h>
#include <freeradius-devel/server/rad_assert.h>
#include <freeradius-devel/unlang/compile.h>
#include <freeradius-devel/unlang/module.h>

static int virtual_server_parse(TALLOC_CTX *ctx, void *out, void *parent,
				CONF_ITEM *ci, UNUSED CONF_PARSER const *rule);

static CONF_PARSER submodule_config[] = {
	{ FR_CONF_OFFSET("network_name", FR_TYPE_STRING, eap_aka_sim_state_conf_t, network_name ) },
	{ FR_CONF_OFFSET("request_identity", FR_TYPE_BOOL, eap_aka_sim_state_conf_t, request_identity ),
			 .func = cf_table_parse_uint32, .uctx = fr_aka_sim_id_request_table },
	{ FR_CONF_OFFSET("ephemeral_id_length", FR_TYPE_UINT8, eap_aka_sim_state_conf_t, ephemeral_id_length ), .dflt = "14" },	/* 14 for compatibility */
	{ FR_CONF_OFFSET("protected_success", FR_TYPE_BOOL, eap_aka_sim_state_conf_t, protected_success ), .dflt = "no" },
	{ FR_CONF_OFFSET("virtual_server", FR_TYPE_VOID, eap_aka_sim_state_conf_t, virtual_server), .func = virtual_server_parse },
	CONF_PARSER_TERMINATOR
};

static fr_dict_t *dict_eap_aka_sim;

extern fr_dict_autoload_t rlm_eap_aka_dict[];
fr_dict_autoload_t rlm_eap_aka_dict[] = {
	{ .out = &dict_eap_aka_sim, .base_dir="eap/aka-sim", .proto = "eap-aka-sim"  },
	{ NULL }
};

static int virtual_server_parse(UNUSED TALLOC_CTX *ctx, void *out, UNUSED void *parent,
				CONF_ITEM *ci, UNUSED CONF_PARSER const *rule)
{
	CONF_SECTION	*server_cs;

	server_cs = virtual_server_find(cf_pair_value(cf_item_to_pair(ci)));
	if (!server_cs) {
		cf_log_err(ci, "virtual-server \"%s\" not found", cf_pair_value(cf_item_to_pair(ci)));
		return -1;
	}

	*((CONF_SECTION **)out) = server_cs;

	return 0;
}

#define ACTION_SECTION(_out, _field, _verb, _name) \
do { \
	CONF_SECTION *_tmp; \
	_tmp = cf_section_find(server_cs, _verb, _name); \
	if (_tmp) { \
		if (unlang_compile(_tmp, MOD_AUTHORIZE, NULL) < 0) return -1; \
		found = true; \
	} \
	if (_out) _out->_field = _tmp; \
} while (0)

/** Compile virtual server sections
 *
 * Called twice, once when a server with an eap-aka namespace is found, and once
 * when an EAP-AKA module is instantiated.
 *
 * The first time is with actions == NULL and is to compile the sections and
 * perform validation.
 * The second time is to write out pointers to the compiled sections which the
 * EAP-AKA module will use to execute unlang code.
 *
 */
static int mod_section_compile(eap_aka_sim_actions_t *actions, CONF_SECTION *server_cs)
{
	bool found = false;

	if (!fr_cond_assert(server_cs)) return -1;

	/*
	 *	Identity negotiation
	 */
	ACTION_SECTION(actions, send_identity_request, "send", "Identity-Request");
	ACTION_SECTION(actions, recv_identity_response, "recv", "Identity-Response");

	/*
	 *	Full-Authentication
	 */
	ACTION_SECTION(actions, send_challenge_request, "send", "Challenge-Request");
	ACTION_SECTION(actions, recv_challenge_response, "recv", "Challenge-Response");

	/*
	 *	Fast-Re-Authentication
	 */
	ACTION_SECTION(actions, send_reauthentication_request, "send", "Reauthentication-Request");
	ACTION_SECTION(actions, recv_reauthentication_response, "recv", "Reauthentication-Response");

	/*
	 *	Failures originating from the supplicant
	 */
	ACTION_SECTION(actions, recv_client_error, "recv", "Client-Error");
	ACTION_SECTION(actions, recv_authentication_reject, "recv", "Authentication-Reject");
	ACTION_SECTION(actions, recv_syncronization_failure, "recv", "Syncronization-Failure");

	/*
	 *	Failure originating from the server
	 */
	ACTION_SECTION(actions, send_failure_notification, "send", "Failure-Notification");
	ACTION_SECTION(actions, recv_failure_notification_ack, "recv", "Failure-Notification-ACK");

	/*
	 *	Protected success indication
	 */
	ACTION_SECTION(actions, send_success_notification, "send", "Success-Notification");
	ACTION_SECTION(actions, recv_success_notification_ack, "recv", "Success-Notification-ACK");

	/*
	 *	Final EAP-Success and EAP-Failure messages
	 */
	ACTION_SECTION(actions, send_eap_success, "send", "EAP-Success");
	ACTION_SECTION(actions, send_eap_failure, "send", "EAP-Failure");

	/*
	 *	Fast-Reauth vectors
	 */
	ACTION_SECTION(actions, store_session, "store", "session");
	ACTION_SECTION(actions, load_session, "load", "session");
	ACTION_SECTION(actions, clear_session, "clear", "session");

	/*
	 *	Pseudonym processing
	 */
	ACTION_SECTION(actions, store_pseudonym, "store", "pseudonym");
	ACTION_SECTION(actions, load_pseudonym, "load", "pseudonym");
	ACTION_SECTION(actions, clear_pseudonym, "clear", "pseudonym");

	/*
	 *	Warn if we couldn't find any actions.
	 */
	if (!found) {
		cf_log_warn(server_cs, "No \"eap-aka-prime\" actions found in virtual server \"%s\"",
			    cf_section_name2(server_cs));
	}

	return 0;
}

static int mod_instantiate(void *instance, UNUSED CONF_SECTION *conf)
{
	eap_aka_sim_state_conf_t	*inst = talloc_get_type_abort(instance, eap_aka_sim_state_conf_t);

	if (mod_section_compile(&inst->actions, inst->virtual_server) < 0) return -1;

	return 0;
}

/** Compile any virtual servers with the "eap-aka" namespace
 *
 */
static int mod_namespace_load(CONF_SECTION *server_cs)
{
	return mod_section_compile(NULL, server_cs);
}

static int mod_load(void)
{
	if (virtual_namespace_register("eap-aka-prime", "eap-aka-sim", "eap/aka-sim", mod_namespace_load) < 0) return -1;

	if (fr_aka_sim_init() < 0) return -1;

	fr_aka_sim_xlat_register();

	return 0;
}

static void mod_unload(void)
{
	fr_aka_sim_xlat_unregister();

	fr_aka_sim_free();
}

/*
 *	The module name should be the only globally exported symbol.
 *	That is, everything else should be 'static'.
 */
extern rlm_eap_submodule_t rlm_eap_aka_prime;
rlm_eap_submodule_t rlm_eap_aka_prime = {
	.name		= "eap_aka_prime",
	.magic		= RLM_MODULE_INIT,

	.provides	= { FR_EAP_METHOD_AKA_PRIME },

	.inst_size	= sizeof(eap_aka_sim_state_conf_t),
	.inst_type	= "eap_aka_sim_state_conf_t",
	.config		= submodule_config,

	.onload		= mod_load,
	.unload		= mod_unload,

	.instantiate	= mod_instantiate,

	.session_init	= aka_sim_state_machine_start,	/* Initialise a new EAP session */
	.namespace	= &dict_eap_aka_sim
};
