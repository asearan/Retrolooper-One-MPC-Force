# RetroLooper One 0.1.0 — hardware test build

Copyright © 2026 IAMTHEBAND LLC, Retroloop USA.

A dedicated Akai Force port of RetroLooper One, with its own VST ID (`RlF1`), binary, native helper and installer packages. The MPC One+ build is unchanged.

The stereo looper retains sequence-sized allocation, a 120-second maximum, four undo levels, threshold recording, Reverse, HP/LP filters, MIDI assignments, Wave/Tape/Meter views, project recall and the same graphic style. Touchscreen buttons and physical pads control capture. Force supplies the clock and global transport.

Automatic loop length and default phase use the **global Arrange loop**, with ÷2, ×1 and ×2 available. The port consumes the host's VST2 cycle start/end positions; the live Force test must confirm those match Arrange, including changes to its loop range. It does not use the selected Matrix clip as its intended length source. “Next bar” is an optional launch mode.

## Force controls

The matching **RetroLooper One Controls (Force) 0.1.0** helper provides native track ownership, editor-driven track selection, selected-track input preparation and pad colours. It is a background helper; only RetroLooper One appears as an insert effect. Do not install the MPC Buttons helper on Force.

The looper grid uses the **bottom two rows** only while its editor is visible: P1–P8 on the bottom row, P9–P16 on the row above, each left to right. Assign mode uses vertical-drag knobs; Play mode uses buttons. The upper 48 pads pass through unchanged. Closing the editor or entering a native mute/performance mode gives the entire grid back to Force. A live pad/LED test is still required.

Capture preparation uses the selected owning Audio track, switches native REC ARM off to avoid duplicate timeline recording and prepares Monitor IN. Other tracks containing RetroLooper One use Monitor OFF and keep their recorded loops playing. Put effects **before** RetroLooper to record their sound into each layer. An insert receives the signal at its position in the chain; it cannot capture later insert effects.

## Validation and supported firmware

The Force device provided for this port runs 3.10. Its exact host image is pinned by GNU build ID `154ae9752e0f3c6490912a703b0dfe4b668b343d`. Other firmware builds fail closed in the helper until separately profiled.

Passed:

- Sanitized Mac DSP/host tests for audio, timing, multipliers, threshold start, reverse, persistence, memory and multiple instances.
- Force image hash, build ID and all 18 helper code/type guards.
- Read-only live application, Project, TrackPool and AudioProgram graph checks on Force.
- Isolated ARM tests executed on Force, using fixture memory and fake ALSA for ownership, monitor/arm preparation, MIDI passthrough, pad identity, native mode handoff and RGB restoration.
- Production `.so` audio tests on Force: exact /2, x1 and x2 frame counts, clean synthetic playback, four-instance capture isolation, overdub handoff, threshold sample alignment, exact stereo Reverse, local transport and loop-present feedback.
- Native skin/schema, layout and resource audits.

**Installed on the provided Force; startup verified.** Both installed binary hashes match the tested build, the native UI worker is ready and the existing cursor helper is preserved. The native RGB stream has been learned. Live Arrange timing, audio recording/playback, track switching, touch gestures and physical pad actions remain to be tested. This remains a hardware test release. Save the Force project before future application restarts.

WAV export writes to the plugin's `Exports` folder. Automatic placement on the native Audio timeline remains unimplemented on both targets; this test build does not claim to bounce onto the timeline.

## Distribution and build

Share the two ZIP packages together from `releases/Force/`, following that folder's installation notes. They use the standard community catalog/plugin and addin formats. Keep them separate from `releases/MPC One+/`.

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
