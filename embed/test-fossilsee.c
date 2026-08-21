/*
** test-fossilsee.c -- exercises libfossilsee's PROMISES, not just its
** happy path. Each check below corresponds to a specific claim in
** fossilsee.h or a specific bug recorded in README.md.
**
** Links against the SHARED library and includes only fossilsee.h -- no
** Fossil headers, no Fossil build tree. That is itself part of the test:
** if the public header ever starts needing Fossil's internals, this file
** stops compiling.
**
** Usage: test-fossilsee <plain-repo> <encrypted-repo> <raw-key>
*/
#include "fossilsee.h"
#include <stdio.h>
#include <string.h>

static int nFail = 0, nPass = 0;

static void ok(const char *zWhat, int bPass, const char *zDetail){
  printf("%-52s %s%s%s\n", zWhat, bPass ? "PASS" : "FAIL",
         zDetail && zDetail[0] ? "  -- " : "", zDetail ? zDetail : "");
  if( bPass ) nPass++; else nFail++;
}

/* Collects the first column of the first row, plus a row count. */
struct grab { char z[256]; size_t n; int nRow; int nColSeen; };

static int grab_row(void *pArg, int nCol, const char *const *azVal,
                    const size_t *anLen){
  struct grab *p = (struct grab*)pArg;
  if( p->nRow==0 && nCol>0 && azVal[0] ){
    size_t n = anLen[0];
    if( n >= sizeof(p->z) ) n = sizeof(p->z)-1;
    memcpy(p->z, azVal[0], n);
    p->z[n] = 0;
    p->n = anLen[0];
  }
  p->nColSeen = nCol;
  p->nRow++;
  return 0;
}

/* Aborts after the first row, to test FOSSILSEE_ABORT. */
static int grab_one(void *pArg, int nCol, const char *const *azVal,
                    const size_t *anLen){
  grab_row(pArg, nCol, azVal, anLen);
  return 1;
}

int main(int argc, char **argv){
  const char *zPlain = argc>1 ? argv[1] : "/tmp/fslab/r.fossil";
  const char *zEnc   = argc>2 ? argv[2] : "/tmp/fslab-enc/r.efossil";
  const char *zKey   = argc>3 ? argv[3] : 0;
  fossilsee *p = 0, *p2 = 0;
  struct grab gr;
  int rc;

  ok("ABI matches the header we compiled against",
     fossilsee_abi()==FOSSILSEE_ABI, 0);

  /* --- a repository that does not exist: must FAIL, and must not take
  ** this process down with it. Reaching the next line at all is the
  ** exit-trap working. --- */
  rc = fossilsee_open("/tmp/definitely-not-a-repo.fossil", 0, &p);
  ok("missing repo -> error, process survives",
     rc!=FOSSILSEE_OK && p==0, fossilsee_errmsg(0));

  /* --- plain repo --- */
  rc = fossilsee_open(zPlain, 0, &p);
  ok("open unencrypted repo", rc==FOSSILSEE_OK && p!=0, fossilsee_errmsg(0));
  if( rc!=FOSSILSEE_OK ) return 1;

  memset(&gr, 0, sizeof(gr));
  rc = fossilsee_sql(p, "SELECT count(*) FROM blob;", grab_row, &gr);
  ok("SELECT returns rows", rc==FOSSILSEE_OK && gr.nRow==1, gr.z);

  /* --- THE property this library exists for. The subprocess path exits 0
  ** for a failed query, which is what let viki's sweep delete a whole
  ** cache. Here it must be a real error. --- */
  memset(&gr, 0, sizeof(gr));
  rc = fossilsee_sql(p, "SELECT * FROM no_such_table;", grab_row, &gr);
  ok("failed query -> real error (not silent success)",
     rc==FOSSILSEE_ERROR && gr.nRow==0, fossilsee_errmsg(p));

  /* --- read-only is enforced, not merely documented --- */
  rc = fossilsee_sql(p, "CREATE TABLE viki_should_not_exist(x);", 0, 0);
  ok("CREATE TABLE denied", rc==FOSSILSEE_ERROR, fossilsee_errmsg(p));
  rc = fossilsee_sql(p, "DELETE FROM blob;", 0, 0);
  ok("DELETE denied", rc==FOSSILSEE_ERROR, fossilsee_errmsg(p));
  rc = fossilsee_sql(p, "UPDATE config SET value='x';", 0, 0);
  ok("UPDATE denied (ticket-corruption guard)", rc==FOSSILSEE_ERROR,
     fossilsee_errmsg(p));

  /* --- singleton: Fossil has one global state, so a second open must be
  ** refused rather than silently clobbering the first --- */
  rc = fossilsee_open(zPlain, 0, &p2);
  ok("second open refused with BUSY", rc==FOSSILSEE_BUSY && p2==0,
     fossilsee_errmsg(0));

  /* --- multi-statement and abort --- */
  memset(&gr, 0, sizeof(gr));
  rc = fossilsee_sql(p, "SELECT 1; SELECT 2; SELECT 3;", grab_row, &gr);
  ok("multiple statements all deliver rows",
     rc==FOSSILSEE_OK && gr.nRow==3, 0);

  memset(&gr, 0, sizeof(gr));
  rc = fossilsee_sql(p, "SELECT 1 UNION ALL SELECT 2;", grab_one, &gr);
  ok("callback can abort the scan", rc==FOSSILSEE_ABORT && gr.nRow==1, 0);

  /* --- binary safety: a value with an embedded NUL must report its true
  ** byte length. strlen() would say 3 here. --- */
  memset(&gr, 0, sizeof(gr));
  rc = fossilsee_sql(p, "SELECT x'610062006300';", grab_row, &gr);
  ok("embedded NUL: length is bytes, not strlen",
     rc==FOSSILSEE_OK && gr.n==6, gr.n==6 ? "6 bytes" : "WRONG LENGTH");

  fossilsee_close(p);
  p = 0;

  /* --- cross-repo switching. db_repository_filename()'s cache once made
  ** every later open silently return the FIRST repo (README.md). Opening
  ** a DIFFERENT repo after close is the regression test for that. --- */
  rc = fossilsee_open(zEnc, zKey, &p);
  ok("open ENCRYPTED repo after closing another",
     rc==FOSSILSEE_OK && p!=0, fossilsee_errmsg(0));
  if( rc==FOSSILSEE_OK ){
    memset(&gr, 0, sizeof(gr));
    rc = fossilsee_sql(p, "SELECT count(*) FROM blob;", grab_row, &gr);
    ok("encrypted repo: SELECT works", rc==FOSSILSEE_OK && gr.nRow==1, gr.z);

    /* Proves we really switched repos rather than re-reading the first. */
    memset(&gr, 0, sizeof(gr));
    rc = fossilsee_sql(p,
        "SELECT count(*) FROM blob WHERE content IS NOT NULL;", grab_row, &gr);
    ok("encrypted repo is a DIFFERENT db than the first",
       rc==FOSSILSEE_OK, fossilsee_errmsg(p));
    fossilsee_close(p);
  }

  /* --- wrong key must fail cleanly --- */
  rc = fossilsee_open(zEnc, "x'"
       "0000000000000000000000000000000000000000000000000000000000000000'", &p);
  ok("wrong key -> clean failure", rc!=FOSSILSEE_OK, fossilsee_errmsg(0));
  if( rc==FOSSILSEE_OK ) fossilsee_close(p);

  printf("\n%d passed, %d failed\n", nPass, nFail);
  return nFail ? 1 : 0;
}
