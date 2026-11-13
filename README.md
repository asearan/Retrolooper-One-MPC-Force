# Retrolooper One for MPC and Force

MPC OS 3.10.1 loads third-party VST2 plug-ins from a folder named `VST2 Plugins` at the root of the internal drive, an SD card, or USB. Turn on Preferences → Enhancements → Enable Enhancements and External Plugin Support. Each plug-in folder needs Akai's `plugin.json`. Loops and WAV exports stay beside the plug-in. Remove a leftover copy under `Synths` while testing, or MPC may draw that older skin.

The external-plugin build for new projects is [Official 3.10.1 MPC Version](<Official 3.10.1 MPC Version/README.md>). Manufacturer IAMTHEBAND LLC, plugin id `IH97`, Gen1 and Gen2 in one folder. It does not open projects saved with the earlier community ids, and it does not use the device patches. The catalog draft is `mpc-sdk-v1.0.0`.

Saved projects still need the community ids. 0.4.0 keeps `RlO1` for MPC (Gen1 armv7 and Gen2 aarch64). 0.3.0 keeps `RlF1` for Force (Gen1 only). Copy an older `Loops` or `Exports` folder into the new one if you already have takes. These drafts are not marked tested on 3.10.1. Published [0.3.9](https://github.com/asearan/Retrolooper-One-MPC-Force/releases/tag/mpc-v0.3.9) and [0.2.10](https://github.com/asearan/Retrolooper-One-MPC-Force/releases/tag/force-v0.2.10) still install through the OpenMPC Plugin Manager, which writes `plugin.json` itself. Hand-copying those older zips does not.

On the host Audio track, use Monitor MERGE and turn native REC ARM off. Use the looper touchscreen, MIDI or Q-Links. The looper follows host tempo and transport. If the host does not report a cycle range, set **Take Bars** on Mixer. Optional Controls stay pinned to MPC One+ 3.10.0.49 and Force 3.10. MPC OS 3.10.1 mounts `/etc` read-only, so those patches will not install on 3.10.1.

| Device | New projects on 3.10.1 | Existing projects | Optional Controls |
| --- | --- | --- | --- |
| MPC, Gen1 and Gen2 | Official 1.0.0, id `IH97` | 0.4.0, id `RlO1` (`mpc-v0.4.0`) | [MPC 3.10.0.49 guide](docs/retrolooper-controls-mpc.md) |
| Force | Gen1 build of the official plugin is not a Force release | 0.3.0, id `RlF1` (`force-v0.3.0`) | [Force 3.10 guide](docs/retrolooper-controls-force.md) |

Read the [MPC guide](docs/MPC-ONE-PLUS.md), [Force guide](docs/FORCE.md), and [build instructions](docs/BUILDING.md).

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
