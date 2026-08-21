/*
** check-dlopen.c -- proves libfossilsee can be LOADED AT RUNTIME and its
** entry points resolved by name.
**
** This is deliberately not an `nm` grep. Two reasons, one of which was
** learned the hard way:
**
**  1. It tests the access pattern that actually matters. The sibling
**     `viki` project never links this library -- it dlopen()s it and
**     dlsym()s exactly these five names, refusing to call anything until
**     fossilsee_abi() agrees with the ABI it compiled against. A library
**     that links fine but cannot be dlopen()ed is useless to it.
**  2. Parsing `nm` output is toolchain-dependent. The first version of
**     this check did that, passed locally on macOS, and failed on the
**     GitHub macOS runner at the FIRST symbol -- with stderr discarded,
**     so the log said only "does not export fossilsee_abi" and could not
**     say why. dlopen/dlsym have real error strings and no output format
**     to parse.
**
** Usage: check-dlopen <path-to-libfossilsee.{so,dylib}>
*/
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

static const char *kSyms[] = {
    "fossilsee_abi", "fossilsee_open", "fossilsee_close",
    "fossilsee_sql", "fossilsee_errmsg", 0
};

int main(int argc, char **argv){
    void *h;
    int i, nBad = 0;
    int (*pAbi)(void);

    if( argc < 2 ){
        fprintf(stderr, "usage: check-dlopen <library>\n");
        return 2;
    }
    h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if( !h ){
        printf("FAIL dlopen(%s): %s\n", argv[1], dlerror());
        return 1;
    }
    printf("ok   dlopen(%s)\n", argv[1]);

    for(i=0; kSyms[i]; i++){
        void *p;
        dlerror();                      /* clear any stale error */
        p = dlsym(h, kSyms[i]);
        if( !p ){
            printf("FAIL dlsym(%s): %s\n", kSyms[i], dlerror());
            nBad++;
        }else{
            printf("ok   dlsym(%s)\n", kSyms[i]);
        }
    }

    /* Resolving the name is not enough: a caller compiled against a
    ** different header would load happily and then misread every struct.
    ** fossilsee_abi() is the guard, so check it REPORTS something usable
    ** rather than merely existing. */
    pAbi = (int(*)(void))dlsym(h, "fossilsee_abi");
    if( pAbi ){
        int v = pAbi();
        if( v > 0 ){
            printf("ok   fossilsee_abi() = %d\n", v);
        }else{
            printf("FAIL fossilsee_abi() returned %d (must be positive)\n", v);
            nBad++;
        }
    }

    dlclose(h);
    printf("\n%d symbol check(s) failed\n", nBad);
    return nBad ? 1 : 0;
}
