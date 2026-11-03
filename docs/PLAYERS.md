StarTropics - native PC port
============================

This program contains no game data. To play you need your own StarTropics (USA)
cartridge dump as a .nes file, made from a cartridge you own. The program checks
the file's hash and refuses anything that is not StarTropics.

Getting started
---------------
1. Run startropics.exe (Windows) or startropics (Linux/macOS).
2. On the ROM tab choose "Choose ROM file..." and pick your .nes file, or drop
   the file onto the window, or start the program with the file as an argument.
3. Go to Play > Start game. The chosen file is remembered in startropics.ini.

Menu (Esc in game)
------------------
Play      Start / resume, reset, save state, load state, quit
Graphics  Display mode (windowed / fullscreen / fullscreen exclusive),
          window size 1x-6x, scaling (integer / fit window / stretch),
          aspect ratio (square pixels 8:7 / 4:3 CRT / 16:9 stretched),
          filter (nearest / linear / sharp), overscan crop,
          low-latency sync (phase-locks to the display), run-ahead (0-3 frames;
          2 cancels the game's own input lag)
Audio     Volume, latency preset (Low / Normal / Safe)
Controls  Rebind the eight NES buttons (select a row, press Enter, press the key)
ROM       Choose or change the cartridge dump

Default keys
------------
D-pad: arrow keys    A (jump): X    B (attack): Z    Start: Enter    Select: Right Shift
Gamepad: A = right face button (B on Xbox), B = bottom face button (A on Xbox),
Start, Back = Select, D-pad or left stick; Guide or LB opens the menu.
Keys and pad buttons can be rebound on the Controls tab: select a row, press
Enter, then press the new key or pad button. "Reset to defaults" restores them.

Other keys: Tab (hold) fast-forward, P pause, F5 save state, F8 load state,
F11 or Alt+Enter fullscreen, F12 screenshot (screenshots folder), Ctrl+R reset.

Files
-----
saves/<rom name>.sav     battery save (the game's own save slots)
saves/<rom name>.state   save state (F5 / F8)
screenshots/             F12 screenshots
startropics.ini          settings and the remembered ROM path

Legal
-----
StarTropics is a trademark and copyright of Nintendo. This port is an unofficial
fan project; it only runs with a cartridge dump you provide. Do not share your
.nes file.
