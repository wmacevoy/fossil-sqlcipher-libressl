# Security: at-rest encryption design

fossil-see is a build of Fossil (https://fossil-scm.org) with SQLCipher providing at-rest encryption for the repository file, and LibreSSL providing both libcrypto (for SQLCipher) and libssl (TLS for sync). This document describes what the encryption layer defends against, and how the `PRAGMA key` reaches Fossil's existing SEE (SQLite Encryption Extension) scaffolding via `build/patches/fossil-db-key.patch`.

Consumers of fossil-see (pizza-party-vote-fossil, fossil-app, and future projects) build their own trust/identity/authorization model on top; this document only covers the storage-encryption layer itself.

## What's in scope

Repository files whose name matches `*.efossil` are treated as encrypted; plain `*.fossil` repos are unencrypted stock SQLite, unaffected by any of this. The encryption boundary is entirely storage — identity, commit signing, sync transport security, and access-control policy are handled by Fossil itself (accounts, capabilities, TLS) and are out of scope here.

## Threats in scope (when a repo is `.efossil`)

1. **Stolen device / disk / image theft.** An adversary with the raw repository file, but not the key, cannot read it — SQLCipher renders the file HMAC-verified ciphertext, not partially-readable SQLite.
2. **Backup exposure.** Backups of `.efossil` files remain ciphertext.
3. **Multi-user workstation / shared host hygiene.** A second local account, or a compromised sibling process, reading the repo file off disk gets ciphertext, not plaintext.

## Threats explicitly out of scope

1. **Live compromise of a running process holding the key.** The key lives in memory (an mlock'd page, via Fossil's own SEE machinery) while the repo is open; a compromised process can read it.
2. **RAM extraction / cold-boot attacks** against a live process.
3. **Repo integrity / forgery** — handled by Fossil's hash chain and whatever signing convention a consumer project layers on top.
4. **Network interception** — handled by LibreSSL TLS, not this layer.
5. **Authorization/identity policy** — entirely a consumer concern (Fossil accounts/capabilities, or whatever a project builds on top).

## Key-source mechanism (`fossil-db-key.patch`)

Fossil's stock SEE code, when compiled with `--with-see=1`, prompts interactively for a passphrase on every open of an `.efossil` repo. `build/patches/fossil-db-key.patch` replaces that prompt with a priority chain, implemented in `see_decrypt_master_key()` and the patched `db_maybe_obtain_encryption_key()` in `src/db.c`:

1. **`FOSSIL_SEE_KEY` env var.** An escape hatch, documented as testing/CI-only — anyone using it should know this defeats at-rest protection for as long as the variable is in the process environment.
2. **gpg-decrypted `<repo-dir>/keys/master.key.asc`.** The repository's own directory may carry a gpg-encrypted key blob; `gpg --decrypt --output - keys/master.key.asc` (no `--batch`, so gpg-agent can broker passphrase entry or a smart-card prompt) recovers the key. This is the mechanism for a shared-key deployment: encrypt the key once to every authorized recipient's gpg public key with `gpg --encrypt --armor --recipient ... --recipient ...`, commit the blob next to the repo. How that blob gets produced, rotated, and who the recipients are is a consumer-project decision (see pizza-party-vote-fossil's `docs/threat-model.md` for one fully worked example: a frozen voter roster, convener-generated key, multi-recipient gpg encrypt at election genesis).
3. **Fossil's original interactive prompt**, only if `FOSSIL_SEE_STOCK_PROMPT=1` is set — a compatibility escape hatch for anyone who wants stock behavior back.

If none of the three produce a key, `fossil_fatal()` fails the open with a descriptive error rather than silently proceeding.

## Known bug, fixed here: `fossil server`/`fossil ui` on SEE builds

Stock Fossil (verified 2.28 and 2.29) has a logic bug in `db_maybe_obtain_encryption_key()`: server-mode startup pre-allocates a zeroed placeholder page for the key (so forked request-children can inherit it once a real key is obtained), but the key-obtain check tests only pointer non-NULLness, not validity — so it mistakes the zeroed placeholder for a real (empty) key and skips every key source above entirely, failing every repo open with `SQLITE_NOTADB`. `build/patches/fossil-server-key-validator.patch` fixes this by consulting Fossil's own `db_have_saved_encryption_key()` validator instead. Full write-up, drafted for an upstream report: `docs/upstream-report-fossil-server-see-key.md` (not yet filed as of this writing).

## Known bug, fixed here: the gpg key-source path used pKey after freeing its backing memory

`fossil-db-key.patch`'s gpg key source (`see_decrypt_master_key()` +
its caller in `db_maybe_obtain_encryption_key()`) never worked: it
populated `pKey` via `blob_set(pKey, zGpgKey)`, then immediately called
`fossil_secure_zero(zGpgKey, ...)` and `fossil_free(zGpgKey)`. Fossil's
own `blob_set()` (see `blob_init()` in `src/blob.c`) does **not** copy
the string — it aliases `pBlob->aData` directly to the pointer passed
in (that's what the separate `blob_set_dynamic()` exists for: taking
ownership of a malloc'd string it *will* free later). So `pKey` ended
up pointing at memory that had just been zeroed and then freed, for
every subsequent use of it — both the immediate
`db_set_saved_encryption_key(pKey)` call and the caller's own
`blob_str(&key)` used to build the `PRAGMA key(%Q)` command. The
practical symptom: any `.efossil` repo relying on the `keys/
master.key.asc` group-key mechanism (mode 2 — the case `FOSSIL_SEE_KEY`
exists specifically to bypass for testing) failed to open at all,
`SQLITE_NOTADB`/`hmac check failed`, even though `gpg --decrypt` itself
succeeded and returned the correct key. Confirmed via `strace`: gpg
exits 0 with the right key material read from `master.key.asc`, but
SQLCipher still rejects the (by-then-corrupted) key it's handed.

This is **not** a stock-Fossil bug — `blob_set()` behaves exactly as
Fossil documents it; the bug is that this project's own patch used the
"borrow" function where it needed the "copy/own" one. Fix: use
`blob_append(pKey, zGpgKey, strlen(zGpgKey))` instead of `blob_set()`,
which does a real `memcpy` into `pKey`'s own buffer, so `pKey` survives
independently of `zGpgKey`'s lifetime. Verified end to end: `fossil-see
sql "select count(*) from blob;" -R repo.efossil` now succeeds with
**no** `FOSSIL_SEE_KEY` set at all, purely via `gpg --decrypt` against
`keys/master.key.asc` (agent-cached passphrase); `fossil-see server`
against the same repo also opens correctly (checked `/timeline` and
`/wiki` both return normally, not `SQLITE_NOTADB`).

## Master-key randomness

Whoever generates a shared key (e.g. the value used for `FOSSIL_SEE_KEY`, or the plaintext fed into the `keys/master.key.asc` gpg-encrypt step) should draw it from a CSPRNG. fossil-see's own binary links LibreSSL libcrypto, so `RAND_bytes()` is already on hand for any consumer that wants to generate the key from within the same toolchain rather than shelling out.

## Operational notes

- `keys/master.key.asc` does not need restrictive file permissions — the file is itself gpg-encrypted ciphertext and is meant to be committed to a synced repo. `chmod 0700` the clone directory as ordinary hygiene.
- `FOSSIL_SEE_KEY` in the environment is visible to anything that can read that process's environment (e.g. `/proc/<pid>/environ` on Linux, or any co-resident process with sufficient privilege) for as long as the process runs. Use it for CI/testing, not production key delivery.
