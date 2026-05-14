/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Shared session helpers for res_pjsip_cisco_* modules that work
 * with active SIP dialogs. Kept separate from cisco_endpoint.h so
 * modules that only deal with sorcery / REGISTER-time supplements
 * don't pull in res_pjsip_session and channel.h.
 *
 * Bodies live in res/cisco_session.c, compiled into
 * res_pjsip_cisco_endpoint.so; other cisco_* modules resolve the
 * symbols at load time.
 */

#ifndef _RES_PJSIP_CISCO_SESSION_H
#define _RES_PJSIP_CISCO_SESSION_H

#include "asterisk.h"

#include <pjsip.h>

#include "asterisk/channel.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/res_pjsip_session.h"

/*!
 * \brief Ref-bumped accessor for a session's owner channel.
 *
 * Locks the session, reads session->channel under the lock, and
 * ref-bumps before unlocking — same convention stock res_pjsip_caller_id
 * (res_pjsip_caller_id.c:570) and the cisco-usecallmanager chan_sip
 * patch's MCID handler use to avoid racing with channel destruction
 * on another thread.
 *
 * \retval ref-bumped channel; caller must ast_channel_unref()
 * \retval NULL if session is NULL or has no channel (yet, or any more)
 */
struct ast_channel *cisco_session_channel_ref(struct ast_sip_session *session);

/*!
 * \brief Resolve a Cisco-XML &lt;dialogid&gt; triple to a live ast_sip_session.
 *
 * Cisco RemoteCC and Conference XML dialogids are encoded from the
 * phone's viewpoint: the XML's localtag is the phone's tag and the
 * XML's remotetag is Asterisk's tag. \c pjsip_ua_find_dialog takes the
 * tuple from PJSIP's viewpoint (local = Asterisk), so this helper does
 * the swap before calling in.
 *
 * The PJSIP dialog lookup is locked (PJ_TRUE) for the duration of the
 * session ref-bump, so the dialog can't be torn down between match and
 * the ref. The returned session survives the dialog dec_lock.
 *
 * \retval ref-bumped session; caller must \c ao2_cleanup
 * \retval NULL on missing args or no dialog match
 */
struct ast_sip_session *cisco_dialog_session_lookup(
	const char *call_id, const char *phone_local_tag,
	const char *phone_remote_tag);

/*!
 * \brief Send a REFER response with Refer-Sub: false. Handles both
 *        in-dialog (REFER inside an INVITE dialog) and out-of-dialog
 *        (Cisco RemoteCC REFERs that arrive before any dialog exists)
 *        cases.
 *
 * Out-of-dialog branch goes through Asterisk's stateful wrapper so
 * retransmissions are coalesced by the UAS transaction layer.
 */
void cisco_send_refer_response(pjsip_rx_data *rdata, int code,
	struct ast_sip_endpoint *endpoint);

#endif /* _RES_PJSIP_CISCO_SESSION_H */
