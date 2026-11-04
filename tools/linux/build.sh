#!/bin/sh
# tools/linux/build.sh - builds and tests the Linux and Steam Deck packages. Run it inside the
# container made from tools/linux/Dockerfile (see there). Needs generated/ (make recomp);
# the tests also need your cartridge dump in rom/.
set -e
cd "$(dirname "$0")/../.."
J=$(nproc)
M="make -j$J PYTHON=python3 SDL_STATIC=1"

$M headless headless-recomp
$M BUILD=build-linux startropics
$M BUILD=build-deck DECK_EDITION=1 startropics

ROM=$(ls rom/*.nes 2>/dev/null | head -n 1 || true)
if [ -n "$ROM" ]; then
    echo "== cosimulation (interpreter vs native code)"
    python3 tests/cosim.py --frames 3000
    # no display in a container: SDL's offscreen video and dummy audio drivers
    export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy
    for b in build-linux build-deck; do
        echo "== $b self-tests"
        ./$b/startropics "$ROM" --selftest 300 "/tmp/st-$b.png" | grep selftest
        ./$b/startropics --trigger-test | tail -n 1
        ./$b/startropics --touch-test | tail -n 1
        ./$b/startropics "$ROM" --chord-test | tail -n 1
        ./$b/startropics "$ROM" --rewind-test 1200 | grep rewind-test
        ./$b/startropics "$ROM" --ui-shot "/tmp/ui-$b.png" 1 60 | grep ui-shot
        grep -E "^(display_mode|aspect|scaling|resolution)=" $b/startropics.ini | tr '\n' ' '; echo
        sed -i 's/^textures=.*/textures=1/' $b/startropics.ini   # the remastered graphics
        ./$b/startropics "$ROM" --game-shot "/tmp/tex-$b.png" 900 | grep game-shot
    done
fi

echo "== shared libraries the binary needs"
ldd build-linux/startropics
echo "== newest glibc symbol version it needs"
objdump -T build-linux/startropics | grep -o 'GLIBC_[0-9.]*' | sort -V | uniq | tail -n 1

rm -f build-linux/startropics.ini build-deck/startropics.ini
rm -rf build-linux/saves build-deck/saves
$M BUILD=build-linux dist
$M BUILD=build-deck DECK_EDITION=1 dist
ls -l dist/
