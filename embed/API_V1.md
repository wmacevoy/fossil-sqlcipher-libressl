# libfossilsee v1 — the API v0 should have been

**Status: PROPOSAL, for Warren's decision.** Nothing here is built. It exists
because a real consumer (viki) finished adopting v0 and the adoption itself is
the evidence that v0's boundary is in the wrong place.

> *"it seems that libfossilsee is missing something if it is better to just
> query sql directly — what is the ticket api, etc. there should be no forks,
> push / pull would be fundamental for sync on ios."* — Warren, 2026-08-29

---

## 0. What a fork actually buys, and what libfossilsee actually is

Before any of the below: the plain version, because "push/pull forks" is not
self-explanatory.

**libfossilsee is a database handle, not a program handle.** Its entire
exported surface is five symbols:

    fossilsee_abi  fossilsee_open  fossilsee_close  fossilsee_sql  fossilsee_errmsg

`fossilsee_open()` hands back a **read-only SQLite connection** to the
repository file, with Fossil's own SQL functions registered (`content()`,
`decompress()`). So a caller can read anything Fossil **stores**. It cannot
invoke anything Fossil **does**.

**And a fork is a pipe around Fossil's `printf`.** Literally — viki's
`run_capture()` is:

```c
    pipe(pipefd);
    pid = fork();
    if( pid == 0 ){
        dup2(pipefd[1], STDOUT_FILENO);   /* <- the entire point */
        execvp(argv[0], argv);
    }
```

That is "run this Fossil command and capture what it printed." viki does not
fork for isolation, or safety, or crash containment. **It forks because
fork+exec+pipe is the only implemented way to call a Fossil command and read
the result.**

So every fork in viki is a very expensive function call, and the interesting
question for each one is *what C code is it reaching that SQL cannot?*

| viki forks for | what the fork was buying | verdict |
|---|---|---|
| `uv export` | zlib decompression | **nothing** — `decompress()` is a registered SQL function; the fork was never needed |
| `uv add` | permission to write one row | **permission, not capability** — `unversioned_write()` is one `REPLACE INTO`; the connection is read-only by policy (`fs_authorizer`) |
| `uv sync` | `sync_unversioned()` | **real behaviour** — ~4,700 lines across `xfer.c`, `http.c`, `http_socket.c`, `http_transport.c`. No `SELECT` does this |

Only the third is intrinsic. That is the whole answer to "what makes push/pull
a fork": one leg needed nothing, one needs a permission, and one needs a
network protocol implemented in C.

### 0a. So what is missing from libfossilsee is ONE primitive

Not the verb list in §4 — that is what should be *built on top*. The single
missing thing is the ability to invoke a Fossil command in-process and get its
output back:

```c
typedef int (*fossilsee_out)(void *pArg, const char *zChunk, size_t n);
int fossilsee_cmd(fossilsee*, int argc, char **argv, fossilsee_out, void*);
```

Every Fossil command is `fossil_main(argc, argv)` reporting through stdout, so
reaching one in-process needs exactly two things:

1. **Enter and leave it safely.** *Already solved.* `embed/harness.c` proves
   repeated in-process `fossil_main()` calls with `fossil_exit()` intercepted
   and turned into a `longjmp`, via a registered handler rather than a linker
   trick — which is why it works with Apple's linker, the iOS-adjacent target
   the whole exercise exists to unblock.
2. **Capture what it printed.** *Not built.* This is the item README's
   "What's still open" names first, and it gates everything that is not SQL.

**The fork is today's implementation of item 2.** That is the cleanest way to
see the gap: viki already has output capture — it is just spelled `fork`, and
`fork` is the one spelling iOS does not have.

### 0b. "Then the API looks like the command line" — and that is the point

I first wrote here that `fossilsee_cmd()` "is not a good API — an argv shim
fails §1's abstraction test as surely as a raw SQL string does." **That was
wrong, and the abstraction test says so.**

The test is *can you use it correctly without reading its implementation?*

- Raw SQL over `tagxref` **fails**: you must know that `tagxref.rid` is the
  artifact and `tagxref.value` is the page size, that `GROUP BY 1 + max(mtime)`
  is what avoids a superseded page, that the body is a counted `W` card. Eight
  such facts, all from Fossil's source (§1).
- `argv` **passes**: `fossil help sync` documents it. Nobody reads
  `sync_unversioned()` to use `fossil sync`. And the CLI is the surface Fossil
  actually maintains compatibility for — so binding to argv inherits a
  stability promise that binding to internal C functions does not have.

So the command line is not a fallback for lack of a real API. It **is** the
real API, and it is better versioned than anything we would invent.

### 0c. The factoring, and it is three parts not two

`fossil_main()` today is process init + argv munging + dispatch; the repository
open happens *inside* the commands. So the split is:

    fossil_process_init()     sqlite3_config(MULTITHREAD), sqlite3_config(LOG),
                              the SQLite version floor, and REGISTERING any VFS
                              that will later be named.  ONCE per process.
    fossil_context_*()        config + vfs + version + connection.  ONE PER
                              TRIBE.  This is what viki retains.
    fossil_command(argc,argv) a pure function of the current context.

**A context is four things, and the VFS is one of them** -- Warren's
correction, and Fossil already agrees with it. An earlier draft here put "VFS
register" in process init, which conflates two different operations:

    REGISTERING a vfs    a process-wide name -> implementation table.  Once.
    SELECTING  a vfs     per connection, BY NAME.  Per tribe.

`db.c` opens with `sqlite3_open_v2(zDbName, &db, flags, g.zVfsName)` -- the VFS
is an argument to the open -- and `zVfsName` is already a field of `Global`
(`main.c:159`, "The VFS to use for database connections"), settable by `--vfs`
or `$FOSSIL_VFS`. So a tribe repointing its VFS needs no new mechanism; it is
one field of the context Fossil already carries. That matters on iOS, where
the file layer is exactly the thing that differs.

    typedef struct {
        FossilConfig  cfg;     /* user, flags, paths                        */
        const char   *zVfs;    /* per-tribe; passed to sqlite3_open_v2      */
        FossilVersion ver;     /* this repo's schema/compat state           */
        sqlite3      *db;      /* the connection                            */
    } FossilContext;

The connection could be opened lazily, but there is little reason to: almost
nothing in a context is useful before it is open, so a lazy handle mostly buys
a NULL check at every use site.

`fossil-swappable-context.patch` already did the hard half: `g` is `(*gp)` with
`_Thread_local Global *gp`, so "the current context" is a pointer a caller can
set. Retain is the natural discipline for setting it — `RETAIN_BEGIN(VikiRepo,
r, guard)` pushes `gp`, `RETAIN_END` restores it, and the Fossil C code four
frames down reads `g` without knowing who pushed.

### 0d. The weak point is OUTPUT, not input

argv in is documented and stable. stdout out is neither. The division that
follows:

| | mechanism | why |
|---|---|---|
| **reads** | SQL | already works, structured, nothing to parse |
| **actions** | argv | the result is mostly "did it work" |

That collapses the output-capture problem for actions: capture it for
diagnostics, do not parse it.

**One caveat, with a live example.** `viki_cache.c` does
`strstr(zOut, "uv-pull-only")` because `fossil uv sync` **exits 0** when the
server refused the push for want of the `y` capability — so viki silently
published nothing for a long time (QUEUE 35). An action that hides its failure
in prose forces exactly the text-scraping this division is trying to avoid.

The fix is not to teach viki to scrape more carefully. **We own this
repository:** patch the exit status, so "did it work" is answerable without
reading the chatter. Any other command found doing the same gets patched the
same way — a much smaller and more durable body of work than a typed verb per
feature.

So §4's typed verbs are **no longer the recommendation.** They remain a thin,
optional convenience for the handful of calls viki makes most, but the
load-bearing interface is `fossil_command(argc, argv)` against a retained
context, plus SQL for reads.

---

## 1. The test v0 fails

From `SOFTWARE-ENGINEERING-2.md` §5:

> **Can you use it correctly without reading its implementation?**
>
> If yes, it is an abstraction and it is doing its job. If you have to look
> inside to use it right, it is not an abstraction — it is a function call
> with extra steps, and the boundary it advertises is a lie.

v0 advertises "read-only SQL against a repository". To use that correctly
against Fossil, viki had to learn all of the following from Fossil's **source**,
not from `fossilsee.h`:

| What had to be read out of Fossil's source | Where it bit |
|---|---|
| `tagxref.rid` is the wiki artifact; `tagxref.value` is the page SIZE — and `listAllWikiPages` aliases `value+0` as `wrid`, which reads like a rid and is not one | first draft resolved the wrong rid |
| `GROUP BY 1` + `max(mtime)` is how you get the CURRENT page rather than a superseded one | the same defect a forum round-trip already found once |
| Plain `wiki list` hides `checkin/`, `branch/`, `tag/`, `ticket/` association pages | corpus would have silently grown |
| The wiki body is a **counted** `W <n>` card, not a line-oriented field | needs `find_w_card()` |
| Single-line manifest cards are escaped (`\s` for space) and need `defossilize()` semantics | FTS tokenized `\sseal` as one word; every title word after the first became unsearchable |
| `content()` exists only because `add_content_sql_commands()` registered it, which `db_open_repository()` does not call | three extractors silently produced nothing |
| `decompress()` exists, and strips uv's 4-byte length prefix itself | a per-blob subprocess was kept for a year on the belief it did not |
| The `ticket` table is a materialized view whose columns a project may change | naming a missing column fails the whole PREPARE, not one row |

Every row is "something true on the inside escaped to the outside." A caller
that gets any of them wrong does not get an error — it gets a **quietly
different corpus**, which is the failure mode a memory system can least afford.

## 2. And one of them is a Meaning leak between transports

`pragma_table_info('ticket')` **succeeds** through `fossil sql` and fails
`not authorized` through libfossilsee. Same SQL, same SQLite, same repository —
different answer depending on which transport you happened to test.

The cause is defensible and the detail is the point. `fs_authorizer()`
allow-lists exactly four operations — `SQLITE_SELECT`, `SQLITE_READ`,
`SQLITE_FUNCTION`, `SQLITE_RECURSIVE` — and denies everything else. Using an
authorizer to make "no writes" a property rather than a promise is exactly
right. But `SQLITE_PRAGMA` **is not a write**, and it is denied anyway, because
the policy is written as an allow-list of what v0 happened to need rather than
as a statement of what it forbids. The **leak** is that the API's advertised
constraint ("read-only") and its actual constraint ("these four opcodes") are
different sets, and only the first one is documented. Measured this week: viki's ticket extractor worked on
the subprocess path, shipped, and indexed zero tickets in-process. The probe
caught it as "the two transports disagree" — but only because that probe
exists. Nothing in the API said this could happen.

Per §5's rule — *a contract that does not state how it fails has not stated
anything* — the authorizer's policy is part of the interface. Either document
exactly what it denies, or (better) stop making callers care by giving them
verbs instead of a SQL string.

## 3. Sync is not an extension. It is the product.

v0 chose read-only SQL as "the smallest useful slice". For a consumer on iOS
that is the smallest slice that is **not** useful, and this is the sharpest
point in Warren's note.

There is no `fork()` on iOS. If sync is not in the library, a tribe on a phone
can never receive anything a peer wrote. What it holds is not a cache — a cache
is something that can be refreshed. **It is a screenshot.** viki's whole claim
is a distributed private memory; a peer that can only read what it already has
is not participating in one.

So v1's slice is not "v0 plus conveniences". It is: **the operations a peer
needs to stay a peer.**

## 4. Proposed v1 surface

Domain verbs, not a database handle. Each returns a status and, where it
produces bytes, writes them into a caller-supplied buffer or callback — no
stdout anywhere in the contract.

```c
/* --- sync: the reason v1 exists ------------------------------------- */
int fossilsee_sync(fossilsee*, const char *zUrl, int mode,   /* PUSH|PULL|BOTH */
                   int msTimeout, fossilsee_progress, void*);
int fossilsee_clone(const char *zUrl, const char *zRepo, const char *zKey,
                    int msTimeout, fossilsee_progress, void*);

/* --- unversioned: what viki cache push/pull actually needs ----------- */
int fossilsee_uv_put(fossilsee*, const char *zName, const void*, size_t);
int fossilsee_uv_get(fossilsee*, const char *zName, fossilsee_blob*);
int fossilsee_uv_list(fossilsee*, fossilsee_row, void*);
int fossilsee_uv_sync(fossilsee*, const char *zUrl, int msTimeout);

/* --- wiki: read AND write, because authoring is a peer operation ----- */
int fossilsee_wiki_list(fossilsee*, fossilsee_row, void*);
int fossilsee_wiki_get(fossilsee*, const char *zName, fossilsee_doc*);
int fossilsee_wiki_put(fossilsee*, const char *zName, const char *zMimetype,
                       const void*, size_t);

/* --- tickets: the API Warren asked about ---------------------------- */
int fossilsee_ticket_list(fossilsee*, fossilsee_row, void*);
int fossilsee_ticket_get(fossilsee*, const char *zUuid, fossilsee_ticket*);
int fossilsee_ticket_new(fossilsee*, const fossilsee_field*, int nField,
                         char *zUuidOut);
int fossilsee_ticket_change(fossilsee*, const char *zUuid,
                            const fossilsee_field*, int nField);

/* --- content by hash, so callers stop learning the blob schema ------- */
int fossilsee_content(fossilsee*, const char *zHash, fossilsee_blob*);

/* --- the escape hatch, kept, and now honestly labelled --------------- */
int fossilsee_sql(fossilsee*, const char *zSql, fossilsee_row, void*);
```

`fossilsee_sql()` **stays**. An escape hatch is right — viki's own `viki sql`
exists on the same argument, and no verb set anticipates every query. What
changes is that it stops being the *only* door, so a caller reaches for it when
it wants something unusual rather than to do the ordinary thing.

### Why the ticket API cannot be SQL, specifically

This is not a style preference and `embed/README.md` already states the rule:
the `ticket`/`ticketchng` tables are a **materialized view of DVCS-versioned
artifact history**. A ticket change is an *artifact* — hash-addressed,
Merkle-linked, and merged field-by-field by Fossil when two peers edit the same
ticket offline. Writing the tables directly produces rows with no artifact
behind them: the queryable data and the hash chain diverge, the next `rebuild`
silently discards the write, and no peer ever sees it.

That merge behaviour is also precisely why viki wants tickets in the first
place — `ARBITRATION.md` §2b proposes moving notes and supersession onto
ticket changes exactly *because* Fossil arbitrates them. An API that let a
caller write the view would hand it a way to lose the property it came for.

So: **reads may be SQL; writes must be artifact creation.** v0 enforces that
with an authorizer, which is the correct mechanism and the wrong altitude —
it forbids the mistake without offering the right way to do the thing.

## 5. What blocks v1, honestly

Nothing here is free, and `README.md`'s "What's still open" already names the
first item.

1. **Output capture.** Every verb above is `fossil_main(argc,argv)` underneath,
   and Fossil writes to stdout. This is the gate on all of it. README already
   identifies the two options (per-call redirection to a buffer, or Fossil's
   built-in JSON command API — noting the JSON API is *not* a separate
   invocation mechanism, just an easier-to-parse stdout).
2. **HTTP has no timeout.** Verified rather than inherited: `http_socket.c`
   and `http_transport.c` contain no `SO_RCVTIMEO`/`SO_SNDTIMEO` and no
   connect timeout, and every `fossil_set_timeout()` call site in `main.c` is
   inside `cmd_cgi()`, `cmd_webserver()` or `test_warning_page()` — server and
   CGI paths, never client sync. A stalled mobile network hangs a sync forever. This is a **Time leak**
   in §5's taxonomy — something that looks like a call and is an unbounded
   network round-trip — and it is why `msTimeout` is in every signature above
   rather than left to the caller to wish for. README flags that the natural
   fix (watchdog closing the socket) surfaces as an I/O error, which is a
   plausible route into the untrapped `fossil_panic()` gap. That tension is
   real and needs its own design; it does not go away by not having an API.
3. **The prologue is mirrored, not shared.** `fs_prime_globals()` copies a
   slice of `fossil_main()`'s setup, and a future Fossil that adds a required
   init step compiles clean and fails at runtime. Writes and sync reach much
   more of that prologue than read-only SQL does, so v1 raises the odds of
   exactly that failure. The durable fix — `fossil_embed_open_repository()`
   patched in beside `fossil_main()` — is listed in README as not done, and
   should land **before** v1 rather than after.
4. **Reentrancy and the single worker thread.** Already documented: `Global g`
   is not thread-local and Fossil is not reentrant. Sync is long-running, so
   v1 makes the "one dedicated serializing thread" rule load-bearing rather
   than theoretical.

## 6. Sequencing this suggests

1. `fossil_embed_open_repository()` upstream in Fossil — shares the prologue,
   removes the mirroring hazard before more of it is depended on.
2. Output capture. Everything else is gated on it.
3. `uv_*` + `sync` — the smallest set that makes a phone a peer, and the one
   viki needs to stop forking at all. This is the slice that pays for itself
   first.
4. Ticket and wiki verbs, reads first.
5. Ticket/wiki **writes**, once artifact-creation paths are exercised
   in-process.

viki can adopt each step as it lands: it already routes every Fossil read
through one function (`fossil_sql_framed()`), and its remaining forks are
exactly `fossil uv add/sync/export` plus the `viki-identity` verifier. Step 3
removes the first group. The verifier is a separate question — viki links no
crypto by design, and `libfossilsee` already carries LibreSSL, which makes
`fossilsee_verify_ed25519()` a plausible fifth item rather than an obvious one.
