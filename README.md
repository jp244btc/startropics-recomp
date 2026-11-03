# StarTropics native PC port (static recompilation)

A native Windows/Linux/macOS build of Nintendo's *StarTropics* (NES, 1990),
produced by **statically recompiling your own cartridge dump** into C and
linking it against a small hardware runtime (PPU, APU, MMC6 mapper, input).

This repository contains **no game data and no game code**. The recompiler,
the runtime and the frontend are original work (MIT licence). The game's
6502 program, graphics, music, text and level data all come from the `.nes`
file you supply from a cartridge you own; the C code generated from it lives
in `generated/`, which git ignores, and the executable loads the ROM file again
at start-up for the assets. Without your dump nothing here runs.

```
your StarTropics (USA).nes ──► recomp/recomp.py ──► generated/*.c ─┐
                                                                    ├─► gcc ─► build/startropics
runtime/ (PPU, APU, MMC6, CPU fallback) + frontend/ (SDL2) ────────┘
                 ▲
your .nes again, at run time, for CHR graphics, level data, text, music
```

## What works

Verified in this session (headless, scripted input, screenshots inspected):

* boot, intro cinematic, title screen, attract mode (island map)
* file select, name registration (letter grid, END, slot select), battery-backed MMC6 save RAM
* chapter 1 prelude text, arrival on C-Island, overworld walking and scrolling in all directions
* the recompiled code is **bit-identical** to the reference interpreter over a 6,500-frame
  session (14.6 million instructions; CPU registers and cycle counts, RAM, save RAM,
  framebuffer, OAM, palettes, nametables and bank registers compared every frame)
* the interpreter passes nestest (all 8,991 instructions incl. unofficial opcodes and
  exact cycle counts) and 44 of the 63 blargg accuracy test ROMs; the failures are
  dot-exact NMI latency, PPU open-bus decay and exotic unofficial-opcode tests that
  this game does not depend on (see `docs/ARCHITECTURE.md`)

Not yet verified by the author: dungeon (action) scenes with the IRQ-driven status
bar, boss fights, later chapters, and listening to the audio output (the APU passes
blargg's length/sweep/envelope/frame-counter/DMC-rate tests and writes sane WAVs).
Any code the static analysis did not reach still runs correctly through the
built-in interpreter fallback, so missing coverage costs speed, never correctness.

## Legal note

StarTropics is a trademark and copyright of Nintendo. You need your own legally
obtained cartridge dump; the project does not include, download or write one.
Everything derived from your ROM (the generated C, disassembly listings, builds,
saves) stays on your machine. Do not redistribute `generated/` or the built
executable, since those contain the recompiled game.

## Building

Requirements: a C11 compiler (gcc/clang), GNU make, Python 3, SDL2 (frontend only).

**Windows (MSYS2)**: install [MSYS2](https://www.msys2.org), open the *MINGW64* shell and

```
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL2 make python
```

**Linux**: `sudo apt install build-essential python3 libsdl2-dev` (or your distro's equivalent).

Then:

```
git clone <this repo> startropics-recomp
cd startropics-recomp
cp "/path/to/StarTropics (USA).nes" rom/      # your own dump
make recomp            # disassemble + translate your ROM -> generated/*.c  (a few seconds)
make -j8 startropics   # compile ~60k lines of generated C + runtime + frontend (~20 s)
build/startropics      # looks for rom/*.nes next to the executable; or pass a path
```

`make recomp` prints how many instructions and routines it found (about 24,200
instructions in 430 routines for the USA dump from static analysis alone, a few
hundred more with coverage data). Run `make recomp` again after collecting more
coverage (below) and rebuild.

### Controls

| NES | Keyboard | Gamepad (SDL) |
|---|---|---|
| D-pad | Arrow keys | D-pad / left stick |
| A | X | A |
| B | Z | B or X |
| Start | Enter | Start |
| Select | Right Shift | Back |

Hotkeys: **F11** / Alt+Enter fullscreen, **F2** toggle overscan crop, **Tab** (hold)
fast-forward, **P** pause, **F5** / **F8** save / load state, **F12** screenshot
(`screenshots/`), **Ctrl+R** reset, **Esc** quit. Keys and the window scale can be
changed in `startropics.ini`, written next to the executable on first run.

Battery saves go to `saves/<rom name>.sav` (the raw 1 KiB MMC6 RAM, written when
it changes and on exit).

## How it works

* `recomp/recomp.py` parses the iNES header, finds code by recursive descent from
  the reset/NMI/IRQ vectors (optionally seeded with execution coverage from the
  headless harness), works out which 8 KiB bank is mapped where (StarTropics uses
  MMC6 PRG mode 1: bank 30 fixed at $8000, bank 31 fixed at $E000, swappable
  banks at $A000/$C000), and emits one C file per bank with one function per
  routine. Every 6502 instruction becomes a `case` label, so execution can resume
  at any instruction after an interrupt, a bank switch or a cross-bank call.
* `runtime/` is the hardware: a dot-accurate PPU whose fetch addresses drive the
  MMC6 scanline IRQ counter, a cycle-stepped APU with the NES mixer and output
  filters, the MMC6 mapper with its 1 KiB protected RAM, and a 6502 interpreter
  that doubles as the fallback for untranslated code and as the reference model.
  The generated code and the interpreter share the same inline helpers, which is
  what makes exact cosimulation possible.
* `frontend/sdl_main.c` is the player: window, audio, input, saves, states.

Details, the timing model and the exact emission rules are in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Testing

```
sh tests/get_test_roms.sh          # clones the public nestest/blargg collection (33 MB)
make headless                      # interpreter-only harness
make test                          # blargg suites through $6000 status reporting
make headless-recomp               # harness with the recompiled code linked in
python tests/cosim.py              # interpreter vs native, 6,500 scripted frames
```

The headless harness (`build/nes-headless --help`) can also script input
(`--keys "700:START;710:-"`), dump PNG screenshots every N frames, write WAV
audio, record coverage (`--coverage FILE`), trace every instruction and dump RAM.

### Widening coverage

Play through more of the game in the harness or the frontend with coverage
recording, then regenerate:

```
build/nes-headless --rom rom/*.nes --frames 20000 --input my_session.txt --coverage build/cov.bin
make recomp RECOMP_FLAGS="--coverage build/cov.bin"
make -j8 startropics
```

Running the native harness with `--recomp --coverage build/cov.bin` records every
instruction that fell back to the interpreter, so repeating the cycle converges.

## Repository layout

```
recomp/        the static recompiler (Python) and symbols.txt (address labels, no game data)
runtime/       hardware runtime + headless harness (C11)
frontend/      SDL2 player
tests/         accuracy tests, cosimulation, nestest comparison
docs/          architecture notes
generated/     output of make recomp (git-ignored)
rom/           your .nes file (git-ignored)
```
