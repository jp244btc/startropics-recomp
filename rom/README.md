# Put your own cartridge dump here

This folder is where `make recomp` and the game executable look for your
StarTropics (USA) `.nes` file. The file is yours: dump it from a cartridge
you own. Nothing in this folder is tracked by git (see `.gitignore`), and the
project does not contain, download, or write any game data.

Expected dump (ROM image without the 16-byte header):

| Region | SHA-1 of PRG+CHR data | CRC32 |
|---|---|---|
| USA | `74c53fe9ac779f146c59ac01e701c9bf912b3c7b` | `889129cb` |

The loader also accepts old-style headers with junk in bytes 8-15 ("DiskDude!");
it recognises the dump by hash and applies MMC6 semantics regardless.
