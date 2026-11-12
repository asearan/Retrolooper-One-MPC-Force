# Official 3.10.1 MPC Version

Retrolooper One built with the official MPC Plugin SDK for MPC OS 3.10.1 and later. Manufacturer IAMTHEBAND LLC. Plugin id `IH97`.

This version is an external insert. It follows MPC Play, Stop, and Record, and it exports a WAV file. It does not use the optional device patches, so there is no pad page, no MIDI page, no automatic MERGE / REC ARM, and no timeline placement.

Projects saved with the earlier community builds (`RlO1` on MPC, `RlF1` on Force) do not open in this plugin.

## Install

The device must be on MPC OS 3.10.1 or later.

1. Copy the folder `IAMTHEBAND LLC - VST - Retrolooper One` into a folder named exactly `VST2 Plugins` at the root of a USB drive or SD card.
2. Insert the drive.
3. On the device, open Preferences → Enhancements (Beta) and turn on **Enable Enhancements** and **External Plugin Support**.
4. Add Retrolooper One as an insert on an Audio track. It is listed under **External Plugins**.

Set that track’s monitor to **MERGE** and leave native **REC ARM** off when the loop should record only into Retrolooper.

To replace an older copy, copy this folder over it, then remove the insert and add it again. Restart the device if the screen does not update.

## Source

`plugin/` is the SDK project: `plugin.json`, `src/`, `ui.json`, and `skins/`. It builds with the [MPC Plugin SDK](https://github.com/inmusic-software/mpc-plugin-builder-sdk):

```sh
python3 tools/build.py "plugins/Retrolooper One"
```

The ready-to-copy folder in this directory is that build. `plugin/make_skin.py` regenerates `skins/` from the original Retrolooper One art on the development machine.
