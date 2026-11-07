# RetroLooper One — MPC One+ and Force

Copyright © 2026 IAMTHEBAND LLC, Retroloop USA.

A single stereo audio-track looper: record a first take, add overdubs and keep playback aligned to the host. The product name is RetroLooper One on both devices. Each device edition has its own plugin ID and matching integration helper.

![RetroLooper One](docs/images/retrolooper-one-thumbnail.jpg)

| Device | Plugin | Matching helper | Status |
|---|---|---|---|
| MPC One+ | RetroLooper One 0.3.6 | MPC integration 0.2.7 | Hardware tested on modified MPC 3.10.0.49 |
| Force | [Retrolooper One 0.2.5](https://github.com/asearan/Retrolooper-One-MPC-Force/releases/tag/force-v0.2.5) | Force Controls 0.2.5 | Hardware preview; Bounce to Clip tested on Force 3.10 |

## Support development

If Retrolooper One is useful to you, you can support its development with an optional PayPal donation. [Donate via PayPal](https://paypal.me/adamsearan).

## Install

For direct downloads, use the clearly labeled **complete** archive for your device from [Releases](https://github.com/asearan/Retrolooper-One-MPC-Force/releases). Each complete archive contains the plugin ZIP, matching helper ZIP, README and checksums. Extract it and import both component ZIPs into one batch. The effect appears in the plugin browser; its helper runs in the background. Save your project before the installer restarts the device application.

The [OpenMPC catalog submission](https://github.com/open-mpc/mpc-vst-plugins/pull/723) pins the matching helper for each device so a catalog install can add both components in the same batch. Never exchange MPC and Force helpers.

- [MPC One+ installation and use](docs/MPC-ONE-PLUS.md)
- [Force 0.2.5 release, installation and source](https://github.com/asearan/Retrolooper-One-MPC-Force/releases/tag/force-v0.2.5)
- [Build instructions](docs/BUILDING.md)

These releases require an existing community-plugin-enabled/rooted device and the exact supported host build. They are not installers for stock firmware or Gen2 hardware.

## Features

- One stereo loop per insert, up to two minutes, with four undo levels and sequence-sized memory allocation.
- Touch and assigned pads control Record, Play/Stop, Overdub, Mute, Clear, Undo/Redo and Reverse.
- Host clock, transport and phase alignment; host loop length with /2, x1 and x2. MPC uses sequence length; Force targets the global Arrange loop.
- Optional threshold start, main-page HP/LP filters and Q-Link/MIDI assignment.
- Waveform, cassette and meter views; cue blinking and active-state colours.
- Opening an instance selects its owning Audio track and pad grid. Other recorded loops keep playing; capture is selected-track scoped.
- Native Track Mute, Pad Mute and other supported performance modes regain their normal pads and colours.
- WAV export and project recall.

**Force:** Bounce to Clip can fill or replace a Matrix Audio slot. Native Undo and Arrange range verification are pending. **MPC One+:** Export writes a WAV file for manual placement. To print insert effects into a layer, place them before RetroLooper in the channel's chain. A looper cannot record effects after its insert position.

The screenshots below are captures of the native-skin testbench, which uses the same artwork and control geometry. They are UI previews, not claims of additional hardware testing.

![Waveform view](docs/images/retrolooper-one-screenshot.jpg)
![Meter view](docs/images/retrolooper-one-meters.jpg)

## Force preview

![Force experimental preview](docs/images/retrolooper-force-thumbnail.jpg)

## License

GPL-3.0-only; see [LICENSE](LICENSE) and [third-party notices](THIRD_PARTY_NOTICES.md). No proprietary firmware or SDK is distributed. Matching Force 0.2.5 source is attached to its release.
