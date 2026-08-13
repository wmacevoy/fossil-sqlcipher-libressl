# `fossil server` / `fossil ui` fail to open any SEE-encrypted repository: saved-key pointer check bypasses key acquisition entirely

**Status: drafted, not yet filed upstream.** Ready to post to fossil-scm.org's forum or ticket tracker as-is.

*(Relocated here from the pizza-party-vote-fossil project, where it was originally drafted, since the bug and fix live in this shared fossil-see build and are not specific to any one consumer project.)*

## Summary

Any Fossil binary built with `--with-see=1` (SQLite Encryption Extension support) fails to open **every** encrypted repository when run as `fossil server` or `fossil ui`, dying with `SQLITE_NOTADB` / "not a valid repository". The interactive key prompt never fires. Root cause is a pointer-truthiness check in `db_maybe_obtain_encryption_key()` that should instead call the validator Fossil already ships for this exact purpose, `db_have_saved_encryption_key()`.

## Affected versions

Reproduced against Fossil version 2.28 `[1573b8e66e]` by direct source inspection, and separately verified against 2.29 `[7a40eb9748]` — same unvalidated check present in both. Likely present since server-mode SEE key-sharing (`db_setup_for_saved_encryption_key` / `FOSSIL_SEE_PID_KEY`) was introduced; not bisected further back.

## Root cause

`src/db.c`:

1. `cmd_webserver()` (`src/main.c`, shared entry point for both the `server` and `ui` subcommands) calls `db_setup_for_saved_encryption_key()` unconditionally at startup when `USE_SEE` is compiled in:

   ```c
   void db_setup_for_saved_encryption_key(){
     ...
     p = fossil_secure_alloc_page(&n);   /* zeroed mlock'd page */
     ...
     zSavedKey = p;
     savedKeySize = n;
   }
   ```

   This pre-allocates a **zeroed** secure page and points the module-static `zSavedKey` at it, so that once a real key is obtained it can be written into non-pageable memory and inherited by forked request-children. At this point `zSavedKey` is non-NULL, but the buffer it points to is all zero bytes — no key has been obtained yet.

2. Fossil already ships the correct check for "is there really a key here yet":

   ```c
   int db_is_valid_saved_encryption_key(const char *p, size_t n){
     if( p==0 ) return 0;
     if( n==0 ) return 0;
     if( p[0]==0 ) return 0;   /* <-- correctly rejects the zeroed page */
     return 1;
   }
   int db_have_saved_encryption_key(){
     return db_is_valid_saved_encryption_key(zSavedKey, savedKeySize);
   }
   ```

   Called immediately after step 1, `db_have_saved_encryption_key()` correctly returns false, because `zSavedKey[0]==0`.

3. But `db_maybe_obtain_encryption_key()` — the function responsible for deciding whether to prompt for a key — does not use that validator. It uses the raw accessor and a pointer-truthiness check:

   ```c
   static void db_maybe_obtain_encryption_key(
     const char *zDbFile,
     Blob *pKey
   ){
   #if USE_SEE
     if( sqlite3_strglob("*.efossil", zDbFile)==0 ){
       char *zKey = db_get_saved_encryption_key();  /* <-- just returns the pointer */
       if( zKey ){                                  /* <-- non-NULL, so this is true */
         blob_set(pKey, zKey);                       /* <-- sets an EMPTY key (zKey[0]==0) */
       }else{
         char *zPrompt = mprintf("\rencryption key for '%s': ", zDbFile);
         prompt_for_password(zPrompt, pKey, 0);
         ...
       }
     }
   #endif
   }
   ```

   `db_get_saved_encryption_key()` just returns `zSavedKey` — no validity check. Since the pointer is non-NULL (it's the zeroed page from step 1), `if( zKey )` is true, the function believes a key is already saved, sets the key blob to an empty string, and **skips the prompt entirely**. Every subsequent repository open then fails to unlock with the (empty) key: `SQLITE_NOTADB`.

## Suggested fix

Use the validator that already exists for exactly this check:

```diff
--- a/src/db.c
+++ b/src/db.c
@@ -2216,7 +2216,8 @@
 ){
 #if USE_SEE
   if( sqlite3_strglob("*.efossil", zDbFile)==0 ){
-    char *zKey = db_get_saved_encryption_key();
+    char *zKey = db_have_saved_encryption_key()
+                   ? db_get_saved_encryption_key() : 0;
     if( zKey ){
       blob_set(pKey, zKey);
     }else{
```

(Line numbers approximate; the function is `db_maybe_obtain_encryption_key` in `src/db.c`, immediately after `db_maybe_handle_saved_encryption_key_for_process`.)

With this change, on first repository open the prompt/key-acquisition path runs as normal, obtains the real key, and (via the existing `db_set_saved_encryption_key()` call already present later in the function) writes it into the pre-allocated secure page — which is exactly what that page was allocated for, so forked request-children still inherit it as intended. Only the initial "is a key already saved" test was wrong.

## Reproduction

1. Build Fossil with `./configure --with-see=1 ...` against any SQLite build that supports the `PRAGMA key` encryption extension.
2. Create an encrypted repository (`*.efossil` naming, per `sqlite3_strglob("*.efossil", zDbFile)` in the code above) and confirm it opens correctly with `fossil open`/normal CLI commands (key prompt works fine outside server mode, since `db_setup_for_saved_encryption_key` is only called from `cmd_webserver`).
3. Run `fossil server <repo>.efossil` (or `fossil ui <repo>.efossil`) and request any page, or `fossil server --create ...` and try to sync against it.
4. Observe: request fails, server logs/reports the repository as not a valid SQLite database (`SQLITE_NOTADB`) — no key prompt ever occurs, even though the same repository opens fine via any non-server command.

## Impact

Total: `fossil server` and `fossil ui` cannot serve **any** SEE-encrypted repository at all, on any platform, in any released version carrying this code shape. Since the failure mode ("not a valid repository") gives no hint that the key-acquisition path was silently skipped, this is easy to misdiagnose as a corrupted repository or wrong key rather than a never-attempted key lookup.

## Further observations

Checked all five call sites of `db_get_saved_encryption_key()` in the tree (`src/db.c` ×2, `src/main.c`, `src/sqlcmd.c`, `src/winhttp.c`) while preparing this report. Two related points, offered as secondary suggestions rather than part of the primary fix above:

1. **The getter's own doc comment already promises the validated behavior.** `db_get_saved_encryption_key()` is documented as "returns the saved database encryption key -OR- zero if no database encryption key is saved," but the implementation is just `return zSavedKey;` — it does not honor that contract; it returns the zeroed placeholder pointer too. Pushing the `db_have_saved_encryption_key()` check into the getter itself (`return db_have_saved_encryption_key() ? zSavedKey : 0;`) would make the implementation match its documented behavior, and is arguably the more correct fix at the API level.

   Two of the five call sites already work around the getter's actual (undocumented) behavior by hand: `src/sqlcmd.c:293` (`fossil_key`) and `src/winhttp.c:626` both call the getter and then separately check `db_is_valid_saved_encryption_key(p, n)` before trusting the result. That's the established idiom elsewhere in the file, which is why the primary fix above follows the same shape at the one call site that omitted it, rather than changing the getter itself.

   The reason we didn't fold the getter change into the primary fix: `src/db.c:1924` (`db_write_saved_encryption_key_to_process`) calls the getter and immediately does `assert(p!=NULL); assert(n==pageSize);` — it wants the raw allocated-buffer pointer, not a validated key, since its job is handing the buffer address to a child process. We did not fully trace whether this function is ever reachable before a real key has been written into that buffer (i.e., before `db_have_saved_encryption_key()` would be true). If it is, changing the getter to return NULL in that state would turn today's silent empty-key bug into a hard assertion failure there instead — a different failure mode, not obviously an improvement, and one this report can't rule out without reading `db_maybe_handle_saved_encryption_key_for_process` end to end. Worth checking before deciding whether to push the fix down into the getter instead of (or in addition to) the call site above.

2. **A second, lower-severity instance of the same bug shape.** `src/main.c:2831`, in `test_pid_page()` (an admin-gated diagnostic webpage reporting the current pid/key-address/key-size for `--usepidkey` testing), has the same incomplete check: `if( zSavedKey!=0 && savedKeySize>0 )` — missing the `p[0]==0` test that `db_is_valid_saved_encryption_key()` performs. It never unlocks a database (it only prints diagnostic info to a `Setup`-permission-gated page), so the impact is much smaller than the primary bug, but it's the identical reasoning error and worth a one-line fix in the same pass: use `db_have_saved_encryption_key()` there too, instead of the hand-rolled `!=0 && >0` check.

## Verification provenance

- **Empirical** (build it, run `fossil server` against an encrypted repo, observe `SQLITE_NOTADB`, apply the patch, observe success): done in the session that produced `fossil-server-key-validator.patch` in the pizza-party-vote-fossil repo (commit `e93dcd9` there), against the `fossil-app` project's encrypted-hub deployment. Verified end-to-end: encrypted `fossil server` + stock-client clone/sync.
- **Static** (source-reading confirmation of the exact logic above, against the pinned Fossil 2.28 checkout in `vendor/fossil`): done independently in a later session while drafting this report. Corroborates the empirical result; did not re-run the build.
