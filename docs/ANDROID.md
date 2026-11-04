StarTropics native port - Android (handhelds and phones)
========================================================

There are two APKs. Pick the one for your device:

- startropics-android-handheld.apk: for gaming handhelds with built-in controls (AYANEO,
  Retroid, AYN Odin and the like). The controls are the device's own buttons; nothing is
  drawn over the picture.
- startropics-android-phone.apk: for phones and tablets. A translucent on-screen controller
  (d-pad, B, A, Select, Start, MENU) is drawn over the picture. It installs as its own app,
  "StarTropics Touch", so it can sit next to the handheld one; their saves are separate.

Both run on Android 7 or newer (arm64; they also carry an x86_64 build for emulators and
Chromebooks). They contain no game data: you need your own StarTropics (USA) cartridge dump
(.nes).

Install
-------
Copy the APK to the device and open it (allow installing from that app when Android asks),
or from a PC: `adb install startropics-android-handheld.apk` (or -phone.apk).

First start
-----------
The menu opens on the ROM tab. Choose "Choose ROM file..." and pick your .nes file in the
system file picker (for example /sdcard/ROMs/nes on a Retroid). The app copies it into its
own storage, checks its hash and remembers it, so this happens once.
Alternatively put the .nes file in Android/data/org.startropics.nativeport/files/ on the
shared storage (org.startropics.nativeport.touch for the phone app); the app finds it there
by itself.

The picture fills the screen in the shape that matches it: 16:9 widescreen on a 1920x1080
screen like the AYANEO Pocket EVO's, 4:3 on the Retroid Pocket Nova's 1280x960. Both can be
changed on the Graphics tab.

Controls on a phone
-------------------
The on-screen controller: d-pad at the bottom left (slide your thumb around it; the
diagonals are narrower than the four directions), B and A at the bottom right (a thumb
between them presses both, and it can slide from one to the other), Select and Start at
the bottom, MENU at the top for this port's menu (the Back gesture opens it too). In the
menu, tap a tab or a row; tap the arrows of a row to change it. A controller can be used as
well: the on-screen controls step aside while you play with it and come back when you touch
the screen.

Controls on a handheld
----------------------
The built-in controller works as a gamepad. Default buttons (Xbox labels; on a Nintendo-
style layout such as the Retroid's, change them on the Controls tab if they feel wrong):
A = NES A (jump), X = NES B (attack), Start, Select. LT + RT (L2 + R2) together opens the
menu and closes it again; Start + Select together and the Android back button open it
too. R1 (hold) rewinds, R2 (hold) fast-forwards; within 1.5 s after a rewind it goes
forward again to where the rewind began.

Notes
-----
- Saves, save states (9 slots) and settings live in the app's private storage; Android's
  app backup includes them. Uninstalling the app deletes them.
- RetroAchievements: log in on the Achievements tab (an on-screen or physical keyboard
  appears for the user name and password). Only the session token is stored.
- Graphics > Textures: Remastered recolors and shades the game in the manner of the Pixel
  Remaster games. A hand-made tile pack, remaster.pak, if you have one, goes in
  Android/data/org.startropics.nativeport/files/ (…nativeport.touch for the phone app).
- Updates: the Updates tab checks GitHub for a newer version and opens its release page;
  download the APK of your edition there and open it, and it installs over the app with
  its saves and settings kept (Android asks once to allow installs from the browser).
- A log of the last run is written to Android/data/org.startropics.nativeport/files/
  startropics-log.txt (.touch for the phone app; handy with `adb pull` when something goes
  wrong).
- Smooth motion and latency: on a 120 Hz screen (Pocket EVO) the display runs at 120 Hz
  while the app is in front, and every frame is shown for exactly two refreshes, locked
  to the compositor's deadlines (Android 13 and newer report them). A press reaches the
  middle of the screen in about 31 ms on average, against about 50 ms on a NES with a
  CRT; docs/LATENCY.md in the source has the breakdown.
- Building the APK yourself: see the README (android/build_apk.py).
