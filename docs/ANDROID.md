StarTropics native port - Android (AYANEO, Retroid and other handhelds)
========================================================================

startropics-android.apk runs on Android 7 or newer (arm64 handhelds and phones; it also
carries an x86_64 build for emulators and Chromebooks). It contains no game data: you need
your own StarTropics (USA) cartridge dump (.nes).

Install
-------
Copy the APK to the device and open it (allow installing from that app when Android asks),
or from a PC: `adb install startropics-android.apk`.

First start
-----------
The menu opens on the ROM tab. Choose "Choose ROM file..." and pick your .nes file in the
system file picker (for example /sdcard/ROMs/nes on a Retroid). The app copies it into its
own storage, checks its hash and remembers it, so this happens once.
Alternatively put the .nes file in Android/data/org.startropics.nativeport/files/ on the
shared storage; the app finds it there by itself.

The picture fills the screen in the shape that matches it: 16:9 widescreen on a 1920x1080
screen like the AYANEO Pocket EVO's, 4:3 on the Retroid Pocket Nova's 1280x960. Both can be
changed on the Graphics tab.

Controls
--------
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
- A log of the last run is written to Android/data/org.startropics.nativeport/files/
  startropics-log.txt (handy with `adb pull` when something goes wrong).
- Smooth motion and latency: on a 120 Hz screen (Pocket EVO) the display runs at 120 Hz
  while the app is in front, and every frame is shown for exactly two refreshes, locked
  to the compositor's deadlines (Android 13 and newer report them). A press reaches the
  middle of the screen in about 31 ms on average, against about 50 ms on a NES with a
  CRT; docs/LATENCY.md in the source has the breakdown.
- Building the APK yourself: see the README (android/build_apk.py).
