# Retrolooper One — Force build

A dedicated Akai Force build of Retrolooper One, with its own VST ID (`RlF1`). 0.3.0 is the MPC OS 3.10.1 package: copy `RetroLooper - VST - RetroLooper One Force` into `VST2 Plugins` on any drive. Loops and exports stay in that folder. It has not been installed on a 3.10.1 Force yet. The effect runs without a native helper. Optional Force Controls is a separate Device patch and stays pinned to Force 3.10.

The stereo looper retains sequence-sized allocation, a 120-second maximum, four undo levels, threshold recording, Reverse, HP/LP filters, MIDI assignments, Wave/Tape/Meter views, project recall and the same graphic style. Touchscreen buttons and physical pads control capture. Force supplies the clock and global transport.

Automatic loop length and default phase use Force's VST2 cycle start/end positions, with ÷2, ×1 and ×2 available. Global Arrange is the intended timing source. The range discrepancy reported on hardware is still under investigation. “Next bar” is an optional launch mode.

## Force controls

The optional [Force Controls patch](../device-patches/retrolooper-controls-force.md) provides native track ownership, selected-track input preparation, pad colours and Bounce to Clip on the exact verified Force 3.10 host image. Opening a looper selects its Audio track. With the looper open, pressing Track Select retargets the editor and pad grid to that track's looper. The patch runs in the background; Retrolooper One is the only insert effect.

The looper grid uses the **bottom two rows** while its editor is visible. P1–P8 are on the bottom row and P9–P16 are on the row above, each left to right. Play and Assign use the same square touch areas. Tap a button in Play or drag a knob vertically in Assign. Both modes were verified on the Force touchscreen. Closing the editor or entering a native mute or performance mode gives the grid back to Force.

With Controls, capture preparation uses the selected owning Audio track, switches native REC ARM off to avoid duplicate timeline recording and prepares Monitor MERGE. Other tracks containing Retrolooper One use Monitor OFF and keep their recorded loops playing. Without Controls, set Monitor MERGE and REC ARM OFF yourself, and use the looper's touchscreen or assigned MIDI controls. Put effects **before** Retrolooper to record their sound into each layer.

## Validation and supported firmware

The Force device provided for this port runs 3.10. Its exact host image is pinned by GNU build ID `154ae9752e0f3c6490912a703b0dfe4b668b343d`. Other firmware builds fail closed in the helper until separately profiled.

Passed:

- Sanitized Mac DSP/host tests for audio, timing, multipliers, threshold start, reverse, persistence, memory and multiple instances.
- Force image hash, build ID and all 36 helper code/type guards.
- Read-only live application, Project, TrackPool and AudioProgram graph checks on Force.
- Isolated ARM tests executed on Force, using fixture memory and fake ALSA for ownership, monitor/arm preparation, MIDI passthrough, pad identity, native mode handoff and RGB restoration.
- Production `.so` audio tests on Force: exact /2, x1 and x2 frame counts, clean synthetic playback, four-instance capture isolation, overdub handoff, threshold sample alignment, exact stereo Reverse, local transport and loop-present feedback.
- Native skin/schema, layout and resource audits.

**Version 0.2.10 is a hardware test build.** **Timing → Record Offset** defaults to **Auto (7 ms)** for new recordings and overdubs through the built-in analog inputs. The manual control covers 0–50 ms for other signal paths. Save the Force project before installing and restarting the application. Existing Force testers with a legacy `RetroLooper - VST - RetroLooper Force` installation should back up their `Loops` and `Exports` before moving to the new folder.

With Force Controls 0.2.10, finish recording or overdubbing and tap **CLIP** on Looper or **Choose Clip** on Mixer. CLIP blinks while waiting. Press the destination pad to select that Matrix slot and bounce while the looper stays open. You can also open Matrix and choose a destination on the touchscreen. The bounce can run during playback. It renders the stored loop with its filters, level and pan. If the destination changes during rendering, the WAV stays in the plugin's Exports folder.

**WAV** still exports a numbered WAV to `Exports` without changing any clip. Bounce uses separately named WAV files and does not consume a numbered slot. Native clip replacement, playback and Undo require an on-device check before this test build is promoted.

## Distribution and build

The effect ZIP is in `MAC BUILD/DISTRIBUTION/Akai Force/Portable Test 0.2.10/`, with the separate patch in **Optional Controls**. The MPC One+ build has its own distribution folder.

```sh
python3 build_force.py test --sanitize
python3 build_controls.py --zig /path/to/zig-0.13.0
python3 build_force.py arm --zig /path/to/zig-0.13.0
python3 build_force.py preview
python3 workspace/tools/preview_server.py --port 8766
python3 build_force.py package --kit /path/to/pinned/mpc-vst-plugins
python3 ../tools/prepare_distribution.py
```

Generated sources are in `workspace/`; packages are in `dist/`. `workspace/provenance.json` records source hashes and asserted adaptations. Parameter indices/order/defaults remain compatible. The main MPC preview on port 8765 is independent.

References: [community Force skin findings](https://github.com/open-mpc/mpc-vst-plugins/blob/main/docs/OS2_SKINS.md), [Force Q-Link mapping](https://github.com/open-mpc/mpc-vst-plugins/blob/main/docs/NOTES.md).
