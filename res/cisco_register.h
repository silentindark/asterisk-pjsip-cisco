/*
 * Asterisk -- An open source telephony toolkit.
 *
 * REGISTER 200-OK address-change tracking for the res_pjsip_cisco_*
 * supplements (optionsind / bulkupdate / unsolicited_blf).
 *
 * Each REGISTER supplement keeps a per-module ao2 hash of "endpoint id
 * -> canonical-Contact-set last fired" so refresh REGISTERs (which
 * carry the same Contact every ~60s) become no-ops. Mirrors the
 * chan_sip cisco-usecallmanager patch's addrchanged guard in
 * parse_register_contact — see the section comment on
 * cisco_register_address_changed below for the lifecycle.
 *
 * Depends on cisco_rdata.h (and through it cisco_endpoint.h):
 * cisco_register_should_fire calls cisco_endpoint_get to gate on the
 * Cisco flag, and the helpers here are shaped around pjsip_msg /
 * pjsip_tx_data plumbing exposed by the response hook.
 */

#ifndef _RES_PJSIP_CISCO_REGISTER_H
#define _RES_PJSIP_CISCO_REGISTER_H

#include "asterisk.h"

#include <pjsip.h>

#include "asterisk/astobj2.h"
#include "asterisk/res_pjsip.h"
#include "asterisk/sorcery.h"
#include "asterisk/strings.h"
#include "asterisk/utils.h"

#include "cisco_endpoint.h"
#include "cisco_rdata.h"

/*!
 * \brief Did this REGISTER 200 OK actually register a contact?
 *
 * Returns 0 for deregistration responses (Contact: * present, or all
 * Contact headers carry expires=0). Returns 1 for any 200 OK that
 * registers at least one contact with a non-zero lifetime.
 *
 * The cisco_* REGISTER supplements use this to skip post-REGISTER
 * follow-up traffic (bulkupdate REFER, unsolicited NOTIFYs,
 * optionsind body) when the phone is going away — those would
 * either fail or race with contact removal.
 */
static inline int cisco_response_registers_contact(pjsip_msg *msg)
{
	pjsip_contact_hdr *contact;

	if (!msg) {
		return 0;
	}

	contact = (pjsip_contact_hdr *) pjsip_msg_find_hdr(msg,
		PJSIP_H_CONTACT, NULL);
	while (contact) {
		if (contact->star) {
			/* Contact: * unambiguously means deregister. */
			return 0;
		}
		if (contact->expires > 0) {
			return 1;
		}
		contact = (pjsip_contact_hdr *) pjsip_msg_find_hdr(msg,
			PJSIP_H_CONTACT, contact->next);
	}
	return 0;
}

/*!
 * \name Address-change cache for REGISTER supplements
 *
 * The three REGISTER supplements (optionsind / bulkupdate /
 * unsolicited_blf) only need to re-fire their heavy follow-up traffic
 * when an endpoint's set of registered Contact URIs actually changed
 * (initial REGISTER, phone moved IP, NAT pinhole churn). Refresh
 * REGISTERs every ~60s carry the same Contact set and should be no-ops
 * — chan_sip's cisco-usecallmanager patch mirrors this via its
 * addrchanged guard in parse_register_contact.
 *
 * Cache is per-module, in-memory (NOT persistent astdb): asterisk
 * restart, module unload, and module reload all dump the cache,
 * guaranteeing the next REGISTER from each phone is correctly treated
 * as "changed" and re-bootstraps. The chan_sip patch gets this for
 * free because its peer->addr is in-memory; we have to be explicit
 * because the Cisco endpoint registry on PJSIP is sorcery-backed and
 * survives reloads.
 *
 * The cached canonical-contact-set string includes EVERY non-star,
 * non-expires=0 Contact in the REGISTER 200 OK, so the helper is
 * correct under max_contacts > 1 — a new or removed contact later in
 * the response changes the string and the supplement re-fires.
 *
 * Lifecycle:
 *   cisco_register_address_changed()  - read-only check
 *   cisco_register_address_remember() - persist the new value on
 *                                       successful guarded operation
 *   cisco_register_address_forget()   - clear cache for an endpoint
 *                                       on its deregister, so the
 *                                       next re-register with the
 *                                       same Contact URI re-bootstraps
 *
 * Each supplement creates its own ao2 container via
 * cisco_addr_cache_alloc() at load_module time and ao2_cleanup()s it
 * at unload — keeping the three modules' caches independent of each
 * other (so they consistently skip-or-fire as a group regardless of
 * which supplement runs first).
 */
/* @{ */

struct cisco_addr_cache_entry {
	/* Heap-allocated so endpoint IDs longer than any fixed buffer
	 * (Asterisk imposes no upper bound on sorcery object IDs) hash
	 * and compare correctly. A fixed-size key would silently
	 * truncate on remember() while ao2_find() hashes the full ID —
	 * resulting in long-ID endpoints never hitting the cache and
	 * re-firing every refresh REGISTER. */
	char *endpoint_id;
	char *contacts;  /* canonical sorted-by-insertion-order concat */
};

static void cisco_addr_cache_entry_destroy(void *obj)
{
	struct cisco_addr_cache_entry *e = obj;
	ast_free(e->endpoint_id);
	ast_free(e->contacts);
}

static int cisco_addr_cache_hash(const void *obj, const int flags)
{
	const struct cisco_addr_cache_entry *e;
	const char *key;

	switch (flags & OBJ_SEARCH_MASK) {
	case OBJ_SEARCH_KEY:
		key = obj;
		break;
	case OBJ_SEARCH_OBJECT:
		e = obj;
		key = e->endpoint_id;
		break;
	default:
		ast_assert(0);
		return 0;
	}
	return ast_str_hash(key);
}

static int cisco_addr_cache_cmp(void *obj, void *arg, int flags)
{
	const struct cisco_addr_cache_entry *left = obj;
	const char *right_key;

	switch (flags & OBJ_SEARCH_MASK) {
	case OBJ_SEARCH_OBJECT:
		right_key = ((const struct cisco_addr_cache_entry *) arg)->endpoint_id;
		break;
	case OBJ_SEARCH_KEY:
		right_key = arg;
		break;
	default:
		ast_assert(0);
		return 0;
	}
	return strcmp(left->endpoint_id, right_key) ? 0 : CMP_MATCH | CMP_STOP;
}

static inline struct ao2_container *cisco_addr_cache_alloc(void)
{
	return ao2_container_alloc_hash(AO2_ALLOC_OPT_LOCK_MUTEX, 0, 31,
		cisco_addr_cache_hash, NULL, cisco_addr_cache_cmp);
}

/*!
 * \brief Render the canonical contact-set string for the given REGISTER
 *        200 OK into a dynamic ast_str. Iterates every Contact header
 *        with expires > 0 (skips deregister rows). Caller frees with
 *        ast_free().
 *
 * \retval NULL on alloc failure, no contacts, or Contact: * (deregister)
 */
static inline struct ast_str *cisco_response_contacts_canonical(pjsip_msg *msg)
{
	pjsip_contact_hdr *contact;
	struct ast_str *out;
	int saw_any = 0;
	char one[PJSIP_MAX_URL_SIZE];

	if (!msg) {
		return NULL;
	}

	out = ast_str_create(512);
	if (!out) {
		return NULL;
	}

	contact = (pjsip_contact_hdr *) pjsip_msg_find_hdr(msg,
		PJSIP_H_CONTACT, NULL);
	while (contact) {
		int len;

		if (contact->star) {
			/* Deregister-all sentinel — nothing to remember. */
			ast_free(out);
			return NULL;
		}
		if (contact->expires == 0) {
			contact = (pjsip_contact_hdr *) pjsip_msg_find_hdr(msg,
				PJSIP_H_CONTACT, contact->next);
			continue;
		}

		len = pjsip_uri_print(PJSIP_URI_IN_CONTACT_HDR, contact->uri,
			one, sizeof(one) - 1);
		if (len > 0) {
			one[len] = '\0';
			if (saw_any) {
				ast_str_append(&out, 0, "|");
			}
			ast_str_append(&out, 0, "%s", one);
			saw_any = 1;
		}

		contact = (pjsip_contact_hdr *) pjsip_msg_find_hdr(msg,
			PJSIP_H_CONTACT, contact->next);
	}

	if (!saw_any) {
		ast_free(out);
		return NULL;
	}
	return out;
}

/*!
 * \brief Read-only: does the canonical Contact set in \a msg differ
 *        from what \a cache last remembered for this endpoint?
 *
 * Returns 1 (changed, caller should fire) when:
 *   - no cache entry exists for this endpoint, OR
 *   - the canonical Contact set differs from the cached one.
 * Returns 0 (unchanged, caller should skip) only when the canonical
 * Contact set is byte-identical to what was last remembered.
 *
 * Does NOT update the cache. Caller commits via
 * cisco_register_address_remember() AFTER successfully performing the
 * guarded operation; that way a failed body-build or task-queue
 * doesn't leave a "we fired" mark that suppresses the next retry.
 */
static inline int cisco_register_address_changed(pjsip_msg *msg,
	const char *endpoint_id, struct ao2_container *cache)
{
	struct cisco_addr_cache_entry *entry;
	struct ast_str *current;
	int changed = 1;

	if (!msg || !cache || ast_strlen_zero(endpoint_id)) {
		return 1;
	}

	current = cisco_response_contacts_canonical(msg);
	if (!current) {
		return 1;
	}

	entry = ao2_find(cache, endpoint_id, OBJ_SEARCH_KEY);
	if (entry) {
		if (entry->contacts
			&& !strcmp(entry->contacts, ast_str_buffer(current))) {
			changed = 0;
		}
		ao2_cleanup(entry);
	}

	ast_free(current);
	return changed;
}

/*!
 * \brief Persist a precomputed canonical Contact string as "last fired".
 *
 * Use when the work being guarded is asynchronous: compute the canonical
 * string in the response hook (where the pjsip_msg is still alive), stash
 * it in the task data, and call this from inside the task only AFTER the
 * task has actually succeeded. Avoids both the "tdata->msg gone after the
 * hook returns" problem and the "remembered before async work succeeded"
 * problem.
 */
static inline void cisco_register_address_remember_str(const char *endpoint_id,
	struct ao2_container *cache, const char *canonical)
{
	struct cisco_addr_cache_entry *entry;

	if (!cache || ast_strlen_zero(endpoint_id) || ast_strlen_zero(canonical)) {
		return;
	}

	/* Replace any existing entry. */
	ao2_find(cache, endpoint_id,
		OBJ_SEARCH_KEY | OBJ_UNLINK | OBJ_NODATA);

	entry = ao2_alloc(sizeof(*entry), cisco_addr_cache_entry_destroy);
	if (!entry) {
		return;
	}
	entry->endpoint_id = ast_strdup(endpoint_id);
	entry->contacts    = ast_strdup(canonical);
	if (!entry->endpoint_id || !entry->contacts) {
		ao2_cleanup(entry);
		return;
	}
	ao2_link(cache, entry);
	ao2_cleanup(entry);
}

/*!
 * \brief Convenience: compute the canonical Contact string from \a msg
 *        and remember it. Use for synchronous flows (optionsind) where
 *        the guarded operation completes before this call returns.
 */
static inline void cisco_register_address_remember(pjsip_msg *msg,
	const char *endpoint_id, struct ao2_container *cache)
{
	struct ast_str *current;

	if (!msg || !cache || ast_strlen_zero(endpoint_id)) {
		return;
	}
	current = cisco_response_contacts_canonical(msg);
	if (!current) {
		return;
	}
	cisco_register_address_remember_str(endpoint_id, cache,
		ast_str_buffer(current));
	ast_free(current);
}

/*!
 * \brief Forget the cached Contact set for an endpoint. Call when the
 *        endpoint deregisters so the next REGISTER (even with the
 *        same Contact URI) re-bootstraps optionsind / bulkupdate /
 *        unsolicited_blf for the fresh session.
 */
static inline void cisco_register_address_forget(const char *endpoint_id,
	struct ao2_container *cache)
{
	if (!cache || ast_strlen_zero(endpoint_id)) {
		return;
	}
	ao2_find(cache, endpoint_id,
		OBJ_SEARCH_KEY | OBJ_UNLINK | OBJ_NODATA);
}

/*!
 * \brief Combined gate for REGISTER outgoing_response supplements.
 *
 * Encapsulates the four-step preamble open-coded by every REGISTER
 * supplement (optionsind / bulkupdate / unsolicited_blf):
 *
 *   1. Is it a 200 OK to a REGISTER?
 *   2. Is the endpoint flagged Cisco?
 *   3. Is it a deregister? If so, forget() the cache and skip.
 *   4. Has the canonical Contact set changed since last fire? If not, skip.
 *   5. (optional) Capture the canonical string for async paths that
 *      stash it in task data and commit after successful send.
 *
 * \param endpoint        the supplement's endpoint argument
 * \param tdata           the supplement's tdata argument
 * \param addr_cache      the per-supplement address-change cache
 * \param endpoint_id_out written on return-1; lifetime bound by \a endpoint
 * \param canonical_out   if non-NULL on entry, the canonical string is
 *                        captured and heap-allocated here (caller frees
 *                        with ast_free). Pass NULL for the synchronous
 *                        optionsind path that doesn't stash it.
 * \retval 1 caller should proceed with the supplement's body of work
 * \retval 0 caller should bail; the cache lifecycle (forget on dereg,
 *           skip on unchanged) has already been handled internally
 */
static inline int cisco_register_should_fire(struct ast_sip_endpoint *endpoint,
	pjsip_tx_data *tdata, struct ao2_container *addr_cache,
	const char **endpoint_id_out, char **canonical_out)
{
	struct cisco_endpoint *cisco;
	const char *endpoint_id;

	if (!endpoint || !tdata || !tdata->msg) {
		return 0;
	}
	if (tdata->msg->type != PJSIP_RESPONSE_MSG
		|| tdata->msg->line.status.code != 200) {
		return 0;
	}

	endpoint_id = ast_sorcery_object_get_id(endpoint);

	cisco = cisco_endpoint_get(endpoint_id);
	if (!cisco) {
		return 0;
	}
	ao2_cleanup(cisco);

	/* Deregister responses: clear cache so the next re-register
	 * (even at the same URI) re-bootstraps. Sending follow-up traffic
	 * at a phone that just deregistered races with contact removal. */
	if (!cisco_response_registers_contact(tdata->msg)) {
		cisco_register_address_forget(endpoint_id, addr_cache);
		return 0;
	}

	/* Refresh REGISTERs carry the same Contact set every ~60s; skip
	 * the supplement's work unless something actually changed. Mirrors
	 * the chan_sip patch's addrchanged guard. */
	if (!cisco_register_address_changed(tdata->msg, endpoint_id,
			addr_cache)) {
		return 0;
	}

	if (canonical_out) {
		struct ast_str *canonical;

		canonical = cisco_response_contacts_canonical(tdata->msg);
		if (!canonical) {
			return 0;
		}
		*canonical_out = ast_strdup(ast_str_buffer(canonical));
		ast_free(canonical);
		if (!*canonical_out) {
			return 0;
		}
	}

	if (endpoint_id_out) {
		*endpoint_id_out = endpoint_id;
	}
	return 1;
}
/* @} */

#endif /* _RES_PJSIP_CISCO_REGISTER_H */
