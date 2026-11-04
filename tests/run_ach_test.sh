#!/bin/sh
# Builds and runs the RetroAchievements module test (tests/ach_test.c). Needs the network and
# rom/StarTropics (USA).nes. On Windows run it from the MSYS2 MinGW64 shell:
#   MSYSTEM=MINGW64 CHERE_INVOKING=1 /c/msys64/usr/bin/bash -lc 'cd /path/to/startropics-recomp && tests/run_ach_test.sh'
set -e
cd "$(dirname "$0")/.."
B=build-ach
mkdir -p $B/rt $B/rc
CC=${CC:-gcc}
RC_FLAGS="-Ithird_party/rcheevos/include -Ithird_party/rcheevos/src -DRC_CLIENT_SUPPORTS_HASH -DRC_HASH_NO_DISC -DRC_HASH_NO_ENCRYPTED -DRC_HASH_NO_ZIP"
HTTP_FLAGS=
case "$(uname -s)" in
  MINGW*|MSYS*) HTTP_LIBS="-lwinhttp -mconsole" ;;
  *) if pkg-config --exists libcurl 2>/dev/null; then HTTP_FLAGS="-DHTTP_CURL $(pkg-config --cflags libcurl)"; HTTP_LIBS="$(pkg-config --libs libcurl) -lpthread"; else HTTP_LIBS="-lpthread"; fi ;;
esac
# the test defines its own main(): drop SDL's -Dmain=SDL_main
SDL_CFLAGS="$(sdl2-config --cflags | sed 's/-Dmain=SDL_main//')"
for f in cpu mem cart mapper ppu apu sys png palette widescreen recomp_stub; do
    $CC -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Iruntime -c runtime/$f.c -o $B/rt/$f.o
done
for f in third_party/rcheevos/src/api/*.c third_party/rcheevos/src/client/*.c third_party/rcheevos/src/hash/*.c \
         third_party/rcheevos/src/runtime/*.c third_party/rcheevos/src/util/*.c; do
    $CC -O2 -std=gnu11 -w $RC_FLAGS -c "$f" -o $B/rc/$(basename "$f" .c).o
done
$CC -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter $SDL_CFLAGS -Iruntime $RC_FLAGS -DACH_TESTING \
    -c frontend/achievements.c -o $B/fe_achievements_test.o
$CC -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter $SDL_CFLAGS $HTTP_FLAGS -c frontend/http.c -o $B/fe_http.o
$CC -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter $SDL_CFLAGS -Iruntime -Ifrontend -Ithird_party/rcheevos/include \
    -c tests/ach_test.c -o $B/ach_test.o
case "$(uname -s)" in
  MINGW*|MSYS*) SDL_LIBS="$(sdl2-config --static-libs)" ;;
  *) SDL_LIBS="$(sdl2-config --libs)" ;;
esac
$CC $B/ach_test.o $B/fe_achievements_test.o $B/fe_http.o $B/rt/*.o $B/rc/*.o -o $B/ach_test $SDL_LIBS $HTTP_LIBS -lm
./$B/ach_test "$@"
