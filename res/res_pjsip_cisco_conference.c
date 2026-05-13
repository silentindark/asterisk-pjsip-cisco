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
 * Coverage:
 *   - ConfList softkey + Mute / Remove / Update / participant-pick
 *     action softkeys (chan_sip's two-step state machine: a sticky
 *     softkey REFER sets pending action; the next participant-pick
 *     REFER applies it. Default action when no softkey was pressed
 *     first is Mute, matching the patch).
 *   - Confrn (3-way conference creation) is implemented in full,
 *     including holdretrieve REFER + Cisco-flavoured completion NOTIFY,
 *     connected-line "Conference" display token, explicit consult-anchor
 *     softhangup, and the cisco_keep_conference initiator-hangup knob.
 *   - Join (multi-call merge via Select/Unselect) and RmLastConf are
 *     still deferred.
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
#include "asterisk/bridge_channel.h"
#include "asterisk/bridge_features.h"
#include "asterisk/channel.h"
#include "asterisk/callerid.h"
#include "asterisk/pbx.h"
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

/* Chan_sip patch uses UserCallDataSoftKey:Select:... for the sticky
 * action softkeys (the phone sends an "action-set" REFER, then on the
 * next participant click an "apply" REFER) and UserCallDataSoftKey:
 * Update:... for the one-shot menu refresh. confid is the bridge-derived
 * synthetic id we put in <confid> in the part-1 echo above; the phone
 * sends it back so callers correlate stateful traffic to a conference. */
#define MENU_FOOTER_FMT                                                   \
	"  <Prompt>Please select</Prompt>\n"                            \
	"  <SoftKeyItem>\n"                                             \
	"    <Name>Exit</Name>\n"                                       \
	"    <Position>1</Position>\n"                                  \
	"    <URL>SoftKey:Exit</URL>\n"                                 \
	"  </SoftKeyItem>\n"                                            \
	"  <SoftKeyItem>\n"                                             \
	"    <Name>Remove</Name>\n"                                     \
	"    <Position>2</Position>\n"                                  \
	"    <URL>UserCallDataSoftKey:Select:%d:0:%u:0:Remove</URL>\n"  \
	"  </SoftKeyItem>\n"                                            \
	"  <SoftKeyItem>\n"                                             \
	"    <Name>Mute</Name>\n"                                       \
	"    <Position>3</Position>\n"                                  \
	"    <URL>UserCallDataSoftKey:Select:%d:0:%u:0:Mute</URL>\n"    \
	"  </SoftKeyItem>\n"                                            \
	"  <SoftKeyItem>\n"                                             \
	"    <Name>Update</Name>\n"                                     \
	"    <Position>4</Position>\n"                                  \
	"    <URL>UserCallDataSoftKey:Update:%d:0:%u:0:Update</URL>\n"  \
	"  </SoftKeyItem>\n"                                            \
	"</CiscoIPPhoneMenu>\n"

static struct ast_taskprocessor *conference_serializer;

struct conference_dialog_id {
	char call_id[256];
	char local_tag[128];
	char remote_tag[128];
};

/*!
 * \brief Per-endpoint pending ConfList state, set when the phone presses
 *        ConfList and consumed by subsequent action REFERs (which carry
 *        no <dialogid> themselves, only <confid>).
 *
 * Two fields with different lifetimes:
 *   dialog_id  — refreshed on every ConfList press, kept across action
 *                REFERs. Lets the action handler relocate the conference
 *                bridge via the same dialog → session → bridge path the
 *                initial ConfList press uses.
 *   action     — set when the phone presses the Mute/Remove softkey
 *                ("sticky action"), consumed on the next participant
 *                pick. Empty otherwise — chan_sip patch's default in
 *                that case is Mute.
 */
struct conflist_pending {
	char endpoint_id[128];
	struct conference_dialog_id dialog_id;
	int dialog_id_valid;
	char action[16];  /* "" / "Mute" / "Remove" */
};

static struct ao2_container *conflist_pending_actions;
#define CONFLIST_PENDING_BUCKETS 31

static int conflist_pending_hash(const void *obj, int flags)
{
	const struct conflist_pending *p;
	const char *id;

	if (flags & OBJ_KEY) {
		id = obj;
	} else {
		p = obj;
		id = p->endpoint_id;
	}
	return ast_str_case_hash(id);
}

static int conflist_pending_cmp(void *obj, void *arg, int flags)
{
	const struct conflist_pending *l = obj;
	const struct conflist_pending *r;
	const char *id;

	if (flags & OBJ_KEY) {
		id = arg;
	} else {
		r = arg;
		id = r->endpoint_id;
	}
	return strcasecmp(l->endpoint_id, id) ? 0 : CMP_MATCH | CMP_STOP;
}

/*!
 * \brief Find-or-create the pending entry for \a endpoint_id. Returns the
 *        entry with an extra ao2 ref the caller must release.
 *
 * The entry is left in the container so subsequent lookups see the
 * latest state. Caller fills in fields under the entry's ao2 lock if
 * concurrent access is possible.
 */
static struct conflist_pending *conflist_pending_get_or_create(
	const char *endpoint_id)
{
	struct conflist_pending *p;

	if (!conflist_pending_actions || ast_strlen_zero(endpoint_id)) {
		return NULL;
	}
	p = ao2_find(conflist_pending_actions, endpoint_id, OBJ_KEY);
	if (p) {
		return p;
	}
	p = ao2_alloc(sizeof(*p), NULL);
	if (!p) {
		return NULL;
	}
	ast_copy_string(p->endpoint_id, endpoint_id, sizeof(p->endpoint_id));
	ao2_link(conflist_pending_actions, p);
	return p;
}

static void conflist_pending_set_dialog(const char *endpoint_id,
	const struct conference_dialog_id *dialog_id)
{
	struct conflist_pending *p = conflist_pending_get_or_create(endpoint_id);
	if (!p) {
		return;
	}
	ao2_lock(p);
	p->dialog_id = *dialog_id;
	p->dialog_id_valid = 1;
	ao2_unlock(p);
	ao2_ref(p, -1);
}

static void conflist_pending_set_action(const char *endpoint_id,
	const char *action)
{
	struct conflist_pending *p = conflist_pending_get_or_create(endpoint_id);
	if (!p) {
		return;
	}
	ao2_lock(p);
	ast_copy_string(p->action, S_OR(action, ""), sizeof(p->action));
	ao2_unlock(p);
	ao2_ref(p, -1);
}

/*!
 * \brief Look up the stored ConfList state for an endpoint.
 *
 * \param[out] out_dialog_id  Receives the saved dialog_id when present.
 *                            May be NULL if caller only wants the action.
 * \param[out] out_action     Receives the pending action (or empty), AND
 *                            consumes it (clears the entry's action) so
 *                            the next pick falls through to default-Mute.
 *                            Pass NULL + 0 to peek without consuming.
 * \retval 1 if a valid dialog_id was found.
 * \retval 0 otherwise (no prior ConfList press from this endpoint).
 */
static int conflist_pending_lookup(const char *endpoint_id,
	struct conference_dialog_id *out_dialog_id,
	char *out_action, size_t out_action_len)
{
	struct conflist_pending *p;
	int found_dialog;

	if (out_action && out_action_len) {
		out_action[0] = '\0';
	}
	if (!conflist_pending_actions || ast_strlen_zero(endpoint_id)) {
		return 0;
	}
	p = ao2_find(conflist_pending_actions, endpoint_id, OBJ_KEY);
	if (!p) {
		return 0;
	}
	ao2_lock(p);
	found_dialog = p->dialog_id_valid;
	if (found_dialog && out_dialog_id) {
		*out_dialog_id = p->dialog_id;
	}
	if (out_action && out_action_len) {
		ast_copy_string(out_action, p->action, out_action_len);
		p->action[0] = '\0';  /* consume — sticky action is one-shot */
	}
	ao2_unlock(p);
	ao2_ref(p, -1);
	return found_dialog;
}

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
	/* UAS dialog created from the inbound REFER, so we can send the
	 * follow-up Event:refer NOTIFY in-dialog. Cisco firmware requires
	 * the NOTIFY to be in-dialog (same Call-ID + From/To tags as the
	 * REFER) for the phone's UI to transition from "active + held" to
	 * "Conference"; an OOB NOTIFY is silently ignored. We hold a +1
	 * session count on the dialog from REFER receipt; the destructor
	 * below drops it. NULL if pjsip_dlg_create_uas_and_inc_lock failed
	 * at receipt time — in that case the merge still happens but the
	 * phone UI won't transition (better than no merge at all). */
	pjsip_dialog *dlg;
	/* Snapshot of the cisco sorcery flag at REFER-receipt time. We sample
	 * here rather than retain the cisco_endpoint ref across the task so
	 * the task body has one fewer object to manage. */
	int keep_conference;
};

static pjsip_module conference_module;       /* forward decl for inc/dec_session */

/* Forward decls so conference_send_task can call these — the definitions
 * live further down the file alongside the bridge-building task body. */
static void send_holdretrieve_refer(struct conference_task_data *data);
static void send_conference_completion_notify(struct conference_task_data *data);

static void conference_task_data_destroy(void *obj)
{
	struct conference_task_data *data = obj;

	if (data->dlg) {
		pjsip_dlg_dec_session(data->dlg, &conference_module);
	}
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
	REMOTECC_SOFTKEY_CONFLIST,        /* <softkeyeventmsg>ConfList */
	REMOTECC_SOFTKEY_CONFERENCE,      /* <softkeyeventmsg>Conference */
	/* A datapassthroughreq REFER carrying applicationid=1 and (in the
	 * second multipart part) free-form user_call_data — chan_sip patch's
	 * ConfList Mute/Remove/Update/<participant> two-step protocol. */
	REMOTECC_SOFTKEY_CONFLIST_ACTION,
};

struct remotecc_softkey_msg {
	enum remotecc_softkey_kind kind;
	struct conference_dialog_id dialog_id;        /* CONFLIST / CONFERENCE */
	struct conference_dialog_id consult_dialog_id;/* CONFERENCE only */
	/* CONFLIST_ACTION payload — sampled out of the inbound REFER body. */
	unsigned int conf_id;
	char user_call_data[64];
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

/*!
 * \brief Parse a datapassthroughreq REFER and classify it as a ConfList
 *        action (Mute / Remove / Update / select-participant).
 *
 * The REFER body shape is multipart/mixed:
 *   part 1: application/x-cisco-remotecc-request+xml containing
 *           <x-cisco-remotecc-request>
 *             <datapassthroughreq>
 *               <applicationid>1</applicationid>     (SIP_REMOTECC_CONF_LIST)
 *               <confid>NNN</confid>
 *             </datapassthroughreq>
 *           </x-cisco-remotecc-request>
 *   part 2: application/x-cisco-remotecc-cm+xml containing the raw
 *           user_call_data string ("Mute", "Remove", "Update", or a
 *           participant ordinal like "2").
 *
 * The first part we already have (the existing
 * cisco_find_remotecc_request_body call surfaces it). The second part we
 * walk the rdata's full multipart body to find. If applicationid != 1
 * we let res_pjsip_cisco_remotecc handle it (it does callback, etc.).
 *
 * \retval 1 if this is a CONFLIST_ACTION REFER and \a out is populated.
 * \retval 0 otherwise.
 */
static int detect_conflist_action(pjsip_rx_data *rdata, pjsip_msg_body *body,
	struct remotecc_softkey_msg *out)
{
	struct ast_xml_doc *doc = NULL;
	struct ast_xml_node *root;
	struct ast_xml_node *dpt;
	char appid[32];
	char confid[32];
	pjsip_msg_body *full_body;
	pjsip_media_type cm_type;
	pjsip_multipart_part *cm_part;
	int handled = 0;

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

	dpt = ast_xml_find_element(ast_xml_node_get_children(root),
		"datapassthroughreq", NULL, NULL);
	if (!dpt) {
		goto done;
	}

	if (!cisco_xml_copy_child_text(dpt, "applicationid", appid,
			sizeof(appid))) {
		goto done;
	}
	if (atoi(appid) != CONFERENCE_APP_ID_CONF_LIST) {
		/* Not ours — remotecc.c handles other application_ids. */
		goto done;
	}
	memset(out, 0, sizeof(*out));
	out->kind = REMOTECC_SOFTKEY_CONFLIST_ACTION;

	if (cisco_xml_copy_child_text(dpt, "confid", confid, sizeof(confid))) {
		out->conf_id = (unsigned int) strtoul(confid, NULL, 10);
	}

	/* Now extract user_call_data from the second multipart part. The
	 * phone always wraps datapassthroughreq in multipart/mixed; the
	 * x-cisco-remotecc-cm+xml part's body is a plain ASCII string. */
	full_body = rdata->msg_info.msg->body;
	if (!full_body
		|| !cisco_media_type_is(&full_body->content_type, "multipart",
			"mixed")) {
		/* No multipart wrapper — no user_call_data. Treat as a "menu
		 * refresh" (empty user_call_data; queue_conflist_action_task
		 * just re-emits the menu). */
		handled = 1;
		goto done;
	}
	pjsip_media_type_init2(&cm_type, "application",
		"x-cisco-remotecc-cm+xml");
	cm_part = pjsip_multipart_find_part(full_body, &cm_type, NULL);
	if (cm_part && cm_part->body && cm_part->body->data
		&& cm_part->body->len > 0
		&& cm_part->body->len < (int) sizeof(out->user_call_data)) {
		memcpy(out->user_call_data, cm_part->body->data,
			cm_part->body->len);
		out->user_call_data[cm_part->body->len] = '\0';
		ast_trim_blanks(out->user_call_data);
		/* ast_trim_blanks only NUL-terminates after trailing space; for
		 * leading whitespace use ast_skip_blanks-equivalent pattern. */
		{
			char *p = out->user_call_data;
			while (*p == ' ' || *p == '\t' || *p == '\r'
				|| *p == '\n') {
				p++;
			}
			if (p != out->user_call_data) {
				memmove(out->user_call_data, p,
					strlen(p) + 1);
			}
		}
	}

	handled = 1;

done:
	if (doc) {
		ast_xml_close(doc);
	}
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
	ast_str_append(&menu, 0, MENU_FOOTER_FMT,
		CONFERENCE_APP_ID_CONF_LIST, conference_id,
		CONFERENCE_APP_ID_CONF_LIST, conference_id,
		CONFERENCE_APP_ID_CONF_LIST, conference_id);

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

/* ----------------------------------------------------------------------
 * Phase 2: ConfList action softkeys — Mute / Remove / Update.
 *
 * Two-step state machine matching the chan_sip patch's
 * sip_conference_participants:
 *
 *   1. Phone presses ConfList                → we emit menu.
 *   2. Phone presses Remove/Mute softkey     → datapassthroughreq REFER
 *      (user_call_data="Remove"/"Mute") — we STORE the action in
 *      conflist_pending_actions keyed by endpoint and re-emit the menu.
 *   3. Phone clicks a participant            → datapassthroughreq REFER
 *      (user_call_data="<index>") — we CONSUME the pending action
 *      (default Mute if none) and apply it to the matching bridge peer,
 *      then re-emit the menu so the user sees updated state.
 *
 * "Update" is just a menu refresh (no action set, no apply).
 * ---------------------------------------------------------------------- */

struct conflist_action_task_data {
	struct ast_sip_endpoint *endpoint;
	struct ast_sip_contact *contact;
	struct conference_dialog_id dialog_id; /* same shape as ConfList */
	char user_call_data[64];
};

static void conflist_action_task_data_destroy(void *obj)
{
	struct conflist_action_task_data *data = obj;
	ao2_cleanup(data->contact);
	ao2_cleanup(data->endpoint);
}

/*!
 * \brief Toggle mute on the bridge_channel that owns \a peer.
 *
 * \retval 0 on success (mute flag toggled, log line emitted)
 * \retval -1 if peer has no bridge_channel (already left).
 */
static int apply_participant_mute(struct ast_channel *peer,
	const char *endpoint_id)
{
	struct ast_bridge_channel *bc;
	int now_muted;

	bc = ast_channel_get_bridge_channel(peer);
	if (!bc) {
		return -1;
	}
	ast_bridge_channel_lock(bc);
	bc->features->mute = !bc->features->mute;
	now_muted = bc->features->mute;
	ast_bridge_channel_unlock(bc);
	ao2_ref(bc, -1);

	ast_log(LOG_NOTICE,
		"cisco-conference: %s — ConfList %s %s\n",
		endpoint_id, now_muted ? "muted" : "unmuted",
		ast_channel_name(peer));
	return 0;
}

/*!
 * \brief Pull \a peer out of the conference bridge. chan_pjsip's
 *        teardown then BYE's the dialog.
 */
static int apply_participant_remove(struct ast_bridge *bridge,
	struct ast_channel *peer, const char *endpoint_id)
{
	if (ast_bridge_remove(bridge, peer)) {
		ast_log(LOG_WARNING,
			"cisco-conference: %s — ConfList remove failed for %s "
			"(channel may have already left)\n",
			endpoint_id, ast_channel_name(peer));
		return -1;
	}
	ast_log(LOG_NOTICE,
		"cisco-conference: %s — ConfList removed %s from bridge\n",
		endpoint_id, ast_channel_name(peer));
	return 0;
}

static int conflist_action_send_task(void *obj)
{
	struct conflist_action_task_data *data = obj;
	struct ast_sip_session *session;
	struct ast_channel *channel = NULL;
	struct ast_bridge *bridge = NULL;
	struct ast_channel *target_peer = NULL;
	const char *endpoint_id;
	const char *ucd;
	unsigned int conference_id;

	endpoint_id = ast_sorcery_object_get_id(data->endpoint);
	ucd         = data->user_call_data;

	session = cisco_dialog_session_lookup(data->dialog_id.call_id,
		data->dialog_id.local_tag, data->dialog_id.remote_tag);
	if (!session) {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s ConfList action for unknown dialog "
			"(callid=%s)\n", endpoint_id, data->dialog_id.call_id);
		goto cleanup;
	}
	channel = cisco_session_channel_ref(session);
	if (!channel) {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s ConfList action: dialog has no "
			"channel\n", endpoint_id);
		goto cleanup;
	}
	ast_channel_lock(channel);
	bridge = ast_channel_get_bridge(channel);
	ast_channel_unlock(channel);
	if (!bridge) {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s ConfList action: channel is not "
			"in a bridge\n", endpoint_id);
		goto cleanup;
	}

	/* Dispatch on user_call_data. */
	if (!strcmp(ucd, "Mute") || !strcmp(ucd, "Remove")) {
		/* Sticky softkey: remember which action the next participant
		 * pick should trigger. */
		conflist_pending_set_action(endpoint_id, ucd);
		ast_log(LOG_NOTICE,
			"cisco-conference: %s ConfList queued %s for next "
			"participant pick\n", endpoint_id, ucd);
	} else if (ast_strlen_zero(ucd) || !strcmp(ucd, "Update")) {
		/* Just a menu refresh — nothing to do. */
	} else {
		/* Numeric participant index. chan_sip default: if no pending
		 * action was set, treat as Mute. */
		char action[16] = "";
		int idx;

		conflist_pending_lookup(endpoint_id, NULL, action,
			sizeof(action));
		if (ast_strlen_zero(action)) {
			ast_copy_string(action, "Mute", sizeof(action));
		}

		idx = atoi(ucd);
		target_peer = bridge_peer_channel_ref(channel, bridge, idx);
		if (!target_peer) {
			ast_log(LOG_NOTICE,
				"cisco-conference: %s ConfList %s requested for "
				"participant %d but that index isn't in the "
				"bridge (someone left between menu emit and "
				"click?)\n", endpoint_id, action, idx);
		} else if (!strcmp(action, "Remove")) {
			apply_participant_remove(bridge, target_peer,
				endpoint_id);
		} else {
			/* Default + explicit Mute path */
			apply_participant_mute(target_peer, endpoint_id);
		}
	}

	/* Re-emit the menu so the user sees the post-action state. */
	conference_id = (unsigned int)(uintptr_t) bridge;
	send_menu_to_contact(data->endpoint, data->contact, channel, bridge,
		conference_id);

cleanup:
	ast_channel_cleanup(target_peer);
	ao2_cleanup(bridge);
	ao2_cleanup(channel);
	ao2_cleanup(session);
	ao2_cleanup(data);
	return 0;
}

static void queue_conflist_action(struct ast_sip_endpoint *endpoint,
	struct ast_sip_contact *contact,
	const struct conference_dialog_id *dialog_id,
	const char *user_call_data)
{
	struct conflist_action_task_data *data;

	data = ao2_alloc(sizeof(*data), conflist_action_task_data_destroy);
	if (!data) {
		return;
	}
	ao2_ref(endpoint, +1);
	data->endpoint = endpoint;
	ao2_ref(contact, +1);
	data->contact = contact;
	data->dialog_id = *dialog_id;
	ast_copy_string(data->user_call_data,
		S_OR(user_call_data, ""), sizeof(data->user_call_data));

	if (ast_sip_push_task(conference_serializer, conflist_action_send_task,
			data)) {
		ast_log(LOG_WARNING,
			"cisco-conference: failed to queue ConfList action task\n");
		ao2_cleanup(data);
	}
}

static void mark_channel_as_conference(struct ast_channel *channel,
	const char *endpoint_id)
{
	struct ast_party_connected_line connected;

	ast_party_connected_line_init(&connected);
	connected.id.name.str = "Conference";
	connected.id.name.valid = 1;
	connected.id.number.str = "";
	connected.id.number.valid = 1;
	connected.id.name.presentation =
		AST_PRES_ALLOWED | AST_PRES_USER_NUMBER_PASSED_SCREEN;
	connected.id.number.presentation =
		AST_PRES_ALLOWED | AST_PRES_USER_NUMBER_PASSED_SCREEN;
	/* Stock Asterisk's enum doesn't include CONFERENCE (it's added only
	 * by the chan_sip cisco-usecallmanager patch we deliberately avoid
	 * requiring). UNKNOWN is the inert value — the actual "this is a
	 * Conference leg" signal we use is the CISCO_CONFERENCE chan_var
	 * below, which call_extras' rewrite_conference_identity_headers
	 * reads. */
	connected.source = AST_CONNECTED_LINE_UPDATE_SOURCE_UNKNOWN;

	/* Plumb the Conference flag via a channel variable too. The
	 * call_extras hook keys off this rather than the connected-line
	 * source enum, so we stay ABI-compatible with stock Asterisk. */
	pbx_builtin_setvar_helper(channel, "CISCO_CONFERENCE", "1");

	ast_channel_update_connected_line(channel, &connected, NULL);
	ast_log(LOG_NOTICE,
		"cisco-conference: %s — marked %s connected-line as Conference\n",
		endpoint_id, ast_channel_name(channel));
}

static void indicate_remote_unhold(struct ast_channel *channel,
	const char *endpoint_id, const char *role)
{
	if (ast_indicate(channel, AST_CONTROL_UNHOLD)) {
		ast_log(LOG_WARNING,
			"cisco-conference: %s — failed to indicate UNHOLD on "
			"%s %s\n", endpoint_id, role, ast_channel_name(channel));
	} else {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s — indicated UNHOLD on %s %s\n",
			endpoint_id, role, ast_channel_name(channel));
	}
}

/*!
 * \brief Flag chan_phone_A's bridge-channel so the bridge framework
 *        dissolves the entire conf when this channel hangs up.
 *
 * Implements the cisco_keep_conference=no behaviour: when the Confrn
 * initiator drops, the remaining legs get BYE'd by the bridge dissolve
 * (the chan_sip patch's default — "the user left, the conference is
 * over"). With cisco_keep_conference=yes we don't call this and the
 * bridge sticks around as long as ≥1 channel remains, per the
 * AST_BRIDGE_FLAG_DISSOLVE_EMPTY on the bridge itself.
 *
 * The flag is per-bridge-channel; pjsip session refreshes do not
 * trigger it, only an actual hangup. Setting it after ast_bridge_move
 * is safe because the bridge framework consults the flag at hangup
 * time, not at impart time.
 */
static void set_dissolve_on_initiator_hangup(struct ast_channel *channel,
	const char *endpoint_id)
{
	struct ast_bridge_channel *bridge_chan;

	bridge_chan = ast_channel_get_bridge_channel(channel);
	if (!bridge_chan) {
		ast_log(LOG_WARNING,
			"cisco-conference: %s — no bridge_channel for %s; "
			"cisco_keep_conference=no won't fire on initiator "
			"hangup\n", endpoint_id, ast_channel_name(channel));
		return;
	}

	ast_bridge_features_set_flag(bridge_chan->features,
		AST_BRIDGE_CHANNEL_FLAG_DISSOLVE_HANGUP);
	ao2_ref(bridge_chan, -1);

	ast_log(LOG_NOTICE,
		"cisco-conference: %s — initiator hangup will dissolve the "
		"conference (%s flagged DISSOLVE_HANGUP)\n",
		endpoint_id, ast_channel_name(channel));
}

static void send_conference_active_notify_locked(pjsip_dialog *dlg,
	const char *endpoint_id)
{
	pjsip_tx_data *notify_tdata = NULL;
	pj_str_t method_name = pj_str("NOTIFY");
	pjsip_method method;
	pj_str_t hdr_event_name    = pj_str("Event");
	pj_str_t hdr_event_val     = pj_str("refer");
	pj_str_t hdr_substate_name = pj_str("Subscription-State");
	pj_str_t hdr_substate_val  = pj_str("active;expires=60");

	pjsip_method_init_np(&method, &method_name);
	if (pjsip_dlg_create_request(dlg, &method, -1, &notify_tdata)
			!= PJ_SUCCESS) {
		ast_log(LOG_WARNING,
			"cisco-conference: %s — pjsip_dlg_create_request failed "
			"for active refer NOTIFY\n", endpoint_id);
		return;
	}

	pjsip_msg_add_hdr(notify_tdata->msg,
		(pjsip_hdr *) pjsip_generic_string_hdr_create(
			notify_tdata->pool, &hdr_event_name, &hdr_event_val));
	pjsip_msg_add_hdr(notify_tdata->msg,
		(pjsip_hdr *) pjsip_generic_string_hdr_create(
			notify_tdata->pool, &hdr_substate_name,
			&hdr_substate_val));

	if (pjsip_dlg_send_request(dlg, notify_tdata, -1, NULL)
			!= PJ_SUCCESS) {
		ast_log(LOG_WARNING,
			"cisco-conference: %s — pjsip_dlg_send_request failed "
			"for active refer NOTIFY\n", endpoint_id);
		return;
	}

	ast_log(LOG_NOTICE,
		"cisco-conference: %s — active refer NOTIFY sent\n",
		endpoint_id);
}

/*!
 * \brief Create a UAS dialog from the REFER's rdata, send the 202 Accepted
 *        in that dialog, and return the dialog with a held session count.
 *
 * Cisco firmware requires the follow-up Event:refer NOTIFY (the one that
 * transitions the phone's UI to "Conference" after Confrn succeeds) to be
 * in-dialog with the original REFER — i.e. share Call-ID + From/To tags.
 * An OOB NOTIFY against the contact URI is silently ignored. So instead
 * of going through cisco_send_refer_response (which is OOB-friendly and
 * adds Refer-Sub: false), we build the dialog ourselves and send the 202
 * inside it, leaving the implicit subscription open for the NOTIFY.
 *
 * Lifecycle bookkeeping is subtle — pjsip's inc_lock and inc_session both
 * touch sess_count, which makes mismatches silently disastrous:
 *
 *   pjsip_dlg_create_uas_and_inc_lock  — creates dlg, takes mutex, AND
 *     does an inc_lock side effect: sess_count goes 0 → 1.
 *   pjsip_dlg_dec_lock                  — releases mutex AND drops
 *     sess_count by 1. If sess_count hits 0 with no pending tsx, the
 *     dialog is destroyed inside dec_lock.
 *   pjsip_dlg_inc_session / dec_session — independent of the mutex;
 *     this is what we want for keeping a dialog alive across function
 *     returns.
 *
 * So on the success path we must add a real session ref before dec_lock,
 * otherwise sess_count goes 1 → 0 inside dec_lock and the dialog only
 * survives as long as the inbound REFER's UAS tsx keeps tsx_count > 0
 * — a race against the conference_send_task serializer.
 *
 * On the failure path we don't take that session ref, so dec_lock just
 * unwinds the inc_lock side effect and the dialog destroys naturally.
 *
 * \retval dlg with +1 session count held — caller must dec_session via
 *         conference_task_data_destroy.
 * \retval NULL on failure (already logged; nothing to release).
 */
static pjsip_dialog *conference_open_uas_dialog_and_202(pjsip_rx_data *rdata,
	const char *endpoint_id)
{
	pjsip_dialog *dlg = NULL;
	pjsip_tx_data *tdata = NULL;
	int kept = 0;

	if (pjsip_dlg_create_uas_and_inc_lock(pjsip_ua_instance(),
			rdata, NULL, &dlg) != PJ_SUCCESS) {
		ast_log(LOG_WARNING,
			"cisco-conference: %s — pjsip_dlg_create_uas_and_inc_lock "
			"failed for Confrn REFER; falling back to OOB 202 (merge "
			"will run but phone UI won't transition)\n", endpoint_id);
		return NULL;
	}

	if (pjsip_dlg_create_response(dlg, rdata, 202, NULL, &tdata)
			== PJ_SUCCESS
		&& pjsip_dlg_send_response(dlg, pjsip_rdata_get_tsx(rdata),
			tdata) == PJ_SUCCESS) {
		kept = 1;
	} else {
		ast_log(LOG_WARNING,
			"cisco-conference: %s — in-dialog 202 build/send failed; "
			"phone UI will not transition\n", endpoint_id);
	}

	if (kept) {
		/* Real session ref that survives dec_lock. Matched by
		 * dec_session in conference_task_data_destroy. */
		pjsip_dlg_inc_session(dlg, &conference_module);
		send_conference_active_notify_locked(dlg, endpoint_id);
	}
	pjsip_dlg_dec_lock(dlg);
	return kept ? dlg : NULL;
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
 *      chan_phone_B is explicitly hung up after chan_remote_B moves, so
 *      bridge_B dissolves too.
 *
 * chan_phone_B is deliberately NOT imparted — the phone only has one
 * physical audio path; chan_phone_A is the anchor. Same pattern the
 * chan_sip cisco-usecallmanager patch's conference_thread uses
 * (see channels/chan_sip.c, handle_remotecc_conference flow).
 *
 * The inbound REFER gets the same implicit-subscription shape as the
 * chan_sip patch: 202 Accepted, an immediate active Event:refer NOTIFY,
 * and a terminal Cisco-flavoured completion NOTIFY after bridge cleanup.
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
	 * the conference, then mirror chan_sip's channel nudges: UNHOLD the
	 * two remote bridge peers, move them in, and explicitly hang up the
	 * consult-side phone anchor after its remote party has joined the
	 * mix. attempt_recovery=1: if a move fails, put chan back into its
	 * source bridge so it isn't orphaned.
	 *
	 * mark_channel_as_conference and the post-merge REFER/NOTIFY pair
	 * intentionally run AT THE END (after softhangup), because all three
	 * trigger outbound SIP transactions and chan_pjsip session refreshes
	 * that take channel/session locks via bridge-frame relay. With
	 * chan_phone_b still alive earlier in the sequence, the connected-
	 * line re-INVITE (carrying Conference RPID = \2004 after the call_
	 * extras hook unconditionally emits it for Conference-marked legs)
	 * propagates as a frame through the conf bridge and the residual
	 * bridge_b, locking peer channel/session state. ast_softhangup on
	 * chan_phone_b then blocks on that lock and the cisco-conference
	 * serializer wedges — observed live on 2026-05-14 as four channels
	 * stuck Up with the taskprocessor at 0 processed / 4 queued. */
	if (ast_bridge_move(conf, bridge_a, chan_phone_a, NULL, 1)) {
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

	if (!data->keep_conference) {
		set_dissolve_on_initiator_hangup(chan_phone_a, endpoint_id);
	}

	indicate_remote_unhold(chan_remote_a, endpoint_id, "original remote leg");
	if (ast_bridge_move(conf, bridge_a, chan_remote_a, NULL, 1)) {
		ast_log(LOG_WARNING,
			"cisco-conference: %s — failed to move original remote leg "
			"during Confrn; phone audio path may be in an odd state — "
			"hang up to recover.\n", endpoint_id);
		goto cleanup;
	}

	indicate_remote_unhold(chan_remote_b, endpoint_id, "consult remote leg");
	if (ast_bridge_move(conf, bridge_b, chan_remote_b, NULL, 1)) {
		ast_log(LOG_WARNING,
			"cisco-conference: %s — failed to move consult remote leg "
			"during Confrn; phone audio path may be in an odd state — "
			"hang up to recover.\n", endpoint_id);
		goto cleanup;
	}

	if (ast_softhangup(chan_phone_b, AST_SOFTHANGUP_EXPLICIT)) {
		ast_log(LOG_WARNING,
			"cisco-conference: %s — failed to hang up consult-side "
			"anchor %s after Confrn merge\n",
			endpoint_id, ast_channel_name(chan_phone_b));
	} else {
		ast_log(LOG_NOTICE,
			"cisco-conference: %s — hung up consult-side anchor %s\n",
			endpoint_id, ast_channel_name(chan_phone_b));
	}

	ast_log(LOG_NOTICE,
		"cisco-conference: %s — built 3-way conference: %s + %s + %s "
		"(consult-side anchor %s hung up)\n",
		endpoint_id,
		ast_channel_name(chan_phone_a),
		ast_channel_name(chan_remote_a),
		ast_channel_name(chan_remote_b),
		ast_channel_name(chan_phone_b));

	/* Post-merge wire dance, mirroring the chan_sip patch's
	 * conference_thread cleanup paths (NON-join case). Without BOTH of
	 * these the phone's UI stays "active + held" — empirically verified
	 * across three attempts with progressively closer SIP shapes.
	 *
	 * The smoking gun is the FIRST item: until the phone receives a
	 * "<holdretrievereq>" Cisco-private signal targeting the ORIGINAL
	 * call, its UI keeps showing that call as held — even though the
	 * far end is now mixed into the conference. The chan_sip patch's
	 * inline comment says it best: "We need to signal to the phone to
	 * take the first call leg off hold, even though the generator on
	 * that channel has gone due to the masquerade as the phone still
	 * thinks that it is on hold". The SECOND item (Cisco-flavoured
	 * NOTIFY) closes the implicit REFER subscription with a Cisco
	 * status payload rather than RFC 3515 sipfrag — same Subscription-
	 * State header, different Content-Type and body. */
	send_holdretrieve_refer(data);
	send_conference_completion_notify(data);

	/* Mark the phone leg's connected-line as Conference LAST. chan_pjsip
	 * turns this into an outbound re-INVITE on the original call's dialog
	 * carrying the Cisco \2004 display token (added by call_extras' hook),
	 * which is what flips the phone UI from a regular active call to the
	 * Conference glyph + localised label. Done last so any locks the
	 * outbound re-INVITE acquires don't collide with the bridge moves or
	 * the consult-side softhangup above. */
	mark_channel_as_conference(chan_phone_a, endpoint_id);

cleanup:
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

/*!
 * \brief OOB REFER to the phone carrying a <holdretrievereq> body for the
 *        original call's dialog. Cisco-private signal that tells the
 *        firmware "the leg you have on hold is now in the conference,
 *        please un-hold it visually".
 *
 * Identical wire shape to the chan_sip patch's conference_thread inline
 * holdretrievereq emission. Uses the same OOB-REFER-with-Content-ID
 * machinery our ConfList path uses; <dialogid> is filled from the active
 * (originally held) call. localtag is the phone's side per Cisco
 * convention.
 */
static pjsip_msg_body *holdretrieve_build(pj_pool_t *pool, void *vctx)
{
	struct conference_task_data *data = vctx;
	char xml[1024];
	pj_str_t type    = pj_str("application");
	pj_str_t subtype = pj_str("x-cisco-remotecc-request+xml");
	pj_str_t text;

	snprintf(xml, sizeof(xml),
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<x-cisco-remotecc-request>\n"
		" <holdretrievereq>\n"
		"  <dialogid>\n"
		"   <callid>%s</callid>\n"
		"   <localtag>%s</localtag>\n"
		"   <remotetag>%s</remotetag>\n"
		"  </dialogid>\n"
		" </holdretrievereq>\n"
		"</x-cisco-remotecc-request>\n",
		data->active_dialog.call_id,
		data->active_dialog.local_tag,
		data->active_dialog.remote_tag);

	pj_strdup2(pool, &text, xml);
	return pjsip_msg_body_create(pool, &type, &subtype, &text);
}

static void send_holdretrieve_refer(struct conference_task_data *data)
{
	cisco_endpoint_send_refer_to_contact(data->endpoint, data->contact,
		"cisco-conference", "cisco-holdretrieve",
		"holdretrieve REFER",
		holdretrieve_build, data);
}

/*!
 * \brief In-dialog NOTIFY terminating the REFER's implicit subscription,
 *        carrying the Cisco-flavoured x-cisco-remotecc-response body
 *        (chan_sip patch's conference_thread cleanup shape).
 *
 * Body is a tiny <response><code>200</code></response> wrapped in
 * <x-cisco-remotecc-response>. This is the Cisco signal that the
 * conference operation completed (the RFC 3515 message/sipfrag body
 * we tried first was 200-OK'd by the phone but didn't drive UI
 * transition — Cisco firmware reads the Content-Type to decide
 * "this is the conference completion ack").
 *
 * NULLs out data->dlg after a successful send so the destructor doesn't
 * dec_session a second time on top of pjsip's evsub auto-release
 * (which fires when we send Subscription-State: terminated). pjsip
 * was logging "Assert failed: dlg->sess_count > 0" without this guard
 * — non-fatal but a clear sign of double-release.
 */
static void send_conference_completion_notify(struct conference_task_data *data)
{
	const char *endpoint_id = ast_sorcery_object_get_id(data->endpoint);
	pjsip_tx_data *notify_tdata = NULL;
	pj_str_t method_name = pj_str("NOTIFY");
	pjsip_method method;
	pj_str_t hdr_event_name    = pj_str("Event");
	pj_str_t hdr_event_val     = pj_str("refer");
	pj_str_t hdr_substate_name = pj_str("Subscription-State");
	pj_str_t hdr_substate_val  = pj_str("terminated;reason=noresource");
	pj_str_t body_type    = pj_str("application");
	pj_str_t body_subtype = pj_str("x-cisco-remotecc-response+xml");
	pj_str_t body_text    = pj_str(
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<x-cisco-remotecc-response>\n"
		" <response>\n"
		"  <code>200</code>\n"
		" </response>\n"
		"</x-cisco-remotecc-response>\n");
	pjsip_dialog *dlg = data->dlg;

	if (!dlg) {
		return;
	}

	/* inc_lock takes the mutex AND increments sess_count by 1; dec_lock
	 * undoes both. Our caller already added a real session ref via
	 * pjsip_dlg_inc_session in conference_open_uas_dialog_and_202, so the
	 * net sess_count delta of inc_lock/dec_lock here is zero — we never
	 * hit "Assert failed: sess_count > 0" and we never deadlock by
	 * forgetting to dec_lock. (No need to suppress the destructor's
	 * dec_session: we're not in the pjsip-simple evsub layer, so nothing
	 * auto-decrements when we send Subscription-State: terminated.) */
	pjsip_method_init_np(&method, &method_name);
	pjsip_dlg_inc_lock(dlg);
	if (pjsip_dlg_create_request(dlg, &method, -1, &notify_tdata)
			== PJ_SUCCESS) {
		pjsip_msg_add_hdr(notify_tdata->msg,
			(pjsip_hdr *) pjsip_generic_string_hdr_create(
				notify_tdata->pool, &hdr_event_name,
				&hdr_event_val));
		pjsip_msg_add_hdr(notify_tdata->msg,
			(pjsip_hdr *) pjsip_generic_string_hdr_create(
				notify_tdata->pool, &hdr_substate_name,
				&hdr_substate_val));
		notify_tdata->msg->body = pjsip_msg_body_create(
			notify_tdata->pool, &body_type, &body_subtype,
			&body_text);

		if (pjsip_dlg_send_request(dlg, notify_tdata, -1, NULL)
				== PJ_SUCCESS) {
			ast_log(LOG_NOTICE,
				"cisco-conference: %s — in-dialog completion NOTIFY "
				"(x-cisco-remotecc-response code=200) sent to %s\n",
				endpoint_id, data->contact->uri);
		} else {
			ast_log(LOG_WARNING,
				"cisco-conference: %s — pjsip_dlg_send_request failed "
				"for completion NOTIFY; phone UI may not transition\n",
				endpoint_id);
		}
	} else {
		ast_log(LOG_WARNING,
			"cisco-conference: %s — pjsip_dlg_create_request failed "
			"for completion NOTIFY; phone UI may not transition\n",
			endpoint_id);
	}
	pjsip_dlg_dec_lock(dlg);
}

static void queue_conference(struct ast_sip_endpoint *endpoint,
	struct ast_sip_contact *contact,
	const struct conference_dialog_id *active_dialog,
	const struct conference_dialog_id *consult_dialog,
	pjsip_dialog *dlg, int keep_conference)
{
	struct conference_task_data *data;

	data = ao2_alloc(sizeof(*data), conference_task_data_destroy);
	if (!data) {
		/* Couldn't queue — must release the dialog ourselves. */
		if (dlg) {
			pjsip_dlg_dec_session(dlg, &conference_module);
		}
		return;
	}
	ao2_ref(endpoint, +1);
	data->endpoint = endpoint;
	ao2_ref(contact, +1);
	data->contact = contact;
	data->active_dialog   = *active_dialog;
	data->consult_dialog  = *consult_dialog;
	data->dlg             = dlg;
	data->keep_conference = keep_conference;

	if (ast_sip_push_task(conference_serializer, conference_send_task, data)) {
		ast_log(LOG_WARNING,
			"cisco-conference: failed to queue Conference task\n");
		ao2_cleanup(data);   /* destructor releases dlg too */
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
	int keep_conference = 0;

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
	keep_conference = cisco->keep_conference;
	ao2_cleanup(cisco);

	body = cisco_find_remotecc_request_body(rdata);
	if (!body) {
		ao2_cleanup(endpoint);
		return PJ_FALSE;
	}

	/* Try the softkeyeventmsg shape first (ConfList / Conference); if
	 * that doesn't classify, try the datapassthroughreq shape
	 * (ConfList action softkeys). One of the two must produce a non-NONE
	 * kind, otherwise it's a body the remotecc module handles instead. */
	if (!detect_remotecc_softkey(body, &msg) || msg.kind == REMOTECC_SOFTKEY_NONE) {
		if (!detect_conflist_action(rdata, body, &msg)) {
			ao2_cleanup(endpoint);
			return PJ_FALSE;
		}
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
		/* Stash the active-call dialog so subsequent action REFERs
		 * (which carry no <dialogid>, only <confid>) can relocate the
		 * conference bridge. */
		conflist_pending_set_dialog(endpoint_id, &msg.dialog_id);
		cisco_send_refer_response(rdata, 202, endpoint);
		queue_conflist(endpoint, contact, &msg.dialog_id);
		break;
	case REMOTECC_SOFTKEY_CONFLIST_ACTION:
		{
			struct conference_dialog_id stashed_dialog_id;

			if (!conflist_pending_lookup(endpoint_id,
					&stashed_dialog_id, NULL, 0)) {
				ast_log(LOG_NOTICE,
					"cisco-conference: %s sent a ConfList action "
					"(usercalldata='%s') but no prior ConfList press "
					"is on file — ignoring\n",
					endpoint_id, msg.user_call_data);
				cisco_send_refer_response(rdata, 202, endpoint);
				break;
			}
			ast_log(LOG_NOTICE,
				"cisco-conference: %s ConfList action from %s "
				"(usercalldata='%s', confid=%u)\n",
				endpoint_id, contact->uri,
				msg.user_call_data, msg.conf_id);
			cisco_send_refer_response(rdata, 202, endpoint);
			queue_conflist_action(endpoint, contact,
				&stashed_dialog_id, msg.user_call_data);
		}
		break;
	case REMOTECC_SOFTKEY_CONFERENCE:
		ast_log(LOG_NOTICE,
			"cisco-conference: %s pressed Confrn from %s "
			"(active callid=%s, consult callid=%s)\n",
			endpoint_id, contact->uri,
			msg.dialog_id.call_id, msg.consult_dialog_id.call_id);
		{
			pjsip_dialog *dlg = conference_open_uas_dialog_and_202(
				rdata, endpoint_id);

			if (!dlg) {
				/* Fallback: bridge still gets built; phone UI just
				 * won't transition. We already logged why. */
				cisco_send_refer_response(rdata, 202, endpoint);
			}
			queue_conference(endpoint, contact, &msg.dialog_id,
				&msg.consult_dialog_id, dlg, keep_conference);
		}
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

	conflist_pending_actions = ao2_container_alloc_hash(
		AO2_ALLOC_OPT_LOCK_MUTEX, 0, CONFLIST_PENDING_BUCKETS,
		conflist_pending_hash, NULL, conflist_pending_cmp);
	if (!conflist_pending_actions) {
		ast_taskprocessor_unreference(conference_serializer);
		conference_serializer = NULL;
		return AST_MODULE_LOAD_DECLINE;
	}

	if (ast_sip_register_service(&conference_module)) {
		ao2_cleanup(conflist_pending_actions);
		conflist_pending_actions = NULL;
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
	ao2_cleanup(conflist_pending_actions);
	conflist_pending_actions = NULL;
	ast_taskprocessor_unreference(conference_serializer);
	conference_serializer = NULL;
	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER,
	"PJSIP Cisco RemoteCC conference control",
	.support_level = AST_MODULE_SUPPORT_EXTENDED,
	.load = load_module,
	.unload = unload_module,
	.load_pri = AST_MODPRI_CHANNEL_DEPEND,
	.requires = "res_pjsip,res_pjsip_session,res_pjsip_cisco_endpoint",
);
