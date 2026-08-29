# FINDINGS.md — fossil-see

Surprising things discovered while building, with the measurement. Newest first.

---
## libfossilsee is SINGLE-TRIBE, and it is not the `Global g` pointer that decides it

2026-08-29. **Decision (Warren): v1 is single tribe. libfossilsee may as well
treat its context as global.** This entry is why, because the obvious reading —
"one repository per process because `Global g` is a process global" — is only
the first layer, and fixing that layer does not buy multi-tribe.

`build/patches/fossil-swappable-context.patch` makes `g` a macro for `(*gp)`
with `_Thread_local Global *gp`, so the context is selectable per thread. All
~5,000 `g.field` sites compile unchanged, `&g` becomes `gp`, `sizeof(g)` becomes
`sizeof(*gp)`. m1 90/0/0 and fossilsee-probe 24/0 against it. **That patch is
correct and it is not sufficient**, because per-context state also lives in
function statics that no pointer swap moves.

Counted across `src/`:

```
~219   function-local mutable statics
 ~78   file-scope mutable statics
  66   `static Stmt` -- CACHED PREPARED STATEMENTS, in 26 files
```

The 66 are the ones that decide the question. A `Stmt` holds
`sqlite3_stmt *pStmt`, a pointer bound to one specific connection, and they sit
in the hot paths: `manifest.c`, `content.c`, `xfer.c` (sync), `search.c`,
`vfile.c`, `timeline.c`.

**The failure condition is exactly multi-tribe, and only multi-tribe.**
`db_close()` runs `while( db.pAllStmt ){ db_finalize(db.pAllStmt); }`, so
close-then-switch is safe — the statics re-prepare against the new connection.
But *switching without closing* leaves a statement prepared against the other
connection, and switching without closing is the definition of holding two
tribes open at once. Single-tribe never reaches the bug.

db.c's own twelve classify cleanly, and two are already known to bite:

| static | class |
|---|---|
| `zSavedKey`, `savedKeySize` (`db.c:1667`) | per-repo. A second open **silently reuses the first key** — measured: a deliberately wrong key SUCCEEDED after a good open |
| `dbRepositoryFilenameCache` (`db.c:2805`) | per-repo. Returns the FIRST repository ever opened |
| four `static Stmt` | connection-bound |
| `busy`, `once`, a hash return buffer, an argv template | harmless |

**What changes because of this entry:** the audit stops being symptom-driven.
`embed/README.md` says three of these "have now been found one at a time, each
by a different symptom, and there is no reason to think the audit is finished."
It is finishable: `static Stmt` is one grep, and "a static assigned from a `db_*`
call" is another. That turns an open-ended hazard into a bounded list — which is
what makes single-tribe an *informed* choice for v1 rather than a hope.

Two fixes exist for later, neither of which is 66 hand-lifts: give `Stmt` an
owner/generation field checked in `db_step()`, so a swapped context fails
loudly at the point of use; or key the caches off `gp` so a swap simply misses.
The first is better — it converts a silent wrong-connection use into an error.
