/*
 * Minimal stubs for Asterisk allocator wrappers, sufficient to link a
 * test binary against Asterisk's bundled pjproject build.
 *
 * Asterisk's third-party/pjproject patches `malloc`/`free` to route
 * through `__ast_repl_malloc` / `__ast_free` for MALLOC_DEBUG tracing
 * (see third-party/pjproject/source/pjlib/include/pj/asterisk_malloc_debug.h).
 * A standalone test binary doesn't have libasterisk, so we provide
 * pass-through wrappers that drop the file/lineno/func metadata.
 */

#include <stdlib.h>

void *__ast_repl_malloc(size_t size, const char *file, int lineno,
	const char *func);
void __ast_free(void *ptr, const char *file, int lineno, const char *func);

void *__ast_repl_malloc(size_t size, const char *file, int lineno,
	const char *func)
{
	(void) file; (void) lineno; (void) func;
	return malloc(size);
}

void __ast_free(void *ptr, const char *file, int lineno, const char *func)
{
	(void) file; (void) lineno; (void) func;
	free(ptr);
}
