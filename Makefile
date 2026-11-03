# StarTropics native port - build
#
#   make headless        headless test harness (interpreter only)
#   make recomp          run the recompiler on rom/*.nes -> generated/
#   make startropics     native game executable (needs generated/ and SDL2)
#   make test            CPU/PPU/APU/mapper regression tests
#
# Requires: a C11 compiler, GNU make, Python 3; SDL2 for the frontend.

ifeq ($(origin CC),default)
  CC := gcc
endif
PYTHON  ?= python
CFLAGS  ?= -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
LDFLAGS ?=
ifeq ($(OS),Windows_NT)
  EXE := .exe
else
  EXE :=
endif

# first .nes in rom/ (filenames may contain spaces, so avoid $(firstword))
ROM     ?= $(shell ls rom/*.nes 2>/dev/null | head -n 1)
BUILD   ?= build
GEN     := generated

RUNTIME_SRC := runtime/cpu.c runtime/mem.c runtime/cart.c runtime/mapper.c runtime/ppu.c \
               runtime/apu.c runtime/sys.c runtime/png.c runtime/palette.c
RUNTIME_OBJ := $(patsubst runtime/%.c,$(BUILD)/%.o,$(RUNTIME_SRC))

GEN_SRC := $(wildcard $(GEN)/*.c)
GEN_OBJ := $(patsubst $(GEN)/%.c,$(BUILD)/gen/%.o,$(GEN_SRC))

SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null)
SDL_LIBS   := $(shell sdl2-config --libs 2>/dev/null)

.PHONY: all headless recomp startropics test clean

all: headless

$(BUILD)/%.o: runtime/%.c runtime/nes.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

GEN_CFLAGS := -Wno-unused-label -Wno-implicit-fallthrough -Wno-unused-variable

$(BUILD)/gen/%.o: $(GEN)/%.c runtime/nes.h runtime/recomp_rt.h
	@mkdir -p $(BUILD)/gen
	$(CC) $(CFLAGS) $(GEN_CFLAGS) $(RECOMP_DEFS) -Iruntime -c $< -o $@

# ---- headless harness (interpreter only) ----
headless: $(BUILD)/nes-headless$(EXE)

$(BUILD)/nes-headless$(EXE): $(RUNTIME_OBJ) $(BUILD)/headless.o $(BUILD)/recomp_stub.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# ---- headless harness with recompiled code ----
headless-recomp: $(BUILD)/nes-headless-recomp$(EXE)

$(BUILD)/nes-headless-recomp$(EXE): $(RUNTIME_OBJ) $(BUILD)/headless.o $(GEN_OBJ)
	@test -n "$(GEN_SRC)" || (echo "no generated code: run 'make recomp' first" && exit 1)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# ---- recompiler ----
recomp:
	@test -n "$(ROM)" || (echo "put your StarTropics .nes file in rom/ (or pass ROM=path)" && exit 1)
	$(PYTHON) recomp/recomp.py --rom "$(ROM)" --out $(GEN) $(RECOMP_FLAGS)

# ---- native game ----
startropics: $(BUILD)/startropics$(EXE)

$(BUILD)/startropics$(EXE): $(RUNTIME_OBJ) $(BUILD)/sdl_main.o $(GEN_OBJ)
	@test -n "$(GEN_SRC)" || (echo "no generated code: run 'make recomp' first" && exit 1)
	$(CC) $(CFLAGS) $^ -o $@ $(SDL_LIBS) $(LDFLAGS)

$(BUILD)/sdl_main.o: frontend/sdl_main.c runtime/nes.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Iruntime -c $< -o $@

# ---- tests ----
test: headless
	$(PYTHON) tests/run_tests.py

cosim: headless headless-recomp
	$(PYTHON) tests/cosim.py

clean:
	rm -rf $(BUILD)
