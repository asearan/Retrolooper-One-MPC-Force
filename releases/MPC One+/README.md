# RetroLooper One — MPC One+

Hardware tested on MPC One+ 3.10.0.49

Catalog install: select RetroLooper One; the matching helper is added automatically to the same batch. Only one insert effect appears on the device. Save the project before restarting.

Direct/older-installer install: extract this complete archive and import BOTH component ZIPs into one batch. They are the effect and its background helper, not two effects. Do not mix device folders.

# RetroLooper One — MPC One+

Copyright © 2026 IAMTHEBAND LLC, Retroloop USA.

VST is a registered trademark of Steinberg Media Technologies GmbH.
Audio Units is a trademark of Apple Inc.

Plugin 0.3.6 + matching MPC integration helper 0.2.7. Hardware tested on MPC One+ with modified host 3.10.0.49, GNU build ID bc141062ea5ea5be619d98366f423c8102574365. Other host builds and Gen2 devices are not validated.

## Installation

1. Save your MPC project and connect through your existing community installer.
2. In the catalog, select RetroLooper One. The installer adds retrolooper-buttons=0.2.7 automatically. Review the pair and install in one batch.
3. Allow the required MPC application restart.
4. Add RetroLooper One as an **Insert Effect** on an Audio track. In the picker, look under VST when sorted by Type, or RetroLooper when sorted by Manufacturer.

The helper is a background integration library, not a second effect/instrument. It provides native track/editor ownership, input preparation and pad feedback. The portable VST folder and /data/mpc-addins/retrolooper-buttons are managed by the standard installers; use their uninstall commands/manager for removal.

For an offline/manual copy, the complete archive contains both standard installer ZIPs and SHA256SUMS. Extract both ZIPs. Run their install.sh scripts as root with -n to defer restarting, then restart the MPC app once after both finish. The manager route is recommended.

## First take and overdubs

1. Put audio effects you want printed into the loop **before** RetroLooper.
2. Open the intended looper instance. The helper selects its owning Audio track, prepares selected-track live input and gives its pad grid ownership. Other RetroLooper tracks' live input monitoring is closed while their loops can keep playing.
3. Use the looper's Record button or Pad A1 to cue the first take. MPC Play/Stop controls the clock; RetroLooper's touch/pads control capture. Physical MPC Record/Overdub flags do not start capture in the default Touch/Pads mode.
4. Use Overdub or Pad A3 to add layers. Native REC ARM is switched off when capture is cued to avoid recording the same input onto the timeline as well.
5. Threshold Auto Start can wait for input over the chosen level. Host sequence length is the default; /2 and x2 are available.

Bank A defaults: A1 Record, A2 Loop On/Off, A3 Overdub, A4 Mute, A5 Undo, A6 Redo, A7 Clear, A8 Reverse, A9 Stop Now, A10 Auto Start, A11 Export, A12 Sync Start, A13 Length, A14 View, A15 Input Source, A16 Loop Output. Reassign on Pads → Assign with touch drag/Q-Links; Pads → Play triggers functions. Cue states blink; active states stay bright; purple Clear is bright when a loop exists.

Track Mute and Pad Mute retain native pad behaviour. Returning to MAIN or the supported insert-editor contexts restores the looper grid. Other plugin editors retain native pads. In MAIN without an editor, the selected Audio track's first RetroLooper owns Bank A.

## Export and known limits

Export writes a numbered WAV to the internal RetroLooper Exports folder on the tested MPC. Load that WAV into your Audio track manually if you need a timeline clip. Automatic timeline bounce is pending. The looper receives audio at its insert position; monitoring and routing remain native host responsibilities, managed for the selected looper track by the helper.

The helper fails closed on an unknown host image: it does not guess native addresses. Use only the supported pair and firmware; see the root README for separate Force packages.
