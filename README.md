# fossil-see

A Fossil (https://fossil-scm.org) build with SQLCipher-encrypted-at-rest storage and LibreSSL providing libcrypto (for SQLCipher) and libssl (TLS for sync) — an encrypted Fossil server and local sync client. One binary, `fossil-see`, is both: `fossil-see server` runs the hub; `fossil-see clone`/`sync` is the same binary acting as a client against any Fossil remote (encrypted or not — encryption is a per-repo storage choice, invisible to the sync protocol).

This project exists to be depended on, not forked. It currently backs:

- **pizza-party-vote-fossil** — the reference implementation of the Pizza Party Voting mechanism; uses this build for mode-2 (group-key) encrypted elections.
- **fossil-app** — a hub-and-spoke personal/family Fossil deployment (calendar, agents, cross-device sync); uses this build for its externally-hosted encrypted hub.

Both currently duplicate the build recipe by hand rather than depending on this repo directly (it didn't exist as a standalone project until now). Formalizing that dependency — via git submodule, once this repo has a real remote — is a deliberate follow-up, not done as part of this extraction.

## What's here

- `build/build.sh` → `build/dist/fossil-see`: Fossil + SQLCipher + LibreSSL + the mode-aware `PRAGMA key` patch + the server-mode fix. See `build/patches/README.md` for what each patch does.
- `docs/SECURITY.md`: what the at-rest encryption layer defends against, and the `FOSSIL_SEE_KEY` / `keys/master.key.asc` / stock-prompt key-source design.
- `docs/upstream-report-fossil-server-see-key.md`: a stock-Fossil bug found and fixed here (`fossil server`/`fossil ui` failing to open any SEE-encrypted repo), drafted and ready to file at fossil-scm.org, not yet submitted.
- `embed/`: a proof-of-concept (not yet a supported build target) that Fossil's client operations can run in-process — no fork/exec, `exit()` trapped — the constraint an FFI embedding (e.g. Dart/Flutter via `dart:ffi`) needs. See `embed/README.md` for what's proven and what isn't.

## Planned, not yet built

- **`libfossilsee` library target.** A static/shared library exposing an in-process call API for FFI embedding (mobile apps, etc.), built on the `embed/` proof-of-concept. Needs a portable exit-trap (the current proof-of-concept relies on GNU ld's `--wrap=exit`, unavailable on Apple's linker — Apple platforms need `fossil_exit()` itself patched to call a registered handler instead) before it's a real build target.
- **WASM target.** Feasibility not yet assessed: SQLCipher/LibreSSL's WASM story and a browser-storage-backed SQLite VFS (OPFS or similar) are both open questions, not just a cross-compile flag.
- **Optional QuickJS pluggability layer, three-tier by capability** (`--qjs=none|sandbox|system`) for any consumer that wants scriptable server-side hooks. This project has no plugin/hook mechanism today, and none is planned here without a dedicated threat-model doc first — an in-process plugin with access to a live SQLCipher key is a cross-tenant risk on a hosted server, not just a sandboxing problem.

## Building

```
git submodule update --init --recursive
./build/build.sh
./build/dist/fossil-see version
```

No required env vars — LibreSSL is downloaded (pinned tarball + SHA256) and built on first run; SQLCipher's amalgamation is produced from the vendored `sqlcipher-libressl` checkout. See `build/build.sh`'s header comment for optional overrides.

## Vendored dependencies

| Submodule | Upstream | Notes |
|---|---|---|
| `vendor/fossil` | drhsqlite/fossil-mirror | Pinned ref in `build/versions.env` |
| `vendor/sqlcipher-libressl` | wmacevoy/sqlcipher-libressl | Pinned ref in `build/versions.env` |
| LibreSSL (no submodule) | github releases | Downloaded + built into `vendor/libressl-build-out/` on first run |

## License

MIT. See `LICENSE`.
