/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
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
 * @file src/process/radius/base.c
 * @brief RADIUS process module
 *
 * @copyright 2021 The FreeRADIUS server project.
 * @copyright 2021 Network RADIUS SAS (legal@networkradius.com)
 */
#include <freeradius-devel/protocol/freeradius/freeradius.internal.h>

#include <freeradius-devel/radius/radius.h>

#include <freeradius-devel/server/main_config.h>
#include <freeradius-devel/server/module.h>
#include <freeradius-devel/server/pair.h>
#include <freeradius-devel/server/protocol.h>
#include <freeradius-devel/server/state.h>

#include <freeradius-devel/unlang/module.h>
#include <freeradius-devel/unlang/interpret.h>

#include <freeradius-devel/util/debug.h>
#include <freeradius-devel/util/pair.h>
#include <freeradius-devel/util/value.h>

static fr_dict_t const *dict_freeradius;
static fr_dict_t const *dict_radius;

extern fr_dict_autoload_t process_radius_dict[];
fr_dict_autoload_t process_radius_dict[] = {
	{ .out = &dict_freeradius, .proto = "freeradius" },
	{ .out = &dict_radius, .proto = "radius" },
	{ NULL }
};

static fr_dict_attr_t const *attr_auth_type;
static fr_dict_attr_t const *attr_module_failure_message;
static fr_dict_attr_t const *attr_module_success_message;
static fr_dict_attr_t const *attr_stripped_user_name;

static fr_dict_attr_t const *attr_acct_status_type;
static fr_dict_attr_t const *attr_calling_station_id;
static fr_dict_attr_t const *attr_chap_password;
static fr_dict_attr_t const *attr_nas_port;
static fr_dict_attr_t const *attr_packet_type;
static fr_dict_attr_t const *attr_proxy_state;
static fr_dict_attr_t const *attr_service_type;
static fr_dict_attr_t const *attr_state;
static fr_dict_attr_t const *attr_user_name;
static fr_dict_attr_t const *attr_user_password;
static fr_dict_attr_t const *attr_original_packet_code;
static fr_dict_attr_t const *attr_error_cause;

extern fr_dict_attr_autoload_t process_radius_dict_attr[];
fr_dict_attr_autoload_t process_radius_dict_attr[] = {
	{ .out = &attr_auth_type, .name = "Auth-Type", .type = FR_TYPE_UINT32, .dict = &dict_freeradius },
	{ .out = &attr_module_failure_message, .name = "Module-Failure-Message", .type = FR_TYPE_STRING, .dict = &dict_freeradius },
	{ .out = &attr_module_success_message, .name = "Module-Success-Message", .type = FR_TYPE_STRING, .dict = &dict_freeradius },
	{ .out = &attr_stripped_user_name, .name = "Stripped-User-Name", .type = FR_TYPE_STRING, .dict = &dict_freeradius },

	{ .out = &attr_acct_status_type, .name = "Acct-Status-Type", .type = FR_TYPE_UINT32, .dict = &dict_radius },
	{ .out = &attr_calling_station_id, .name = "Calling-Station-Id", .type = FR_TYPE_STRING, .dict = &dict_radius },
	{ .out = &attr_chap_password, .name = "CHAP-Password", .type = FR_TYPE_OCTETS, .dict = &dict_radius },
	{ .out = &attr_nas_port, .name = "NAS-Port", .type = FR_TYPE_UINT32, .dict = &dict_radius },
	{ .out = &attr_proxy_state, .name = "Proxy-State", .type = FR_TYPE_OCTETS, .dict = &dict_radius },
	{ .out = &attr_packet_type, .name = "Packet-Type", .type = FR_TYPE_UINT32, .dict = &dict_radius },
	{ .out = &attr_service_type, .name = "Service-Type", .type = FR_TYPE_UINT32, .dict = &dict_radius },
	{ .out = &attr_state, .name = "State", .type = FR_TYPE_OCTETS, .dict = &dict_radius },
	{ .out = &attr_user_name, .name = "User-Name", .type = FR_TYPE_STRING, .dict = &dict_radius },
	{ .out = &attr_user_password, .name = "User-Password", .type = FR_TYPE_STRING, .dict = &dict_radius },

	{ .out = &attr_original_packet_code, .name = "Extended-Attribute-1.Original-Packet-Code", .type = FR_TYPE_UINT32, .dict = &dict_radius },
	{ .out = &attr_error_cause, .name = "Error-Cause", .type = FR_TYPE_UINT32, .dict = &dict_radius },

	{ NULL }
};

static fr_value_box_t const	*enum_auth_type_accept;
static fr_value_box_t const	*enum_auth_type_reject;

extern fr_dict_enum_autoload_t process_radius_dict_enum[];
fr_dict_enum_autoload_t process_radius_dict_enum[] = {
	{ .out = &enum_auth_type_accept, .name = "Accept", .attr = &attr_auth_type },
	{ .out = &enum_auth_type_reject, .name = "Reject", .attr = &attr_auth_type },
	{ NULL }
};

/*
 *	RADIUS state machine configuration
 */
typedef struct {
	uint64_t	nothing;		// so that "access_request" isn't at offset 0

	CONF_SECTION	*access_request;
	CONF_SECTION	*access_accept;
	CONF_SECTION	*access_reject;
	CONF_SECTION	*access_challenge;

	CONF_SECTION	*accounting_request;
	CONF_SECTION	*accounting_response;

	CONF_SECTION	*status_server;

	CONF_SECTION	*coa_request;
	CONF_SECTION	*coa_ack;
	CONF_SECTION	*coa_nak;

	CONF_SECTION	*disconnect_request;
	CONF_SECTION	*disconnect_ack;
	CONF_SECTION	*disconnect_nak;

	CONF_SECTION	*do_not_respond;
	CONF_SECTION	*protocol_error;	/* @todo - allow protocol error as a reject reply? */

	CONF_SECTION	*new_client;
	CONF_SECTION	*add_client;
	CONF_SECTION	*deny_client;
} process_radius_sections_t;

typedef struct {
	bool		log_stripped_names;
	bool		log_auth;		//!< Log authentication attempts.
	bool		log_auth_badpass;	//!< Log failed authentications.
	bool		log_auth_goodpass;	//!< Log successful authentications.
	char const	*auth_badpass_msg;	//!< Additional text to append to failed auth messages.
	char const	*auth_goodpass_msg;	//!< Additional text to append to successful auth messages.

	char const	*denied_msg;		//!< Additional text to append if the user is already logged
						//!< in (simultaneous use check failed).

	fr_time_delta_t	session_timeout;	//!< Maximum time between the last response and next request.
	uint32_t	max_session;		//!< Maximum ongoing session allowed.

	uint8_t       	state_server_id;	//!< Sets a specific byte in the state to allow the
						//!< authenticating server to be identified in packet
						//!<captures.

	fr_state_tree_t	*state_tree;		//!< State tree to link multiple requests/responses.
} process_radius_auth_t;

typedef struct {
	CONF_SECTION			*server_cs;	//!< Our virtual server.
	process_radius_sections_t	sections;	//!< Pointers to various config sections
							///< we need to execute.
	process_radius_auth_t		auth;		//!< Authentication configuration.
} process_radius_t;

/** Records fields from the original request so we have a known good copy
 */
typedef struct {
	fr_value_box_list_head_t	proxy_state;	//!< These need to be copied into the response in exactly
							///< the same order as they were added.
} process_radius_request_pairs_t;

#define FR_RADIUS_PROCESS_CODE_VALID(_x) (FR_RADIUS_PACKET_CODE_VALID(_x) || (_x == FR_RADIUS_CODE_DO_NOT_RESPOND))

#define PROCESS_PACKET_TYPE		fr_radius_packet_code_t
#define PROCESS_CODE_MAX		FR_RADIUS_CODE_MAX
#define PROCESS_CODE_DO_NOT_RESPOND	FR_RADIUS_CODE_DO_NOT_RESPOND
#define PROCESS_PACKET_CODE_VALID	FR_RADIUS_PROCESS_CODE_VALID
#define PROCESS_INST			process_radius_t
#include <freeradius-devel/server/process.h>

static const CONF_PARSER session_config[] = {
	{ FR_CONF_OFFSET("timeout", FR_TYPE_TIME_DELTA, process_radius_auth_t, session_timeout), .dflt = "15" },
	{ FR_CONF_OFFSET("max", FR_TYPE_UINT32, process_radius_auth_t, max_session), .dflt = "4096" },
	{ FR_CONF_OFFSET("state_server_id", FR_TYPE_UINT8, process_radius_auth_t, state_server_id) },

	CONF_PARSER_TERMINATOR
};

static const CONF_PARSER log_config[] = {
	{ FR_CONF_OFFSET("stripped_names", FR_TYPE_BOOL, process_radius_auth_t, log_stripped_names), .dflt = "no" },
	{ FR_CONF_OFFSET("auth", FR_TYPE_BOOL, process_radius_auth_t, log_auth), .dflt = "no" },
	{ FR_CONF_OFFSET("auth_badpass", FR_TYPE_BOOL, process_radius_auth_t, log_auth_badpass), .dflt = "no" },
	{ FR_CONF_OFFSET("auth_goodpass", FR_TYPE_BOOL,process_radius_auth_t,  log_auth_goodpass), .dflt = "no" },
	{ FR_CONF_OFFSET("msg_badpass", FR_TYPE_STRING, process_radius_auth_t, auth_badpass_msg) },
	{ FR_CONF_OFFSET("msg_goodpass", FR_TYPE_STRING, process_radius_auth_t, auth_goodpass_msg) },
	{ FR_CONF_OFFSET("msg_denied", FR_TYPE_STRING, process_radius_auth_t, denied_msg), .dflt = "You are already logged in - access denied" },

	CONF_PARSER_TERMINATOR
};

static const CONF_PARSER auth_config[] = {
	{ FR_CONF_POINTER("log", FR_TYPE_SUBSECTION, NULL), .subcs = (void const *) log_config },

	{ FR_CONF_POINTER("session", FR_TYPE_SUBSECTION, NULL), .subcs = (void const *) session_config },

	CONF_PARSER_TERMINATOR
};

static const CONF_PARSER config[] = {
	{ FR_CONF_POINTER("Access-Request", FR_TYPE_SUBSECTION, NULL), .subcs = (void const *) auth_config,
	  .offset = offsetof(process_radius_t, auth), },

	CONF_PARSER_TERMINATOR
};

/*
 *	Debug the packet if requested.
 */
static void radius_packet_debug(request_t *request, fr_radius_packet_t *packet, fr_pair_list_t *list, bool received)
{
#ifdef WITH_IFINDEX_NAME_RESOLUTION
	char if_name[IFNAMSIZ];
#endif

	if (!packet) return;
	if (!RDEBUG_ENABLED) return;

	log_request(L_DBG, L_DBG_LVL_1, request, __FILE__, __LINE__, "%s %s ID %d from %s%pV%s:%i to %s%pV%s:%i "
#ifdef WITH_IFINDEX_NAME_RESOLUTION
		       "%s%s%s"
#endif
		       "",
		       received ? "Received" : "Sending",
		       fr_radius_packet_names[packet->code],
		       packet->id,
		       packet->socket.inet.src_ipaddr.af == AF_INET6 ? "[" : "",
		       fr_box_ipaddr(packet->socket.inet.src_ipaddr),
		       packet->socket.inet.src_ipaddr.af == AF_INET6 ? "]" : "",
		       packet->socket.inet.src_port,
		       packet->socket.inet.dst_ipaddr.af == AF_INET6 ? "[" : "",
		       fr_box_ipaddr(packet->socket.inet.dst_ipaddr),
		       packet->socket.inet.dst_ipaddr.af == AF_INET6 ? "]" : "",
		       packet->socket.inet.dst_port
#ifdef WITH_IFINDEX_NAME_RESOLUTION
		       , packet->socket.inet.ifindex ? "via " : "",
		       packet->socket.inet.ifindex ? fr_ifname_from_ifindex(if_name, packet->socket.inet.ifindex) : "",
		       packet->socket.inet.ifindex ? " " : ""
#endif
		       );

	if (received || request->parent) {
		log_request_pair_list(L_DBG_LVL_1, request, NULL, list, NULL);
	} else {
		log_request_proto_pair_list(L_DBG_LVL_1, request, NULL, list, NULL);
	}
}

#define RAUTH(fmt, ...)		log_request(L_AUTH, L_DBG_LVL_OFF, request, __FILE__, __LINE__, fmt, ## __VA_ARGS__)

/*
 *	Return a short string showing the terminal server, port
 *	and calling station ID.
 */
static char *auth_name(char *buf, size_t buflen, request_t *request)
{
	fr_pair_t	*cli;
	fr_pair_t	*pair;
	uint32_t	port = 0;	/* RFC 2865 NAS-Port is 4 bytes */
	char const	*tls = "";
	fr_client_t	*client = client_from_request(request);

	cli = fr_pair_find_by_da(&request->request_pairs, NULL, attr_calling_station_id);

	pair = fr_pair_find_by_da(&request->request_pairs, NULL, attr_nas_port);
	if (pair != NULL) port = pair->vp_uint32;

	if (request->packet->socket.inet.dst_port == 0) tls = " via proxy to virtual server";

	snprintf(buf, buflen, "from client %.128s port %u%s%.128s%s",
		 client ? client->shortname : "", port,
		 (cli ? " cli " : ""), (cli ? cli->vp_strvalue : ""),
		 tls);

	return buf;
}

/*
 *	Make sure user/pass are clean and then create an attribute
 *	which contains the log message.
 */
static void CC_HINT(format (printf, 4, 5)) auth_message(process_radius_auth_t const *inst,
							request_t *request, bool goodpass, char const *fmt, ...)
{
	va_list		 ap;

	bool		logit;
	char const	*extra_msg = NULL;

	char		password_buff[128];
	char const	*password_str = NULL;

	char		buf[1024];
	char		extra[1024];
	char		*p;
	char		*msg;
	fr_pair_t	*username = NULL;
	fr_pair_t	*password = NULL;

	/*
	 *	No logs?  Then no logs.
	 */
	if (!inst->log_auth) return;

	/*
	 * Get the correct username based on the configured value
	 */
	if (!inst->log_stripped_names) {
		username = fr_pair_find_by_da(&request->request_pairs, NULL, attr_user_name);
	} else {
		username = fr_pair_find_by_da(&request->request_pairs, NULL, attr_stripped_user_name);
		if (!username) username = fr_pair_find_by_da(&request->request_pairs, NULL, attr_user_name);
	}

	/*
	 *	Clean up the password
	 */
	if (inst->log_auth_badpass || inst->log_auth_goodpass) {
		password = fr_pair_find_by_da(&request->request_pairs, NULL, attr_user_password);
		if (!password) {
			fr_pair_t *auth_type;

			auth_type = fr_pair_find_by_da(&request->control_pairs, NULL, attr_auth_type);
			if (auth_type) {
				snprintf(password_buff, sizeof(password_buff), "<via Auth-Type = %s>",
					 fr_dict_enum_name_by_value(auth_type->da, &auth_type->data));
				password_str = password_buff;
			} else {
				password_str = "<no User-Password attribute>";
			}
		} else if (fr_pair_find_by_da(&request->request_pairs, NULL, attr_chap_password)) {
			password_str = "<CHAP-Password>";
		}
	}

	if (goodpass) {
		logit = inst->log_auth_goodpass;
		extra_msg = inst->auth_goodpass_msg;
	} else {
		logit = inst->log_auth_badpass;
		extra_msg = inst->auth_badpass_msg;
	}

	if (extra_msg) {
		extra[0] = ' ';
		p = extra + 1;
		if (xlat_eval(p, sizeof(extra) - 1, request, extra_msg, NULL, NULL) < 0) return;
	} else {
		*extra = '\0';
	}

	/*
	 *	Expand the input message
	 */
	va_start(ap, fmt);
	msg = fr_vasprintf(request, fmt, ap);
	va_end(ap);

	RAUTH("%s: [%pV%s%pV] (%s)%s",
	      msg,
	      username ? &username->data : fr_box_strvalue("<no User-Name attribute>"),
	      logit ? "/" : "",
	      logit ? (password_str ? fr_box_strvalue(password_str) : &password->data) : fr_box_strvalue(""),
	      auth_name(buf, sizeof(buf), request),
	      extra);

	talloc_free(msg);
}

/** Keep a copy of some attributes to keep them from being tamptered with
 *
 */
static inline CC_HINT(always_inline)
process_radius_request_pairs_t *radius_request_pairs_store(request_t *request)
{
	fr_pair_t			*proxy_state;
	process_radius_request_pairs_t	*rctx;

	/*
	 *	Don't bother allocing the struct if there's no proxy state to store
	 */
	proxy_state = fr_pair_find_by_da(&request->request_pairs, NULL, attr_proxy_state);
	if (!proxy_state) return 0;

	MEM(rctx = talloc_zero(unlang_interpret_frame_talloc_ctx(request), process_radius_request_pairs_t));
	fr_value_box_list_init(&rctx->proxy_state);

	/*
	 *	We don't use fr_pair_list_copy_by_da, to avoid doing the lookup for
	 *	the first proxy-state attr again.
	 */
	do {
		fr_value_box_t *proxy_state_value;

		MEM((proxy_state_value = fr_value_box_acopy(rctx, &proxy_state->data)));
		fr_value_box_list_insert_tail(&rctx->proxy_state, proxy_state_value);
	} while ((proxy_state = fr_pair_find_by_da(&request->request_pairs, proxy_state, attr_proxy_state)));

	return rctx;
}

static inline CC_HINT(always_inline)
void radius_request_pairs_to_reply(request_t *request, process_radius_request_pairs_t *rctx)
{
	if (!rctx) return;

	/*
	 *	Proxy-State is a link-level signal between RADIUS
	 *	client and server.  RFC 2865 Section 5.33 says that
	 *	Proxy-State is an opaque field, and implementations
	 *	most not examine it, interpret it, or assign it any
	 *	meaning.  Implementations must also copy all Proxy-State
	 *	from the request to the reply.
	 *
	 *	The rlm_radius module already deletes any Proxy-State
	 *	from the reply before appending the proxy reply to the
	 *	current reply.
	 *
	 *	If any policy creates Proxy-State, that could affect
	 *	individual RADIUS links (perhaps), and that would be
	 *	wrong.  As such, we nuke any nonsensical Proxy-State
	 *	added by policies or errant modules, and instead just
	 *	do exactly what the RFCs require us to do.  No more.
	 */
	fr_pair_delete_by_da(&request->reply_pairs, attr_proxy_state);

	RDEBUG3("Adding Proxy-State attributes from request");
	RINDENT();
	fr_value_box_list_foreach(&rctx->proxy_state, proxy_state_value) {
		fr_pair_t *vp;

		MEM(vp = fr_pair_afrom_da(request->reply_ctx, attr_proxy_state));
		fr_value_box_copy(vp, &vp->data, proxy_state_value);
		fr_pair_append(&request->reply_pairs, vp);
		RDEBUG3("&reply.%pP", vp);
	}
	REXDENT();
}

/** A wrapper around recv generic which stores fields from the request
 */
RECV(generic_radius_request)
{
	module_ctx_t			our_mctx = *mctx;

	fr_assert_msg(!mctx->rctx, "rctx not expected here");
	our_mctx.rctx = radius_request_pairs_store(request);
	mctx = &our_mctx;	/* Our mutable mctx */

	return CALL_RECV(generic);
}

/** A wrapper around send generic which restores fields
 *
 */
RESUME(generic_radius_response)
{
	if (mctx->rctx) radius_request_pairs_to_reply(request, talloc_get_type_abort(mctx->rctx, process_radius_request_pairs_t));

	return CALL_RESUME(send_generic);
}

RECV(access_request)
{
	process_radius_t const		*inst = talloc_get_type_abort_const(mctx->inst->data, process_radius_t);

	/*
	 *	Only reject if the state has already been thawed.
	 *	It could be that the state value wasn't intended
	 *	for us, and we're just proxying upstream.
	 */
	if (fr_state_to_request(inst->auth.state_tree, request) < 0) {
		return CALL_SEND_TYPE(FR_RADIUS_CODE_ACCESS_REJECT);
	}

	return CALL_RECV(generic_radius_request);
}

RESUME(auth_type);

RESUME(access_request)
{
	rlm_rcode_t			rcode = *p_result;
	fr_pair_t			*vp;
	CONF_SECTION			*cs;
	fr_dict_enum_value_t const		*dv;
	fr_process_state_t const	*state;
	process_radius_t const		*inst = talloc_get_type_abort_const(mctx->inst->data, process_radius_t);

	PROCESS_TRACE;

	fr_assert(rcode < RLM_MODULE_NUMCODES);

	/*
	 *	See if the return code from "recv Access-Request" says we reject, or continue.
	 */
	UPDATE_STATE(packet);

	request->reply->code = state->packet_type[rcode];
	if (!request->reply->code) request->reply->code = state->default_reply;

	/*
	 *	Something set reject, we're done.
	 */
	if (request->reply->code == FR_RADIUS_CODE_ACCESS_REJECT) {
		RDEBUG("The 'recv Access-Request' section returned %s - rejecting the request",
		       fr_table_str_by_value(rcode_table, rcode, "<INVALID>"));

	send_reply:
		UPDATE_STATE(reply);

		fr_assert(state->send != NULL);
		return CALL_SEND_STATE(state);
	}

	if (request->reply->code) {
		goto send_reply;
	}

	/*
	 *	A policy _or_ a module can hard-code the reply.
	 */
	if (!request->reply->code) {
		vp = fr_pair_find_by_da(&request->reply_pairs, NULL, attr_packet_type);
		if (vp && FR_RADIUS_PROCESS_CODE_VALID(vp->vp_uint32)) {
			request->reply->code = vp->vp_uint32;
			goto send_reply;
		}
	}

	/*
	 *	Run authenticate foo { ... }
	 *
	 *	If we can't find Auth-Type, OR if we can't find
	 *	Auth-Type = foo, then it's a reject.
	 */
	vp = fr_pair_find_by_da(&request->control_pairs, NULL, attr_auth_type);
	if (!vp) {
		RDEBUG("No 'Auth-Type' attribute found, cannot authenticate the user - rejecting the request");

	reject:
		request->reply->code = FR_RADIUS_CODE_ACCESS_REJECT;
		goto send_reply;
	}

	dv = fr_dict_enum_by_value(vp->da, &vp->data);
	if (!dv) {
		RDEBUG("Invalid value for 'Auth-Type' attribute, cannot authenticate the user - rejecting the request");

		goto reject;
	}

	/*
	 *	The magic Auth-Type Accept value
	 *	which means skip the authenticate
	 *	section.
	 *
	 *	And Reject means always reject.  Tho the admin should
	 *	just return "reject" from the section.
	 */
	if (fr_value_box_cmp(enum_auth_type_accept, dv->value) == 0) {
		request->reply->code = FR_RADIUS_CODE_ACCESS_ACCEPT;
		goto send_reply;

	} else if (fr_value_box_cmp(enum_auth_type_reject, dv->value) == 0) {
		request->reply->code = FR_RADIUS_CODE_ACCESS_REJECT;
		goto send_reply;
	}

	cs = cf_section_find(inst->server_cs, "authenticate", dv->name);
	if (!cs) {
		RDEBUG2("No 'authenticate %s { ... }' section found - rejecting the request", dv->name);
		goto reject;
	}

	/*
	 *	Run the "Authenticate = foo" section.
	 *
	 *	And continue with sending the generic reply.
	 */
	RDEBUG("Running 'authenticate %s' from file %s", cf_section_name2(cs), cf_filename(cs));
	return unlang_module_yield_to_section(p_result, request,
					      cs, RLM_MODULE_NOOP, resume_auth_type,
					      NULL, 0, mctx->rctx);
}

RESUME(auth_type)
{
	static const fr_process_rcode_t auth_type_rcode = {
		[RLM_MODULE_OK] =	FR_RADIUS_CODE_ACCESS_ACCEPT,
		[RLM_MODULE_FAIL] =	FR_RADIUS_CODE_ACCESS_REJECT,
		[RLM_MODULE_INVALID] =	FR_RADIUS_CODE_ACCESS_REJECT,
		[RLM_MODULE_NOOP] =	FR_RADIUS_CODE_ACCESS_REJECT,
		[RLM_MODULE_NOTFOUND] =	FR_RADIUS_CODE_ACCESS_REJECT,
		[RLM_MODULE_REJECT] =	FR_RADIUS_CODE_ACCESS_REJECT,
		[RLM_MODULE_UPDATED] =	FR_RADIUS_CODE_ACCESS_REJECT,
		[RLM_MODULE_DISALLOW] = FR_RADIUS_CODE_ACCESS_REJECT,
	};

	rlm_rcode_t			rcode = *p_result;
	fr_pair_t			*vp;
	fr_process_state_t const	*state;

	PROCESS_TRACE;

	fr_assert(rcode < RLM_MODULE_NUMCODES);

	if (auth_type_rcode[rcode] == FR_RADIUS_CODE_DO_NOT_RESPOND) {
		request->reply->code = auth_type_rcode[rcode];
		UPDATE_STATE(reply);

		RDEBUG("The 'authenticate' section returned %s - not sending a response",
		       fr_table_str_by_value(rcode_table, rcode, "<INVALID>"));

		fr_assert(state->send != NULL);
		return state->send(p_result, mctx, request);
	}

	/*
	 *	Most cases except handled...
	 */
	if (auth_type_rcode[rcode]) request->reply->code = auth_type_rcode[rcode];

	switch (request->reply->code) {
	case 0:
		RDEBUG("No reply code was set.  Forcing to Access-Reject");
		request->reply->code = FR_RADIUS_CODE_ACCESS_REJECT;
		FALL_THROUGH;

	/*
	 *	Print complaints before running "send Access-Reject"
	 */
	case FR_RADIUS_CODE_ACCESS_REJECT:
		RDEBUG2("Failed to authenticate the user");

		/*
		 *	Maybe the shared secret is wrong?
		 */
		vp = fr_pair_find_by_da(&request->request_pairs, NULL, attr_user_password);
		if (vp) {
			if (RDEBUG_ENABLED2) {
				uint8_t const *p;

				p = (uint8_t const *) vp->vp_strvalue;
				while (*p) {
					int size;

					size = fr_utf8_char(p, -1);
					if (!size) {
						RWDEBUG("Unprintable characters in the password. "
							"Double-check the shared secret on the server "
							"and the NAS!");
						break;
					}
					p += size;
				}
			}
		}
		break;

	/*
	 *	Access-Challenge sections require a State.  If there is
	 *	none, create one here.  This is so that the State
	 *	attribute is accessible in the "send Access-Challenge"
	 *	section.
	 */
	case FR_RADIUS_CODE_ACCESS_CHALLENGE:
		if ((vp = fr_pair_find_by_da(&request->reply_pairs, NULL, attr_state)) != NULL) {
			uint8_t buffer[16];

			fr_rand_buffer(buffer, sizeof(buffer));

			MEM(pair_update_reply(&vp, attr_state) >= 0);
			fr_pair_value_memdup(vp, buffer, sizeof(buffer), false);
		}
		break;

	default:
		break;

	}
	UPDATE_STATE(reply);

	fr_assert(state->send != NULL);
	return state->send(p_result, mctx, request);
}

RESUME(access_accept)
{
	fr_pair_t			*vp;
	process_radius_t const		*inst = talloc_get_type_abort_const(mctx->inst->data, process_radius_t);

	PROCESS_TRACE;

	vp = fr_pair_find_by_da(&request->request_pairs, NULL, attr_module_success_message);
	if (vp) {
		auth_message(&inst->auth, request, true, "Login OK (%pV)", &vp->data);
	} else {
		auth_message(&inst->auth, request, true, "Login OK");
	}

	/*
	 *	Check that there is a name which can be used to
	 *	identify the user.  The configuration depends on
	 *	User-Name or Stripped-User-Name existing, and being
	 *	(mostly) unique to that user.
	 */
	if (!request->parent &&
	    ((vp = fr_pair_find_by_da(&request->request_pairs, NULL, attr_user_name)) != NULL) &&
	    (vp->vp_strvalue[0] == '@') &&
	    !fr_pair_find_by_da(&request->request_pairs, NULL, attr_stripped_user_name)) {
		RWDEBUG("User-Name is anonymized, and no Stripped-User-Name exists.");
		RWDEBUG("It may be difficult or impossible to identify the user.");
		RWDEBUG("Please update Stripped-User-Name with information which identifies the user.");
	}

	fr_state_discard(inst->auth.state_tree, request);
	radius_request_pairs_to_reply(request, mctx->rctx);
	RETURN_MODULE_OK;
}

RESUME(access_reject)
{
	fr_pair_t			*vp;
	process_radius_t const		*inst = talloc_get_type_abort_const(mctx->inst->data, process_radius_t);

	PROCESS_TRACE;

	vp = fr_pair_find_by_da(&request->request_pairs, NULL, attr_module_failure_message);
	if (vp) {
		auth_message(&inst->auth, request, false, "Login incorrect (%pV)", &vp->data);
	} else {
		auth_message(&inst->auth, request, false, "Login incorrect");
	}

	fr_state_discard(inst->auth.state_tree, request);
	radius_request_pairs_to_reply(request, mctx->rctx);
	RETURN_MODULE_OK;
}

RESUME(access_challenge)
{
	process_radius_t const		*inst = talloc_get_type_abort_const(mctx->inst->data, process_radius_t);

	PROCESS_TRACE;

	/*
	 *	Cache the state context.
	 *
	 *	If this fails, don't respond to the request.
	 */
	if (fr_request_to_state(inst->auth.state_tree, request) < 0) {
		return CALL_SEND_TYPE(FR_RADIUS_CODE_DO_NOT_RESPOND);
	}

	fr_assert(request->reply->code == FR_RADIUS_CODE_ACCESS_CHALLENGE);
	radius_request_pairs_to_reply(request, mctx->rctx);
	RETURN_MODULE_OK;
}

RESUME(acct_type)
{
	static const fr_process_rcode_t acct_type_rcode = {
		[RLM_MODULE_FAIL] =	FR_RADIUS_CODE_DO_NOT_RESPOND,
		[RLM_MODULE_INVALID] =	FR_RADIUS_CODE_DO_NOT_RESPOND,
		[RLM_MODULE_NOTFOUND] =	FR_RADIUS_CODE_DO_NOT_RESPOND,
		[RLM_MODULE_REJECT] =	FR_RADIUS_CODE_DO_NOT_RESPOND,
		[RLM_MODULE_DISALLOW] = FR_RADIUS_CODE_DO_NOT_RESPOND,
	};

	rlm_rcode_t			rcode = *p_result;
	fr_process_state_t const	*state;

	PROCESS_TRACE;

	fr_assert(rcode < RLM_MODULE_NUMCODES);
	fr_assert(FR_RADIUS_PROCESS_CODE_VALID(request->reply->code));

	if (acct_type_rcode[rcode]) {
		fr_assert(acct_type_rcode[rcode] == FR_RADIUS_CODE_DO_NOT_RESPOND);

		request->reply->code = acct_type_rcode[rcode];
		UPDATE_STATE(reply);

		RDEBUG("The 'accounting' section returned %s - not sending a response",
		       fr_table_str_by_value(rcode_table, rcode, "<INVALID>"));

		fr_assert(state->send != NULL);
		return state->send(p_result, mctx, request);
	}

	request->reply->code = FR_RADIUS_CODE_ACCOUNTING_RESPONSE;
	UPDATE_STATE(reply);

	fr_assert(state->send != NULL);
	return state->send(p_result, mctx, request);
}

RESUME(accounting_request)
{
	rlm_rcode_t			rcode = *p_result;
	fr_pair_t			*vp;
	CONF_SECTION			*cs;
	fr_dict_enum_value_t const	*dv;
	fr_process_state_t const	*state;
	process_radius_t const		*inst = talloc_get_type_abort_const(mctx->inst->data, process_radius_t);

	PROCESS_TRACE;

	fr_assert(rcode < RLM_MODULE_NUMCODES);

	UPDATE_STATE(packet);
	fr_assert(state->packet_type[rcode] != 0);

	request->reply->code = state->packet_type[rcode];
	UPDATE_STATE_CS(reply);

	if (request->reply->code == FR_RADIUS_CODE_DO_NOT_RESPOND) {
		RDEBUG("The 'recv Accounting-Request' section returned %s - not sending a response",
		       fr_table_str_by_value(rcode_table, rcode, "<INVALID>"));

	send_reply:
		fr_assert(state->send != NULL);
		return CALL_SEND_STATE(state);
	}

	/*
	 *	Run accounting foo { ... }
	 */
	vp = fr_pair_find_by_da(&request->request_pairs, NULL, attr_acct_status_type);
	if (!vp) goto send_reply;

	dv = fr_dict_enum_by_value(vp->da, &vp->data);
	if (!dv) goto send_reply;

	cs = cf_section_find(inst->server_cs, "accounting", dv->name);
	if (!cs) {
		RDEBUG2("No 'accounting %s { ... }' section found - skipping...", dv->name);
		goto send_reply;
	}

	/*
	 *	Run the "Acct-Status-Type = foo" section.
	 *
	 *	And continue with sending the generic reply.
	 */
	return unlang_module_yield_to_section(p_result, request,
					      cs, RLM_MODULE_NOOP, resume_acct_type,
					      NULL, 0, mctx->rctx);
}

#if 0
// @todo - send canned responses like in v3?
RECV(status_server)
{
	RETURN_MODULE_FAIL;
}

RESUME(status_server)
{
	RETURN_MODULE_FAIL;
}
#endif

RESUME(protocol_error)
{
	fr_pair_t 			*vp;

	PROCESS_TRACE;

	fr_assert(FR_RADIUS_PACKET_CODE_VALID(request->reply->code));

	/*
	 *	https://tools.ietf.org/html/rfc7930#section-4
	 */
	vp = fr_pair_find_by_da_nested(&request->reply_pairs, NULL, attr_original_packet_code);
	if (!vp) {
		vp = fr_pair_afrom_da(request->reply_ctx, attr_original_packet_code);
		if (vp) {
			vp->vp_uint32 = request->packet->code;
			fr_pair_append(&request->reply_pairs, vp);
		}
	}

	/*
	 *	If there's no Error-Cause, then include a generic 404.
	 */
	vp = fr_pair_find_by_da(&request->reply_pairs, NULL, attr_error_cause);
	if (!vp) {
		vp = fr_pair_afrom_da(request->reply_ctx, attr_error_cause);
		if (vp) {
			vp->vp_uint32 = FR_ERROR_CAUSE_VALUE_INVALID_REQUEST;
			fr_pair_append(&request->reply_pairs, vp);
		}
	}

	/*
	 *	And do the generic processing after running a "send" section.
	 */
	return CALL_RESUME(send_generic);
}

static unlang_action_t mod_process(rlm_rcode_t *p_result, module_ctx_t const *mctx, request_t *request)
{
	fr_process_state_t const *state;

	(void) talloc_get_type_abort_const(mctx->inst->data, process_radius_t);

	PROCESS_TRACE;

	request->component = "radius";
	request->module = NULL;
	fr_assert(request->dict == dict_radius);

	fr_assert(FR_RADIUS_PACKET_CODE_VALID(request->packet->code));

	UPDATE_STATE(packet);

	if (!state->recv) {
		REDEBUG("Invalid packet type (%u)", request->packet->code);
		RETURN_MODULE_FAIL;
	}

	radius_packet_debug(request, request->packet, &request->request_pairs, true);

	return state->recv(p_result, mctx, request);
}

static int mod_instantiate(module_inst_ctx_t const *mctx)
{
	process_radius_t	*inst = talloc_get_type_abort(mctx->inst->data, process_radius_t);

	inst->auth.state_tree = fr_state_tree_init(inst, attr_state, main_config->spawn_workers, inst->auth.max_session,
						   inst->auth.session_timeout, inst->auth.state_server_id,
						   fr_hash_string(cf_section_name2(inst->server_cs)));

	return 0;
}

static int mod_bootstrap(module_inst_ctx_t const *mctx)
{
	process_radius_t	*inst = talloc_get_type_abort(mctx->inst->data, process_radius_t);

	inst->server_cs = cf_item_to_section(cf_parent(mctx->inst->conf));
	if (virtual_server_section_attribute_define(inst->server_cs, "authenticate", attr_auth_type) < 0) return -1;

	return 0;
}

/*
 *	rcodes not listed under a packet_type
 *	mean that the packet code will not be
 *	changed.
 */
static fr_process_state_t const process_state[] = {
	[ FR_RADIUS_CODE_ACCESS_REQUEST ] = {
		.packet_type = {
			[RLM_MODULE_FAIL]	= FR_RADIUS_CODE_ACCESS_REJECT,
			[RLM_MODULE_INVALID]	= FR_RADIUS_CODE_ACCESS_REJECT,
			[RLM_MODULE_REJECT]	= FR_RADIUS_CODE_ACCESS_REJECT,
			[RLM_MODULE_DISALLOW]	= FR_RADIUS_CODE_ACCESS_REJECT,
			[RLM_MODULE_NOTFOUND]	= FR_RADIUS_CODE_ACCESS_REJECT
		},
		.rcode = RLM_MODULE_NOOP,
		.recv = recv_access_request,
		.resume = resume_access_request,
		.section_offset = offsetof(process_radius_sections_t, access_request),
	},
	[ FR_RADIUS_CODE_ACCESS_ACCEPT ] = {
		.packet_type = {
			[RLM_MODULE_FAIL]	= FR_RADIUS_CODE_ACCESS_REJECT,
			[RLM_MODULE_INVALID]	= FR_RADIUS_CODE_ACCESS_REJECT,
			[RLM_MODULE_REJECT]	= FR_RADIUS_CODE_ACCESS_REJECT,
			[RLM_MODULE_DISALLOW]	= FR_RADIUS_CODE_ACCESS_REJECT
		},
		.rcode = RLM_MODULE_NOOP,
		.send = send_generic,
		.resume = resume_access_accept,
		.section_offset = offsetof(process_radius_sections_t, access_accept),
	},
	[ FR_RADIUS_CODE_ACCESS_REJECT ] = {
		.packet_type = {
			[RLM_MODULE_FAIL]	= FR_RADIUS_CODE_ACCESS_REJECT,
			[RLM_MODULE_INVALID]	= FR_RADIUS_CODE_ACCESS_REJECT,
			[RLM_MODULE_REJECT]	= FR_RADIUS_CODE_ACCESS_REJECT,
			[RLM_MODULE_DISALLOW]	= FR_RADIUS_CODE_ACCESS_REJECT
		},
		.rcode = RLM_MODULE_NOOP,
		.send = send_generic,
		.resume = resume_access_reject,
		.section_offset = offsetof(process_radius_sections_t, access_reject),
	},
	[ FR_RADIUS_CODE_ACCESS_CHALLENGE ] = {
		.packet_type = {
			[RLM_MODULE_FAIL]	= FR_RADIUS_CODE_ACCESS_REJECT,
			[RLM_MODULE_INVALID]	= FR_RADIUS_CODE_ACCESS_REJECT,
			[RLM_MODULE_REJECT]	= FR_RADIUS_CODE_ACCESS_REJECT,
			[RLM_MODULE_DISALLOW]	= FR_RADIUS_CODE_ACCESS_REJECT
		},
		.rcode = RLM_MODULE_NOOP,
		.send = send_generic,
		.resume = resume_access_challenge,
		.section_offset = offsetof(process_radius_sections_t, access_challenge),
	},

	[ FR_RADIUS_CODE_ACCOUNTING_REQUEST ] = {
		.packet_type = {
			[RLM_MODULE_NOOP]	= FR_RADIUS_CODE_ACCOUNTING_RESPONSE,
			[RLM_MODULE_OK]		= FR_RADIUS_CODE_ACCOUNTING_RESPONSE,
			[RLM_MODULE_UPDATED]	= FR_RADIUS_CODE_ACCOUNTING_RESPONSE,
			[RLM_MODULE_HANDLED]	= FR_RADIUS_CODE_ACCOUNTING_RESPONSE,

			[RLM_MODULE_FAIL]	= FR_RADIUS_CODE_DO_NOT_RESPOND,
			[RLM_MODULE_INVALID]	= FR_RADIUS_CODE_DO_NOT_RESPOND,
			[RLM_MODULE_NOTFOUND]	= FR_RADIUS_CODE_DO_NOT_RESPOND,
			[RLM_MODULE_REJECT]	= FR_RADIUS_CODE_DO_NOT_RESPOND,
			[RLM_MODULE_DISALLOW]	= FR_RADIUS_CODE_DO_NOT_RESPOND
		},
		.rcode = RLM_MODULE_NOOP,
		.recv = recv_generic_radius_request,
		.resume = resume_accounting_request,
		.section_offset = offsetof(process_radius_sections_t, accounting_request),
	},
	[ FR_RADIUS_CODE_ACCOUNTING_RESPONSE ] = {
		.packet_type = {
			[RLM_MODULE_FAIL]	= FR_RADIUS_CODE_DO_NOT_RESPOND,
			[RLM_MODULE_INVALID]	= FR_RADIUS_CODE_DO_NOT_RESPOND,
			[RLM_MODULE_NOTFOUND]	= FR_RADIUS_CODE_DO_NOT_RESPOND,
			[RLM_MODULE_REJECT]	= FR_RADIUS_CODE_DO_NOT_RESPOND,
			[RLM_MODULE_DISALLOW]	= FR_RADIUS_CODE_DO_NOT_RESPOND
		},
		.rcode = RLM_MODULE_NOOP,
		.send = send_generic,
		.resume = resume_generic_radius_response,
		.section_offset = offsetof(process_radius_sections_t, accounting_response),
	},
	[ FR_RADIUS_CODE_STATUS_SERVER ] = { /* @todo - negotiation, stats, etc. */
		.packet_type = {
			[RLM_MODULE_OK]		= FR_RADIUS_CODE_ACCESS_ACCEPT,
			[RLM_MODULE_UPDATED]	= FR_RADIUS_CODE_ACCESS_ACCEPT,

			[RLM_MODULE_FAIL]	= FR_RADIUS_CODE_ACCESS_REJECT,
			[RLM_MODULE_INVALID]	= FR_RADIUS_CODE_ACCESS_REJECT,
			[RLM_MODULE_NOTFOUND]	= FR_RADIUS_CODE_ACCESS_REJECT,
			[RLM_MODULE_REJECT]	= FR_RADIUS_CODE_ACCESS_REJECT,
			[RLM_MODULE_NOOP]	= FR_RADIUS_CODE_ACCESS_REJECT,
			[RLM_MODULE_DISALLOW]	= FR_RADIUS_CODE_ACCESS_REJECT
		},
		.rcode = RLM_MODULE_NOOP,
		.recv = recv_generic,
		.resume = resume_recv_generic,
		.section_offset = offsetof(process_radius_sections_t, status_server),
	},
	[ FR_RADIUS_CODE_COA_REQUEST ] = {
		.packet_type = {
			[RLM_MODULE_NOOP]	= FR_RADIUS_CODE_COA_ACK,
			[RLM_MODULE_OK]		= FR_RADIUS_CODE_COA_ACK,
			[RLM_MODULE_UPDATED]	= FR_RADIUS_CODE_COA_ACK,
			[RLM_MODULE_NOTFOUND]	= FR_RADIUS_CODE_COA_ACK,

			[RLM_MODULE_FAIL]	= FR_RADIUS_CODE_COA_NAK,
			[RLM_MODULE_INVALID]	= FR_RADIUS_CODE_COA_NAK,
			[RLM_MODULE_REJECT]	= FR_RADIUS_CODE_COA_NAK,
			[RLM_MODULE_DISALLOW]	= FR_RADIUS_CODE_COA_NAK
		},
		.rcode = RLM_MODULE_NOOP,
		.recv = recv_generic_radius_request,
		.resume = resume_recv_generic,
		.section_offset = offsetof(process_radius_sections_t, coa_request),
	},
	[ FR_RADIUS_CODE_COA_ACK ] = {
		.packet_type = {
			[RLM_MODULE_FAIL]	= FR_RADIUS_CODE_COA_NAK,
			[RLM_MODULE_INVALID]	= FR_RADIUS_CODE_COA_NAK,
			[RLM_MODULE_REJECT]	= FR_RADIUS_CODE_COA_NAK,
			[RLM_MODULE_DISALLOW]	= FR_RADIUS_CODE_COA_NAK
		},
		.rcode = RLM_MODULE_NOOP,
		.send = send_generic,
		.resume = resume_generic_radius_response,
		.section_offset = offsetof(process_radius_sections_t, coa_ack),
	},
	[ FR_RADIUS_CODE_COA_NAK ] = {
		.packet_type = {
			[RLM_MODULE_FAIL]	= FR_RADIUS_CODE_COA_NAK,
			[RLM_MODULE_INVALID]	= FR_RADIUS_CODE_COA_NAK,
			[RLM_MODULE_REJECT]	= FR_RADIUS_CODE_COA_NAK,
			[RLM_MODULE_DISALLOW]	= FR_RADIUS_CODE_COA_NAK
		},
		.rcode = RLM_MODULE_NOOP,
		.send = send_generic,
		.resume = resume_generic_radius_response,
		.section_offset = offsetof(process_radius_sections_t, coa_nak),
	},
	[ FR_RADIUS_CODE_DISCONNECT_REQUEST ] = {
		.packet_type = {
			[RLM_MODULE_NOOP]	= FR_RADIUS_CODE_DISCONNECT_ACK,
			[RLM_MODULE_OK]		= FR_RADIUS_CODE_DISCONNECT_ACK,
			[RLM_MODULE_UPDATED]	= FR_RADIUS_CODE_DISCONNECT_ACK,
			[RLM_MODULE_NOTFOUND]	= FR_RADIUS_CODE_DISCONNECT_ACK,

			[RLM_MODULE_FAIL]	= FR_RADIUS_CODE_DISCONNECT_NAK,
			[RLM_MODULE_INVALID]	= FR_RADIUS_CODE_DISCONNECT_NAK,
			[RLM_MODULE_REJECT]	= FR_RADIUS_CODE_DISCONNECT_NAK,
			[RLM_MODULE_DISALLOW]	= FR_RADIUS_CODE_DISCONNECT_NAK
		},
		.rcode = RLM_MODULE_NOOP,
		.send = send_generic,
		.resume = resume_generic_radius_response,
		.section_offset = offsetof(process_radius_sections_t, disconnect_request),
	},
	[ FR_RADIUS_CODE_DISCONNECT_ACK ] = {
		.packet_type = {
			[RLM_MODULE_FAIL]	= FR_RADIUS_CODE_DISCONNECT_NAK,
			[RLM_MODULE_INVALID]	= FR_RADIUS_CODE_DISCONNECT_NAK,
			[RLM_MODULE_REJECT]	= FR_RADIUS_CODE_DISCONNECT_NAK,
			[RLM_MODULE_DISALLOW]	= FR_RADIUS_CODE_DISCONNECT_NAK
		},
		.rcode = RLM_MODULE_NOOP,
		.send = send_generic,
		.resume = resume_generic_radius_response,
		.section_offset = offsetof(process_radius_sections_t, disconnect_ack),
	},
	[ FR_RADIUS_CODE_DISCONNECT_NAK ] = {
		.packet_type = {
			[RLM_MODULE_FAIL]	= FR_RADIUS_CODE_DISCONNECT_NAK,
			[RLM_MODULE_INVALID]	= FR_RADIUS_CODE_DISCONNECT_NAK,
			[RLM_MODULE_REJECT]	= FR_RADIUS_CODE_DISCONNECT_NAK,
			[RLM_MODULE_DISALLOW]	= FR_RADIUS_CODE_DISCONNECT_NAK
		},
		.rcode = RLM_MODULE_NOOP,
		.send = send_generic,
		.resume = resume_generic_radius_response,
		.section_offset = offsetof(process_radius_sections_t, disconnect_nak),
	},
	[ FR_RADIUS_CODE_PROTOCOL_ERROR ] = { /* @todo - fill out required fields */
		.packet_type = {
			[RLM_MODULE_FAIL] =	FR_RADIUS_CODE_DO_NOT_RESPOND,
			[RLM_MODULE_INVALID] =	FR_RADIUS_CODE_DO_NOT_RESPOND,
			[RLM_MODULE_REJECT] =	FR_RADIUS_CODE_DO_NOT_RESPOND,
			[RLM_MODULE_DISALLOW] = FR_RADIUS_CODE_DO_NOT_RESPOND
		},
		.rcode = RLM_MODULE_NOOP,
		.send = send_generic,
		.resume = resume_protocol_error,
		.section_offset = offsetof(process_radius_sections_t, protocol_error),
	},
	[ FR_RADIUS_CODE_DO_NOT_RESPOND ] = {
		.packet_type = {
			[RLM_MODULE_NOOP]	= FR_RADIUS_CODE_DO_NOT_RESPOND,
			[RLM_MODULE_OK]		= FR_RADIUS_CODE_DO_NOT_RESPOND,
			[RLM_MODULE_UPDATED]	= FR_RADIUS_CODE_DO_NOT_RESPOND,
			[RLM_MODULE_HANDLED]	= FR_RADIUS_CODE_DO_NOT_RESPOND,

			[RLM_MODULE_NOTFOUND]	= FR_RADIUS_CODE_DO_NOT_RESPOND,
			[RLM_MODULE_FAIL]	= FR_RADIUS_CODE_DO_NOT_RESPOND,
			[RLM_MODULE_INVALID]	= FR_RADIUS_CODE_DO_NOT_RESPOND,
			[RLM_MODULE_REJECT]	= FR_RADIUS_CODE_DO_NOT_RESPOND,
			[RLM_MODULE_DISALLOW]	= FR_RADIUS_CODE_DO_NOT_RESPOND
		},
		.rcode = RLM_MODULE_NOOP,
		.send = send_generic,
		.resume = resume_send_generic,
		.section_offset = offsetof(process_radius_sections_t, do_not_respond),
	}
};

static virtual_server_compile_t const compile_list[] = {
	{
		.name = "recv",
		.name2 = "Access-Request",
		.component = MOD_AUTHORIZE,
		.offset = PROCESS_CONF_OFFSET(access_request),
	},
	{
		.name = "send",
		.name2 = "Access-Accept",
		.component = MOD_POST_AUTH,
		.offset = PROCESS_CONF_OFFSET(access_accept),
	},
	{
		.name = "send",
		.name2 = "Access-Challenge",
		.component = MOD_POST_AUTH,
		.offset = PROCESS_CONF_OFFSET(access_challenge),
	},
	{
		.name = "send",
		.name2 = "Access-Reject",
		.component = MOD_POST_AUTH,
		.offset = PROCESS_CONF_OFFSET(access_reject),
	},

	{
		.name = "recv",
		.name2 = "Accounting-Request",
		.component = MOD_PREACCT,
		.offset = PROCESS_CONF_OFFSET(accounting_request),
	},
	{
		.name = "send",
		.name2 = "Accounting-Response",
		.component = MOD_ACCOUNTING,
		.offset = PROCESS_CONF_OFFSET(accounting_response),
	},

	{
		.name = "recv",
		.name2 = "Status-Server",
		.component = MOD_AUTHORIZE,
		.offset = PROCESS_CONF_OFFSET(status_server),
	},
	{
		.name = "recv",
		.name2 = "CoA-Request",
		.component = MOD_AUTHORIZE,
		.offset = PROCESS_CONF_OFFSET(coa_request),
	},
	{
		.name = "send",
		.name2 = "CoA-ACK",
		.component = MOD_POST_AUTH,
		.offset = PROCESS_CONF_OFFSET(coa_ack),
	},
	{
		.name = "send",.name2 = "CoA-NAK",
		.component = MOD_AUTHORIZE,
		.offset = PROCESS_CONF_OFFSET(coa_nak),
	},
	{
		.name = "recv",
		.name2 = "Disconnect-Request",
		.component = MOD_AUTHORIZE,
		.offset = PROCESS_CONF_OFFSET(disconnect_request),
	},
	{
		.name = "send",
		.name2 = "Disconnect-ACK",
		.component = MOD_POST_AUTH,
		.offset = PROCESS_CONF_OFFSET(disconnect_ack),
	},
	{
		.name = "send",
		.name2 = "Disconnect-NAK",
		.component = MOD_POST_AUTH,
		.offset = PROCESS_CONF_OFFSET(disconnect_nak),
	},
	{
		.name = "send",
		.name2 = "Protocol-Error",
		.component = MOD_POST_AUTH,
		.offset = PROCESS_CONF_OFFSET(protocol_error),
	},
	{
		.name = "send",
		.name2 = "Do-Not-Respond",
		.component = MOD_POST_AUTH,
		.offset = PROCESS_CONF_OFFSET(do_not_respond),
	},
	{
		.name = "authenticate",
		.name2 = CF_IDENT_ANY,
		.component = MOD_AUTHENTICATE
	},
	{
		.name = "accounting",
		.name2 = CF_IDENT_ANY,
		.component = MOD_AUTHENTICATE
	},

	{
		.name = "new",
		.name2 = "client",
		.component = MOD_AUTHORIZE,
		.offset = PROCESS_CONF_OFFSET(new_client),
	},
	{
		.name = "add",
		.name2 = "client",
		.component = MOD_AUTHORIZE,
		.offset = PROCESS_CONF_OFFSET(add_client),
	},
	{
		.name = "deny",
		.name2 = "client",
		.component = MOD_AUTHORIZE,
		.offset = PROCESS_CONF_OFFSET(deny_client),
	},
	COMPILE_TERMINATOR
};

extern fr_process_module_t process_radius;
fr_process_module_t process_radius = {
	.common = {
		.magic		= MODULE_MAGIC_INIT,
		.name		= "radius",
		.config		= config,
		.inst_size	= sizeof(process_radius_t),

		.bootstrap	= mod_bootstrap,
		.instantiate	= mod_instantiate
	},
	.process	= mod_process,
	.compile_list	= compile_list,
	.dict		= &dict_radius,
};
