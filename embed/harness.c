/*
** harness.c -- in-process Fossil proof-of-concept (embed/).
**
** Originally developed in the fossil-app project (2026-08-13) to answer
** the iOS-embedding feasibility question for that project's Flutter
** client. Promoted into fossil-see so any consumer can build an FFI-style
** embedding on this shared, verified foundation. See ../README.md (this
** directory) for what's proven and what's still open.
**
** Simulates the iOS constraint on Linux: ONE process, no fork/exec for
** client operations, fossil commands invoked repeatedly in-process via
** fossil_main(), with exit() intercepted (linker --wrap) and turned into
** a longjmp back to the harness -- exactly what a dart:ffi shim would do.
** GNU ld only (see ../README.md "What's still open").
**
** Build (from a fossil-see build tree, after ./build/build.sh has run):
**   objcopy --redefine-sym main=fossil_cli_main <fossil-src>/bld/main.o <fossil-src>/bld/main_h.o
**   cc -o harness harness.c <fossil-src>/bld/*.o (main.o swapped for main_h.o) \
**      -Wl,--wrap=exit -lresolv -lssl -lcrypto -lz -ldl -lpthread -lm
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <unistd.h>

extern int fossil_main(int argc, char **argv);
extern int sqlite3_shutdown(void);
extern void db_clear_delete_on_failure(void);   /* our 12-line db.c patch */

static jmp_buf exit_jmp;
static int in_command = 0;

/* Every call to exit() inside fossil lands here instead. */
void __real_exit(int);
void __wrap_exit(int rc){
  if( in_command ){
    longjmp(exit_jmp, rc + 1000);   /* +1000 so rc==0 is distinguishable */
  }
  __real_exit(rc);
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
