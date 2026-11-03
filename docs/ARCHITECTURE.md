# Architecture

## Goals and constraints

1. The repository ships no game data and nothing derived from the ROM.
   Everything that comes out of the user's cartridge dump (the recompiled C,
   disassembly listings, the built executable) is generated locally and ignored
   by git.
2. The port must be *correct first*: anything the recompiler cannot translate
   must still run, and the translated code must be provably equivalent to a
   reference interpreter.
3. Ordinary tooling only: C11, GNU make, Python 3, SDL2.

## Components

```
runtime/nes.h        shared state (cpu, ppu, apu, mapper, cart, sys) + inline ALU/memory helpers
runtime/cpu.c        6502 interpreter (fallback + reference model)
runtime/ppu.c        dot-based PPU, sprite evaluation, A12 notification to the mapper
runtime/apu.c        pulse x2, triangle, noise, DMC, frame counter, mixer, output filters
runtime/mapper.c     NROM, MMC1 (for test ROMs), MMC3/MMC6 with scanline IRQ and RAM protection
runtime/mem.c        CPU bus: PPU/APU/controller registers, OAM DMA, PRG RAM
runtime/cart.c       iNES / NES 2.0 loader, SHA-1/CRC32, MMC6 detection, battery file
runtime/sys.c        power/reset, frame loop, save states
runtime/headless.c   command-line harness (tests, scripting, screenshots, traces, coverage)
runtime/recomp_rt.h  helpers used by the generated code
recomp/recomp.py     the static recompiler
frontend/sdl_main.c  SDL2 player
```

## Timing model

The CPU is the master clock. Each instruction adds its base cycle count up
front (page-crossing penalties are added when the dummy read happens) and then
calls `sys_sync()`, which runs the PPU to `cpu.cycles * 3` dots and the APU to
`cpu.cycles`, then polls interrupts. Register accesses (`$2000-$2007`,
`$4000-$4017`) catch the PPU/APU up before they execute, so a read of `$2002`
sees the flag state at the end of the instruction, which is when the real bus
read happens.

Interrupts: NMI is edge-latched by the PPU (vblank flag AND enable), IRQ is a
level mask with three sources (mapper, APU frame, DMC). An edge or assertion
that lands on the last cycle of an instruction is only seen by the next one,
and CLI/SEI/PLP poll with the *previous* I flag, which is what makes blargg's
`cli_latency` pass. The `$2002` read race with the vblank flag is modelled
(read one dot early suppresses the flag, read on/after the set dot suppresses
the NMI).

The frame loop in `sys.c` runs instructions until the PPU reports the start of
scanline 240. In native mode `sys_sync()` also returns nonzero at that point so
the generated routine returns to the dispatcher at the same instruction
boundary the interpreter would stop at; this keeps the per-frame state hashes
comparable.

## PPU and the MMC6 IRQ

The PPU is stepped dot by dot with the standard fetch schedule (nametable,
attribute, pattern low/high every 8 dots during dots 1-256 and 321-336, sprite
fetches during 257-320, dummy nametable fetches at 338/340, odd-frame dot skip).
Every fetch reports its address to `mapper_ppu_addr()`, and `$2006`/`$2007`
accesses do too, so the MMC3/MMC6 IRQ counter clocks on real A12 rising edges
with the usual filter (A12 must have been low for at least 10 dots). This
passes blargg's `1-clocking`, `2-details`, `3-A12_clocking` and `5-MMC3` tests.
StarTropics enables the IRQ only in scenes with a split (`STA $E000,X` with X
= 0/1 at `$864E`), using it as the scheduler for the per-scene task through the
vector at `$01CC`.

MMC6 specifics (NES 2.0 mapper 4 submapper 1, also selected by hash for
old-style headers): 1 KiB of battery RAM at `$7000-$73FF` mirrored through
`$7FFF`, enabled by `$8000` bit 5, with `$A001` bits 4/5 (write/read, first
half) and 6/7 (second half). The game writes `$F0` to open both halves while
saving and `$30` or `$B0` the rest of the time.

## Recompiler

### Discovery

* Fixed banks (`$8000` = second-to-last bank, `$E000` = last bank in PRG mode
  1; `$C000`/`$E000` in mode 0) are seeded from the three vectors.
* Swappable banks get a unit per base address they are executed at. The base is
  taken from coverage data when available, otherwise by counting which window
  the bank's own JSR/JMP operands point into.
* Recursive descent follows fallthrough, branches, JMP abs and the return path
  of JSR. Targets in swappable windows cannot be resolved statically (any bank
  could be mapped there), so they become candidate roots for *every* unit at
  that base. Decoding data as code this way is harmless: the extra routines are
  never entered unless the game really jumps there, in which case they are
  exactly what the CPU would execute.
* Coverage files (`--coverage`) are one byte per PRG byte, bit *k* set when the
  byte started an instruction while mapped in slot *k* (`$8000 + k*$2000`).
  The headless harness writes them, and in native mode it records only the
  instructions that fell back to the interpreter, so the loop converges.

### Emission

* One C file per (bank, base) unit; one `static void` function per routine. A
  routine is a connected component of the intra-unit control-flow graph
  (fallthrough, branches, JMP abs), capped at `--max-routine` instructions so
  compilers stay fast.
* Every instruction is `case 0xADDR: L_ADDR:` inside the routine's `switch
  (cpu.pc)`, followed by the translation and `SYNC(next)`. Branches and in-
  routine JMPs become `goto`; everything else that leaves the routine (JSR,
  RTS, RTI, JMP (ind), BRK, cross-routine jumps, interrupts, frame end) sets
  `cpu.pc` and returns to the dispatcher.
* The emitted operand code mirrors the interpreter: dummy reads on indexed page
  crossings, the double write of read-modify-write instructions, zero-page
  wraparound, the JMP (ind) page bug, open-bus-free fast paths when an operand
  range is provably RAM.
* Any store that can reach `$8000-$FFFF` is a mapper write; the routine returns
  to the dispatcher afterwards so a bank switch of its own slot is honoured.
* `recomp_run()` looks up `(bank currently mapped in the slot of cpu.pc, offset)`
  in a per-unit table and calls the routine; a miss (code from RAM, an untranslated
  address, a jammed CPU) returns 0 and the frame loop executes one interpreted
  instruction instead.

### Cosimulation

`tests/cosim.py` runs the same key script through `nes-headless` (interpreter)
and `nes-headless-recomp --recomp` and compares a per-frame FNV-1a hash of CPU
registers + cycle count, RAM, save RAM, framebuffer, OAM, palettes, nametables
and bank registers. On a mismatch, build the tracing variant
(`make BUILD=build-trace RECOMP_DEFS=-DRECOMP_TRACE headless-recomp`) and diff
the per-instruction traces (`--trace FILE --trace-start FRAME [--watch ADDR]`).

## Known inaccuracies

Blargg tests still failing, and why they do not matter for this game:

| test | what it checks |
|---|---|
| cpu_interrupts_v2 2-5 | sub-instruction NMI/IRQ/BRK/DMA interactions |
| ppu_vbl_nmi 05, 07, 08, 10 | NMI timing to the exact PPU dot, odd-frame skip vs. late BG enable |
| apu_test 7-dmc_basics | one DMC buffer-refill detail |
| mmc3_test_2 4, 6 | one scanline-timing sub-test; "alternate" (old revision) MMC3 behaviour |
| cpu_exec_space | executing code from I/O registers |
| ppu_open_bus, ppu_read_buffer | open-bus decay and a late read-buffer sub-test |
| apu_reset 4017_timing, irq_flag_cleared, works_immediately | reset-time APU details |
| instr_misc | the unstable SHA/SHX/SHY/TAS opcodes |

Other simplifications: sprite evaluation is done in one step at dot 257
(overflow flag without the hardware's buggy scan), the PPU warm-up period is
not enforced, DMC DMA does not stall the CPU, and the APU uses a box-filter
resampler plus the 90 Hz / 440 Hz high-pass and 14 kHz low-pass stages.

## Widescreen

`runtime/widescreen.c` draws 96 extra pixels on each side of the frame from the
overworld's own map data (format in `docs/MAP_FORMATS.md`): the scene is
recognised from the game-mode variables, the camera is derived from the player
position and the scroll the PPU latched for the frame (`ppu.frame_t`,
`frame_x`, `frame_ctrl`, `frame_chr_bank`, snapshotted at scanline 0), and each
16 x 16 cell is decoded through the same screen-grid / row / metatile / variant
path the game's strip drawer uses. `ppu.line_emph` now keeps the whole `$2001`
value per line so lines the game blanks stay blank in the bands. The headless
harness (`--widescreen`) checks the decoder against the PPU's nametables and
pixels. The frontend's "16:9 (widescreen)" aspect choice shows this frame; other
scene types (dungeon rooms with the status-bar split) are not decoded and get
black bars around the normal picture; the IRQ modes are described in the formats file.

## Where to take it next

* Decode the dungeon room format (IRQ modes 3/4, `$8200` split) so the status
  bar can move into the side band and rooms get widescreen too.
* Play further with coverage recording to translate the remaining routines
  (the first dungeon, bosses, later chapters) and extend the cosimulation script.
* Hooks keyed by ROM address (a `patches/` layer called from the dispatcher)
  would allow port-style enhancements without touching the generated code:
  rebindable auto-fire, removing lag, save-anywhere.
* The generated code and `recomp/symbols.txt` are the starting point for a
  readable hand decompilation: label routines, replace generated bodies with
  hand-written C one at a time, and keep the cosimulation green.
