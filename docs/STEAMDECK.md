StarTropics native port - Steam Deck edition
=============================================

This build starts in fullscreen at the Deck's own 1280x800 with the 16:10 widescreen
view (the overworld map is decoded from your ROM to fill the sides), and the menus are
driven with the Deck's controls. It contains no game data: you need your own
StarTropics (USA) cartridge dump (.nes).

Install (Desktop Mode)
----------------------
1. Extract this folder somewhere in your home folder, for example ~/Games/StarTropics.
2. Copy your StarTropics (USA) .nes file into that same folder. The program finds it
   there by itself the first time (it checks the file's hash and remembers it).
3. Open Steam, then Games > Add a Non-Steam Game to My Library > Browse. Switch the file
   type filter to "All Files", pick `startropics` in the folder, then Add Selected
   Programs. Optionally set startropics.png as the shortcut's icon.
4. Return to Game Mode and start StarTropics from your library.

Controls (Steam's default gamepad layout)
-----------------------------------------
D-pad or left stick   move
A                     NES A (jump)
X                     NES B (attack)
Menu (three lines)    Start
View (two squares)    Select
L2 + R2 together      this port's menu (graphics, audio, controls, save states,
                      RetroAchievements); again to close it. Start + Select works
                      too, and Steam's own menu as usual.
R1 (hold)             rewind
R2 (hold)             fast-forward; within 1.5 s after a rewind it goes forward
                      again to where the rewind began
Every button can be changed on the menu's Controls tab.

Tips
----
- Deck OLED: for perfectly even motion set the refresh rate to 60 Hz for this game in
  the quick access menu (Performance). The NES runs at 60 Hz.
- Latency: Graphics > Low-latency sync (on by default) hands each frame to the screen
  without vsync, as Mesen does by default, so no frame waits in a queue; with run-ahead
  the game reacts two frames sooner than in Mesen at its defaults. Switch it off for
  vsync: the smoothest scrolling, at up to one frame more latency.
- Saves (the game's own and the 9 save state slots), settings and screenshots are kept
  in the game folder.
- RetroAchievements: log in on the menu's Achievements tab. The password is only sent
  to retroachievements.org to obtain a session token; only the token is stored.
- The full player notes are in README.txt.
