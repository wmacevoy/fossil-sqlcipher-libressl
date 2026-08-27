# Calendar over wiki — explored here, promoted to viki

**Status: ABANDONED IN THIS REPO.** Not a dead end. The work moved up to viki,
which is the correct home, and this branch is the record of why. The live work
is in `wmacevoy/viki`; nothing here should be revived without reading that
first.

**Authoritative spec:** viki `CALENDAR_DESIGN_V2.md`, branch
`calendar/interchange-v2`. This document is deliberately *not* a copy of it — a
second copy of a spec is the thing that rots. This is the fossil-see-side
record: what was explored, what was found about Fossil itself, and why it left.

---

## 1. What was explored

Representing a calendar as iCalendar (RFC 5545) documents stored in Fossil
**wiki artifacts** at mimetype `text/calendar` — one wiki page = one
`VCALENDAR` = one calendar collection — with concurrent edits resolved by
`(UID, RECURRENCE-ID) → max(SEQUENCE, DTSTAMP)`, which is RFC 5546's precedence
rule rather than anything invented.

The appeal was that it needs no new artifact type and no change to Fossil's
write path: the payload carries its own merge keys, so it converges where the
wiki storing it cannot.

---

## 2. Why it left this repo — feature versioning for robots

This is the whole reason, and it is not about calendars.

An agent does not query a calendar through a UI. It queries the relational
projection directly, with SQL. **That makes the schema the API** — there is no
adapter layer in between to absorb change. And an agent that builds a robot does
not re-derive its query; it runs the SQL it was handed, on a schedule, forever.

A rename is the *good* case: it errors, loudly, the first time. The damage is
**semantic drift under stable names**. Decide that `TENTATIVE` counts as busy,
and every column name is unchanged, every query still parses, every query still
returns rows — and the robot quietly starts making different scheduling
decisions. Nothing errors. Nobody finds out.

So the representation needs a version consumers can *assert against*: semver
where major means "I broke you", a declared compatibility window, a deprecation
horizon so a robot can report trouble before it breaks, and somewhere to publish
all three.

**fossil-see has none of that infrastructure, and cannot grow it honestly.** It
is a build of Fossil. It has no manifest, no epoch mechanism, and no consumers
to version against. Its version tracks the binary and a pinned `FOSSIL_REF`
(`build/versions.env`) — and coupling a calendar contract to that would be worse
than nothing, because a Fossil bump would imply a calendar change that did not
happen and consumers would learn to ignore the check.

**viki has all of it already.** `viki-manifest` pins `model_id`, model path and
checksum, chunking params and a schema version; it is both a uv blob and a
committed artifact for tamper-evidence; it is signed. The epoch pattern already
lets two versions coexist through a migration, and already treats derived data
as disposable and rebuildable. Adding `calendar_format`, `calendar_schema` and
`tzdata_version` is one field each — not new machinery.

The calendar was therefore **promoted, not abandoned**. It belongs where the
versioning lives. Anything in this repo that says otherwise — including the
existence of this branch — is a record, not a plan.

---

## 3. What this repo established about Fossil, which stays true either way

These are findings about Fossil itself, produced here, cited to `FOSSIL_REF`
42e3bc1 so they can be re-verified rather than trusted. They outlive the
calendar question.

**Fossil's wiki has no merge, no conflict detection, and no stale-edit check.**
`grep -ni "fork\|merge\|conflict\|stale"` across all of `src/wiki.c` returns
nothing. `wiki_cmd_commit()` (`wiki.c:2101`) emits exactly one P card, and every
caller resolves the parent as the **server-side tip** (`json_wiki.c:378`,
`wiki.c:1029`, `wiki.c:2474`) — a client cannot supply the version it edited
against. Two loss paths follow, and both are live today for every wiki page in
every Fossil repo:

- concurrent writers on one server → writes chain linearly, the later document
  wholly replaces the earlier;
- two clones editing offline → both artifacts claim the same parent, the page
  forks into two leaves, display takes `ORDER BY mtime DESC` (`wiki.c:1668`) and
  the other leaf is silently shadowed.

**Consequence for any future work:** *any* design storing documents in Fossil
wiki pages must read over history, not over leaves, or it loses writes with no
error. This is the finding most worth keeping.

**The mimetype gate.** `wiki_filter_mimetypes()` (`wiki.c:176`) allowlists only
`azStyles` plus `text/x-markdown` and `text/plain`; anything else silently
becomes `text/x-fossil-wiki`. It is the single chokepoint for all 13 call sites
— CLI, `/wikiedit`, JSON API, technotes, and the search indexer.

**W cards are byte-transparent.** `W <size>\n<content>\n` is length-prefixed
(`manifest.c:1026`), so RFC 5545's 75-octet CRLF folding round-trips at the
artifact layer, and `blob_to_lf_only()` is not called from `wiki.c`. The
CGI/textarea path was never verified.

**Technotes are the wrong primitive for scheduled time.** The `E` card
(`manifest.c:652`) carries an occurrence timestamp "distinct from the D
timestamp" — one timestamp in *artifact metadata*, with no duration and no
recurrence. It conflates transaction time with valid time, which is the mistake
the whole design exists to avoid.

**FTS5 is compiled in** (`chat.c:314`, `search.c:1759`); **rtree is used
nowhere.** Verify before relying on rtree for interval indexing.

---

## 4. The one change that would come back here

If viki's OQ-1 ever resolves toward wiki-stored authoring, exactly one
fossil-see-side change is needed — a one-line widening of the mimetype
allowlist:

```diff
--- a/src/wiki.c
+++ b/src/wiki.c
@@ -183,7 +183,8 @@ const char *wiki_filter_mimetypes(const char *zMimetype){
     if(  fossil_strcmp(zMimetype, "text/x-markdown")==0
-        || fossil_strcmp(zMimetype, "text/plain")==0 ){
+        || fossil_strcmp(zMimetype, "text/plain")==0
+        || fossil_strcmp(zMimetype, "text/calendar")==0 ){
       return zMimetype;
     }
```

Deliberately **not** committed as `build/patches/*.patch`, because the feature is
abandoned here and the build should not carry a patch for it. Two notes for
whoever adopts it:

- `build/build.sh` applies patches **by explicit name** with `if [ -f ... ]`
  guards, not by glob — so adopting this needs both the patch file and its own
  invocation block. Dropping a file into `patches/` does nothing on its own.
- Do **not** add `text/calendar` to `azStyles`. That array drives the wiki
  editor's mimetype dropdown and would invite hand-editing ICS in a textarea.
  `wiki_render_by_mimetype()`'s existing `else` branch already emits escaped
  `<pre>`, which is correct for ICS, so no render case is needed.

It is also **not needed for the direction viki actually took.** Ingest reads
iCalendar from Google Calendar and O365 into a projection; it never stores ICS
in a Fossil wiki page. This patch is required only for the authoring path.

---

## 5. The prototype

`docs/calendar-over-wiki-prototype.html` — one self-contained file, no
dependencies, no network. Open it in a browser.

It is a working implementation of the resolution rules, not a mockup: a real ICS
parser, the union reducer, recurrence with `EXDATE` and `RECURRENCE-ID`
overrides, and the four RFC 5545 time forms.

Two controls make the arguments above reproducible in one click rather than
taken on faith:

- **Resolve over: All versions / Leaf only** — switch it and the LibreSSL review
  snaps from the 15th back to the 8th, because one write clobbered another.
  That is SS 3's loss, demonstrated.
- **Display in** — of the four time forms, the UTC and zoned ones move, the
  floating one does not, and no offset is stored anywhere.

Its mock data stands in for `/json/wiki/get?uuid=...` over the artifact history.

---

## 6. Where the live work is

| | |
|---|---|
| Spec | viki `CALENDAR_DESIGN_V2.md`, branch `calendar/interchange-v2` |
| Implementation | viki `src/viki_cal.c` — the ICS shredder, assertion tier |
| Queue entry | viki `QUEUE.md` SS 52 |
| Standing decision | viki D-2 — calendar as ticket-style artifacts plus a local projection. **Unchanged.** |

The interchange spec is specified, not adopted. The shredder is on the critical
path either way, because SS 2.3's ingest job needs it whichever way OQ-1
resolves.
