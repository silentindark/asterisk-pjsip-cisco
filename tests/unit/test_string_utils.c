/*
 * Example pjlib-linked unit test. Demonstrates the harness pattern
 * the rest of the unit/ tests should follow: each test program is a
 * standalone C executable with its own main(), initialises pjlib if
 * the code under test needs pj_str_t / pjsip_sip_uri / etc., and
 * uses assert() to pin behaviour.
 *
 * Today this exercises two genuinely-pure helpers that this codebase
 * relies on:
 *
 *   pj_stricmp2  — case-insensitive C-string vs pj_str_t compare,
 *                  the underlying primitive for cisco_media_type_is.
 *   pj_strchr    — character search within a pj_str_t, used by
 *                  cisco_copy_sip_uri_hostport to detect IPv6 hosts
 *                  needing bracket-wrapping.
 *
 * The production helpers themselves (cisco_media_type_is,
 * cisco_copy_sip_uri_hostport, cisco_xml_copy_child_text, …) live
 * inside res_pjsip_cisco_endpoint.so. Exposing them to a standalone
 * test binary requires stubbing the broader Asterisk runtime
 * (ast_log, ast_str_*, ao2_*, ast_xml_*) they transitively pull in
 * via res_pjsip.h. That stub layer is intentionally absent in this
 * initial harness: the smoke target catches load-time breakage of
 * the .so artefact itself, and pinning the pjlib primitives below
 * catches the failure mode that has bitten this codebase before
 * (silent breakage of pj_stricmp2 / pj_strchr semantics across
 * pjproject versions).
 *
 * Add a new test: copy this file as test_<name>.c, append <name> to
 * UNIT_BINS in tests/unit/Makefile, write main() with assert()s. The
 * Makefile wires up pjproject via pkg-config or PJPROJECT_DIR
 * automatically.
 */

#include <stdio.h>
#include <string.h>

#include <pjlib.h>

/* pjproject's config_site.h defines NDEBUG, which would silently turn
 * every assert() in this file into ((void)0). Re-enable asserts before
 * pulling in <assert.h>. */
#undef NDEBUG
#include <assert.h>

static pj_caching_pool g_cp;

static void setup(void)
{
	pj_status_t status = pj_init();
	assert(status == PJ_SUCCESS);
	pj_caching_pool_init(&g_cp, NULL, 0);
}

static void teardown(void)
{
	pj_caching_pool_destroy(&g_cp);
	pj_shutdown();
}

static void test_pj_stricmp2_basic(void)
{
	pj_str_t s = pj_str("application");

	assert(pj_stricmp2(&s, "application") == 0);
	assert(pj_stricmp2(&s, "APPLICATION") == 0);
	assert(pj_stricmp2(&s, "Application") == 0);
	assert(pj_stricmp2(&s, "applicationx") != 0);
	assert(pj_stricmp2(&s, "applicatio") != 0);
	assert(pj_stricmp2(&s, "") != 0);
}

static void test_pj_stricmp2_media_subtypes(void)
{
	/* The same call cisco_media_type_is makes on the inbound REFER
	 * body's media subtype. Encoding sensitivity here matters: Cisco
	 * firmware sends mixed-case content-types. */
	pj_str_t a = pj_str("x-cisco-remotecc-request+xml");
	pj_str_t b = pj_str("X-Cisco-RemoteCC-Request+XML");
	pj_str_t c = pj_str("x-cisco-remotecc-response+xml");

	assert(pj_stricmp2(&a, "x-cisco-remotecc-request+xml") == 0);
	assert(pj_stricmp2(&b, "x-cisco-remotecc-request+xml") == 0);
	assert(pj_stricmp2(&c, "x-cisco-remotecc-request+xml") != 0);
}

static void test_pj_strchr_ipv6_detection(void)
{
	/* cisco_copy_sip_uri_hostport uses pj_strchr(host, ':') to decide
	 * whether the host needs bracket-wrapping in the splice. */
	pj_str_t ipv4   = pj_str("192.0.2.1");
	pj_str_t ipv6   = pj_str("2001:db8::1");
	pj_str_t fqdn   = pj_str("phone.example.org");
	pj_str_t empty  = pj_str("");

	assert(pj_strchr(&ipv4, ':') == NULL);
	assert(pj_strchr(&ipv6, ':') != NULL);
	assert(pj_strchr(&fqdn, ':') == NULL);
	assert(pj_strchr(&empty, ':') == NULL);
}

static void test_pj_str_immediate_init(void)
{
	/* The pj_str(literal) macro is used pervasively in the conference
	 * module for one-shot type/subtype/method names. Confirm it
	 * preserves length without including the trailing NUL. */
	pj_str_t s = pj_str("NOTIFY");

	assert(s.slen == 6);
	assert(s.ptr != NULL);
	assert(memcmp(s.ptr, "NOTIFY", 6) == 0);
}

int main(void)
{
	setup();

	test_pj_stricmp2_basic();
	test_pj_stricmp2_media_subtypes();
	test_pj_strchr_ipv6_detection();
	test_pj_str_immediate_init();

	teardown();
	printf("test_string_utils: 4 cases passed.\n");
	return 0;
}
