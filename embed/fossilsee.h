/*
** fossilsee.h -- the narrow, stable C API for libfossilsee.
**
** libfossilsee packages the in-process Fossil embedding proven by
** harness.c (see README.md) as a loadable library. This header is the
** ENTIRE public surface: nothing from Fossil's own headers appears here,
** no `struct Global`, no `fossil_main`, and deliberately no
** `fossil_exit`/`fossil_embed_init` -- the exit-trap wiring the embedding
** requires is an internal implementation detail, per the API-design
** recommendation in README.md. A caller includes this file and links (or
** dlopen()s) the library; it never needs Fossil's build tree.
**
** SCOPE, v0: READ-ONLY SQL against a repository. That is deliberately the
** smallest useful slice, and it is the one slice README.md's "What's still
** open" section says needs NONE of the unsolved problems: "The one place
** worth skipping the argv/stdout shim entirely: SQL queries, which can go
** straight through db.c's db_prepare/db_step/db_column_* functions with
** zero argv parsing involved." No output capture, no stdout scraping, no
** argv marshalling.
**
** NOT IN v0, ON PURPOSE:
**   - Writes of any kind. The connection is opened read-only.
**   - Tickets. README.md: tickets "must stay on the argv shim (fossil
**     ticket set/show), never raw SQL writes" -- the ticket/tktchng tables
**     are a materialized view of DVCS-versioned artifact history, and
**     writing them directly desyncs the hash-chained history from the
**     queryable data. A read-only connection makes that mistake
**     impossible to make through this API rather than merely discouraged.
**   - wiki/sync/clone. Those need the argv shim and therefore output
**     capture, which is still open.
**
** SINGLETON. Fossil keeps its state in one process-global `g`, so at most
** ONE repository can be open per process. fossilsee_open() fails with
** FOSSILSEE_BUSY rather than corrupting that state if you try for two.
** This is a property of Fossil, not a shortcut taken here.
*/
#ifndef FOSSILSEE_H
#define FOSSILSEE_H

#include <stddef.h>   /* size_t -- a public header must not rely on the
                      ** includer having pulled this in already. */

#ifdef __cplusplus
extern "C" {
#endif

/* ABI version. Bumped on any incompatible change to the declarations
** below. A dlopen()ing caller should check this BEFORE calling anything
** else, because that caller compiled against some other copy of this
** header and has no other way to find out they disagree. */
#define FOSSILSEE_ABI 1
int fossilsee_abi(void);

/* Result codes. Deliberately distinct from SQLite's so a caller never
** confuses the two; fossilsee_errmsg() carries the detail. */
#define FOSSILSEE_OK      0
#define FOSSILSEE_ERROR   1   /* open/query failed; see fossilsee_errmsg() */
#define FOSSILSEE_BUSY    2   /* a repository is already open (singleton) */
#define FOSSILSEE_ABORT   3   /* the row callback asked to stop */

typedef struct fossilsee fossilsee;

/*
** Open a repository read-only.
**
** zKey selects the encryption key for a fossil-see (.efossil) repository
** and may be:
**   NULL      -- unencrypted repo, or take the key from FOSSIL_SEE_KEY
**   "x'<64 hex>'"  -- a RAW 256-bit key, bypassing PBKDF2 entirely.
**                     MEASURED at 6.4ms/open vs 333ms for a passphrase
**                     (52x) on the unmodified binary, and still fully
**                     encrypted. Prefer this for anything that opens
**                     repeatedly; see FINDINGS.md in the viki project.
**   anything else  -- treated as a passphrase (PBKDF2, ~333ms).
**
** On success *ppOut receives a handle. On failure it is set to NULL and a
** FOSSILSEE_* code is returned; use fossilsee_errmsg(NULL) for the
** message, since there is no handle to hang it off.
*/
int fossilsee_open(const char *zRepo, const char *zKey, fossilsee **ppOut);

/* Close and release. Safe on NULL. Runs the full per-repo reset that
** README.md's "fossil_embed_reset()" item describes, so a subsequent
** fossilsee_open() of a DIFFERENT repository behaves correctly -- the
** cross-repo state bug fixed in db_repository_filename() is exactly the
** failure this bundling exists to make un-forgettable. */
void fossilsee_close(fossilsee *p);

/*
** Row callback for fossilsee_sql().
**
** azVal[i] is the i'th column's value, or NULL for SQL NULL. anLen[i] is
** its length IN BYTES.
**
** TAKE THE LENGTH. Fossil stores arbitrary bytes -- attachment and
** unversioned-file payloads routinely contain embedded NUL -- so strlen()
** on azVal[i] silently truncates mid-artifact. viki hit exactly this bug
** through the subprocess path and had to thread a byte count through its
** capture helper for the same reason. Values are NUL-terminated as a
** convenience for known-text columns, but the terminator is past anLen[i],
** not a substitute for it.
**
** Return 0 to continue, nonzero to stop the scan (fossilsee_sql then
** returns FOSSILSEE_ABORT).
**
** Pointers are valid ONLY for the duration of the call; copy what you keep.
*/
typedef int (*fossilsee_row)(void *pArg, int nCol,
                             const char *const *azVal, const size_t *anLen);

/*
** Run zSql, invoking xRow once per result row.
**
** Multiple statements separated by ';' are executed in order; rows from
** every statement that produces them are delivered to the same callback.
**
** RETURNS A REAL ERROR CODE, and that is the point of this function
** existing at all. The subprocess path (`fossil sql --readonly`) exits 0
** whether a query returned no rows or failed to prepare, so a caller
** cannot tell "this repo has no forum posts" from "the forumpost table
** does not exist". In viki that ambiguity caused sweep_sources() to
** DELETE EVERY forum row in its cache, and forced a `SELECT '#viki-eof'`
** sentinel to be appended to every extractor query as a workaround. Here,
** a failed prepare is FOSSILSEE_ERROR with the message in
** fossilsee_errmsg(), and zero rows means zero rows.
*/
int fossilsee_sql(fossilsee *p, const char *zSql,
                  fossilsee_row xRow, void *pArg);

/* Last error message, or "" if none. Owned by the library; valid until the
** next call on the same handle. Accepts NULL to report an open() failure
** that never produced a handle. */
const char *fossilsee_errmsg(fossilsee *p);

#ifdef __cplusplus
}
#endif
#endif /* FOSSILSEE_H */
