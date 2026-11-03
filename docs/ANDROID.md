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
The built-in controller works as a gamepad. Default buttons (Xbox positions; on a Nintendo-
style layout such as the Retroid's, swap them on the Controls tab if they feel reversed):
right face button = NES A (jump), bottom face button = NES B (attack), Start, Select.
Start + Select together, or the Android back button, opens the menu. R1 (hold) rewinds,
R2 (hold) fast-forwards; right after a rewind it goes forward again to where it began.

Notes
-----
- Saves, save states (9 slots) and settings live in the app's private storage; Android's
  app backup includes them. Uninstalling the app deletes them.
- RetroAchievements: log in on the Achievements tab (an on-screen or physical keyboard
  appears for the user name and password). Only the session token is stored.
- A log of the last run is written to Android/data/org.startropics.nativeport/files/
  startropics-log.txt (handy with `adb pull` when something goes wrong).
- Building the APK yourself: see the README (android/build_apk.py).
