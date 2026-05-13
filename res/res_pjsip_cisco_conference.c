/*
 * Asterisk -- An open source telephony toolkit.
 *
 * res_pjsip_cisco_conference
 *
 * Cisco Enterprise SIP conference control. Phase 1: read-only ConfList
 * softkey — when the user presses ConfList during an active call, the
 * phone sends an x-cisco-remotecc-request+xml REFER with
 * <softkeyeventmsg><softkeyevent>ConfList</softkeyevent>. We resolve
 * the supplied <dialogid> to the matching ast_sip_session, walk the
 * channel's bridge, and reply with a multipart REFER back to the phone
 * containing a Cisco proprietary <CiscoIPPhoneMenu> listing each
 * bridge participant.
 *
 * Wire shapes mirror the chan_sip cisco-usecallmanager patch's
 * channels/sip/conference.c:751 sip_conference_participants.
 *
 * Phase 1 is deliberately limited:
 *   - Mute / Remove / Update softkeys on the menu are NOT yet wired up.
 *     Those arrive as datapassthroughreq REFERs with applicationid=1
 *     (SIP_REMOTECC_CONF_LIST) and a <usercalldata> field; this module
 *     does not yet handle them and they fall through to the regular
 *     RemoteCC handler (which 603 Declines).
 *   - Confrn (3-way conference creation) is implemented as of phase 1b:
 *     parses the inbound REFER's <dialogid> + <consultdialogid>, resolves
 *     both to ast_sip_session, and stitches the two existing 1:1 bridges
 *     into one MULTIMIX bridge via ast_bridge_move. Sends 202 with
 *     Refer-Sub: false (the phone supports norefersub, so no follow-up
 *     NOTIFY is needed). Join (multi-call merge via Select/Unselect) and
 *     RmLastConf are deferred.
 *
 * Architecture:
 *   - Registers a pjsip_module at PJSIP_MOD_PRIORITY_APPLICATION - 2,
 *     one slot before res_pjsip_cisco_remotecc, so we see incoming
 *     REFERs with x-cisco bodies first. ConfList REFERs we claim and
 *     return PJ_TRUE; everything else returns PJ_FALSE so the
 *     remotecc handler still gets its turn.
 *   - Resolves the XML <dialogid> to an ast_sip_session via
 *     pjsip_ua_find_dialog + ast_sip_dialog_get_session — independent
 *     of the remotecc dialog registry, so conference.so does not need
 *     a cross-module symbol export.
 */

/*** MODULEINFO
	<depend>res_pjsip</depend>
	<depend>res_pjsip_session</depend>
	<depend>libxml2</depend>
	<support_level>extended</support_level>
 ***/

#include "asterisk.h"

#include <pjsip.h>
#include <pjsip_ua.h>
#include <pjsip/sip_multipart.h>
#include <pjlib.h>

#include "asterisk/module.h"
#include "asterisk/bridge.h"
#include "asterisk/channel.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"
#include "asterisk/taskprocessor.h"
#include "asterisk/xml.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/res_pjsip_session.h"
#include "asterisk/sorcery.h"

#include "cisco_endpoint.h"
#include "cisco_session.h"

/* application_id values mirror the chan_sip patch's
 * enum sip_remotecc_application in channels/sip/include/remotecc.h.
 * SIP_REMOTECC_NONE = 0, SIP_REMOTECC_CONF_LIST = 1. */
#define CONFERENCE_APP_ID_CONF_LIST 1

#define CONFERENCE_MAX_BODY 8192
#define CONFERENCE_MENU_PARTICIPANT_NAME 64

/* Header of the multipart REFER body we send back: a datapassthroughreq
 * echo so the phone correlates the response with its ConfList request,
 * followed by a CiscoIPPhoneMenu listing participants. The conference
 * id is synthesised from the bridge's pointer-derived counter — we
 * don't track Cisco-specific conference state in Phase 1. */
#define MENU_DATAPASSTHROUGH_FMT                                          \
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"                  \
	"<x-cisco-remotecc-request>\n"                                  \
	"  <datapassthroughreq>\n"                                      \
	"    <applicationid>%d</applicationid>\n"                       \
	"    <transactionid>0</transactionid>\n"                        \
	"    <stationsequence>StationSequenceLast</stationsequence>\n"  \
	"    <displaypriority>2</displaypriority>\n"                    \
	"    <appinstance>0</appinstance>\n"                            \
	"    <routingid>0</routingid>\n"                                \
	"    <confid>%u</confid>\n"                                     \
	"  </datapassthroughreq>\n"                                     \
	"</x-cisco-remotecc-request>\n"

#define MENU_HEADER_FMT                                                   \
	"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"                  \
	"<CiscoIPPhoneMenu>\n"                                          \
	"  <Title>Conference</Title>\n"

#define MENU_ITEM_FMT                                                     \
	"  <MenuItem>\n"                                                \
	"    <Name>%s</Name>\n"                                         \
	"    <URL>UserCallData:%d:0:%u:0:%d</URL>\n"                    \
	"  </MenuItem>\n"

/* Phase 1: prompt only. Mute/Remove/Update softkey items omitted —
 * adding them would be misleading because we don't yet handle the
 * usercalldata REFERs they would generate. */
#define MENU_FOOTER                                                       \
	"  <Prompt>Phase 1 (read-only)</Prompt>\n"                      \
	"  <SoftKeyItem>\n"                                             \
	"    <Name>Exit</Name>\n"                                       \
	"    <Position>1</Position>\n"                                  \
	"    <URL>SoftKey:Exit</URL>\n"                                 \
	"  </SoftKeyItem>\n"                                            \
	"</CiscoIPPhoneMenu>\n"

static struct ast_taskprocessor *conference_serializer;

struct conference_dialog_id {
	char call_id[256];
	char local_tag[128];
	char remote_tag[128];
};

struct conflist_task_data {
	struct ast_sip_endpoint *endpoint;
	/* The specific contact that pressed ConfList. Captured at rx time
	 * (rdata is gone by the time the task runs) and used to send the
	 * menu REFER back to only that phone — not all contacts of a
	 * shared-line AOR. */
	struct ast_sip_contact *contact;
	struct conference_dialog_id dialog_id;
};

static void conflist_task_data_destroy(void *obj)
{
	struct conflist_task_data *data = obj;
	ao2_cleanup(data->contact);
	ao2_cleanup(data->endpoint);
}

/* ----------------------------------------------------------------------
 * Phase 1a: Conference softkey — wire-level scaffolding only.
 *
 * Parses the inbound Conference REFER's two dialog IDs and logs what we
 * WOULD do. No bridge manipulation yet. Phase 1b adds dialog→channel
 * resolution and the actual multimix bridge plumbing.
 * ---------------------------------------------------------------------- */

struct conference_task_data {
	struct ast_sip_endpoint *endpoint;
	struct ast_sip_contact *contact;            /* who pressed Confrn */
	struct conference_dialog_id active_dialog;  /* <dialogid>: the live call */
	struct conference_dialog_id consult_dialog; /* <consultdialogid>: the held call */
};

static void conference_task_data_destroy(void *obj)
{
	struct conference_task_data *data = obj;
	ao2_cleanup(data->contact);
	ao2_cleanup(data->endpoint);
}

/*!
 * \brief Parsed view of an inbound Cisco RemoteCC softkey REFER.
 *
 * Populated by detect_remotecc_softkey; consumed by the dispatch in
 * conference_on_rx_request.
 */
enum remotecc_softkey_kind {
	REMOTECC_SOFTKEY_NONE = 0,
	REMOTECC_SOFTKEY_CONFLIST,
	REMOTECC_SOFTKEY_CONFERENCE,
};

struct remotecc_softkey_msg {
	enum remotecc_softkey_kind kind;
	struct conference_dialog_id dialog_id;        /* always populated */
	struct conference_dialog_id consult_dialog_id;/* Conference only */
};

/*!
 * \brief Copy the three child fields of <dialogid> / <consultdialogid> /
 *        <joindialogid> into \a out.
 *
 * \retval 1 on a fully-populated dialog ref (callid + both tags non-empty),
 * \retval 0 otherwise.
 */
static int copy_dialog_id(struct ast_xml_node *dialog_node,
	struct conference_dialog_id *out)
{
	if (!dialog_node) {
		return 0;
	}
	memset(out, 0, sizeof(*out));
	cisco_xml_copy_child_text(dialog_node, "callid", out->call_id,
		sizeof(out->call_id));
	cisco_xml_copy_child_text(dialog_node, "localtag", out->local_tag,
		sizeof(out->local_tag));
	cisco_xml_copy_child_text(dialog_node, "remotetag", out->remote_tag,
		sizeof(out->remote_tag));
	return !ast_strlen_zero(out->call_id)
		&& !ast_strlen_zero(out->local_tag)
		&& !ast_strlen_zero(out->remote_tag);
}

/*!
 * \brief Parse a Cisco RemoteCC REFER body and classify the softkey.
 *
 * Returns 1 + populates \a out for ConfList and Conference (with both
 * <dialogid> AND, for Conference, <consultdialogid>). Returns 0 for any
 * other softkey (which leaves the REFER to res_pjsip_cisco_remotecc).
 *
 * For Conference: both dialog blocks must be present and complete;
 * otherwise we 0 it and let the caller pass through.
 */
static int detect_remotecc_softkey(pjsip_msg_body *body,
	struct remotecc_softkey_msg *out)
{
	struct ast_xml_doc *doc;
	struct ast_xml_node *root;
	struct ast_xml_node *softkey_msg;
	char softkey[64];
	int handled = 0;

	memset(out, 0, sizeof(*out));

	if (!body || body->len > CONFERENCE_MAX_BODY) {
		return 0;
	}

	doc = cisco_xml_read_body(body);
	if (!doc) {
		return 0;
	}

	root = ast_xml_get_root(doc);
	if (!root || strcasecmp(ast_xml_node_get_name(root),
			"x-cisco-remotecc-request")) {
		goto done;
	}

	softkey_msg = ast_xml_find_element(ast_xml_node_get_children(root),
		"softkeyeventmsg", NULL, NULL);
	if (!softkey_msg) {
		goto done;
	}

	if (!cisco_xml_copy_child_text(softkey_msg, "softkeyevent", softkey,
			sizeof(softkey))) {
		goto done;
	}

	if (!strcmp(softkey, "ConfList")) {
		out->kind = REMOTECC_SOFTKEY_CONFLIST;
	} else if (!strcmp(softkey, "Conference")) {
		out->kind = REMOTECC_SOFTKEY_CONFERENCE;
	} else {
		/* Not ours — let remotecc.c handle (HLog/MCID/Park/Join/…). */
		goto done;
	}

	if (!copy_dialog_id(ast_xml_find_element(
			ast_xml_node_get_children(softkey_msg),
			"dialogid", NULL, NULL),
			&out->dialog_id)) {
		out->kind = REMOTECC_SOFTKEY_NONE;
		goto done;
	}

	if (out->kind == REMOTECC_SOFTKEY_CONFERENCE) {
		if (!copy_dialog_id(ast_xml_find_element(
				ast_xml_node_get_children(softkey_msg),
				"consultdialogid", NULL, NULL),
				&out->consult_dialog_id)) {
			ast_log(LOG_WARNING,
				"cisco-conference: Conference REFER missing or "
				"incomplete <consultdialogid> — ignoring\n");
			out->kind = REMOTECC_SOFTKEY_NONE;
			goto done;
		}
	}

	handled = 1;

done:
	ast_xml_close(doc);
	return handled;
}

static struct ast_channel *bridge_peer_channel_ref(struct ast_channel *self,
	struct ast_bridge *bridge, int index)
{
	struct ao2_container *peers;
	struct ao2_iterator iter;
	struct ast_channel *peer;
	struct ast_channel *match = NULL;
	int i = 0;

	peers = ast_bridge_peers(bridge);
	if (!peers) {
		return NULL;
	}

	iter = ao2_iterator_init(peers, 0);
	while ((peer = ao2_iterator_next(&iter))) {
		if (peer == self) {
			ao2_cleanup(peer);
			continue;
		}
		if (i == index) {
			match = peer;  /* keep ref */
			break;
		}
		++i;
		ao2_cleanup(peer);
	}
	ao2_iterator_destroy(&iter);
	ao2_cleanup(peers);
	return match;
}

static void append_menu_item(struct ast_str **out, struct ast_channel *peer,
	unsigned int conference_id, int index)
{
	char raw_name[CONFERENCE_MENU_PARTICIPANT_NAME];
	char escaped_name[CONFERENCE_MENU_PARTICIPANT_NAME * 5 + 1];
	struct ast_party_caller *caller;

	raw_name[0] = '\0';

	ast_channel_lock(peer);
	caller = ast_channel_caller(peer);
	if (caller->id.name.valid && !ast_strlen_zero(caller->id.name.str)) {
		ast_copy_string(raw_name, caller->id.name.str, sizeof(raw_name));
	} else if (caller->id.number.valid && !ast_strlen_zero(caller->id.number.str)) {
		ast_copy_string(raw_name, caller->id.number.str, sizeof(raw_name));
	} else {
		snprintf(raw_name, sizeof(raw_name), "Anonymous %d", index);
	}
	ast_channel_unlock(peer);

	if (ast_xml_escape(raw_name, escaped_name, sizeof(escaped_name))) {
		/* Source somehow grew past 5x — fall back to a placeholder so
		 * the menu doesn't get truncated mid-element. */
		snprintf(escaped_name, sizeof(escaped_name), "Participant %d", index);
	}

	ast_str_append(out, 0, MENU_ITEM_FMT, escaped_name,
		CONFERENCE_APP_ID_CONF_LIST, conference_id, index);
}

/*!
 * \brief Build the multipart REFER body listing bridge participants.
 *
 * \param pool          tdata pool the body is allocated from
 * \param self          the channel that sent ConfList (excluded from the menu)
 * \param bridge        the bridge enumerated for participants
 * \param conference_id synthetic id used in the URL action of each MenuItem
 * \retval NULL on alloc / format failure
 */
static pjsip_msg_body *make_menu_body(pj_pool_t *pool, struct ast_channel *self,
	struct ast_bridge *bridge, unsigned int conference_id)
{
	pj_str_t boundary = pj_str("uniqueBoundary");
	pjsip_msg_body *multipart;
	struct ast_str *menu;
	struct ast_str *header;
	int index;
	struct ast_channel *peer;

	multipart = pjsip_multipart_create(pool, NULL, &boundary);
	if (!multipart) {
		return NULL;
	}

	header = ast_str_create(512);
	if (!header) {
		return NULL;
	}
	ast_str_set(&header, 0, MENU_DATAPASSTHROUGH_FMT,
		CONFERENCE_APP_ID_CONF_LIST, conference_id);
	cisco_remotecc_multipart_add_part(pool, multipart, ast_str_buffer(header));
	ast_free(header);

	menu = ast_str_create(2048);
	if (!menu) {
		return NULL;
	}
	ast_str_set(&menu, 0, MENU_HEADER_FMT);

	for (index = 0; (peer = bridge_peer_channel_ref(self, bridge, index)); ++index) {
		append_menu_item(&menu, peer, conference_id, index);
		ast_channel_unref(peer);
	}

	if (!index) {
		ast_str_append(&menu, 0,
			"  <MenuItem>\n"
			"    <Name>(no other participants)</Name>\n"
			"    <URL>SoftKey:Exit</URL>\n"
			"  </MenuItem>\n");
	}
	ast_str_append(&menu, 0, MENU_FOOTER);

	/* The menu part uses application/x-cisco-remotecc-cm+xml, NOT the
	 * shared helper's default application/x-cisco-remotecc-request+xml.
	 * Cisco firmware looks at the second part's content-type to decide
	 * whether to render the CiscoIPPhoneMenu; mislabelling it as
	 * x-cisco-remotecc-request+xml silently drops the menu (which is
	 * how this looked: REFER 202'd by firmware, no menu rendered). The
	 * chan_sip patch (handle_remotecc_conflist) gets this right. So
	 * inline pjsip_multipart_create_part here rather than using the
	 * cisco_remotecc_multipart_add_part shared helper. */
	{
		pj_str_t menu_type    = pj_str("application");
		pj_str_t menu_subtype = pj_str("x-cisco-remotecc-cm+xml");
		pj_str_t menu_text;
		pjsip_multipart_part *menu_part;

		pj_strdup2(pool, &menu_text, ast_str_buffer(menu));
		menu_part = pjsip_multipart_create_part(pool);
		if (menu_part) {
			menu_part->body = pjsip_msg_body_create(pool,
				&menu_type, &menu_subtype, &menu_text);
			pjsip_multipart_add_part(pool, multipart, menu_part);
		}
	}
	ast_free(menu);

	return multipart;
}

struct conflist_build_ctx {
	struct ast_channel *self;
	struct ast_bridge *bridge;
	unsigned int conference_id;
};

static pjsip_msg_body *conflist_build_adapter(pj_pool_t *pool, void *vctx)
{
	struct conflist_build_ctx *ctx = vctx;
	return make_menu_body(pool, ctx->self, ctx->bridge, ctx->conference_id);
}

static void send_menu_to_contact(struct ast_sip_endpoint *endpoint,
	struct ast_sip_contact *contact,
	struct ast_channel *self, struct ast_bridge *bridge,
	unsigned int conference_id)
{
	struct conflist_build_ctx ctx = {
		.self          = self,
		.bridge        = bridge,
		.conference_id = conference_id,
	};

	ast_log(LOG_NOTICE,
		"cisco-conference: pushing ConfList menu to %s @ %s\n",
		ast_sorcery_object_get_id(endpoint), contact->uri);

	cisco_endpoint_send_refer_to_contact(endpoint, contact,
		"cisco-conference", "cisco-conflist", "ConfList menu",
		conflist_build_adapter, &ctx);
}

static int conflist_send_task(void *obj)
{
	struct conflist_task_data *data = obj;
	struct ast_sip_session *session;
	struct ast_channel *channel = NULL;
	struct ast_bridge *bridge = NULL;
	const char *endpoint_id;
	unsigned int conference_id;

	endpoint_id = ast_sorcery_object_get_id(data->endpoint);

	session = cisco_dialog_session_lookup(data->dialog_id.call_id,
		data->dialog_id.local_tag, data->dialog_id.remote_tag);
	if (!session) {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s sent ConfList for unknown dialog "
			"(callid=%s localtag=%s remotetag=%s)\n",
			endpoint_id, data->dialog_id.call_id,
			data->dialog_id.local_tag, data->dialog_id.remote_tag);
		goto cleanup;
	}

	channel = cisco_session_channel_ref(session);
	if (!channel) {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s sent ConfList for dialog with no channel\n",
			endpoint_id);
		goto cleanup;
	}

	ast_channel_lock(channel);
	bridge = ast_channel_get_bridge(channel);
	ast_channel_unlock(channel);

	if (!bridge) {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s pressed ConfList outside any bridge\n",
			endpoint_id);
		goto cleanup;
	}

	/* Synthesise a stable per-bridge id for the menu URLs. The menu's
	 * <confid> is consulted by the firmware when subsequent
	 * datapassthroughreq REFERs come in for Mute/Remove/Update — Phase 1
	 * doesn't yet handle those, but giving each bridge a deterministic
	 * id keeps Phase 2 wire-compatible. Truncating the bridge pointer
	 * isn't ideal but matches the chan_sip patch's per-conference
	 * counter shape. */
	conference_id = (unsigned int)(uintptr_t) bridge;

	send_menu_to_contact(data->endpoint, data->contact,
		channel, bridge, conference_id);

cleanup:
	ao2_cleanup(bridge);
	ao2_cleanup(channel);
	ao2_cleanup(session);
	ao2_cleanup(data);
	return 0;
}

static void queue_conflist(struct ast_sip_endpoint *endpoint,
	struct ast_sip_contact *contact,
	const struct conference_dialog_id *dialog_id)
{
	struct conflist_task_data *data;

	data = ao2_alloc(sizeof(*data), conflist_task_data_destroy);
	if (!data) {
		return;
	}

	ao2_ref(endpoint, +1);
	data->endpoint = endpoint;
	ao2_ref(contact, +1);
	data->contact = contact;
	data->dialog_id = *dialog_id;

	if (ast_sip_push_task(conference_serializer, conflist_send_task, data)) {
		ast_log(LOG_WARNING,
			"cisco-conference: failed to queue ConfList task\n");
		ao2_cleanup(data);
	}
}

/*!
 * \brief Build a 3-way multimix bridge from the two existing 1:1 bridges.
 *
 * The phone has two SIP dialogs at the moment Confrn is pressed:
 *
 *   active_dialog  — the original call (now held during the consult).
 *                    chan_phone_A is the phone's leg; chan_remote_A is
 *                    the other party.
 *   consult_dialog — the consult call (currently active).
 *                    chan_phone_B is the phone's leg; chan_remote_B is
 *                    the other party.
 *
 * The merge:
 *   1. Create a fresh multimix bridge.
 *   2. Move chan_phone_A from bridge_A into the conference.
 *   3. Move chan_remote_A from bridge_A — bridge_A is now empty and
 *      dissolves (DISSOLVE_EMPTY).
 *   4. Move chan_remote_B from bridge_B into the conference.
 *      bridge_B still has chan_phone_B sitting in it alone; the phone
 *      observes the audio merge and BYEs that leg on its own, after
 *      which bridge_B dissolves too.
 *
 * chan_phone_B is deliberately NOT imparted — the phone only has one
 * physical audio path; chan_phone_A is the anchor. Same pattern the
 * chan_sip cisco-usecallmanager patch's conference_thread uses
 * (see channels/chan_sip.c, handle_remotecc_conference flow).
 *
 * No Event:refer NOTIFY is sent — cisco_send_refer_response already put
 * `Refer-Sub: false` on our 202, so no implicit subscription exists for
 * the phone to wait on. Verified against CP-7975G/9.4.2 on 2026-05-13:
 * the phone's REFER advertises norefersub in its Supported header and
 * accepts the Refer-Sub: false acknowledgement cleanly.
 */
static int conference_send_task(void *obj)
{
	struct conference_task_data *data = obj;
	const char *endpoint_id = ast_sorcery_object_get_id(data->endpoint);
	struct ast_sip_session *session_a = NULL;
	struct ast_sip_session *session_b = NULL;
	struct ast_channel *chan_phone_a = NULL;
	struct ast_channel *chan_phone_b = NULL;
	struct ast_channel *chan_remote_a = NULL;
	struct ast_channel *chan_remote_b = NULL;
	struct ast_bridge *bridge_a = NULL;
	struct ast_bridge *bridge_b = NULL;
	struct ast_bridge *conf = NULL;

	session_a = cisco_dialog_session_lookup(data->active_dialog.call_id,
		data->active_dialog.local_tag, data->active_dialog.remote_tag);
	session_b = cisco_dialog_session_lookup(data->consult_dialog.call_id,
		data->consult_dialog.local_tag, data->consult_dialog.remote_tag);
	if (!session_a || !session_b) {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s pressed Confrn but %s dialog "
			"doesn't match any live session (active callid=%s, "
			"consult callid=%s) — aborting\n",
			endpoint_id,
			!session_a && !session_b ? "neither"
				: !session_a ? "active" : "consult",
			data->active_dialog.call_id, data->consult_dialog.call_id);
		goto cleanup;
	}

	chan_phone_a = cisco_session_channel_ref(session_a);
	chan_phone_b = cisco_session_channel_ref(session_b);
	if (!chan_phone_a || !chan_phone_b) {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s — one or both Confrn legs has no "
			"live channel (active=%p, consult=%p) — aborting\n",
			endpoint_id, chan_phone_a, chan_phone_b);
		goto cleanup;
	}

	chan_remote_a = ast_channel_bridge_peer(chan_phone_a);
	chan_remote_b = ast_channel_bridge_peer(chan_phone_b);
	if (!chan_remote_a || !chan_remote_b) {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s — can't conference (%s has no "
			"bridge peer; happens when a leg is talking to a "
			"single-channel app like Playback/MoH rather than a "
			"real call)\n", endpoint_id,
			!chan_remote_a && !chan_remote_b ? "both legs"
				: !chan_remote_a ? "active leg" : "consult leg");
		goto cleanup;
	}

	ast_channel_lock(chan_phone_a);
	bridge_a = ast_channel_get_bridge(chan_phone_a);
	ast_channel_unlock(chan_phone_a);
	ast_channel_lock(chan_phone_b);
	bridge_b = ast_channel_get_bridge(chan_phone_b);
	ast_channel_unlock(chan_phone_b);
	if (!bridge_a || !bridge_b) {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s — one or both legs has no bridge "
			"object (active=%p, consult=%p) — aborting\n",
			endpoint_id, bridge_a, bridge_b);
		goto cleanup;
	}

	/* MULTIMIX | NATIVE: softmix mixer with native pass-through where
	 * possible. SMART: let the bridging framework auto-promote tech as
	 * channel count changes. DISSOLVE_EMPTY: tear down when the last
	 * channel leaves. No DISSOLVE_HANGUP — we want the mix to keep
	 * going if the initiator hangs up (cisco_keep_conference=yes
	 * implicit for Phase 1; configurable later). TRANSFER_BRIDGE_ONLY:
	 * don't let an attended-transfer-into-conference race the merge. */
	conf = ast_bridge_base_new(
		AST_BRIDGE_CAPABILITY_MULTIMIX | AST_BRIDGE_CAPABILITY_NATIVE,
		AST_BRIDGE_FLAG_DISSOLVE_EMPTY | AST_BRIDGE_FLAG_SMART
			| AST_BRIDGE_FLAG_TRANSFER_BRIDGE_ONLY,
		"cisco_conference", NULL, NULL);
	if (!conf) {
		ast_log(LOG_WARNING,
			"cisco-conference: %s — ast_bridge_base_new failed; "
			"aborting Confrn\n", endpoint_id);
		goto cleanup;
	}

	/* Move chan_phone_A first so the user's audio path follows it into
	 * the conference; then drag remote_A across (bridge_A goes empty,
	 * dissolves); then remote_B (chan_phone_B stays in bridge_B alone
	 * until the phone BYEs that leg). attempt_recovery=1: if a move
	 * fails, put chan back into its source bridge so it isn't orphaned. */
	if (ast_bridge_move(conf, bridge_a, chan_phone_a, NULL, 1)
		|| ast_bridge_move(conf, bridge_a, chan_remote_a, NULL, 1)
		|| ast_bridge_move(conf, bridge_b, chan_remote_b, NULL, 1)) {
		ast_log(LOG_WARNING,
			"cisco-conference: %s — partial bridge move during Confrn; "
			"some channels may have been left in their original bridges. "
			"Phone audio path may be in an odd state — hang up to recover.\n",
			endpoint_id);
		/* Leave the (possibly-partial) conf bridge in place; dissolving
		 * it now could yank channels that did succeed into limbo. The
		 * DISSOLVE_EMPTY flag will collect it once everyone leaves. */
		goto cleanup;
	}

	ast_log(LOG_NOTICE,
		"cisco-conference: %s — built 3-way conference: %s + %s + %s "
		"(consult-side anchor %s left in residual bridge until phone "
		"BYEs it)\n",
		endpoint_id,
		ast_channel_name(chan_phone_a),
		ast_channel_name(chan_remote_a),
		ast_channel_name(chan_remote_b),
		ast_channel_name(chan_phone_b));

	/* Phone UI cue: Cisco firmware (CP-7975G/9.4.2 verified) needs an
	 * Event: refer NOTIFY to transition its display from "active + held"
	 * to "Conference" after Confrn succeeds — even though we sent
	 * Refer-Sub: false on the 202 (which RFC 4488 says means "don't
	 * expect a NOTIFY"). The chan_sip cisco-usecallmanager patch sends
	 * an in-dialog NOTIFY for exactly this reason. We try the simpler
	 * out-of-dialog pattern first (same shape as our Park-orbit NOTIFYs)
	 * — Cisco firmware MAY correlate on Event:refer alone without
	 * strict in-dialog matching. If it doesn't, we'll fall back to
	 * pjsip_xfer_create_uas + an in-dialog NOTIFY in a follow-up. */
	{
		pjsip_tx_data *tdata = NULL;

		if (!ast_sip_create_request("NOTIFY", NULL, data->endpoint,
				NULL, data->contact, &tdata)) {
			ast_sip_add_header(tdata, "Event", "refer");
			ast_sip_add_header(tdata, "Subscription-State",
				"active;expires=60");
			if (ast_sip_send_request(tdata, NULL, data->endpoint,
					NULL, NULL)) {
				ast_log(LOG_WARNING,
					"cisco-conference: %s — Conference NOTIFY "
					"send failed for %s (bridge OK, phone UI may "
					"not transition)\n",
					endpoint_id, data->contact->uri);
			} else {
				ast_log(LOG_NOTICE,
					"cisco-conference: %s — Event:refer NOTIFY "
					"sent to %s\n",
					endpoint_id, data->contact->uri);
			}
		}
	}

cleanup:
	/* Release our +1 ref on the conf bridge — channels imparted into it
	 * hold their own refs, so it stays alive until DISSOLVE_EMPTY fires. */
	ao2_cleanup(conf);
	ao2_cleanup(bridge_a);
	ao2_cleanup(bridge_b);
	ast_channel_cleanup(chan_remote_a);
	ast_channel_cleanup(chan_remote_b);
	ast_channel_cleanup(chan_phone_a);
	ast_channel_cleanup(chan_phone_b);
	ao2_cleanup(session_a);
	ao2_cleanup(session_b);
	ao2_cleanup(data);
	return 0;
}

static void queue_conference(struct ast_sip_endpoint *endpoint,
	struct ast_sip_contact *contact,
	const struct conference_dialog_id *active_dialog,
	const struct conference_dialog_id *consult_dialog)
{
	struct conference_task_data *data;

	data = ao2_alloc(sizeof(*data), conference_task_data_destroy);
	if (!data) {
		return;
	}
	ao2_ref(endpoint, +1);
	data->endpoint = endpoint;
	ao2_ref(contact, +1);
	data->contact = contact;
	data->active_dialog  = *active_dialog;
	data->consult_dialog = *consult_dialog;

	if (ast_sip_push_task(conference_serializer, conference_send_task, data)) {
		ast_log(LOG_WARNING,
			"cisco-conference: failed to queue Conference task\n");
		ao2_cleanup(data);
	}
}

static pj_bool_t conference_on_rx_request(pjsip_rx_data *rdata)
{
	struct ast_sip_endpoint *endpoint;
	struct cisco_endpoint *cisco;
	const char *endpoint_id;
	pjsip_msg_body *body;
	struct remotecc_softkey_msg msg;
	struct ast_sip_contact *contact;

	if (!rdata || !rdata->msg_info.msg
		|| rdata->msg_info.msg->type != PJSIP_REQUEST_MSG) {
		return PJ_FALSE;
	}

	if (pjsip_method_cmp(&rdata->msg_info.msg->line.req.method,
			pjsip_get_refer_method())) {
		return PJ_FALSE;
	}

	endpoint = cisco_rdata_get_endpoint(rdata);
	if (!endpoint) {
		return PJ_FALSE;
	}
	endpoint_id = ast_sorcery_object_get_id(endpoint);

	cisco = cisco_endpoint_get(endpoint_id);
	if (!cisco) {
		ao2_cleanup(endpoint);
		return PJ_FALSE;
	}
	ao2_cleanup(cisco);

	body = cisco_find_remotecc_request_body(rdata);
	if (!body || !detect_remotecc_softkey(body, &msg)
		|| msg.kind == REMOTECC_SOFTKEY_NONE) {
		ao2_cleanup(endpoint);
		return PJ_FALSE;
	}

	/* Capture WHICH contact pressed the softkey. The rdata's source
	 * IP:port matches the host:port in exactly one of the endpoint's
	 * registered contacts (the phone that sent the REFER). The follow-up
	 * REFER / NOTIFY goes back to that one only — otherwise on a shared
	 * line every phone receives the response. */
	contact = cisco_endpoint_find_contact_from_rdata(endpoint, rdata);
	if (!contact) {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s pressed a softkey but source "
			"%s:%d doesn't match any registered contact — "
			"can't target the response\n", endpoint_id,
			rdata->pkt_info.src_name, rdata->pkt_info.src_port);
		cisco_send_refer_response(rdata, 202, endpoint);
		ao2_cleanup(endpoint);
		return PJ_TRUE;
	}

	switch (msg.kind) {
	case REMOTECC_SOFTKEY_CONFLIST:
		ast_log(LOG_NOTICE,
			"cisco-conference: %s pressed ConfList from %s (callid=%s)\n",
			endpoint_id, contact->uri, msg.dialog_id.call_id);
		cisco_send_refer_response(rdata, 202, endpoint);
		queue_conflist(endpoint, contact, &msg.dialog_id);
		break;
	case REMOTECC_SOFTKEY_CONFERENCE:
		ast_log(LOG_NOTICE,
			"cisco-conference: %s pressed Confrn from %s "
			"(active callid=%s, consult callid=%s)\n",
			endpoint_id, contact->uri,
			msg.dialog_id.call_id, msg.consult_dialog_id.call_id);
		cisco_send_refer_response(rdata, 202, endpoint);
		queue_conference(endpoint, contact, &msg.dialog_id,
			&msg.consult_dialog_id);
		break;
	default:
		/* Compiler completeness — kind == NONE already returned above. */
		break;
	}

	ao2_cleanup(contact);
	ao2_cleanup(endpoint);
	return PJ_TRUE;
}

static pjsip_module conference_module = {
	.name             = { "cisco-conference", 16 },
	.id               = -1,
	/* One slot before res_pjsip_cisco_remotecc so we see ConfList
	 * REFERs first; non-ConfList traffic falls through to remotecc. */
	.priority         = PJSIP_MOD_PRIORITY_APPLICATION - 2,
	.on_rx_request    = conference_on_rx_request,
};

static int load_module(void)
{
	conference_serializer = ast_sip_create_serializer("pjsip/cisco-conference");
	if (!conference_serializer) {
		return AST_MODULE_LOAD_DECLINE;
	}

	if (ast_sip_register_service(&conference_module)) {
		ast_taskprocessor_unreference(conference_serializer);
		conference_serializer = NULL;
		return AST_MODULE_LOAD_DECLINE;
	}

	return AST_MODULE_LOAD_SUCCESS;
}

static int unload_module(void)
{
	/* Refuse runtime unload — same rationale as every other cisco_*
	 * module: pjsip service unregister doesn't wait for in-flight
	 * callbacks on other servant threads, and the serializer may have
	 * pending menu sends queued. */
	if (!ast_shutdown_final()) {
		return -1;
	}

	ast_sip_unregister_service(&conference_module);
	ast_taskprocessor_unreference(conference_serializer);
	conference_serializer = NULL;
	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER,
	"PJSIP Cisco RemoteCC conference control (Phase 1: ConfList)",
	.support_level = AST_MODULE_SUPPORT_EXTENDED,
	.load = load_module,
	.unload = unload_module,
	.load_pri = AST_MODPRI_CHANNEL_DEPEND,
	.requires = "res_pjsip,res_pjsip_session,res_pjsip_cisco_endpoint",
);
