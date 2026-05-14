/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Shared session helpers for res_pjsip_cisco_* modules that work
 * with active SIP dialogs. Bodies for the declarations in
 * cisco_session.h; linked into res_pjsip_cisco_endpoint.so and
 * resolved by the other cisco_* modules at load time.
 */

#include "asterisk.h"

#include <pjsip.h>
#include <pjsip_ua.h>

#include "asterisk/astobj2.h"
#include "asterisk/channel.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/res_pjsip_session.h"
#include "asterisk/strings.h"

#include "cisco_session.h"

struct ast_channel *cisco_session_channel_ref(struct ast_sip_session *session)
{
	struct ast_channel *channel;

	if (!session) {
		return NULL;
	}

	ao2_lock(session);
	channel = session->channel;
	if (channel) {
		ast_channel_ref(channel);
	}
	ao2_unlock(session);

	return channel;
}

struct ast_sip_session *cisco_dialog_session_lookup(
	const char *call_id, const char *phone_local_tag,
	const char *phone_remote_tag)
{
	pj_str_t call_id_pj;
	pj_str_t pj_local_tag;
	pj_str_t pj_remote_tag;
	pjsip_dialog *dlg;
	struct ast_sip_session *session;

	if (ast_strlen_zero(call_id) || ast_strlen_zero(phone_local_tag)
		|| ast_strlen_zero(phone_remote_tag)) {
		return NULL;
	}

	pj_cstr(&call_id_pj, call_id);
	pj_cstr(&pj_local_tag, phone_remote_tag);   /* swap */
	pj_cstr(&pj_remote_tag, phone_local_tag);   /* swap */

	dlg = pjsip_ua_find_dialog(&call_id_pj, &pj_local_tag, &pj_remote_tag,
		PJ_TRUE);
	if (!dlg) {
		return NULL;
	}
	session = ast_sip_dialog_get_session(dlg);
	pjsip_dlg_dec_lock(dlg);
	return session;
}

void cisco_send_refer_response(pjsip_rx_data *rdata, int code,
	struct ast_sip_endpoint *endpoint)
{
	pjsip_dialog *dlg;
	pjsip_tx_data *tdata = NULL;

	dlg = pjsip_rdata_get_dlg(rdata);
	if (dlg) {
		if (pjsip_dlg_create_response(dlg, rdata, code, NULL, &tdata)
				!= PJ_SUCCESS) {
			return;
		}
	} else if (ast_sip_create_response(rdata, code, NULL, &tdata)) {
		return;
	}

	ast_sip_add_header(tdata, "Refer-Sub", "false");

	if (dlg) {
		pjsip_dlg_send_response(dlg, pjsip_rdata_get_tsx(rdata), tdata);
	} else {
		ast_sip_send_stateful_response(rdata, tdata, endpoint);
	}
}
