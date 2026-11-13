# Retrolooper One for MPC and Force

MPC OS 3.10.1 loads third-party VST2 plug-ins from a folder named `VST2 Plugins` at the root of the internal drive, an SD card, or USB, with Preferences → Enhancements → External Plugin Support on. Each plug-in folder needs Akai's `plugin.json`. These builds keep the existing `RlO1` and `RlF1` IDs so saved projects still recall them.

0.4.0 for MPC ships a Gen1 armv7 build and a Gen2 aarch64 build, plus one SDK zip for both. 0.3.0 for Force ships the Gen1 armv7 build Force uses. Loops and WAV exports are stored beside the plug-in, so the same folder works on any drive. Copy an older `Loops` or `Exports` folder into the new one if you already have takes. Remove a leftover copy under `Synths` while testing, or MPC may draw that older skin.

These 3.10.1 packages are draft releases until someone installs them on a device. They are not marked tested on 3.10.1. Published 0.3.9 and 0.2.10 still install through the OpenMPC Plugin Manager, which writes `plugin.json` itself. Hand-copying those older zips does not.

On the host Audio track, use Monitor MERGE and turn native REC ARM off to avoid recording to the timeline too. Use the looper touchscreen, MIDI or Q-Links. The looper follows host tempo and transport. If the host does not report a cycle range, set **Take Bars** on Mixer. Native track and pad integration stays in the optional Controls patches, which still accept only MPC One+ 3.10.0.49 and Force 3.10. MPC OS 3.10.1 mounts `/etc` read-only, so those patches will not install on 3.10.1.

| Device | MPC OS 3.10.1 draft | Published effect | Optional Controls |
| --- | --- | --- | --- |
| MPC, Gen1 and Gen2 | 0.4.0 (`mpc-v0.4.0`) | [0.3.9](https://github.com/asearan/Retrolooper-One-MPC-Force/releases/tag/mpc-v0.3.9) | [MPC 3.10.0.49 guide](docs/retrolooper-controls-mpc.md) |
| Force | 0.3.0 (`force-v0.3.0`) | [0.2.10](https://github.com/asearan/Retrolooper-One-MPC-Force/releases/tag/force-v0.2.10) | [Force 3.10 guide](docs/retrolooper-controls-force.md) |

Read the [MPC guide](docs/MPC-ONE-PLUS.md), [Force guide](docs/FORCE.md), and [build instructions](docs/BUILDING.md).

The external-plugin build for MPC OS 3.10.1 and later is [Official 3.10.1 MPC Version](<Official 3.10.1 MPC Version/README.md>). It does not use the device patches.

## On the device

| MPC One+ | Force |
| --- | --- |
| ![Retrolooper One on MPC One+, Wave view, with record, overdub, and pad controls](docs/images/retrolooper-one-screenshot.jpg) | ![Retrolooper One on Force, Tape view, with the Force pad labels](docs/images/retrolooper-force-screenshot.jpg) |
| ![Retrolooper One tape view with a recorded loop](docs/images/retrolooper-one-cassette.jpg) | ![Retrolooper One level meters](docs/images/retrolooper-one-meters.jpg) |

If Retrolooper One is useful to you, you can [support development via PayPal](https://paypal.me/adamsearan).

## Trademarks

VST is a registered trademark of Steinberg Media Technologies GmbH.

Audio Units is a trademark of Apple Inc.

Copyright © 2026 IAMTHEBAND LLC, Retroloop USA. GPL-3.0-only.
