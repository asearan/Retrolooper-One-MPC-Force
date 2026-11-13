# Retrolooper One on MPC One+

On MPC OS 3.10.1, turn on Preferences → Enhancements → External Plugin Support and copy `RetroLooper - VST - Retrolooper One` into `VST2 Plugins` at the root of a drive. 0.4.0 includes `plugin.json`, a Gen1 armv7 build, and a Gen2 aarch64 build. The SDK zip is the single folder for both. The `RlO1` ID is unchanged. Use native Monitor MERGE and REC ARM OFF for loop-only input capture; place any effects to be printed before the looper.

If a copy is still in `Synths`, MPC may draw that skin instead. Remove it while testing. Saved projects that already point at the old folder still load. Copy `Loops` and `Exports` into the new folder so existing takes stay with the plug-in. 0.4.0 has not been installed on a 3.10.1 device yet.

Cue a take or overdub on the touchscreen, assign MIDI or a Q-Link if desired, and run MPC Play. Retrolooper follows host transport and tempo. If the host does not supply a usable cycle range, set Take Bars on Mixer. Tap **EXPORT** to save a WAV in the `Exports` folder next to the plug-in. Import that WAV manually into an MPC Audio track.

The separate [MPC Controls patch](retrolooper-controls-mpc.md) adds Bank A colours and actions, track selection, selected-track MERGE routing and timeline bounce on the supported modified MPC One+ 3.10.0.49 host image. With transport stopped, **EXPORT** also places the loop at bar 1 on the selected Audio track when the destination range is empty. MPC Undo removes the region. Other device and firmware combinations have not been validated.

![Retrolooper One on MPC One+](images/retrolooper-one-screenshot.jpg)

## Trademarks

VST is a registered trademark of Steinberg Media Technologies GmbH.

Audio Units is a trademark of Apple Inc.
