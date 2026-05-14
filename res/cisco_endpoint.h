/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Foundation header for the res_pjsip_cisco_* family of modules.
 *
 * Defines the 'cisco' sorcery type, the cisco_endpoint_get accessor,
 * and the astdb-backed feature-state accessors (DND, HuntGroup,
 * call-forward-all) that the dialplan, feature-event SUBSCRIBE handler,
 * bulkupdate, and PIDF body builders all share as their single source
 * of truth.
 *
 * URI / XML / rdata utilities live in cisco_rdata.h; REGISTER 200-OK
 * address-change tracking in cisco_register.h; OOB REFER + multipart
 * sending in cisco_refer.h; session / dialog lookup in cisco_session.h.
 * Each of those includes this one (or transitively does so) for the
 * cisco_endpoint_get gating check.
 *
 * pjsip.conf usage:
 *
 *     [1010]
 *     type           = cisco
 *     line_index     = 1
 *     subscribe      = 1001,1002,1003,1004,1005,1006,1007
 *
 * The section name MUST match the endpoint section name. The presence
 * of a [name] type=cisco section is the gating signal — endpoints
 * without one are treated as non-Cisco and the cisco_* modules leave
 * them alone.
 */

#ifndef _RES_PJSIP_CISCO_ENDPOINT_H
#define _RES_PJSIP_CISCO_ENDPOINT_H

#include "asterisk.h"

#include "asterisk/astdb.h"
#include "asterisk/presencestate.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/sorcery.h"
#include "asterisk/stringfields.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"

struct cisco_endpoint {
	SORCERY_OBJECT(details);
	AST_DECLARE_STRING_FIELDS(
		/*! Comma-separated list of extensions whose state we push to
		 * the phone via unsolicited NOTIFY at REGISTER time. */
		AST_STRING_FIELD(subscribe);
		/*! Default context to look up subscribe= extensions in.
		 * Empty -> use the default "local_sip_phone" fallback. */
		AST_STRING_FIELD(subscribe_context);
		/*! Comma-separated list of OTHER cisco-endpoint IDs that
		 * share the same physical phone (multi-line phone, one
		 * endpoint per line button). The bulkupdate REFER emits a
		 * <contact line="N"> element for each, sourcing line_index
		 * from each alias's own cisco sorcery object. Empty
		 * (default) = single-line phone. */
		AST_STRING_FIELD(aliases);
		/*! Dialplan extension the RemoteCC Park softkey blind-transfers
		 * the call to — i.e. res_parking's parkext. Default "700";
		 * change to match res_parking.conf if a site uses a different
		 * parkext. The context is resolved per call (TRANSFER_CONTEXT
		 * chan var, else the endpoint's context). */
		AST_STRING_FIELD(parkext);
	);
	/*! Cisco line button index (1 for primary line on most phones). */
	int line_index;
	/*! When DND is enabled, reject calls (1) vs ring silently (0).
	 * This is a static preference; the on/off state itself is read
	 * at runtime from astdb (key DND/<endpoint-id>). */
	int dnd_busy;
	/*! Confrn outcome when the initiator hangs up while ≥2 other parties
	 * are still in the conference bridge. 0 = match chan_sip's default:
	 * dissolve the conference, BYE the remaining legs (single-button
	 * "I left, the call's over" mental model). 1 = persist: the
	 * remaining legs keep talking, useful for receptionist-pattern
	 * "drop in, connect them, drop out" flows. Mapped to per-bridge-
	 * channel DISSOLVE_HANGUP flag on chan_phone_a in conference_send_
	 * task. */
	int keep_conference;
};

/*!
 * \brief Inline helper: retrieve cisco config for an endpoint by id.
 * \retval ao2 ref-bumped cisco_endpoint, NULL if endpoint isn't
 *         configured as Cisco.
 *
 * Caller must ao2_cleanup() the returned object.
 *
 * Implemented inline so each consuming module gets its own copy and
 * we don't need to export a symbol from res_pjsip_cisco_endpoint.so
 * — Asterisk's per-module .exports linker scripts hide everything by
 * default, and exporting cross-module helpers reliably is a fight
 * with the build system. The struct definition above is the shared
 * contract; the sorcery type registration is the only thing
 * res_pjsip_cisco_endpoint.so uniquely owns.
 */
static inline struct cisco_endpoint *cisco_endpoint_get(const char *id)
{
	if (ast_strlen_zero(id)) {
		return NULL;
	}
	return ast_sorcery_retrieve_by_id(ast_sip_get_sorcery(), "cisco", id);
}

/*!
 * \name astdb feature-state accessors
 *
 * Cisco feature state (DND, hunt-group login, call-forward-all) is
 * stored in astdb so the dialplan, the feature-event SUBSCRIBE handler,
 * and the bulkupdate / PIDF body builders all share one source of
 * truth. The astdb key shape is part of the documented project
 * contract — see README.md.
 *
 * Setter convention: pass enabled=0 (or NULL/empty target for CF) to
 * delete the key rather than store a "no" value.
 */
/* @{ */

static inline int cisco_dnd_is_enabled(const char *endpoint_id)
{
	char value[16];

	if (ast_strlen_zero(endpoint_id)) {
		return 0;
	}
	if (ast_db_get("DND", endpoint_id, value, sizeof(value))) {
		return 0;
	}
	return ast_true(value);
}

static inline void cisco_dnd_set(const char *endpoint_id, int enabled)
{
	/* No NULL guard: matches the original direct ast_db_put/del call
	 * sites. A defensive ast_strlen_zero(endpoint_id) here gives GCC
	 * grounds to infer that endpoint_id may be NULL at every caller,
	 * which trips -Wformat-overflow on subsequent ast_log("%s", ...). */
	if (enabled) {
		ast_db_put("DND", endpoint_id, "YES");
	} else {
		ast_db_del("DND", endpoint_id);
	}

	/* Surface the change to BLF watchers. A hint of the form
	 *   exten => N,hint,PJSIP/N,PJSIP:N
	 * picks this up through the "PJSIP" presence-state provider
	 * res_pjsip_cisco_endpoint registers (see that module), and
	 * res_pjsip_exten_state then NOTIFYs the watching phones — whose
	 * Cisco firmware renders <ce:dnd/> as a red lamp. Mirrors the
	 * chan_sip cisco-usecallmanager patch, which fired
	 * ast_presence_state_changed from sip_handle_publish_presence and
	 * the `sip donotdisturb` CLI. DND off reports NOT_SET rather than
	 * AVAILABLE so this provider only ever adds the DND signal and never
	 * masks another presence source '&'-combined into the same hint. */
	ast_presence_state_changed(enabled ? AST_PRESENCE_DND : AST_PRESENCE_NOT_SET,
		NULL, NULL, "PJSIP:%s", endpoint_id);
}

static inline int cisco_huntgroup_is_in(const char *endpoint_id)
{
	char value[16];

	if (ast_strlen_zero(endpoint_id)) {
		return 0;
	}
	if (ast_db_get("HuntGroup", endpoint_id, value, sizeof(value))) {
		return 0;
	}
	return ast_true(value);
}

static inline void cisco_huntgroup_set(const char *endpoint_id, int enabled)
{
	/* See cisco_dnd_set re: no NULL guard. */
	if (enabled) {
		ast_db_put("HuntGroup", endpoint_id, "YES");
	} else {
		ast_db_del("HuntGroup", endpoint_id);
	}
}

/*!
 * \brief Read the call-forward-all target into \a buf (empty when unset).
 * \return the (NUL-terminated) buf pointer, never NULL.
 */
static inline const char *cisco_cfwd_get(const char *endpoint_id,
	char *buf, size_t buflen)
{
	if (!buf || buflen == 0) {
		return "";
	}
	buf[0] = '\0';
	if (ast_strlen_zero(endpoint_id)) {
		return buf;
	}
	if (ast_db_get("CF", endpoint_id, buf, buflen)) {
		buf[0] = '\0';
	}
	return buf;
}

static inline void cisco_cfwd_set(const char *endpoint_id, const char *target)
{
	/* See cisco_dnd_set re: no NULL guard on endpoint_id. */
	if (ast_strlen_zero(target)) {
		ast_db_del("CF", endpoint_id);
	} else {
		ast_db_put("CF", endpoint_id, target);
	}
}
/* @} */

#endif /* _RES_PJSIP_CISCO_ENDPOINT_H */
