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

## For players: the launcher

`make dist` packages a single executable (SDL2 linked statically, no DLLs) with a
launcher in the style of the N64 recompilation projects: a tabbed menu
(**Play / Graphics / Audio / Controls / Achievements / ROM**) over a hero backdrop. On the ROM tab
"Choose ROM file..." opens the system file dialog; dropping a `.nes` file onto the
window or passing it on the command line also works. The file's hash is checked
and anything that is not a StarTropics dump is refused. Once a valid dump is
loaded the launcher runs the game's own intro, title screen and demo in the
background with sound, so the intro and title music play straight from the ROM
and the dimmed, upscaled title screen becomes the backdrop. Put a
`launcher-background.bmp` next to the executable to use a picture of your own
instead (no artwork is bundled: Nintendo's promotional art cannot be shipped).

Options (all saved to `startropics.ini`): windowed / fullscreen / exclusive
fullscreen, resolution presets (720p, 1280x800, 1080p, 1440p, 2560x1600, 4K: the
window size, the exclusive display mode, or the render size in borderless
fullscreen), window size 1x-6x, scaling (integer / fit window / stretch), aspect
ratio (square pixels 8:7, 4:3 CRT, 16:9 or 16:10 widescreen: below), filter
(nearest / linear / sharp), CRT-style shaders (scanlines, aperture mask, glow,
curvature; three strengths), overscan crop, a no-sprite-limit mode (every sprite
on a line is drawn, so nothing flickers), volume, and rebinding of the eight NES
buttons and the rewind key. Nine save state slots, rewind (hold a key to step
back through the last minutes of play) and RetroAchievements (log in on the
Achievements tab; unlocks pop up in game; hardcore mode supported) are built in.
Esc in game opens the same menu as a pause screen. XInput and other gamepads are handled through SDL's game
controller API (A = A, B or X = B, Start, Back = Select, D-pad or left stick;
the Guide button or LB opens the menu); pads SDL has no mapping for fall back to
a generic joystick layout, and `startropics --list-controllers out.txt` writes
what SDL detected.

Note on distribution: a prebuilt executable necessarily embeds the recompiled
game logic (as the N64 recompilation releases do), while every asset still comes
from the player's own ROM at run time. Building from source as described below
keeps everything derived from the ROM on your own machine.

**16:9 (widescreen)** and **16:10 (widescreen)**, the third and fourth aspect ratios, are real widescreen views, not a stretch: a tile-based
NES game has no picture beyond the 256-pixel edge, so the port decodes the
overworld's map data from your ROM (the screen grid, the run-length coded
metatile rows and the metatile table; `docs/MAP_FORMATS.md`) and draws the
world to the left and right of the normal picture with the same CHR banks,
palette and scroll the PPU used, so there is no seam. The centre stays the
game's own output. Only background is extended: the game simulates nothing
beyond the edge, so no enemies or people appear there. Scenes whose format is
not decoded (dungeons, menus, text screens) show the normal 4:3 picture between
black bars. The pixels keep their 4:3 CRT shape, so 341 of the 448 decoded
pixels are visible (318 with the overscan crop). Nothing is ever stretched: the
Scaling row's "Stretch (distorts)" is the one option that changes the pixel shape.

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
coverage (below) and rebuild. `make dist` zips the executable with the player
notes (`docs/PLAYERS.md`) into `dist/`.

### Controls

| NES | Keyboard | Gamepad (SDL) |
|---|---|---|
| D-pad | Arrow keys | D-pad / left stick |
| A (jump) | X | B (right face button, like the NES layout) |
| B (attack) | Z | A (bottom face button) |
| Start | Enter | Start |
| Select | Right Shift | Back |

Both the keys and the pad buttons are rebindable on the Controls tab (select a
row, press Enter, then the new key or pad button). Every connected controller is
opened and merged, so a pad that Windows exposes twice (Xbox over Bluetooth)
works whichever instance reports input.

### Latency

The port is tuned to feel like real hardware or better; see
[docs/LATENCY.md](docs/LATENCY.md) for the full breakdown. In short: the loop is
phase-locked to the display's vertical blank and reads the controller about
3 ms before each frame is presented (frame delay), the GPU queue is one frame
deep, **run-ahead** (default 2 frames, the game's own reaction time) removes
StarTropics' built-in input lag entirely, and audio sits at 25-45 ms with dynamic
rate control. Exclusive fullscreen bypasses the desktop compositor. All of this is
adjustable on the Graphics and Audio tabs, and `startropics --play --stats
out.txt` logs the measured pacing once a second.

Hotkeys: **Esc** menu, **F11** / Alt+Enter fullscreen, **F2** toggle overscan crop,
**Tab** (hold) fast-forward, **P** pause, **F5** / **F8** save / load state,
**F6** / **F7** previous / next save state slot (nine per ROM), **Backspace** (hold;
pad RB) rewind, **F12** screenshot (`screenshots/`), **Ctrl+R** reset. Keys and
display settings are in the menu and in `startropics.ini` next to the executable.

Battery saves go to `saves/<rom name>.sav` (the raw 1 KiB MMC6 RAM, written when
it changes and on exit); save states to `saves/<rom name>.state1` .. `.state9`.

### Shaders, rewind, achievements

The shader presets are composed from textured render passes on the SDL renderer
(a scanline beam profile stretched over the picture, an aperture-grille mask
multiplied in at screen resolution, a glow added back from a small copy of the
frame, and barrel curvature with vignette drawn through a mesh), so they work on
every backend without a shader language and add nothing to the input path.

Rewind keeps a history of machine states as run-length-coded XOR deltas
(`frontend/rewind.c`): a state is about 24 KB, a delta typically under 1 KB, so
the 32 MB history holds many minutes at 30 snapshots a second. Holding the key
steps back two frames per frame shown, re-running each restored state for its
picture and sound.

RetroAchievements support uses the official
[rcheevos](https://github.com/RetroAchievements/rcheevos) client library,
vendored under `third_party/rcheevos` (MIT licence; the licence text ships with
the package). The ROM is identified by the RetroAchievements hash (the MD5 of
the headerless image; StarTropics USA is game 1985), achievement logic reads the
emulated memory through a side-effect-free peek, HTTP runs on worker threads
(WinHTTP on Windows, libcurl elsewhere when available) and only the session
token is stored. Hardcore mode disables save state loading and rewind.

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
* `frontend/sdl_main.c` is the player: window, audio, input, saves, states,
  rewind, shaders; `frontend/achievements.c` is the RetroAchievements glue.

Details, the timing model and the exact emission rules are in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Testing

```
sh tests/get_test_roms.sh          # clones the public nestest/blargg collection (33 MB)
make headless                      # interpreter-only harness
make test                          # blargg suites through $6000 status reporting
make headless-recomp               # harness with the recompiled code linked in
python tests/cosim.py              # interpreter vs native, 6,500 scripted frames
sh tests/run_ui_bind_test.sh       # menu rebinding and text rows (synthetic events)
gcc -O2 -Ifrontend tests/rewind_test.c frontend/rewind.c -o build/rewind_test && build/rewind_test
sh tests/run_ach_test.sh           # RetroAchievements module against the live server (network)
build/startropics rom/*.nes --rewind-test 1200   # whole rewind path: every re-run frame must match
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
frontend/      SDL2 player (menu, video, audio, input, rewind, achievements)
third_party/   rcheevos (RetroAchievements client, MIT) and its make include
tests/         accuracy tests, cosimulation, nestest comparison, module tests
docs/          architecture notes
generated/     output of make recomp (git-ignored)
rom/           your .nes file (git-ignored)
```
