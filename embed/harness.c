/*
** harness.c -- in-process Fossil proof-of-concept (embed/).
**
** Originally developed in the fossil-app project (2026-08-13) to answer
** the iOS-embedding feasibility question for that project's Flutter
** client. Promoted into fossil-see so any consumer can build an FFI-style
** embedding on this shared, verified foundation. See ../README.md (this
** directory) for what's proven and what's still open.
**
** Simulates the iOS constraint: ONE process, no fork/exec for client
** operations, fossil commands invoked repeatedly in-process via
** fossil_main(), with exit() intercepted and turned into a longjmp back
** to the harness -- exactly what a dart:ffi shim would do.
**
** PORTABLE as of build/patches/fossil-embed-exit-trap.patch: the
** interception happens inside Fossil's own fossil_exit() (util.c), via a
** registered handler (fossil_embed_init()), not a linker trick -- no
** GNU-ld-specific `-Wl,--wrap=exit` involved, so this now builds and
** runs with Apple's linker too (the actual iOS-adjacent target this
** whole exercise exists to unblock). See ../README.md's "Portable exit
** trap" section for exactly what this does and does NOT cover (notably:
** fossil_panic()'s abort() path is a documented, deliberate gap, not
** covered by this or any exit()-based mechanism).
**
** Build (from a fossil-see build tree, after ./build/build.sh has run --
** from within vendor/fossil, reusing the exact compile flags build.sh
** just used to build everything else, visible in its own output):
**   cc <same -I/-D flags build.sh used> -Dmain=fossil_cli_main \
**      -o bld/main_h.o -c bld/main_.c
**   cc -o harness ../../embed/harness.c EVERY_OBJECT_IN_bld_EXCEPT_main.o \
**      -lresolv -lssl -lcrypto -lz -ldl -lpthread -lm
** (Linux flags shown; macOS drops -ldl/-lresolv, see build.sh's own
** platform branch for the exact list this project already maintains.)
**
** Recompiling main_.c with -Dmain=fossil_cli_main (rather than the
** original recipe's `objcopy --redefine-sym`) is deliberate: objcopy is
** a GNU-binutils tool, absent by default on macOS (no llvm-objcopy
** either, confirmed via `xcrun --find`) -- the exact same portability
** trap this whole file exists to get out from under. A compile-time
** #define needs nothing beyond the C compiler already required to build
** fossil-see itself, on every platform this project targets.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <unistd.h>

extern int fossil_main(int argc, char **argv);
extern int sqlite3_shutdown(void);
extern void db_clear_delete_on_failure(void);   /* our 12-line db.c patch */
extern void fossil_embed_init(void (*exitHandler)(int));  /* our exit-trap patch */
extern void fossil_reset_fatal_guard(void);  /* our fossil_fatal() once-guard fix */
extern void fossil_reset_repository_filename_cache(void);  /* our db_repository_filename() cache fix */

static jmp_buf exit_jmp;
static int in_command = 0;

/* Registered once, in main(), via fossil_embed_init() -- this is what a
** real dart:ffi shim's own registered handler would do. */
static void harness_exit_handler(int rc){
  if( in_command ){
    longjmp(exit_jmp, rc + 1000);   /* +1000 so rc==0 is distinguishable */
  }
  /* exit() reached outside a tracked fossil_cmd() call (shouldn't happen
  ** in this harness -- every fossil_main() invocation goes through
  ** fossil_cmd() below -- but there's no jmp_buf to jump to if it does). */
  exit(rc);
}

/* Run one fossil command in-process; returns its "exit code". */
static int fossil_cmd(int n, ...);
#include <stdarg.h>
static int fossil_cmd(int n, ...){
  va_list ap;
  char **argv = malloc(sizeof(char*) * (n + 2));
  int i, rc, jmp;
  argv[0] = strdup("fossil");
  va_start(ap, n);
  fprintf(stderr, "\n=== fossil");
  for(i = 0; i < n; i++){
    argv[i+1] = strdup(va_arg(ap, const char*));
    fprintf(stderr, " %s", argv[i+1]);
  }
  fprintf(stderr, " ===\n");
  va_end(ap);
  argv[n+1] = 0;

  in_command = 1;
  jmp = setjmp(exit_jmp);
  if( jmp == 0 ){
    rc = fossil_main(n + 1, argv);      /* returned normally */
  }else{
    rc = jmp - 1000;                    /* came back via exit() */
  }
  in_command = 0;
  fflush(NULL);  /* surface any buffered fossil_print/fatal output now */
  /* CRITICAL: forget stale delete-on-failure registrations, else a later
  ** fossil_fatal() deletes files created by earlier successful commands
  ** (observed: it deleted the repository). */
  db_clear_delete_on_failure();
  /* CRITICAL: without this, fossil_fatal()'s reentrancy guard -- a plain
  ** C static, never touched by fossil_main()'s per-call memset(&g,...) --
  ** stays tripped forever after the FIRST fatal error this process ever
  ** sees. Every fossil_fatal() after that still returns a nonzero rc (the
  ** exit-trap keeps working), but silently prints no message at all,
  ** which is exactly the kind of thing that looks like it's working in
  ** testing and then gives an embedder "operation failed" with zero
  ** diagnostic text in the field. See embed/README.md. */
  fossil_reset_fatal_guard();
  /* CRITICAL: without this, db_repository_filename() silently keeps
  ** returning the FIRST repository this process ever opened, for every
  ** later command that doesn't pass an explicit repository path -- the
  ** cross-repo state bug. See embed/README.md. */
  fossil_reset_repository_filename_cache();
  /* fshell does this between commands so sqlite3_config() in the next
  ** fossil_main() call starts from a clean slate. */
  sqlite3_shutdown();
  /* argv intentionally leaked: fossil may hold pointers into it (g.argv).
  ** A real FFI shim would arena-allocate per call. */
  return rc;
}

static void write_file(const char *path, const char *text){
  FILE *f = fopen(path, "w");
  fputs(text, f);
  fclose(f);
}

int main(int argc, char **argv){
  int rc, fail = 0;
  int with_net = (argc > 1 && strcmp(argv[1], "--net") == 0);
  const char *url = with_net ? argv[2] : 0;

#define CHECK(desc, expr) do{ rc = (expr); \
  fprintf(stderr, "--- %-40s rc=%d %s\n", desc, rc, rc==0?"OK":"FAIL"); \
  if(rc) fail++; }while(0)

  fossil_embed_init(harness_exit_handler);
  system("rm -rf /tmp/ffi-lab && mkdir -p /tmp/ffi-lab/wc");

  CHECK("version",            fossil_cmd(1, "version"));
  CHECK("version again",      fossil_cmd(1, "version"));  /* trivial re-entry */
  CHECK("init",               fossil_cmd(4, "init", "/tmp/ffi-lab/r.fossil",
                                            "-A", "warren"));
  chdir("/tmp/ffi-lab/wc");
  CHECK("open",               fossil_cmd(2, "open", "/tmp/ffi-lab/r.fossil"));
  CHECK("backoffice-disable", fossil_cmd(4, "settings", "backoffice-disable",
                                            "1", "--exact"));
  write_file("note1.md", "# field note\nsix horses at site two\n");
  CHECK("add",                fossil_cmd(2, "add", "note1.md"));
  CHECK("commit 1",           fossil_cmd(5, "commit", "-m", "first note",
                                            "--user", "warren"));
  write_file("note1.md", "# field note\nsix horses at site two\nhay half gone\n");
  CHECK("commit 2 (dirty)",   fossil_cmd(5, "commit", "-m", "update note",
                                            "--user", "warren"));
  CHECK("timeline",           fossil_cmd(3, "timeline", "-n", "5"));
  CHECK("status",             fossil_cmd(1, "status"));
  /* an intentional failure: commit with nothing changed -> fossil_fatal path */
  rc = fossil_cmd(5, "commit", "-m", "empty", "--user", "warren");
  fprintf(stderr, "--- %-40s rc=%d %s\n", "empty commit (expect fail)", rc,
          rc != 0 ? "OK(expected-nonzero)" : "UNEXPECTED-ZERO");
  if( rc == 0 ) fail++;
  /* prove the process survived the fatal: run another command */
  CHECK("survives fossil_fatal",  fossil_cmd(1, "changes"));

  if( with_net ){
    fprintf(stderr, "\n*** network phase against %s ***\n", url);
    system("rm -rf /tmp/ffi-lab/clone-wc && mkdir -p /tmp/ffi-lab/clone-wc");
    CHECK("clone over http",  fossil_cmd(4, "clone", "--save-http-password",
                                            url, "/tmp/ffi-lab/c.fossil"));
    chdir("/tmp/ffi-lab/clone-wc");
    CHECK("open clone",       fossil_cmd(2, "open", "/tmp/ffi-lab/c.fossil"));
    write_file("from-harness.md", "# hello from in-process fossil\n");
    CHECK("add (clone)",      fossil_cmd(2, "add", "from-harness.md"));
    CHECK("commit (clone)",   fossil_cmd(5, "commit", "-m", "in-process commit",
                                   "--user", "warren"));
    CHECK("push",             fossil_cmd(1, "push"));
    CHECK("pull",             fossil_cmd(1, "pull"));
    CHECK("sync",             fossil_cmd(1, "sync"));
  }

  fprintf(stderr, "\n############ HARNESS RESULT: %s (%d failures) ############\n",
          fail ? "FAIL" : "ALL PASS", fail);
  fflush(NULL);
  _exit(fail ? 1 : 0);   /* skip accumulated atexit handlers, like an app would */
}
