"""Cosimulation check: run the same scripted session through the interpreter
and through the recompiled code and compare per-frame state hashes.

usage: python tests/cosim.py [--frames N] [--keys SPEC] [--rom FILE]

Exit code 0 means every frame matched (CPU registers and cycle count, RAM,
save RAM, framebuffer, OAM, palettes, nametables, bank registers).
"""
import argparse
import glob
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = ".exe" if os.name == "nt" else ""

# Title -> file select -> register the name "AAA" -> start chapter 1 -> walk around the beach.
DEFAULT_KEYS = (
    "700:START;710:-;1150:START;1160:-;1300:A;1310:-;1400:A;1410:-;1500:A;1510:-;"
    "1700:LEFT;1710:-;1800:A;1810:-;2000:SELECT;2010:-;2100:SELECT;2110:-;2150:SELECT;2160:-;"
    "2200:START;2210:-;3000:START;3010:-;3600:A;3610:-;4000:DOWN;4300:LEFT;4800:-;5200:UP;5600:LEFT;6000:-"
)


def run(exe, rom, frames, keys, out, extra):
    cmd = [exe, "--rom", rom, "--frames", str(frames), "--keys", keys, "--frame-hash", out, "--quiet"] + extra
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print(p.stdout, p.stderr)
        raise SystemExit(f"{exe} failed")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=6500)
    ap.add_argument("--keys", default=DEFAULT_KEYS)
    ap.add_argument("--rom", default=None)
    args = ap.parse_args()
    rom = args.rom or (glob.glob(os.path.join(ROOT, "rom", "*.nes")) or [None])[0]
    if not rom:
        raise SystemExit("no ROM: put your .nes file in rom/ or pass --rom")
    interp = os.path.join(ROOT, "build", "nes-headless" + EXE)
    native = os.path.join(ROOT, "build", "nes-headless-recomp" + EXE)
    for exe in (interp, native):
        if not os.path.exists(exe):
            raise SystemExit(f"missing {exe}: run 'make headless headless-recomp' first")
    h1 = os.path.join(ROOT, "build", "cosim_interp.txt")
    h2 = os.path.join(ROOT, "build", "cosim_native.txt")
    run(interp, rom, args.frames, args.keys, h1, [])
    run(native, rom, args.frames, args.keys, h2, ["--recomp"])
    a = open(h1).read().splitlines()
    b = open(h2).read().splitlines()
    bad = [i for i, (x, y) in enumerate(zip(a, b)) if x != y]
    n = min(len(a), len(b))
    if bad:
        print(f"MISMATCH: first divergence at frame {bad[0]} ({len(bad)} of {n} frames differ)")
        print("Build a tracing harness to pinpoint it:")
        print("  make BUILD=build-trace RECOMP_DEFS=-DRECOMP_TRACE headless-recomp")
        print("  then run both harnesses with --trace FILE --trace-start <frame> and diff the traces")
        sys.exit(1)
    print(f"OK: {n} frames bit-identical between interpreter and recompiled code")


if __name__ == "__main__":
    main()
