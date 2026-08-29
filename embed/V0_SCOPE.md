> **SUPERSEDED 2026-08-29.** viki is not building on libfossilsee. Warren:
> *"libfossil is a dead end with hundreds of patch points and a bad api."*
> The replacement is `viki-core` — SQLite as the contract, Fossil's patterns
> as ancestry only, no filesystem, no network, no subprocess. See
> `wmacevoy/viki` `core/README.md`.
>
> This document is kept as the record of what was measured, not as a plan.
> Nothing in it should be started.

# v0 scope and size

> *"how much work is v0 - viki is just fossil-see as a contract and no
> libfossil dependency but factored and context/thread safe?"* — Warren,
> 2026-08-29

**Roughly a week of focused work, dominated by testing rather than typing.**
The two things that looked like the big risks turned out to already exist in
embryo. Sizes below are grounded in counts from this tree, not impressions.

## Already done

| | |
|---|---|
| per-thread swappable context | `fossil-swappable-context.patch` — `g` is `(*gp)`, `_Thread_local Global *gp`. m1 90/0/0, fossilsee-probe 24/0 |
| portable exit trap | `fossil_embed_init()`, proven by `harness.c` on Apple's linker |
| the read path | SQL already covers wiki, ticket, uv, forum, ckin, note, tchg, attach — viki's index forks zero times |

## v0 work

**1. Factor `fossil_main()` — ~1 day, low risk.**
It is already process-init + argv munging + dispatch, and `fs_prime_globals()`
in `fossilsee.c` already mirrors the context half. The durable form is
`fossil_embed_open_repository()` beside `fossil_main()`, which `README.md`
already names as the right fix for the mirroring hazard.

    fossil_process_init()      sqlite3_config, VFS register, version floor
    fossil_context_open/close  config + vfs + version + connection
    fossil_command(argc,argv)  a pure function of the current context

**2. Output capture — ~1½ days, and much smaller than feared.**
It does not need inventing, because **Fossil already has a buffered output
mode**: `g.cgiOutput` is a per-context int (`0` command-line, `1` CGI, `2`
after CGI) and `fossil_print()` branches on it into `cgi_vprintf()`. Adding a
third mode that writes to a caller-supplied sink follows the established
pattern.

    1165  fossil_print()      <- the funnel
       7  fossil_puts()
       2  fossil_vprint()
      96  raw printf()        <- the tail
       4  fwrite/fputs stdout

Three functions carry 1174 of ~1274 output sites. The 96-call tail clusters in
`export.c` (26, git-fast-export), `search.c` (17), `delta.c` (8) and
`vfile.c` (6) — none on a path viki needs. **`http_transport.c` (8) is the one
that matters**, because it is sync, and it should be audited rather than
assumed.

**3. Thread safety — ~½ day IF the mechanical fix works, and this is the main
unknown.**
`gp` is done. The remaining per-context state is 66 `static Stmt` across 26
files plus `zSavedKey`/`savedKeySize` and `dbRepositoryFilenameCache`. For the
**one repo per thread** model, marking them `_Thread_local` is a single
mechanical pass — each thread then caches its own statements against its own
connection. **Untested.** It does not address two repos open on ONE thread,
which stays a hard block (see `docs/FINDINGS.md`).

**4. The ABI and viki's side — ~1 day.**
`fossilsee_cmd(argc, argv, sink)` plus retiring viki's three fork sites
(`viki_cache.c` ×2, `viki_index.c` ×1). viki links nothing new: `libfossilsee`
stays `dlopen`, so "no libfossil dependency" is preserved by construction and
`build/fossilsee-probe.sh` S1 already asserts it.

## Explicitly NOT in v0

- **Multiple local repos in one process or thread.** Hard block; the 66
  statics' switch-without-close case. `docs/FINDINGS.md` has the two candidate
  lifts.
- **A mobile-grade sync timeout.** Verified: no `SO_RCVTIMEO`/`SO_SNDTIMEO`
  anywhere in `http_socket.c`/`http_transport.c`, and every
  `fossil_set_timeout()` call site is `cmd_cgi`/`cmd_webserver`/
  `test_warning_page`. A stalled network hangs a sync forever. This is a
  *design* item, not a lift — the obvious watchdog fix surfaces as an I/O
  error, which is a plausible route into the untrapped `fossil_panic()` gap.

## What makes the estimate believable, and what would break it

Believable because the two feared items are already half-built: the exit trap
is proven, and buffered output is an existing per-context mode rather than a
new mechanism.

It breaks if `_Thread_local` on 66 cached `Stmt`s misbehaves, or if the
`http_transport.c` printfs turn out to carry information sync needs. Both are
cheap to check first, and both should be checked **before** the factoring
rather than after.
