# fossil-see build patches

Small patches applied to the upstream Fossil source tree during the build. Each is self-contained, attributed to a specific Fossil revision (`FOSSIL_REF` in `../versions.env`), and small enough to read in one sitting.

## Patches in this directory

- **`fossil-db-key.patch`** — wires a mode-aware key source into Fossil's existing SEE scaffolding (`db_maybe_obtain_encryption_key` in `src/db.c`). Priority: `FOSSIL_SEE_KEY` env var (testing-only escape hatch) → gpg-decrypted `<repo-dir>/keys/master.key.asc` → Fossil's stock interactive prompt, only if `FOSSIL_SEE_STOCK_PROMPT=1`. Fails fast with a descriptive `fossil_fatal()` if none produce a key. See `../../docs/SECURITY.md` for the full design.

- **`fossil-server-key-validator.patch`** — fixes `fossil server`/`fossil ui` on SEE/SQLCipher builds. Server startup (`db_setup_for_saved_encryption_key()`) pre-allocates a zeroed mlock'd page for the key so forked request children can inherit it; `db_maybe_obtain_encryption_key()` checked only that the saved-key pointer was non-NULL, mistook the zero page for a real key, skipped every key source above, and every repo open failed `SQLITE_NOTADB`. Fix consults Fossil's own `db_have_saved_encryption_key()` validator. Apply after `fossil-db-key.patch`. Stock Fossil (verified 2.28, 2.29) has the identical unvalidated check — see `../../docs/upstream-report-fossil-server-see-key.md` (drafted, not yet filed with fossil-scm.org).

- **`fossil-db-embed.patch`** — adds `db_clear_delete_on_failure()`, exposing one new function; changes no existing behavior. Needed for any future in-process/embedded use (repeated `fossil_main()` calls in one process must clear stale delete-on-failure registrations between calls, or a later failure deletes files created by an earlier successful command — including the repository itself). See `../../embed/README.md`.

## Conventions

- One patch per concern. Don't bundle unrelated edits.
- Patches are unified diffs (`diff -u` or `git diff` output), applied with `patch -p1` from the Fossil source root.
- Verify against the pinned `FOSSIL_REF` before treating a patch as ready.
