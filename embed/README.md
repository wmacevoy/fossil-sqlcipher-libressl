# embed/ — in-process Fossil proof-of-concept

**Status: proof-of-concept, not a supported build target yet.** Originally developed in the `fossil-app` project (2026-08-13) to answer one question for that project's iOS Flutter client: can Fossil's client operations run *in-process* — one process, no fork/exec, `exit()` trapped — invoked repeatedly, the way a `dart:ffi` shim must call them? Promoted here so any consumer of fossil-see (not just fossil-app) can build an FFI-style embedding on a shared, verified foundation instead of re-deriving it.

`harness.c` simulates the constraint on Linux by linking Fossil's objects into a harness (`main` renamed, `exit()` wrapped into a `longjmp`, `fossil_main()` called like a function).

## Proven (on Linux, with GNU ld)

1. **Repeated in-process invocation works.** version -> init -> open -> settings -> add -> commit -> commit -> timeline -> status, all in one process, all rc=0. `fossil_main` does `memset(&g,0,...)` on entry — the main global is designed to re-init per call.
2. **The process survives `fossil_fatal`.** An intentional failure (empty commit) longjmps back; subsequent commands still work.
3. **The full networked lifecycle works in-process over HTTP:** clone -> open -> add -> commit (with autosync pull, server check-in lock, push) -> push/pull/sync. The sync client never forks — fork exists only in the *server* loop, ssh/`file://` transports, external-tool launches, and backoffice.

## Bug found and fixed (why this experiment mattered)

**Delete-on-failure carryover — catastrophic class.** `init`/`open` register "delete this file if we fail" entries (the repo! `.fslckout`!) in a file-static list that is *never* cleared between in-process calls. A later `fossil_fatal` fired the stale list and **deleted the repository**. Fix: `../build/patches/fossil-db-embed.patch`, a 12-line patch adding `db_clear_delete_on_failure()`, called by the shim after every command. Found in an hour with this harness, instead of in the field on a phone.

## Required shim rules (all verified)

- Call `db_clear_delete_on_failure()` after every command.
- Call `sqlite3_shutdown()` between commands (fshell precedent).
- Set `backoffice-disable=1` on every repo at open/clone — and compile backoffice's `fork()` out entirely for constrained platforms (`#ifdef`), belt-and-braces.
- No interactive prompts ever: clone with `--save-http-password`, pass `--user`, pre-answer everything via flags.
- Serialize commands (one at a time) — Fossil is not re-entrant concurrently.
- One repository per process lifetime for a v1 embedding. A handful of function-statics cache repo identity forever (e.g. `db_repository_filename`'s `zRepo`, the versioned-settings cache, static prepared statements). Cross-repo switching in-process misbehaves (observed: commit consulted the wrong checkout). A real multi-repo embedding needs a `fossil_embed_reset()` that clears an auditable, grep-able list of statics — not attempted here.
- Handle check-in lock contention in the embedding's UX: autosync commit takes a server-side parent lock; on "Might fork / parent locked", auto-`update` and retry (or `--override-lock` after the ~60s timeout).

## What's still open before this is a real build target

- **Portable exit trap.** The harness uses GNU ld's `-Wl,--wrap=exit`, which does not exist on Apple's linker. Since Fossil is built from source here anyway, the correct fix is patching `fossil_exit()` itself to call a registered handler function instead of relying on a link trick — not yet written.
- **Output capture.** Fossil prints to stdout; a real embedding needs per-call redirection to a buffer, or should use Fossil's built-in JSON command API (`fossil json timeline`, etc., already compiled in) as the structured interface instead of scraping text output.
- **Cross-compilation** to each target ABI (arm64-apple-ios, Android NDK, etc.) is unattempted here — this proof-of-concept only ran on Linux x86_64.
- **Packaging as `libfossilsee`** (static or shared library + a small, stable C API) hasn't been designed; `harness.c` is a one-off test program with its own `main()`, not a library.

## Build (historical recipe, Linux + GNU ld only, from a `build/build.sh` output tree)

```
objcopy --redefine-sym main=fossil_cli_main <fossil-build-dir>/bld/main.o <fossil-build-dir>/bld/main_h.o
cc -o harness harness.c <fossil-build-dir>/bld/*.o \
   -Wl,--wrap=exit -lresolv -lssl -lcrypto -lz -ldl -lpthread -lm
./harness            # local-only phase
./harness --net URL  # also exercises the networked lifecycle against URL
```

Not run as part of `build/build.sh` — this directory is not yet wired into the standard build.
