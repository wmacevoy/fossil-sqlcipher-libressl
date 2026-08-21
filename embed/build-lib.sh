#!/bin/sh
# build-lib.sh -- build libfossilsee (shared + static) and its test.
#
# Prerequisite: ../build/build.sh has already run, so vendor/fossil/bld/
# holds the compiled objects and vendor/libressl-build-out/ the TLS libs.
# This script deliberately reuses the EXACT flags that build produced,
# read from vendor/fossil/Makefile, rather than restating them -- a second
# copy of the flag list is a thing that silently drifts.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
BLD="$ROOT/vendor/fossil/bld"
MK="$ROOT/vendor/fossil/Makefile"
OUT="$ROOT/build/dist"

[ -d "$BLD" ] || { echo "build-lib: no $BLD -- run build/build.sh first" >&2; exit 1; }
ls "$BLD"/*.o >/dev/null 2>&1 || { echo "build-lib: no objects in $BLD -- run build/build.sh first" >&2; exit 1; }

# Pull the flag lines out of the Makefile the fossil build generated.
TCCFLAGS=$(sed -n 's/^TCCFLAGS[[:space:]]*=[[:space:]]*//p' "$MK")
CFLAGS_=$(sed -n 's/^CFLAGS[[:space:]]*=[[:space:]]*//p' "$MK")
LIB=$(sed -n 's/^LIB[[:space:]]*=[[:space:]]*//p' "$MK")
# TCCFLAGS references $(CFLAGS); expand it.
TCCFLAGS=$(printf '%s' "$TCCFLAGS" | sed "s|\\\$(CFLAGS)|$CFLAGS_|")

INC="-I$BLD -I$ROOT/vendor/fossil -I$ROOT/vendor/fossil/src -I$ROOT/vendor/fossil/extsrc -I$HERE"

# This library's own objects go in their OWN directory, never into $BLD.
# bld/ is the fossil build's output; dropping an extra main_h.o in there
# risks it being picked up by a later fossil link (duplicate main), and a
# build system you have to reason about to stay safe is one you will
# eventually get wrong.
LIBOBJ="$ROOT/build/libobj"
rm -rf "$LIBOBJ"; mkdir -p "$LIBOBJ" "$OUT"
cd "$BLD"

# main_.c holds fossil's main(); recompile it with main renamed so the
# library can link every other object without owning the entry point.
# -Dmain=... rather than objcopy --redefine-sym: objcopy is GNU binutils,
# absent on macOS (see harness.c's note) -- a #define needs only the
# compiler already required to get this far.
echo "build-lib: compiling main_.c with renamed entry point"
# shellcheck disable=SC2086
cc $TCCFLAGS $INC -Dmain=fossil_cli_main -o "$LIBOBJ/main_h.o" -c main_.c

echo "build-lib: compiling fossilsee.c"
# shellcheck disable=SC2086
cc $TCCFLAGS $INC -o "$LIBOBJ/fossilsee.o" -c "$HERE/fossilsee.c"

# every fossil object EXCEPT the one carrying main()
OBJS=$(ls *.o | grep -vE '^main\.o$' | tr '\n' ' ')

echo "build-lib: linking static libfossilsee.a"
ar rcs "$OUT/libfossilsee.a" "$LIBOBJ/fossilsee.o" "$LIBOBJ/main_h.o" $OBJS

case "$(uname -s)" in
  Darwin) SOEXT=dylib; SOFLAG="-dynamiclib"; DL_LIB="" ;;
  # glibc needs -ldl for dlopen; it is inside libSystem on macOS, where
  # naming it fails the link outright.
  *)      SOEXT=so;    SOFLAG="-shared";     DL_LIB="-ldl" ;;
esac

echo "build-lib: linking shared libfossilsee.$SOEXT"
# shellcheck disable=SC2086
cc $SOFLAG -o "$OUT/libfossilsee.$SOEXT" "$LIBOBJ/fossilsee.o" "$LIBOBJ/main_h.o" $OBJS $LIB -lpthread -lm

echo "build-lib: building check-dlopen (RUNTIME loading, which is how the"
echo "           sibling viki project actually consumes this library)"
cc -O2 -I"$HERE" -o "$OUT/check-dlopen" "$HERE/check-dlopen.c" $DL_LIB

echo "build-lib: building test-fossilsee (links the SHARED library, so a"
echo "           missing/incompatible .$SOEXT fails loudly here, not later)"
# shellcheck disable=SC2086
cc $TCCFLAGS -I"$HERE" -o "$OUT/test-fossilsee" "$HERE/test-fossilsee.c" \
   "$OUT/libfossilsee.$SOEXT"

echo "build-lib: done ->"
ls -la "$OUT/libfossilsee."* "$OUT/test-fossilsee"
