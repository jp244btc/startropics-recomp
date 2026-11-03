# Input latency

Where the time goes between a button press and the pixels, and what the port
does about each part. Numbers are for a 60 Hz class display; `startropics
--play --stats out.txt` logs the measured values on your machine once a second.

| stage | real NES + CRT | this port, defaults |
|---|---|---|
| controller report | ~0 (wired, polled by the game) | USB pad 1-4 ms, Bluetooth 8-15 ms |
| waiting for the game to read the pad | up to 1 frame | ~3 ms: frame delay polls just before the deadline |
| the game's own reaction (StarTropics: 2 frames) | 33 ms | 0 ms with run-ahead 2 |
| emulation + draw | - | ~2.5 ms (included in the frame delay) |
| wait for the display | 0-16 ms (beam position) | 0-16 ms scan-out, phase-locked to the vblank |
| compositor / monitor processing | 0 | windowed: ~1 frame (DWM); exclusive fullscreen: 0; plus the monitor's own lag |
| sound effect delay | 0 | ~25-45 ms (audio preset Low / Normal) |

## What the frontend does

**Frame delay.** Each loop iteration first waits for the display's vertical
blank (`IDXGIOutput::WaitForVBlank` on the monitor the window is on, which also
works on hybrid-GPU laptops), then sleeps until `deadline - work - margin`,
where `work` is the measured time to emulate and draw a frame and `margin`
starts at 2 ms and grows if a deadline is missed. Only then are events polled,
the pad read, the frame emulated and presented. Input is therefore sampled
about 3 ms before the frame is handed to the display. On displays whose
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

**Audio.** Dynamic rate control nudges the APU's output rate by up to 1.2% so
the queue sits at the preset target (Low 16 ms, Normal 32 ms, Safe 70 ms) plus
a small device buffer. This also absorbs the difference between the NES's
60.0988 Hz and the display's rate.

**Exclusive fullscreen** bypasses the desktop compositor, which otherwise adds
about one frame in a window; it is the recommended mode for play.

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
  far from 16.6 ms).
* Graphics > Run-ahead: 0-3 frames.
* Audio > Latency: Low / Normal / Safe.
* Prefer a wired controller; Bluetooth adds roughly 10 ms before SDL sees the press.
