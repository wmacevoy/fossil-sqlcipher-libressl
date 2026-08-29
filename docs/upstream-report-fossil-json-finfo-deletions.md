# `finfo` silently omits every file deletion on two of its three routes; `/json/finfo`'s `isDel` column has been unreachable since 2012

**Status: drafted, not yet filed upstream.** Ready to post to fossil-scm.org's forum as-is.

## Summary

`/json/finfo` (and the `fossil json finfo` CLI wrapper) reports a file's history
with every **deletion missing**. A file that was added, modified and then
`fossil rm`'d comes back as two entries — `added`, `modified` — with no
indication the file was ever removed.

The route computes a deletion flag and never emits it. `json_finfo.c` selects
`(mlink.fid==0) AS isDel` and passes it to `json_artifact_status_to_string()`,
which returns `"added"`, `"modified"` or `"removed"`. But the same query
inner-joins `blob b` on `b.rid=mlink.fid`. A deletion has `mlink.fid==0`, and no
blob has `rid==0`, so the join removes exactly the rows `isDel` exists to flag.
The column is dead code and `"removed"` is unreachable from this route.

**The same inner join appears in the CLI's log-mode query** (`finfo.c:223`), so
`fossil finfo FILENAME` omits deletions too. Only the **web** `/finfo` page is
correct: `finfo_page()` (`finfo.c:453`) carries an explicit
`mlink.fid>0 OR NOT EXISTS(...)` clause for exactly this case.

So of the three routes onto one file's history, two disagree with the third:

| route | function | deletions |
|---|---|---|
| `/finfo` (web) | `finfo_page()`, `finfo.c:453` | **reported** |
| `fossil finfo` (CLI) | `finfo_cmd()`, `finfo.c:223` | omitted |
| `/json/finfo` | `json_finfo.c:80` | omitted |

Measured on the repo built by the reproduction below, running each route's own
query verbatim:

```
CLI query        -> add, mod
web page query   -> add, mod, del (isDel=1)
```

## Affected versions

Reproduced against 2.28 `[1573b8e66e]`. The line is unchanged on current trunk
(`4ef9f4083` at time of writing); `src/json_finfo.c` has had no substantive
change since 2024-02-02 (`83fd55bc6`, trailing-whitespace removal).

The two commits involved:

- `06349cd36` (2012-03-02) added `/json/finfo` with the inner join.
- `80d77014c` (2012-03-22) added the `isDel` column to that query.

So `isDel` has never worked: the join that defeats it predates it by twenty
days.

## Reproduction

```sh
fossil init r.fossil
mkdir co && cd co && fossil open ../r.fossil
echo one >  f.txt && fossil add f.txt && fossil commit -m add
echo two >> f.txt                     && fossil commit -m mod
fossil rm f.txt                       && fossil commit -m del

fossil json finfo --name f.txt   # two entries: added, modified
fossil finfo f.txt               # three -- the CLI sees the deletion
```

Observed `state` values before the fix:

```
state=modified  comment=mod
state=added     comment=add
```

After:

```
state=removed   comment=del
state=modified  comment=mod
state=added     comment=add
```

## Fix

Make the blob join outer, so a deletion row survives it. Shown for
`json_finfo.c`, which is the route this report was produced against and the one
verified end to end:

```diff
--- a/src/json_finfo.c
+++ b/src/json_finfo.c
-        "  FROM mlink, blob b, event, blob ci, filename"
+        "  FROM mlink LEFT JOIN blob b ON b.rid=mlink.fid,"
+        "       event, blob ci, filename"
         " WHERE filename.name=%Q"
         "   AND mlink.fnid=filename.fnid"
-        "   AND b.rid=mlink.fid"
```

Nothing else needs to change. On a deletion row `b.uuid` and `b.size` are NULL;
`json_new_string()` maps NULL to NULL and `cson_object_set()` then unsets the
key, so the row carries `"state": "removed"` and **no** `"uuid"` — which is
correct, because a removal names no file content. `b.size` yields 0 via
`db_column_int64()`.

`finfo.c:223` takes the identical change. It is deliberately **not** proposed
here as a finished patch, because the CLI *prints* what the JSON route only
returns: `finfo_cmd()` formats each row as `artifact: [%S]` from `b.uuid`, which
is NULL on a deletion. Fossil's `%S` is NULL-safe (`printf.c:726` substitutes
`""`), so the fix does not crash — it renders `artifact: []`, and what a
deletion row should actually *say* is a UI decision that belongs to whoever
maintains this, not to a drive-by patch. Reporting it rather than guessing.

## Compatibility note

This is a behaviour change for any consumer that assumes every element of
`payload.checkins` has a `"uuid"`. That assumption was already unsound for the
`json_artifact.c` route, which shares `json_artifact_status_to_string()`, but a
consumer written only against `/json/finfo` could not have encountered a
`"removed"` row before, since none was reachable.

Filing it as a bug rather than a feature request because the intent is
unambiguous in the code: the column, the status string, and the `"removed"`
literal are all present and were clearly meant to be emitted. The web `/finfo`
page having handled this correctly all along is the strongest evidence that
omitting deletions was never the intended behaviour.
