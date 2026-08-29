/*
** fossilsee.c -- implementation of libfossilsee (see fossilsee.h for the
** public contract, README.md for what the embedding as a whole proves).
**
** This file is the ONLY place that includes Fossil's internal headers.
** Callers include fossilsee.h, which names none of them. That split is
** what lets the library be dlopen()ed by a program (viki) whose own build
** knows nothing about Fossil's build tree.
*/
#include "config.h"
#include <sqlite3.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* db.h uses PID_T but does not define it -- src/db.c defines it locally
** before its own #include "db.h", with a "BUGBUG: This (PID_T) does not
** work inside of INTERFACE block" comment upstream. Mirror db.c exactly. */
#if USE_SEE
#if defined(_WIN32)
typedef DWORD PID_T;
#else
typedef pid_t PID_T;
#endif
#endif
#include "db.h"

#include "fossilsee.h"

/* Patched-in embedding hooks. Declared here rather than pulled from a
** header for the same reason harness.c declares them: they are a handful
** of symbols, and naming them explicitly documents the exact surface this
** library depends on. */
extern void fossil_embed_init(void (*exitHandler)(int));
extern void fossil_context_init(void);   /* gp is per-thread; must be primed */
extern void fossil_reset_fatal_guard(void);
extern void fossil_reset_repository_filename_cache(void);
extern void db_clear_delete_on_failure(void);
/* Registers content(), compress(), decompress() and
** gather_artifact_stats() on a connection. Public in sqlcmd.c. */
extern int add_content_sql_commands(sqlite3 *db);
#if USE_SEE
/* Frees, zeroes and unlocks Fossil's process-global saved encryption key.
** Already public in db.c; libfossilsee simply has to remember to call it. */
extern void db_unsave_encryption_key(void);
#endif

/* ---------------------------------------------------------------------
** Singleton state. Fossil's own state lives in one process-global `g`,
** so this library cannot be more concurrent than Fossil is. Rather than
** pretend otherwise with a per-handle struct, there is exactly one
** instance and fossilsee_open() refuses a second.
*/
struct fossilsee { int isOpen; };
static struct fossilsee g_one;
static char g_err[512];

/* ---- exit trap -------------------------------------------------------
** Fossil reports most failures by calling fossil_fatal(), which calls
** exit(). In a library that would take the CALLER's process down. The
** exit-trap patch (build/patches/fossil-embed-exit-trap.patch) lets us
** register a handler that longjmp()s back here instead. Portable: no
** -Wl,--wrap=exit, so this works with Apple's linker too. See README.md.
**
** NOT covered: fossil_panic()'s abort() path. That is a documented,
** deliberate gap upstream in the harness, and it is inherited here --
** a panic still kills the process. */
static jmp_buf g_jmp;
static int g_inCall = 0;

static void fs_exit_handler(int rc){
  if( g_inCall ) longjmp(g_jmp, rc + 1000);  /* +1000: keep rc==0 distinct */
  exit(rc);
}

static void fs_seterr(const char *zMsg){
  if( !zMsg ) zMsg = "";
  snprintf(g_err, sizeof(g_err), "%s", zMsg);
}

/* ---- one-time process init ------------------------------------------
** A SLICE of fossil_main()'s prologue -- only the parts db_open_repository()
** actually reaches. It is a slice and not a call to fossil_main() itself
** because fossil_main() memset()s `g` at entry and tears the database down
** at exit, so it cannot be used to establish state that outlives the call.
**
** Each line below is here because omitting it was OBSERVED to break, not
** because it looked necessary:
**
**   g.nameOfExe -- db_open() does strcmp(<path>, g.nameOfExe) for the
**     apndvfs (Fossil-appended-to-its-own-executable) check. With the
**     zeroed default this is strcmp(x, NULL) and SEGFAULTS. This was the
**     actual crash found while bringing this file up; it is the one that
**     makes hand-rolling the prologue worth documenting rather than
**     trusting to inspection.
**   g.httpHeader -- blob routines assume an initialised blob.
**   g.argc/g.argv -- Fossil's find_option() walks these; a NULL argv is
**     dereferenced by option parsing reached from the open path.
**   g.now -- timestamps taken during open.
**   sqlite3_config(MULTITHREAD) -- matches what fossil_main() selects.
**
** RISK, stated plainly: this mirrors upstream code rather than sharing it,
** so a future Fossil that adds a required initialisation step will compile
** clean here and fail at runtime. The durable fix is a
** fossil_embed_open_repository() entry point patched into main.c next to
** fossil_main(), where it would share the prologue instead of copying it.
** That is deliberately NOT done yet -- see README.md's open items.
*/
static char *g_argv[] = { (char*)"fossil", 0 };
static int g_initDone = 0;

static void fs_init_once(void){
  if( g_initDone ) return;
  fossil_context_init();   /* BEFORE any g.* access -- gp is _Thread_local
                           ** and this thread never went through fossil_main() */
  fossil_embed_init(fs_exit_handler);
  sqlite3_config(SQLITE_CONFIG_MULTITHREAD);
  g_initDone = 1;
}

static void fs_prime_globals(void){
  memset(&g, 0, sizeof(g));
  g.now = time(0);
  g.httpHeader = empty_blob;
  g.argc = 1;
  g.argv = g_argv;
  g.nameOfExe = g_argv[0];
  g.zPhase = "fossilsee";
}

/* ---- read-only enforcement ------------------------------------------
** fossilsee.h promises the connection is read-only, and that promise is
** load-bearing: README.md records that writing the ticket/tktchng tables
** directly desyncs Fossil's hash-chained artifact history from its
** materialized SQL view -- silent corruption, not an unsupported path.
**
** An authorizer is used rather than opening the file O_RDONLY because
** db_open_repository() itself legitimately WRITES during open (it does
** db_set_int("hash-policy", ...) when the policy is unset). So the
** authorizer is installed AFTER the open completes: Fossil gets to
** finish initialising, and every statement the CALLER submits is then
** restricted to reads. */
static int fs_authorizer(void *pArg, int op, const char *z1, const char *z2,
                         const char *z3, const char *z4){
  (void)pArg; (void)z1; (void)z2; (void)z3; (void)z4;
  switch( op ){
    case SQLITE_SELECT:
    case SQLITE_READ:
    case SQLITE_FUNCTION:
    case SQLITE_RECURSIVE:   /* WITH RECURSIVE -- ancestry queries need it */
      return SQLITE_OK;
    default:
      return SQLITE_DENY;
  }
}

/* ---- public API ------------------------------------------------------ */

int fossilsee_abi(void){ return FOSSILSEE_ABI; }

const char *fossilsee_errmsg(fossilsee *p){
  (void)p;             /* singleton: one error slot, handle or not */
  return g_err;
}

int fossilsee_open(const char *zRepo, const char *zKey, fossilsee **ppOut){
  int jmp, rc;

  if( ppOut ) *ppOut = 0;
  g_err[0] = 0;
  if( !zRepo || !zRepo[0] ){
    fs_seterr("no repository path given");
    return FOSSILSEE_ERROR;
  }
  if( g_one.isOpen ){
    fs_seterr("a repository is already open (Fossil keeps one global state "
              "per process; close the current handle first)");
    return FOSSILSEE_BUSY;
  }

  fs_init_once();
  fs_prime_globals();

  /* The key reaches Fossil the same way it does for the CLI, so a raw
  ** x'<hex>' key gets SQLCipher's no-KDF path (6.4ms vs 333ms measured)
  ** with no special-casing here. Only set it when the caller supplied
  ** one, so a NULL zKey leaves an inherited FOSSIL_SEE_KEY intact. */
  if( zKey && zKey[0] ) setenv("FOSSIL_SEE_KEY", zKey, 1);

  g_inCall = 1;
  jmp = setjmp(g_jmp);
  if( jmp==0 ){
    db_open_repository(zRepo);
    rc = 0;
  }else{
    rc = jmp - 1000;
  }
  g_inCall = 0;

  if( rc!=0 || g.db==0 ){
    /* fossil_fatal() has already printed the detail to stderr; the guard
    ** must be cleared or every LATER fatal in this process prints nothing
    ** at all (the silent-diagnostics bug README.md documents). */
    fossil_reset_fatal_guard();
    db_clear_delete_on_failure();
    if( !g_err[0] ) fs_seterr("could not open repository");
    return FOSSILSEE_ERROR;
  }

  /* REGISTER FOSSIL'S OWN SQL FUNCTIONS. db_open_repository() does not do
  ** this -- `fossil sql` does it separately -- so without this line an
  ** in-process connection is NOT equivalent to the subprocess it is meant
  ** to replace, and every query using content() fails with "no such
  ** function: content". Found exactly that way: viki's check-in, tech-note
  ** and attachment extractors all call content(), and all three silently
  ** became "not authoritative" when run through this library.
  **
  ** Only this set. sqlcmd.c also defines db_protect()/db_protect_pop(),
  ** which its own comment restricts to `fossil sql --test` because they
  ** disable Fossil's write-protection defences; a library whose whole
  ** connection is read-only has no business offering them. */
  add_content_sql_commands(g.db);

  sqlite3_set_authorizer(g.db, fs_authorizer, 0);
  g_one.isOpen = 1;
  if( ppOut ) *ppOut = &g_one;
  return FOSSILSEE_OK;
}

void fossilsee_close(fossilsee *p){
  if( !p || !p->isOpen ) return;
  if( g.db ) sqlite3_set_authorizer(g.db, 0, 0);

  g_inCall = 1;
  if( setjmp(g_jmp)==0 ){
    db_close(0);        /* 0 => do not report errors; we are tearing down */
  }
  g_inCall = 0;

  /* The bundled per-repo reset README.md asks fossil_embed_reset() to be.
  ** Each of these was a separately-diagnosed bug in the harness; calling
  ** them as a set here is the whole point of packaging this as a library
  ** rather than leaving it as a recipe an embedder must remember. */
  db_clear_delete_on_failure();
  fossil_reset_fatal_guard();
  fossil_reset_repository_filename_cache();
#if USE_SEE
  /* CLEAR THE SAVED KEY. db.c keeps the last encryption key in a
  ** process-global static (zSavedKey), so without this a LATER
  ** fossilsee_open() silently reuses it and IGNORES the zKey it was
  ** handed. Measured, and it is not a subtle failure: opening a repo
  ** with a deliberately WRONG key SUCCEEDED, because the right key from
  ** a previous open was still cached. The same wrong key as the first
  ** open in a fresh process correctly fails. That is the same class of
  ** bug as db_repository_filename()'s cache (README.md) -- a
  ** function-static that outlives the operation it belongs to.
  **
  ** It is also why the key does not sit in memory for the lifetime of a
  ** long-running embedder: this zeroes and unlocks the page. */
  db_unsave_encryption_key();
#endif
  sqlite3_shutdown();

  p->isOpen = 0;
}

int fossilsee_sql(fossilsee *p, const char *zSql,
                  fossilsee_row xRow, void *pArg){
  const char *zTail;
  int rcOut = FOSSILSEE_OK;

  g_err[0] = 0;
  if( !p || !p->isOpen ){ fs_seterr("no repository open"); return FOSSILSEE_ERROR; }
  if( !zSql ){ fs_seterr("no SQL given"); return FOSSILSEE_ERROR; }

  zTail = zSql;
  while( zTail && *zTail && rcOut==FOSSILSEE_OK ){
    sqlite3_stmt *pStmt = 0;
    const char *zNext = 0;
    int rc = sqlite3_prepare_v2(g.db, zTail, -1, &pStmt, &zNext);

    if( rc!=SQLITE_OK ){
      fs_seterr(sqlite3_errmsg(g.db));
      return FOSSILSEE_ERROR;
    }
    if( !pStmt ){ zTail = zNext; continue; }   /* whitespace/comment only */

    for(;;){
      rc = sqlite3_step(pStmt);
      if( rc==SQLITE_ROW ){
        int nCol = sqlite3_column_count(pStmt);
        const char **azVal;
        size_t *anLen;
        int i, abort_;

        if( !xRow ) continue;
        azVal = (const char**)malloc(sizeof(char*) * (nCol>0?nCol:1));
        anLen = (size_t*)malloc(sizeof(size_t) * (nCol>0?nCol:1));
        if( !azVal || !anLen ){
          free((void*)azVal); free(anLen); sqlite3_finalize(pStmt);
          fs_seterr("out of memory");
          return FOSSILSEE_ERROR;
        }
        for(i=0; i<nCol; i++){
          /* blob, not text: values may contain embedded NUL (attachment and
          ** unversioned payloads routinely do), and the caller is told to
          ** use anLen[] rather than strlen() for exactly that reason.
          ** _blob() before _bytes() is the documented SQLite order. */
          azVal[i] = (const char*)sqlite3_column_blob(pStmt, i);
          anLen[i] = (size_t)sqlite3_column_bytes(pStmt, i);
        }
        abort_ = xRow(pArg, nCol, azVal, anLen);
        free((void*)azVal);
        free(anLen);
        if( abort_ ){ rcOut = FOSSILSEE_ABORT; break; }
        continue;
      }
      if( rc==SQLITE_DONE ) break;
      /* A denied write lands here: SQLITE_AUTH, with a message saying so. */
      fs_seterr(sqlite3_errmsg(g.db));
      sqlite3_finalize(pStmt);
      return FOSSILSEE_ERROR;
    }
    sqlite3_finalize(pStmt);
    zTail = zNext;
  }
  return rcOut;
}
