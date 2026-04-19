/*
 * source_manifest.h - canonical source-group registry for the build.
 *
 * This file is the single authoritative list of which .c files belong to each
 * source group.  Two consumers keep themselves in sync from it:
 *
 *   1. src/compiler/driver.c  - reads it via X-macros to build the argv arrays
 *      that are passed to the linker driver when ncc links an executable.
 *
 *   2. The root Makefile       - extracts each group with a $(shell grep …)
 *      one-liner.  Any change here is automatically picked up on the next make
 *      invocation; no Makefile variable needs to be touched manually.
 *
 * To add or remove a source file:
 *   - Edit the appropriate FOREACH_*_SOURCE macro below.
 *   - That is the *only* place the change is needed.
 *
 * Usage in C (X-macro pattern):
 *
 *   #define EMIT(s) s,
 *   static char *const my_sources[] = { FOREACH_COMPILER_SOURCE(EMIT) };
 *   #undef EMIT
 */

#ifndef SOURCE_MANIFEST_H
#define SOURCE_MANIFEST_H

/* ── compiler (ncc) ─────────────────────────────────────────────────────── */

#define FOREACH_COMPILER_SOURCE(X) \
    X("src/compiler/backend.c") \
    X("src/compiler/backend_expressions.c") \
    X("src/compiler/backend_codegen.c") \
    X("src/compiler/driver.c") \
    X("src/compiler/ir.c") \
    X("src/compiler/object_writer.c") \
    X("src/compiler/parser.c") \
    X("src/compiler/parser_types.c") \
    X("src/compiler/parser_expressions.c") \
    X("src/compiler/parser_declarations.c") \
    X("src/compiler/parser_statements.c") \
    X("src/compiler/preprocessor.c") \
    X("src/compiler/semantic.c") \
    X("src/compiler/source.c") \
    X("src/compiler/lexer.c")

/* ── shared runtime / utilities (all tools) ─────────────────────────────── */

#define FOREACH_SHARED_SOURCE(X) \
    X("src/shared/runtime/memory.c") \
    X("src/shared/runtime/string.c") \
    X("src/shared/runtime/parse.c") \
    X("src/shared/runtime/io.c") \
    X("src/shared/runtime/unicode.c") \
    X("src/shared/tool_io.c") \
    X("src/shared/tool_cli.c") \
    X("src/shared/tool_regex.c") \
    X("src/shared/tool_path.c") \
    X("src/shared/tool_fs.c") \
    X("src/shared/archive_util.c")

/* ── crypto primitives ───────────────────────────────────────────────────── */

#define FOREACH_CRYPTO_SOURCE(X) \
    X("src/shared/crypto/crypto_util.c") \
    X("src/shared/crypto/md5.c") \
    X("src/shared/crypto/sha256.c") \
    X("src/shared/crypto/sha512.c")

/* ── hash utilities (hash_util + crypto) ────────────────────────────────── */

#define FOREACH_HASH_SOURCE(X) \
    X("src/shared/hash_util.c") \
    FOREACH_CRYPTO_SOURCE(X)

/* ── shell (sh) private implementation ──────────────────────────────────── */

#define FOREACH_SHELL_SOURCE(X) \
    X("src/tools/sh/shell_parser.c") \
    X("src/tools/sh/shell_execution.c") \
    X("src/tools/sh/shell_builtins.c") \
    X("src/tools/sh/shell_interactive.c")

/* ── POSIX host platform ─────────────────────────────────────────────────── */

#define FOREACH_HOST_PLATFORM_SOURCE(X) \
    X("src/platform/posix/fs.c") \
    X("src/platform/posix/process.c") \
    X("src/platform/posix/identity.c") \
    X("src/platform/posix/net.c") \
    X("src/platform/posix/time.c")

/* ── Linux freestanding target platform ─────────────────────────────────── */

#define FOREACH_TARGET_PLATFORM_SOURCE(X) \
    X("src/platform/linux/fs.c") \
    X("src/platform/linux/process.c") \
    X("src/platform/linux/identity.c") \
    X("src/platform/linux/net.c") \
    X("src/platform/linux/time.c") \
    X("src/arch/x86_64/linux/syscall_stubs.S")

/* ── SSH client private implementation ───────────────────────────────────── */

#define FOREACH_SSH_CLIENT_SOURCE(X) \
    X("src/tools/ssh/ssh_core.c") \
    X("src/tools/ssh/ssh_known_hosts.c") \
    X("src/tools/ssh/ssh_client_io.c") \
    X("src/tools/ssh/ssh_client_identity.c") \
    X("src/tools/ssh/ssh_client_kex.c") \
    X("src/tools/ssh/ssh_client_auth.c") \
    X("src/tools/ssh/ssh_client_channel.c") \
    X("src/tools/ssh/ssh_client.c")

#endif /* SOURCE_MANIFEST_H */
