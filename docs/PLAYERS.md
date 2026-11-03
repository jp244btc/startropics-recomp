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

Menu (Esc in game, or Start + Select together)
----------------------------------------------
Play          Start / resume, reset, save state, load state, the save state slot
              (1-9, with the date the slot was last saved), end game, quit
Graphics      Display mode (windowed / fullscreen / fullscreen exclusive),
              resolution (auto, the sizes your display offers, and always
              1920x1080 and 3840x2160: the window size, the display mode in
              exclusive fullscreen, or the size the picture is rendered at in
              fullscreen), window size 1x-6x (shown while the resolution is Auto),
              scaling (integer: exact pixel multiples, may leave borders / fit: fills
              the screen, keeps the shape / stretch: fills, distorts),
              aspect ratio (square pixels 8:7 / 4:3 CRT / 16:9 or 16:10 widescreen:
              the overworld map is decoded beside the picture, other scenes get
              black bars, nothing is stretched),
              filter (nearest / linear / sharp),
              shader (off / scanlines / CRT / CRT curved / glow) and its strength,
              overscan crop,
              sprite limit (8 per line as on the console, or unlimited: no flicker
              when many sprites share a line),
              low-latency sync (phase-locks to the display), run-ahead (0-3 frames;
              2 cancels the game's own input lag).
              The line under the rows says what the settings produce (for example
              "Picture 2560x1440 in the 2560x1600 display mode") and why there are
              borders or bars when there are: Integer only uses whole multiples of
              the NES picture, and a 16:9 picture leaves bars on a 16:10 screen (the
              16:10 aspect fills it).
Audio         Volume, latency preset (Low / Normal / Safe)
Controls      Rebind the eight NES buttons and the rewind key (select a row, press
              Enter, press the key or pad button)
Achievements  RetroAchievements: log in, hardcore mode, progress (below)
ROM           Choose or change the cartridge dump

Default keys
------------
D-pad: arrow keys    A (jump): X    B (attack): Z    Start: Enter    Select: Right Shift
Rewind: hold Backspace (pad: RB)    Fast-forward: hold Tab (pad: RT, the right trigger)
Gamepad: A = right face button (B on Xbox), B = bottom face button (A on Xbox),
Start, Back = Select, D-pad or left stick. Start + Select pressed together opens
the menu (also Guide or LB; on the keyboard Enter + Right Shift, or Esc). The game
never sees that Start or Select: the program steps back to the moment before the
first of the two went down (except in RetroAchievements hardcore mode). Buttons
still held when you leave the menu reach the game only after you let go of them.
Keys and pad buttons can be rebound on the Controls tab: select a row, press
Enter, then press the new key or pad button. "Reset to defaults" restores them.

Other keys: P pause, F5 save state, F8 load state,
F6 / F7 previous / next save state slot, F11 or Alt+Enter fullscreen,
F12 screenshot (screenshots folder), Ctrl+R reset.

Save states and rewind
----------------------
There are nine save state slots per ROM. F5 saves to the current slot, F8 loads
it, F6 / F7 change the slot (a message in the corner shows the slot and when it
was last saved); the Play tab shows the same. Hold the rewind key to go back
through the last several minutes of play (two frames of history per frame shown);
release it to continue from there. Went back too far? Hold fast-forward: right
after a rewind it goes forward again through the stretch you rewound, at the same
speed, and stops where the rewind began. Anywhere else fast-forward speeds the
game up (8x). As soon as you play on from an earlier moment, the stretch after it
is gone. Rewind can be switched off in startropics.ini (rewind_enabled=0). Both are disabled in RetroAchievements hardcore mode.

Shaders
-------
The shader row adds CRT-style effects to the picture: Scanlines (the beam lines
of a CRT), CRT (scanlines, an aperture-grille mask and a soft glow), CRT curved
(the same on a curved tube with a vignette and rounded corners) and Glow alone.
Strength scales all of them. They are drawn by the graphics card on top of the
normal scaling and cost nothing noticeable; input lag does not change.

RetroAchievements
-----------------
With a free account at retroachievements.org the game's achievements unlock
while you play and pop up in the corner. On the Achievements tab enter your user
name and password and choose Log in (Enter on a row edits it, Enter again keeps
it, Ctrl+V pastes). The password is sent once to the RetroAchievements server to
obtain a session token; only your user name and that token are kept
(retroachievements.txt in the data folder), so you stay logged in next time. Log
out forgets it. Hardcore mode makes unlocks count on the hardcore boards; it
disables loading save states and rewind, and switching it resets the game.
Unlocks earned while offline are sent when the connection returns. The ROM is
identified by its hash the way RetroAchievements does it (StarTropics USA is
game 1985 there). This support is built on the rcheevos library (MIT licence,
see LICENSE-rcheevos.txt).

Files
-----
saves/<rom name>.sav       battery save (the game's own save slots)
saves/<rom name>.state1-9  save state slots (F5 / F8, F6 / F7 to pick one)
screenshots/               F12 screenshots
startropics.ini            settings and the remembered ROM path
retroachievements.txt      RetroAchievements session (user name + token), if logged in

These live next to startropics.exe when that folder is writable. If it is not
(Program Files, a read-only location, or the program was started from inside
the zip without extracting it), they go to
%APPDATA%\StarTropicsPort\StarTropics\ instead; the ROM you chose is remembered
either way. Extract the zip to a normal folder for portable use.

The ROM is verified by its hash before it is accepted: the ROM tab shows the
SHA-1, MD5 and CRC32 of your file and whether it matches a known StarTropics
cartridge dump. Anything else is refused.

Legal
-----
StarTropics is a trademark and copyright of Nintendo. This port is an unofficial
fan project; it only runs with a cartridge dump you provide. Do not share your
.nes file.
