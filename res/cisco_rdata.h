/*
 * Asterisk -- An open source telephony toolkit.
 *
 * URI / transport / XML / media-type / rdata-resolver utilities for the
 * res_pjsip_cisco_* family of modules.
 *
 * Lives separately from cisco_endpoint.h so modules that only need the
 * sorcery struct + DND/HuntGroup/CF accessors don't pull in libxml2 +
 * pjsip multipart + transport-state plumbing. The pjsip_module on_rx_request
 * gate (cisco_pjsip_module_match) and the multipart Cisco RemoteCC request
 * body locator live here too — both are rdata-shaped helpers consumed by
 * the modules that hook incoming SIP requests.
 *
 * Depends on cisco_endpoint.h for cisco_endpoint_get (the Cisco-flag check
 * inside cisco_pjsip_module_match).
 */

#ifndef _RES_PJSIP_CISCO_RDATA_H
#define _RES_PJSIP_CISCO_RDATA_H

#include "asterisk.h"

#include <pjlib.h>
#include <pjsip.h>
#include <pjsip/sip_multipart.h>

#include "asterisk/astobj2.h"
#include "asterisk/netsock2.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/sorcery.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"
#include "asterisk/xml.h"

#include "cisco_endpoint.h"

/*!
 * \brief Return the SIP URI inside an outgoing request's From header.
 */
static inline pjsip_sip_uri *cisco_tdata_from_sip_uri(pjsip_tx_data *tdata)
{
	pjsip_fromto_hdr *from;
	pjsip_uri *uri;

	if (!tdata || !tdata->msg) {
		return NULL;
	}

	from = PJSIP_MSG_FROM_HDR(tdata->msg);
	if (!from || !from->uri) {
		return NULL;
	}

	uri = pjsip_uri_get_uri(from->uri);
	if (!uri || (!PJSIP_URI_SCHEME_IS_SIP(uri)
			&& !PJSIP_URI_SCHEME_IS_SIPS(uri))) {
		return NULL;
	}

	return (pjsip_sip_uri *) uri;
}

/*!
 * \brief Copy a SIP URI host[:port] into \a buf.
 *
 * PJSIP stores IPv6 URI hosts without brackets. Add them back before
 * callers splice the value into another SIP URI string.
 */
static inline int cisco_copy_sip_uri_hostport(const pjsip_sip_uri *uri,
	char *buf, size_t buflen)
{
	int used;

	if (!uri || !buf || buflen == 0 || !uri->host.slen) {
		return -1;
	}

	if (pj_strchr(&uri->host, ':')) {
		used = snprintf(buf, buflen, "[%.*s]",
			(int) uri->host.slen, uri->host.ptr);
	} else {
		used = snprintf(buf, buflen, "%.*s",
			(int) uri->host.slen, uri->host.ptr);
	}
	if (used < 0 || used >= (int) buflen) {
		return -1;
	}

	if (uri->port) {
		int port_used = snprintf(buf + used, buflen - used, ":%d",
			uri->port);

		if (port_used < 0 || port_used >= (int) (buflen - used)) {
			return -1;
		}
	}

	return 0;
}

/* Copy a usable host string out of an explicitly configured transport
 * state into buf: external_signaling_address (NAT) if set, else the
 * bind address, skipping the wildcard 0.0.0.0 / :: case. Returns buf,
 * or NULL if the transport offers nothing usable. */
static inline const char *cisco_transport_state_domain(
	struct ast_sip_transport_state *tstate, char *buf, size_t buflen)
{
	if (!tstate) {
		return NULL;
	}
	if (!ast_sockaddr_isnull(&tstate->external_signaling_address)) {
		ast_copy_string(buf,
			ast_sockaddr_stringify_host_remote(
				&tstate->external_signaling_address),
			buflen);
		return buf;
	}
	if (pj_sockaddr_has_addr(&tstate->host)) {
		pj_sockaddr_print(&tstate->host, buf, (int) buflen, 2);
		return buf;
	}
	return NULL;
}

/*!
 * \brief Resolve the SIP domain string to use in Cisco-generated URI
 *        fragments.
 *
 * Prefer the From URI Asterisk/PJSIP already built on \a tdata. That
 * preserves endpoint from_domain, selected transport, local address,
 * port, and IPv6 bracketing decisions made by core PJSIP code. The
 * endpoint transport fallback is only for paths that do not have a
 * tx_data yet.
 *
 * \param endpoint  Cisco endpoint receiving the message
 * \param tdata     outgoing request, after ast_sip_create_request()
 * \param buf       caller-supplied scratch buffer
 * \param buflen    size of buf
 * \return pointer to a NUL-terminated string. Lifetime is bounded by
 *         \a endpoint or \a buf depending on which case matched.
 */
static inline const char *cisco_endpoint_local_domain(
	struct ast_sip_endpoint *endpoint, pjsip_tx_data *tdata, char *buf,
	size_t buflen)
{
	pjsip_sip_uri *from_uri;
	struct ast_sip_transport_state *tstate;
	const char *result = NULL;

	if (!endpoint || !buf || buflen == 0) {
		return "localhost";
	}

	from_uri = cisco_tdata_from_sip_uri(tdata);
	if (!cisco_copy_sip_uri_hostport(from_uri, buf, buflen)) {
		return buf;
	}

	if (!ast_strlen_zero(endpoint->fromdomain)) {
		return endpoint->fromdomain;
	}

	/* The endpoint's own transport=, if it has one. */
	if (!ast_strlen_zero(endpoint->transport)
		&& (tstate = ast_sip_get_transport_state(endpoint->transport))) {
		result = cisco_transport_state_domain(tstate, buf, buflen);
		ao2_ref(tstate, -1);
		if (result) {
			return result;
		}
	}

	ast_copy_string(buf, "localhost", buflen);
	return buf;
}

/*!
 * \brief Parse a pjsip_msg_body's data as XML.
 *
 * \return ast_xml_doc on success (caller \c ast_xml_close()s),
 *         NULL on empty body or parse failure.
 *
 * libxml2 (under \c ast_xml_read_memory) parses into its own internal
 * representation and treats the input buffer read-only — the pjsip
 * pool buffer is safe to use directly without a defensive copy. The
 * (void *) → (char *) cast is required by the asterisk/xml.h
 * signature; libxml2 does not write through.
 */
static inline struct ast_xml_doc *cisco_xml_read_body(
	const pjsip_msg_body *body)
{
	if (!body || !body->data || body->len == 0) {
		return NULL;
	}
	return ast_xml_read_memory((char *) body->data, body->len);
}

/*!
 * \name Cisco RemoteCC body / rdata helpers
 *
 * Common scaffolding consumed by feature_events, remotecc, conference
 * — and historically reimplemented in each. Lifting here so a future
 * fourth consumer can't drift the shape.
 */
/* @{ */

/*!
 * \brief Case-insensitive match of a pjsip_media_type against type/subtype.
 */
static inline int cisco_media_type_is(const pjsip_media_type *media,
	const char *type, const char *subtype)
{
	return media
		&& !pj_stricmp2(&media->type, type)
		&& !pj_stricmp2(&media->subtype, subtype);
}

/*!
 * \brief Locate the application/x-cisco-remotecc-request+xml body in an
 *        incoming request, walking into a multipart/mixed wrapper if
 *        present.
 *
 * Belt-and-suspenders: checks both \c msg_info.ctype and the body's own
 * \c content_type. Both fields are populated from the same Content-Type
 * header under normal parsing, but they have diverged in the wild on
 * malformed or oddly-framed REFERs from Cisco firmware (multipart
 * wrappers around a single x-cisco-* part, REFERs whose Content-Type
 * was rebuilt by an intermediary). Checking both costs nothing and
 * avoids dropping legitimate Cisco traffic on the floor.
 */
static inline pjsip_msg_body *cisco_find_remotecc_request_body(pjsip_rx_data *rdata)
{
	pjsip_msg_body *body;
	const pjsip_media_type *body_ct;
	const pjsip_media_type *info_ct = NULL;

	if (!rdata || !rdata->msg_info.msg || !rdata->msg_info.msg->body) {
		return NULL;
	}
	body    = rdata->msg_info.msg->body;
	body_ct = &body->content_type;
	if (rdata->msg_info.ctype) {
		info_ct = &rdata->msg_info.ctype->media;
	}

	if (cisco_media_type_is(body_ct, "application", "x-cisco-remotecc-request+xml")
		|| cisco_media_type_is(info_ct, "application", "x-cisco-remotecc-request+xml")) {
		return body;
	}

	if (cisco_media_type_is(body_ct, "multipart", "mixed")
		|| cisco_media_type_is(info_ct, "multipart", "mixed")) {
		pjsip_media_type remotecc_type;
		pjsip_multipart_part *part;

		pjsip_media_type_init2(&remotecc_type, "application",
			"x-cisco-remotecc-request+xml");
		part = pjsip_multipart_find_part(body, &remotecc_type, NULL);
		if (part && part->body) {
			return part->body;
		}
	}
	return NULL;
}

/*!
 * \brief Copy a named XML child's text into \a buf.
 * \return 1 on success (buf non-empty), 0 on failure.
 */
static inline int cisco_xml_copy_child_text(struct ast_xml_node *parent,
	const char *name, char *buf, size_t buflen)
{
	struct ast_xml_node *child;
	const char *text;

	if (!buf || buflen == 0) {
		return 0;
	}
	buf[0] = '\0';

	child = ast_xml_find_element(ast_xml_node_get_children(parent), name,
		NULL, NULL);
	if (!child) {
		return 0;
	}

	text = ast_xml_get_text(child);
	if (!text) {
		return 0;
	}

	ast_copy_string(buf, text, buflen);
	ast_xml_free_text(text);
	return !ast_strlen_zero(buf);
}

/*!
 * \brief Resolve the endpoint from an incoming SIP request. Prefers
 *        rdata's already-attached endpoint reference (set during
 *        authentication); falls back to the To-header user part for
 *        the auth-bypassed paths (PUBLISH-from-MAC, etc.).
 *
 * Caller takes an ao2 reference and must ao2_cleanup() the result.
 */
static inline struct ast_sip_endpoint *cisco_rdata_get_endpoint(pjsip_rx_data *rdata)
{
	struct ast_sip_endpoint *endpoint;
	pjsip_to_hdr *to;
	pjsip_sip_uri *uri;
	char id[128];

	if (!rdata || !rdata->msg_info.msg) {
		return NULL;
	}

	endpoint = ast_pjsip_rdata_get_endpoint(rdata);
	if (endpoint) {
		return endpoint;
	}

	to = (pjsip_to_hdr *) pjsip_msg_find_hdr(rdata->msg_info.msg,
		PJSIP_H_TO, NULL);
	if (!to || !to->uri) {
		return NULL;
	}
	if (!PJSIP_URI_SCHEME_IS_SIP(to->uri)
		&& !PJSIP_URI_SCHEME_IS_SIPS(to->uri)) {
		return NULL;
	}
	uri = (pjsip_sip_uri *) pjsip_uri_get_uri(to->uri);
	if (!uri) {
		return NULL;
	}
	ast_copy_pj_str(id, &uri->user, sizeof(id));
	return ast_sorcery_retrieve_by_id(ast_sip_get_sorcery(), "endpoint", id);
}
/* @} */

/*!
 * \brief Common on_rx_request gate for pjsip_modules at
 *        PJSIP_MOD_PRIORITY_APPLICATION - 1.
 *
 * Combines the four steps every Cisco pjsip_module opens with:
 *   1. is it a REQUEST?
 *   2. does the method match \a method_name (case-insensitive)?
 *   3. (if \a opt_event_name is non-NULL) does the Event header value
 *      match it (case-insensitive)?
 *   4. is there an identified endpoint, and is it Cisco-flagged?
 *
 * \param rdata          the on_rx_request rdata
 * \param method_name    e.g. "REGISTER", "SUBSCRIBE", "PUBLISH"
 * \param opt_event_name e.g. "as-feature-event" or "presence"; NULL
 *                       skips the Event-header check
 *
 * \return ao2-ref'd \c ast_sip_endpoint on match (caller must
 *         \c ao2_cleanup), NULL on any miss. The Cisco-flag check uses
 *         \c cisco_endpoint_get and releases its ref internally — the
 *         returned endpoint is the standard PJSIP endpoint.
 */
static inline struct ast_sip_endpoint *cisco_pjsip_module_match(
	pjsip_rx_data *rdata, const char *method_name,
	const char *opt_event_name)
{
	pj_str_t event_hdr_name;
	pjsip_generic_string_hdr *event_hdr;
	struct ast_sip_endpoint *endpoint;
	struct cisco_endpoint *cisco;

	if (!rdata || !rdata->msg_info.msg
		|| rdata->msg_info.msg->type != PJSIP_REQUEST_MSG) {
		return NULL;
	}

	if (pj_stricmp2(&rdata->msg_info.msg->line.req.method.name,
			method_name)) {
		return NULL;
	}

	if (opt_event_name) {
		pj_cstr(&event_hdr_name, "Event");
		event_hdr = (pjsip_generic_string_hdr *)
			pjsip_msg_find_hdr_by_name(rdata->msg_info.msg,
				&event_hdr_name, NULL);
		if (!event_hdr
			|| pj_stricmp2(&event_hdr->hvalue, opt_event_name)) {
			return NULL;
		}
	}

	endpoint = cisco_rdata_get_endpoint(rdata);
	if (!endpoint) {
		return NULL;
	}

	cisco = cisco_endpoint_get(ast_sorcery_object_get_id(endpoint));
	if (!cisco) {
		ao2_cleanup(endpoint);
		return NULL;
	}
	ao2_cleanup(cisco);

	return endpoint;
}

#endif /* _RES_PJSIP_CISCO_RDATA_H */
