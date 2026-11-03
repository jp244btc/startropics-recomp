# Input latency

Where the time goes between a button press and the pixels, and what the port
does about each part. `startropics --play --stats out.txt` logs the measured
values once a second. On Android, put `--stats` and an absolute file path, one
per line, in `Android/data/org.startropics.nativeport/files/args.txt`.

| stage | real NES + CRT | Mesen 2, default settings | this port, PC | this port, Pocket EVO |
|---|---|---|---|---|
| controller to program | ~0 (polled by the game) | USB pad 1-4 ms, Bluetooth 8-15 ms | USB pad 1-4 ms, Bluetooth 8-15 ms | built-in pad, a few ms |
| press until the pad is read | 0-16.7 ms | 0-16.7 ms (latched once a frame) | 0-16.7 ms | 0-16.7 ms |
| the game's own reaction (StarTropics: 2 frames) | 33 ms | 33 ms (run-ahead off) | 0 (run-ahead 2) | 0 (run-ahead 2) |
| pad read until the frame is handed over | - | ~1 ms | ~3 ms (frame delay) | ~5.5 ms |
| handed over until on screen | 0 (the beam) | vsync off: shown torn at the next scan-out | 0-1 ms: handed over just before the vblank | 12.9-13.7 ms: the compositor's deadline plus 10.3 ms |
| scan-out to mid-screen | 8 ms | 8 ms | 8 ms (60 Hz) | 4 ms (120 Hz) |
| **average, press to mid-screen** | **~50 ms** | **~50 ms**, more in a desktop window | **~20 ms** | **~31 ms** |
| sound effects | 0 | ~60 ms (audio latency 60) | ~25-45 ms (Low / Normal) | ~32 ms queue plus Android's audio buffer |

## What the frontend does

**Frame delay.** On Windows each loop iteration first waits for the display's
vertical blank (`IDXGIOutput::WaitForVBlank` on the monitor the window is on,
which also works on hybrid-GPU laptops), then sleeps until `deadline - work -
margin`, where `work` is the measured time to emulate and draw a frame and
`margin` starts at 2 ms and grows if a deadline is missed. Only then are events
polled, the pad read, the frame emulated and presented. Input is therefore
sampled about 3 ms before the frame is handed to the display. On displays whose
refresh is not a multiple of ~60 Hz (75, 144, 165 Hz) the loop falls back to
timer pacing with the same frame delay, so latency varies by up to one refresh.
Presentation runs without vsync; a one-frame DXGI queue (`SetMaximumFrameLatency(1)`)
means a frame can never sit behind older ones.

**Run-ahead.** StarTropics reads the controller during vblank, acts on it in
the following frame and shows the result one frame after that: two frames of
built-in lag, measured with the headless harness (press at frame 4100, first
changed picture at 4102). With run-ahead the loop emulates the real frame,
saves the machine state, emulates two more frames with the same input, shows
*that* picture, and restores the state. The player sees the game's reaction on
the very frame of the press, which real hardware cannot do. Audio comes from the
real frame only. The default of 2 matches the game's lag exactly; a higher
value would show movement that gets taken back when a button is released.
The self-test (`--selftest N out.png`) verifies that a save / look-ahead /
restore cycle leaves the timeline bit-identical.

**Audio.** Dynamic rate control nudges the APU's output rate so the queue sits
at the preset target (Low 16 ms, Normal 32 ms, Safe 70 ms) plus a small device
buffer: a proportional part of up to 1.2% reacts to the queue, and a slow
integral part learns a steady clock difference (the NES's 60.0988 Hz against
the display's rate), so the queue settles at the target instead of above it.

**Exclusive fullscreen** bypasses the desktop compositor, which otherwise adds
about one frame in a window; it is the recommended mode for play on Windows.

## Android

Android hands frames to its compositor (SurfaceFlinger), which needs each one
by a deadline to show it at a given refresh. From Android 13 on, the
Choreographer reports these frame timelines with every vsync: when a frame must
be queued, and when it will then be on screen. A thread receives every vsync,
and the game loop locks each frame to that deadline grid: it reads the pad and
emulates so that the frame is queued about 2 ms before a deadline, every frame.
On older Android versions the vsync times themselves serve as the grid.

The app also asks for the display mode with the highest refresh rate that is a
whole multiple of 60 Hz (120 Hz on the Pocket EVO's 120 / 144 / 165 Hz panel;
it applies only while the app is in front). Every frame then stays on screen
for exactly two refreshes, and the compositor's deadline-to-screen time is
10.3 ms instead of about 17 ms at 60 Hz. The EVO's panel actually runs at
120.6 Hz, so the game runs 0.35% fast (60.3 fps) like any display-locked
emulator, and the audio rate control absorbs it.

Measured on the Pocket EVO (Android 13, run-ahead 2), app log and the
compositor's own timestamps (`dumpsys SurfaceFlinger --latency`):

```
pacing: vblank phase-lock, display 121 Hz, 2 vblank(s) per frame
frame 16.58 ms (60.31 fps)  input->present 5.4 ms  deadline->screen 10.33 ms
127 frames: every one on screen for exactly 2 refreshes; queued to on screen 12.9-13.7 ms
```

Before this, with open-loop pacing, a frame reached the screen 11.2 to 18.2 ms
after it was queued depending on its phase, and about one frame in 150 (every
2.5 s) stayed on screen for three refreshes, a visible hitch.

## Linux and the Steam Deck

There is no vblank source to lock to here, and a vsynced present can leave a
frame waiting in the queue, because the NES runs a little faster than a 60 Hz
panel. With Low-latency sync on (the default) frames are therefore handed over
by the timer without vsync, which is also Mesen's default: the compositor (the
Deck's gamescope, or a desktop's) shows the newest frame at each refresh, so no
frame waits. With Low-latency sync off, frames are presented with vsync: the
smoothest motion, at up to one frame more latency. This has not been measured
on a Deck.

## Compared with Mesen 2

Mesen 2's defaults, from its source (`Core/Shared/SettingTypes.h`): run-ahead 0,
vertical sync off, audio latency 60 ms, and the controller state is updated at
scanline 241, once a frame.

* At those defaults the port shows the game's reaction to a press two frames
  (33 ms) sooner on every platform: run-ahead 2 removes StarTropics' own delay.
* With Mesen's run-ahead also set to 2 and vsync off, Mesen presents each frame
  as soon as it is emulated and the display shows it torn; the port's Windows
  build hands its frame over about 3 ms before the vblank. The two are within
  a few milliseconds on average, and the port does not tear.
* With Mesen's vsync on, Mesen waits 0-16.7 ms (about 8 ms on average) for the
  vblank, and the port is ahead by that.
* Mesen has no Android version. On the Pocket EVO the port's average from press
  to mid-screen (about 31 ms) is well below a real NES on a CRT (about 50 ms).

## Measured on the development laptop (240 Hz panel, Xbox Series X pad)

```
pacing: vblank phase-lock, 4 vblanks per frame, run-ahead 2
frame 16.60 ms (60.25 fps)  input->present 3.1 ms  work 2.6 ms  audio queued 28 ms
```

Input to present under 4 ms, plus up to one 4.1 ms refresh for scan-out, with
the game's own two frames removed by run-ahead: about the same as a NES on a
CRT, and a little better once run-ahead is counted.

## Tuning

* Graphics > Low-latency sync: Off switches to plain timer pacing (for displays
  or drivers where the vblank wait misbehaves; look at `--stats` for frames
  far from 16.6 ms). On Linux and the Steam Deck, where On already paces by the
  timer without vsync, Off presents with vsync instead.
* Graphics > Run-ahead: 0-3 frames.
* Audio > Latency: Low / Normal / Safe.
* Prefer a wired controller; Bluetooth adds roughly 10 ms before SDL sees the press.
