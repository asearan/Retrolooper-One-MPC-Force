# Retrolooper One

Stereo looper insert for Akai MPC standalone, built with the official MPC Plugin SDK. It records the track it sits on, plays the loop in time with the MPC, and overdubs new layers on top.

Manufacturer: IAMTHEBAND LLC. This build's plugin id is `IH97`. Projects saved with the earlier community builds (`RlO1` on MPC, `RlF1` on Force) do not open in this plugin.

The plugin needs **MPC OS 3.10.1 or later**. On the device, turn on Preferences → Enhancements (Beta) → **Enable Enhancements** and **External Plugin Support**.

## Install

Copy the folder `IAMTHEBAND LLC - VST - Retrolooper One` into a `VST2 Plugins` folder at the root of a USB drive or SD card. Insert the drive, then add Retrolooper One as an insert on an Audio track.

The dry track audio always passes through. Loop audio is added on top. Set the Audio track monitor to **MERGE** and leave native **REC ARM** off when you want the MPC to feed the insert without also printing a timeline clip. This plugin does not change those track settings for you.

## Controls

`src/ControlHelper.h` is the control helper included in this version. It reads MPC play, stop, record, tempo, time signature, cycle and bar position, plus the touchscreen and Q-Link switches.

With **Follow** on (the default):

- MPC Play starts the loop once a take exists. MPC Stop stops it and closes an open take.
- MPC Record starts the first take. After that take is done, release Record and press it again to overdub. The on-screen **Overdub** switch overdubs without that second press.
- The take length is the host cycle when the MPC reports one. Otherwise it is **Bars** (default 4). **Length** scales that by /2, x1 or x2.
- **Launch: Sequence** writes onto the current sequence phase. **Next bar** waits for the next bar.

Turn a one-shot switch on to fire it once (Undo, Redo, Clear, Reverse, Stop, Sync, Export). Turn it off before using it again. Play, Record, Overdub, Mute, Follow, Output and Auto Start stay on while you leave them on.

**Auto Start** holds an empty take until the input reaches **Threshold**. Filters and fade affect what you hear and what Export writes. They are not printed into the stored take. **Feedback** is how much of the previous layer stays during an overdub.

The screen uses the original Retrolooper One artwork on three pages: **LOOPER**, **MIXER**, and **TIMING**. Wave, Tape, and Meters change the center picture. There is no Pads page and no MIDI page.

The default Q-Link bank is **Loop**, **High Pass**, **Low Pass**, then Input. The same four are the track Q-Links before you open the plugin screen.

| Page | What you can run |
| --- | --- |
| LOOPER | Record, Play, Overdub, Stop, Clear, Undo, Redo, Mute, Export, Reverse, Auto Start, Threshold, High Pass, Low Pass, Loop level, Length |
| MIXER | Input, Loop level, Pan, Master, Feedback, Fade, Source, Output, Bars, Reverse, Export |
| TIMING | Length, After recording, Launch, Align loop to start |

Export writes the next free `Loop-01.wav` … `Loop-64.wav` as 16-bit stereo into an `Exports` folder next to the plugin. The loop is also stored in the project, so a saved project recalls it stopped. One undo level is kept. Reverse flips playback direction.

## Limits

- Longest take is 32 seconds at 48 kHz (a little under that at 44.1 kHz). Longer requests are shortened to that cap.
- One undo / redo step. A new record, overdub or clear replaces it.
- Several instances each keep their own buffers (about 37 MB each). Start with one instance.
- CPU is a copy plus, while a loop is playing, a read, an optional filter and a gain. It should sit well under one core.

The older optional device patches (hardware pad colours, selected-track monitor changes, and timeline bounce) edit MPC system files. This official build does not install those patches. Pad colour takeover, automatic MERGE / REC ARM, and placing the loop on the sequence timeline are not available here.

## Programs

Init, Overdub, Half Length, Double, Filtered, Punch, Soft Dub, Manual.

## Licence

Plugin source in this folder is GPL-3.0-only, Copyright © 2026 IAMTHEBAND LLC, Retroloop USA. It is built against the MIT-licensed MPC Plugin SDK. The SDK's own files keep their MIT licence.
