#!/usr/bin/env bash
set -euo pipefail

# Build fossil-see: a Fossil binary with SQLCipher-encrypted-at-rest storage
# and LibreSSL providing both libcrypto (SQLCipher storage encryption) and
# libssl (TLS for sync). This is the shared build recipe behind:
#   - pizza-party-vote-fossil's fossil-ppv (voting protocol reference impl)
#   - fossil-app's encrypted hub deployment
# Neither project forks this recipe; they consume the fossil-see binary +
# patches produced here. See ../README.md for the design.
#
# Modeled on sqlcipher-libressl's own build-sqlcipher-libressl.sh.
#
# Required env: none (all default to the vendored submodules under ../vendor/)
#
# Optional env:
#   LIBRESSL_PREFIX   install prefix containing include/ and lib/{libcrypto.a,libssl.a}
#                     (default: vendor/libressl-build-out; built on first run)
#   LIBRESSL_CACHE    where the downloaded LibreSSL tarball is cached
#                     (default: vendor/libressl-cache)
#   SQLCIPHER_DIR     path to a sqlcipher-libressl checkout (default: vendor/sqlcipher-libressl)
#   FOSSIL_SRC        path to a Fossil source checkout (default: vendor/fossil)
#   FOSSIL_REF        expected git ref of FOSSIL_SRC (verified if set; default from versions.env)
#   OUTPUT_DIR        where to write the built binary (default: build/dist)
#   JOBS              make parallelism (default: nproc or sysctl detected)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUTPUT_DIR="${OUTPUT_DIR:-$SCRIPT_DIR/dist}"

# shellcheck source=./versions.env
. "$SCRIPT_DIR/versions.env"

: "${FOSSIL_SRC:=$REPO_ROOT/vendor/fossil}"
: "${SQLCIPHER_DIR:=$REPO_ROOT/vendor/sqlcipher-libressl}"
: "${LIBRESSL_PREFIX:=$REPO_ROOT/vendor/libressl-build-out}"
: "${LIBRESSL_CACHE:=$REPO_ROOT/vendor/libressl-cache}"

if [ -z "${JOBS:-}" ]; then
    if command -v nproc >/dev/null 2>&1; then
        JOBS="$(nproc)"
    else
        JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 2)"
    fi
fi

mkdir -p "$OUTPUT_DIR"

# -- Step 0: Download + build LibreSSL if not already built --------
if [ ! -f "$LIBRESSL_PREFIX/lib/libcrypto.a" ] || [ ! -f "$LIBRESSL_PREFIX/lib/libssl.a" ]; then
    : "${LIBRESSL_TARBALL_URL:?Set LIBRESSL_TARBALL_URL (see versions.env)}"
    : "${LIBRESSL_TARBALL_SHA256:?Set LIBRESSL_TARBALL_SHA256 (see versions.env)}"
    command -v cmake >/dev/null 2>&1 || { echo "ERR: cmake is required to build LibreSSL"; exit 1; }

    mkdir -p "$LIBRESSL_CACHE"
    tarball="$LIBRESSL_CACHE/libressl-${LIBRESSL_VERSION}.tar.gz"
    if [ ! -f "$tarball" ]; then
        echo "==> Downloading LibreSSL $LIBRESSL_VERSION from $LIBRESSL_TARBALL_URL"
        curl -fsSL "$LIBRESSL_TARBALL_URL" -o "$tarball"
    fi

    actual_sha="$(shasum -a 256 "$tarball" | awk '{print $1}')"
    if [ "$actual_sha" != "$LIBRESSL_TARBALL_SHA256" ]; then
        echo "ERR: LibreSSL tarball SHA256 mismatch"
        echo "     expected: $LIBRESSL_TARBALL_SHA256"
        echo "     actual:   $actual_sha"
        exit 1
    fi

    echo "==> Building LibreSSL $LIBRESSL_VERSION"
    extract_dir="$LIBRESSL_CACHE/libressl-${LIBRESSL_VERSION}"
    rm -rf "$extract_dir"
    tar -xzf "$tarball" -C "$LIBRESSL_CACHE"
    (
        cd "$extract_dir"
        mkdir -p build
        cd build
        cmake .. -DCMAKE_INSTALL_PREFIX="$LIBRESSL_PREFIX" \
            -DLIBRESSL_APPS=OFF -DLIBRESSL_TESTS=OFF -DBUILD_SHARED_LIBS=OFF \
            -DCMAKE_BUILD_TYPE=Release
        make -j"$JOBS"
        make install
    )
fi

# -- Sanity checks ---------------------------------------------------
echo "==> Verifying inputs"
[ -f "$LIBRESSL_PREFIX/lib/libcrypto.a" ]        || { echo "ERR: $LIBRESSL_PREFIX/lib/libcrypto.a missing"; exit 1; }
[ -f "$LIBRESSL_PREFIX/lib/libssl.a" ]           || { echo "ERR: $LIBRESSL_PREFIX/lib/libssl.a missing";    exit 1; }
[ -f "$LIBRESSL_PREFIX/include/openssl/crypto.h" ] || { echo "ERR: LibreSSL headers missing under $LIBRESSL_PREFIX/include"; exit 1; }
[ -d "$SQLCIPHER_DIR" ]                          || { echo "ERR: SQLCIPHER_DIR not a directory: $SQLCIPHER_DIR (did you 'git submodule update --init'?)"; exit 1; }
[ -d "$FOSSIL_SRC" ]                             || { echo "ERR: FOSSIL_SRC not a directory: $FOSSIL_SRC (did you 'git submodule update --init'?)"; exit 1; }

if [ -n "${FOSSIL_REF:-}" ]; then
    actual="$(git -C "$FOSSIL_SRC" rev-parse HEAD 2>/dev/null || echo unknown)"
    if [ "$actual" != "$FOSSIL_REF" ]; then
        echo "ERR: FOSSIL_SRC is at $actual, expected FOSSIL_REF=$FOSSIL_REF"
        exit 1
    fi
fi

# -- Step 1: SQLCipher amalgamation -----------------------------------
echo "==> Producing SQLCipher amalgamation (via sqlcipher-libressl)"
if [ ! -f "$SQLCIPHER_DIR/sqlite3.c" ] || [ ! -f "$SQLCIPHER_DIR/sqlite3.h" ]; then
    (
        cd "$SQLCIPHER_DIR"
        ./configure --with-tempstore=yes \
            CFLAGS="-DSQLITE_HAS_CODEC -DSQLCIPHER_CRYPTO_OPENSSL \
                    -DSQLITE_EXTRA_INIT=sqlcipher_extra_init \
                    -DSQLITE_EXTRA_SHUTDOWN=sqlcipher_extra_shutdown \
                    -I$LIBRESSL_PREFIX/include" \
            LDFLAGS="$LIBRESSL_PREFIX/lib/libcrypto.a"
        make -j"$JOBS" sqlite3.c
    )
fi
[ -f "$SQLCIPHER_DIR/sqlite3.c" ] || { echo "ERR: sqlite3.c not produced"; exit 1; }

# -- Step 2: Swap SQLCipher into Fossil source tree --------------------
# apply_patch <file> -- apply a patch IDEMPOTENTLY.
#
# `patch -p1 < f` on an already-patched tree prints "Reversed (or
# previously applied) patch detected!  Assume -R? [y]" and, with no tty to
# answer it, takes the default and REVERSES the patch -- silently undoing
# the very change it was asked to make. Re-running this script then leaves
# vendor/fossil half-patched and the build fails much later with a
# confusing error. Measured, not theorised: it corrupted the tree here on
# 2026-08-21 and the recovery was `git -C vendor/fossil checkout -- . &&
# git clean -fd`.
#
# --forward never reverses. The dry runs distinguish the three cases that
# matter -- applies cleanly, already applied, does not apply at all -- so
# the third is a hard error instead of being folded into the second.
apply_patch() {
    _p="$1"
    if patch -p1 --forward --dry-run < "$_p" >/dev/null 2>&1; then
        patch -p1 --forward < "$_p"
    elif patch -p1 --reverse --dry-run < "$_p" >/dev/null 2>&1; then
        echo "    (already applied, skipping)"
    else
        echo "ERR: $_p does not apply to this tree, and is not already applied" >&2
        exit 1
    fi
}

echo "==> Patching Fossil source"
cp "$SQLCIPHER_DIR/sqlite3.c" "$FOSSIL_SRC/extsrc/sqlite3-see.c"
cp "$SQLCIPHER_DIR/sqlite3.h" "$FOSSIL_SRC/extsrc/sqlite3.h"
cp "$FOSSIL_SRC/extsrc/shell.c" "$FOSSIL_SRC/extsrc/shell-see.c"

MAINMK="$FOSSIL_SRC/src/main.mk"
SEE_OLD='SEE_FLAGS.1 = -DSQLITE_HAS_CODEC -DSQLITE_SHELL_DBKEY_PROC=fossil_key'
SEE_NEW="${SEE_OLD} -DSQLITE_THREADSAFE=1 -DSQLITE_TEMP_STORE=2 -DSQLITE_EXTRA_INIT=sqlcipher_extra_init -DSQLITE_EXTRA_SHUTDOWN=sqlcipher_extra_shutdown"
if ! grep -qF "$SEE_OLD" "$MAINMK"; then
    echo "ERR: SEE_FLAGS.1 line not found verbatim in main.mk; check FOSSIL_REF" >&2
    exit 1
fi
awk -v old="$SEE_OLD" -v new="$SEE_NEW" '
    !done {
        i = index($0, old)
        if (i > 0) {
            $0 = substr($0, 1, i - 1) new substr($0, i + length(old))
            done = 1
        }
    } { print }
' "$MAINMK" > "$MAINMK.tmp" && mv "$MAINMK.tmp" "$MAINMK"

# build/patches/fossil-db-key.patch wires the mode-aware key source into
# Fossil's existing SEE scaffolding (db_maybe_obtain_encryption_key in
# src/db.c): FOSSIL_SEE_KEY env var > gpg-decrypt keys/master.key.asc >
# stock prompt only if FOSSIL_SEE_STOCK_PROMPT=1. See patches/README.md
# and ../docs/SECURITY.md for the design.
if [ -f "$SCRIPT_DIR/patches/fossil-db-key.patch" ]; then
    echo "  applying fossil-db-key.patch"
    ( cd "$FOSSIL_SRC" && apply_patch "$SCRIPT_DIR/patches/fossil-db-key.patch" )
else
    echo "  WARN: patches/fossil-db-key.patch absent - built binary will use Fossil's stock SEE prompt-for-passphrase behavior"
fi

# build/patches/fossil-server-key-validator.patch fixes 'fossil server'/
# 'fossil ui' with SEE/SQLCipher: server startup pre-allocates a zeroed
# saved-key page and the unvalidated pointer check in
# db_maybe_obtain_encryption_key then skips every key source, so all repo
# opens fail SQLITE_NOTADB. Applies on top of fossil-db-key.patch (also
# applies to stock db.c, with offset). Without it, only one-shot CLI/CGI
# invocations of an encrypted repo work; a long-running 'fossil-see server'
# does not. See patches/README.md and ../docs/upstream-report-fossil-server-see-key.md
# (drafted for fossil-scm.org, not yet filed).
if [ -f "$SCRIPT_DIR/patches/fossil-server-key-validator.patch" ]; then
    echo "  applying fossil-server-key-validator.patch"
    ( cd "$FOSSIL_SRC" && apply_patch "$SCRIPT_DIR/patches/fossil-server-key-validator.patch" )
else
    echo "  WARN: patches/fossil-server-key-validator.patch absent - 'fossil-see server' will fail to open *.efossil repos (SQLITE_NOTADB); one-shot CLI use is unaffected"
fi

# build/patches/fossil-db-embed.patch adds db_clear_delete_on_failure(),
# needed by any future in-process/embedded use (repeated fossil_main()
# calls in one process; see ../embed/README.md). Purely additive - exposes
# one new function, does not change CLI/server behavior. Safe to always
# apply.
if [ -f "$SCRIPT_DIR/patches/fossil-db-embed.patch" ]; then
    echo "  applying fossil-db-embed.patch"
    ( cd "$FOSSIL_SRC" && apply_patch "$SCRIPT_DIR/patches/fossil-db-embed.patch" )
else
    echo "  WARN: patches/fossil-db-embed.patch absent - db_clear_delete_on_failure() will not be available for future embedded/FFI use"
fi

# build/patches/fossil-embed-exit-trap.patch adds fossil_embed_init(), the
# portable (non-GNU-ld-specific) exit trap needed for in-process/FFI
# embedding on platforms with no --wrap=exit equivalent (Apple's linker,
# critically -- this is what ../embed/harness.c's `-Wl,--wrap=exit` cannot
# do on iOS). Purely additive - handler defaults to unset, byte-identical
# stock CLI behavior when not registered. Safe to always apply.
if [ -f "$SCRIPT_DIR/patches/fossil-embed-exit-trap.patch" ]; then
    echo "  applying fossil-embed-exit-trap.patch"
    ( cd "$FOSSIL_SRC" && apply_patch "$SCRIPT_DIR/patches/fossil-embed-exit-trap.patch" )
else
    echo "  WARN: patches/fossil-embed-exit-trap.patch absent - fossil_embed_init() will not be available for future embedded/FFI use"
fi

# build/patches/fossil-json-finfo-deletions.patch fixes a REAL UPSTREAM BUG,
# not a fossil-see need: /json/finfo silently omits every deletion.
#
# json_finfo.c computes `(mlink.fid==0) AS isDel` and feeds it to
# json_artifact_status_to_string(), which can return "added", "modified" or
# "removed" -- but the same query inner-joins `blob b` on `b.rid=mlink.fid`,
# and a deletion has fid==0 while no blob has rid==0.  So the join deletes
# exactly the rows isDel exists to flag: the column is dead code and
# "removed" is unreachable from this route.  The CLI `fossil finfo` gets it
# right (finfo.c has an explicit `mlink.fid>0 OR NOT EXISTS(...)` clause);
# only the JSON path is wrong.
#
# Measured before writing the patch, on a scratch repo with one file added,
# modified, then `fossil rm`'d: the shipped query returns two rows (added,
# modified) and the LEFT JOIN returns three, the third being isDel=1.
#
# This one is a candidate to send upstream -- see patches/README.md.
if [ -f "$SCRIPT_DIR/patches/fossil-json-finfo-deletions.patch" ]; then
    echo "  applying fossil-json-finfo-deletions.patch"
    ( cd "$FOSSIL_SRC" && apply_patch "$SCRIPT_DIR/patches/fossil-json-finfo-deletions.patch" )
else
    echo "  WARN: patches/fossil-json-finfo-deletions.patch absent - /json/finfo will under-report file history (deletions invisible)"
fi

# build/patches/fossil-swappable-context.patch makes Fossil's process-global
# context reachable through a PER-THREAD pointer: `#define g (*gp)` with
# `_Thread_local Global *gp`. Every one of the ~5,000 `g.field` sites compiles
# unchanged, `&g` becomes `gp`, `sizeof(g)` becomes `sizeof(*gp)`.
#
# WHY: `Global g` is why Fossil can open exactly ONE repository per process,
# which is why fossilsee.h documents a SINGLETON, which is why a device can
# host only one tribe. The database is one field of that struct.
#
# TWO TRAPS, both hit while writing it:
#   - `Global g;` must stay in main.c TEXTUALLY. mkheaders decides which
#     generated header needs `struct Global` by looking for files that use the
#     symbol `g`; rename that line and the struct stops being emitted and every
#     file fails with "incomplete type".
#   - gp is _Thread_local, so a thread that never called fossil_main() has a
#     NULL context. fossil_context_init() primes it and MUST be called by any
#     embedder that opens a repository directly -- embed/fossilsee.c does.
if [ -f "$SCRIPT_DIR/patches/fossil-swappable-context.patch" ]; then
    echo "  applying fossil-swappable-context.patch"
    ( cd "$FOSSIL_SRC" && apply_patch "$SCRIPT_DIR/patches/fossil-swappable-context.patch" )
else
    echo "  WARN: patches/fossil-swappable-context.patch absent - one repository per process"
fi

# -- Step 3: Configure Fossil -------------------------------------------
echo "==> Configuring Fossil"
# -DSQLITE_HAS_CODEC here (not just in SEE_FLAGS.1 in main.mk) matters:
# main.mk's SEE_FLAGS is only added to the sqlite3.o/shell.o compile rules,
# but db.c (USE_SEE-gated, via the global -DUSE_SEE from --with-see=1) calls
# sqlite3_key()/sqlite3_key_v2() directly. Those prototypes are guarded by
# SQLITE_HAS_CODEC in SQLCipher's sqlite3.h; without it here too, db.c's
# compile unit never sees them -> implicit-declaration build failure.
# CFLAGS passed to configure flows into every compile unit via TCCFLAGS, so
# this is the one place that reaches db.c as well as sqlite3.o/shell.o.
#
# -fPIC is here for embed/build-lib.sh, which links libfossilsee.so from
# these same objects. ELF refuses that outright without it -- measured in
# CI on linux-x86_64: "relocation R_X86_64_PC32 against symbol stderr can
# not be used when making a shared object; recompile with -fPIC". Mach-O
# does not care, which is exactly why this went unnoticed while the
# library was developed on macOS.
#
# It goes HERE rather than in build-lib.sh on purpose. Eight of the 158
# objects (sqlite3-see.o, shell.o, th*.o, pikchr.o, cson_amalgamation.o,
# linenoise.o) are built from extsrc/ with per-file flags this script does
# not spell out; recompiling them in build-lib.sh would mean duplicating a
# flag list that would then silently drift. One flag, one place.
(
    cd "$FOSSIL_SRC"
    ./configure \
        --with-openssl="$LIBRESSL_PREFIX" \
        --with-see=1 \
        --json \
        --internal-sqlite=1 \
        CFLAGS="-DSQLCIPHER_CRYPTO_OPENSSL -DSQLITE_HAS_CODEC -O2 -fPIC" \
        LIBS="$LIBRESSL_PREFIX/lib/libssl.a $LIBRESSL_PREFIX/lib/libcrypto.a"
)

# -- Step 4: Build ---------------------------------------------------
echo "==> Building Fossil"
make -C "$FOSSIL_SRC" -j"$JOBS"

# -- Step 5: Install ---------------------------------------------------
echo "==> Installing to $OUTPUT_DIR"
cp "$FOSSIL_SRC/fossil" "$OUTPUT_DIR/fossil-see"
ls -lh "$OUTPUT_DIR/fossil-see"

# -- Step 6: Smoke test ---------------------------------------------------
echo "==> Smoke tests"
"$OUTPUT_DIR/fossil-see" version

echo "==> Build complete"
