# Retrolooper One — Force / MPC One+ VST2 beta

Single stereo looper effect for Akai Force and MPC. On MPC OS 3.10.1 the folder goes in `VST2 Plugins` at the root of a drive, with External Plugin Support on. The MPC package includes Gen1 armv7 and Gen2 aarch64. Force uses the Gen1 build. This is a Linux `.so`, not a Mac `.vst` bundle. The optional Controls patches are installed separately and stay pinned to MPC One+ 3.10.0.49 and Force 3.10.

## Portable test for MPC 0.4.0 and Force 0.3.0

The two builds share a helper-free engine and keep their existing VST IDs. Install the effect alone for touchscreen and MIDI capture, host tempo and transport, Q-Links and WAV export. Set the Audio track's native monitor to MERGE and leave native REC ARM off if you want to record only into Retrolooper. Set **Take Bars** when the host does not supply a cycle range. For printed effects, put Retrolooper after those effects in the insert chain.

Native pad colours/actions, automatic track ownership and monitor/arm preparation, and Force Matrix bounce are opt-in Controls features. See [MPC Controls](device-patches/retrolooper-controls-mpc.md) and [Force Controls](device-patches/retrolooper-controls-force.md) for exact host compatibility, backups, and removal. These are test releases; complete the on-device checks before catalog submission. The notes below describe earlier builds.

## v0.3.6: trigger pads and explicit capture

Helper v0.2.7 adds visible-instance track selection: opening RetroLooper selects its owning MPC Audio track using the live editor's effect session and validated insert ownership. The displayed instance also owns Bank A when a track contains multiple loopers. Selection occurs on editor opening/instance changes; repeated UI polling does not override later manual track choices. Other plugin editors retain their native pads. This helper update is installed and isolated-test verified. Startup and all 18 installed helper files passed verification. The live saved-project check confirmed that opening a different instance changes native track selection and matches the physical pad owner, with selected input monitoring ready and no MPC restarts. The main plugin remains v0.3.6.

Helper v0.2.6 adds native pad-mode handoff: Track Mute, Pad Mute, Step Sequencer and other native modes receive their normal pad input and colours. Returning to MAIN or the insert editor restores the looper grid. The helper reads actual mode/popup state, so touchscreen mode changes are covered. Pending pad events cannot survive switching away and back. The main plugin remains v0.3.6. The handoff is installed on the MPC One+ and isolated-test verified. Startup and all installed helper files passed verification. Live saved-project tests confirmed native Track Mute colours/actions, Pad Mute colours, and restored looper colours/Pad 14 view switching after returning.

Pads has a PLAY mode with 4×4 trigger buttons and an ASSIGN mode with larger rotary selectors. Turn the assignment knobs or MPC Q-Links to change each pad’s function; MIDI Learn also reassigns either touch or physical pads. Assignment knobs use the standard 128-frame rotary graphics, explicit vertical touch dragging and a small hysteresis around each function boundary to reject finger jitter. Each pad's colour and action label update with its assignment. The buttons and hardware LEDs blink while cued and stay bright while active. The purple Clear button/pad stays bright when audio exists and dims when empty.

Capture defaults to Touch/Pads, including projects saved by v0.3.5 or earlier. Use RetroLooper Record or Pad A1 for a first take, and Overdub or Pad A3 for layers. MPC supplies sequence length, tempo, Play/Stop and phase. Its physical REC/OVERDUB flags do not start or end capture in this mode. Cue recording or an existing-loop overdub while stopped, then start MPC Play; the cue follows the configured timing. MPC Stop ends capture. A legacy MPC Rec mode remains only for compatibility/configuration through its existing parameter.

Helper v0.2.5 initializes the default Bank A without waiting for a bank-selection MIDI message absent from the observed One+ session. Explicit native bank button messages still switch the binding context. With no editor visible, pad controls use the selected Audio track's first RetroLooper. The helper now discovers RGB messages across the MPC controller's output streams: the real One+ sends pad RGB through a different output stream from button LEDs. It learns the actual 0x56 message header and native colours; no device ID is hardcoded. Native REC/OVERDUB no longer prepares/disarms an Audio track just because those physical buttons were pressed; RetroLooper capture cues still prepare that selected track.

Validation includes the sanitized Mac engine suite, synthetic ARM/MPC tests, two-output controller and RGB-restoration regression, layout/schema checks, and browser tests. Final updated native touch/physical-pad behaviour still requires the saved-project hardware test after installation.

## v0.3.5: touch transport, live Reverse and physical Bank A

Touch controls affect only their RetroLooper instance; MPC controls global transport. Queued Record/Play/Overdub/Mute blink and stay highlighted while active, including muted playback. Record on an existing loop toggles overdubbing. Touch End Dub and Stop remain effective across host-position rebases. A first-take cue survives a temporary track-bridge refresh, while actual capture still waits for confirmed ownership and native input readiness.

Reverse now edits both channels during playback without resetting the sequence phase. The worker keeps an immutable playback snapshot audible during the edit. Undo/Redo remain available. Finish recording/overdubbing before reversing. The Tools tab is removed; Pads holds physical assignments, Mixer holds WAV slot/import/export, and the main page retains Reverse and Export. Existing edit/MIDI parameter indices remain available.

This release pairs with **RetroLooper MPC Buttons 0.2.4** for the MPC One+ firmware build already validated here. Press physical Bank A after startup. Pads operate the first RetroLooper insert on the selected Audio track, with matching function colours and dynamic pad-number badges on the main buttons. Other banks keep their native behaviour. The Pads page supports touch/Q-Link reassignment; on MIDI, select a function, enable Learn and press a physical Bank A pad to assign it. External MIDI bindings are independent. Assignments save with each instance.

Default pads, numbered from the bottom-left hardware pad: 1 Record, 2 Play/Stop, 3 Overdub, 4 Mute, 5 Undo, 6 Redo, 7 Clear, 8 Reverse, 9 Stop Now, 10 Auto Start, 11 Export, 12 Sync to Start, 13 Sequence Length, 14 View, 15 Input Source, 16 Loop Output. Pad functions use the existing action semantics: triggers fire once, options cycle, and continuous values step per press. Colours identify functions; queued modes blink and active modes stay bright.

The helper learns the real controller's pad-colour message header and restores its cached native colours when looper pad control is inactive. Physical LED output and the final Bank A mapping still require the hardware test; the parser, queue, MIDI preservation and colour restoration are tested with a fake controller. Missing LED output reports LED WAIT instead of guessing a controller model. No host executable or firmware patch is installed.

## v0.3.4: Auto selectors and selected-track input monitoring

The front page uses explicit **AUTO OFF / AUTO ON** selectors. These use the same native grouped controls as the working view selectors, replacing the single Auto Start button that did not latch on the MPC. MIDI Learn uses explicit Off/On selectors for the same reason.

This update requires **RetroLooper MPC Buttons 0.2.3**. Selecting an Audio track with RetroLooper prepares its native MPC Monitor to **IN**, and sets monitoring **OFF** on other Audio tracks containing RetroLooper. Live input therefore follows the selected track. Stored loops and native timeline clips retain their normal playback path; the plugin preserves the supplied insert buffers instead of silencing unselected tracks' complete audio. Input/output port assignments and tracks without RetroLooper remain unchanged.

Companion 0.2.2 also recognizes MPC’s saved MERGE mode, which 0.2.1 incorrectly rejected for all instances. A manual Monitor OFF/AUTO/MERGE choice on the stable selected track is respected until selection changes or a Record/Overdub cue prepares input again. The native REC ARM Off preparation still applies only to the selected owning track. Capture waits for both native disarming and input-route acknowledgement. Native monitoring changes are restricted to the fingerprinted MPC One+ 3.10.0.49 model and execute on its UI thread through the host's locking, notifying property setter. The live Project-owned track list, owner chains, enum bounds and native mutex layout are checked before any monitor changes.

Companion 0.2.3 corrects the native IN/AUTO mapping: OFF=0, IN=1, AUTO=2, MERGE=3. The IN value was confirmed by explicitly choosing IN on the hardware and reading the native property. The earlier 0.2.1/0.2.2 companion selected AUTO when capture was cued, which could cut live input with native REC ARM Off. The routing regression now checks the confirmed raw IN value independently of the implementation enum. Four-track selection handoff and the Auto ON selector have been observed on hardware; capture with corrected IN and selected-instance isolation subsequently passed the live four-track test, and the new layer repeated cleanly. This prepares MPC's native input route; it does not introduce a separate audio-device capture path while the native monitor remains OFF.

## v0.3.3: threshold first-take start

The front page adds **AUTO START** and a **THRESHOLD** knob, adjustable from -72 to 0 dBFS (default -36 dBFS). Auto Start defaults **Off**, including when loading older projects. Input Gain remains on Mixer. The sequence/take-length readout moves into the status row; HP, LP, loop level, Reverse and the existing actions remain on the main page. Auto Start and Threshold are available through the LOOPER Q-Link bank, host automation and MIDI Learn. There are now 51 visible MIDI targets and 58 stored binding slots; released parameter and binding indices stay unchanged.

With Auto Start On, physical MPC REC or RetroLooper's REC ARM arms an empty first take. It waits while MPC is stopped or the selected capture input stays below threshold, then starts at the first qualifying input sample while MPC plays. Input choice and Input Gain apply before detection. It records the current sequence length and /2, x1 or x2 ratio, writing audio directly at its sequence phase. An interrupted first take retains the full musical window with silence in the unrecorded portion. Stop/restart, saved audio and exported WAV therefore retain their sequence alignment. No second loop-sized audio buffer is added.

Existing loops continue overdubbing under MPC Record/Overdub; threshold detection does not chop up later layers. Selected-track ownership, native arm-off preparation and the explicit REC requirement remain in force. This uses audio supplied to the insert by MPC; native Monitor-OFF input capture remains unfinished.

Validation: the complete sanitized Mac suite passes, including exact onset/phase, stop/restart, partial-take silence, nonfinite/unselected-input rejection, gain-zero protection, recall and v0.3.2 project migration. The isolated ARM host on the MPC passes exact sample onset/phase and selection-loss capture closure, alongside the four-instance host tests. The native layout has 82 touch targets of at least 56 pixels and passes schema checks. Browser controls also pass a generated-input threshold test. The running MPC project has not yet tested this version's physical input/touch behavior.


## v0.3.1: selected Audio-track capture and native REC ARM

This build pairs with **RetroLooper MPC Buttons 0.2.0**. On the MPC One+ running the fingerprinted 3.10.0.49 build, the companion follows the live selected Track and walks its actual insert chain to the owning VST instance. It does not use editor focus, recent control touches, or the last saved project. Only that Audio track's RetroLooper may capture; switching away closes its capture and cancels queued recording/overdub cues while loop playback continues. Selecting an existing loop during MPC Record/Overdub hands capture to that loop; an empty loop still needs an explicit REC action.

Cueing RetroLooper Record/Overdub requests native **REC ARM OFF** on its own selected Audio track. Physical REC and OVERDUB are delivered after a bounded wait for native UI preparation (at most 80 ms); controller bytes and mappings remain unchanged. On-screen/MIDI capture waits for the arm-off acknowledgement. Monitoring and routing remain with MPC; use **Monitor IN** for live input. Native REC ARM stays Off until you change it. Other tracks' arm settings are not targeted.

Native model changes run on MPC's UI queue and use its notifying record-arm property setter, including its filter. Native calls are fingerprinted to this exact firmware build. Missing, unsupported, stale or mismatched ownership prevents capture, preserving playback. **Force/other firmware native selection and disarming remain unvalidated.** The main plugin keeps its strict single VST export; the companion exports only its seven intended controller/track functions.

Validation: four-instance capture/handoff/cue cancellation and disarm-before-capture acknowledgement pass the sanitized native host and isolated ARM host on the MPC. Read-only checks on the live MPC confirmed selection changes and exact track-to-instance ownership. The model tests check the native setter payload, stale snapshots, bad ownership/bounds and queue callback lifetime. Version 0.3.1 and companion 0.2.0 are installed on the MPC One+; native UI queue execution is confirmed with four instances loaded. A physical REC press while stopped changed the selected Audio track's native REC ARM from On to Off, confirmed both on screen and by the native model. MPC remained active without a restart. The selected instance entered Overdub while the other three played; the subsequent host-position interruption is addressed in the staged update below.

## v0.3.2 installed update

An existing loop now resumes automatic overdubbing after a host-position jump while MPC remains in Record. Previously, repositioning restarted loop playback but capture waited indefinitely for another recording or selection transition. Empty loops still require an explicit REC action. Selection/preparation messages now clear when their cause is resolved, while unrelated edit/file messages are preserved.

The sanitized Mac suite and isolated four-instance ARM test on the MPC pass, including a forced position jump followed by continued overdubbing across sequence wraps. Native skin, touch-target and schema checks pass. Version 0.3.2 is installed on the MPC One+; all 177 payload files match, user audio and other plugin registrations are preserved, and MPC restarted successfully. It uses the already-installed companion 0.2.0 and preserves parameter indices, MIDI bindings and saved loop format.

The main page includes a dedicated **Reverse** button beside Undo/Redo, Mute and Export. Reverse also appears in the LOOPER Q-Link bank and retains its existing MIDI assignment and stopped-loop edit behavior.

To print track insert effects into each new layer, place those effects **before RetroLooper** and make RetroLooper the final insert. Its recorder receives their processed output; previously recorded loop material leaves RetroLooper after those effects, so later changes do not reprocess old layers. Effects after RetroLooper remain live output effects and are not printed into its recorder. Automatic insert reordering is not implemented.

Recording live input with the MPC track's **Monitor OFF remains unimplemented and unverified**. The current insert records only the buffers supplied by MPC. The native routing investigation has not established a supported input tap before the monitor gate. This build does not silently enable Monitor IN, open a second hardware audio device, or claim to capture downstream effects.

## v0.3.0: host-driven audio-track workflow

The main page keeps Record Arm, Loop On/Off, Overdub, Stop, Clear, Undo/Redo and Mute, with **Input, Loop HP, Loop LP, Loop Level and Export**. HP, LP, input gain and loop level occupy the first Q-Link bank. Plugin tempo, click, input monitoring and A/B crossfader controls are retired from the interface and MIDI Learn picker. Their VST indices and legacy binding slots remain reserved so old projects retain their other assignments. There are 49 visible MIDI-learn targets and 56 stored binding slots.

**Sequence** launch is the default: playback follows the MPC sequence's phase, with /2, x1 or x2 determining its musical length. **Next bar** waits for the next host bar boundary and launches loop sample zero there. Continuous playback uses sample time between genuine transport changes; PPQ is no longer folded back to one bar on every callback. This fixes fresh multi-bar takes repeating only bar one and prevents small host PPQ jitter from shifting the audio each block.

Existing loops automatically conform to the sequence/ratio as well as new takes. Increasing length repeats completed, grid-aligned material; incomplete or unquantized takes are padded with silence. Reducing length selects a shorter window. The full source is retained in project state, so increasing the sequence again can recover the material outside that window. Resizing runs on the worker. An immutable playback snapshot covers buffer growth and the engine catches up its sample clock afterward. Length changes wait until active capture finishes. Audio/history capacity still follows actual loop needs, rather than reserving two minutes for every empty instance.

HP and LP are smoothed stereo 12 dB/octave filters, logarithmic from 20 Hz to 20 kHz. Both default to **OFF**. They affect loop playback and exported audio; recorded audio and the MPC's dry monitoring stay editable and independent. Filters, launch mode and the other visible functions support MIDI notes/CC as well as Q-Links.

**Export** works while playing or stopped. It snapshots the active loop and writes a stereo PCM24 WAV including the loop filters, loop/master gain and pan. Over-full-scale renders are scaled uniformly to prevent new clipping during PCM conversion. Files appear in the `Exports` folder next to the plug-in (`Loop-NN.wav`), on whichever drive holds `VST2 Plugins`. The next free slot is selected without overwriting an existing file. Tools can import those files; older files in the plugin's `Loops` folder remain importable. The ARM directory-creation and existing-file metadata failures in earlier WAV exports are fixed using POSIX filesystem calls.

The standalone effect saves a WAV for manual import. On the supported modified MPC One+ 3.10.0.49 host, optional **MPC Controls 0.2.13** can also place the loop at bar 1 of the selected Audio track. Stop MPC and use an empty destination range. Native MPC Undo removes the inserted region.

For plugin-only audio capture, the intended MPC routing is **Audio track Rec Arm Off / Monitor In**, leaving the live input connected to the insert while preventing native clip capture. The user confirmed that this routing captures only in RetroLooper and that v0.3.0 loop audio plays cleanly. v0.3.1 adds the live ownership gate and automatic native REC ARM preparation described above; its native UI actions remain to be tested.

Native diagnostics expose a hidden Clock Health readout: `S` counts detected seeks, `D` counts uncovered processing-lock misses, and `E` is peak PPQ prediction error. These are diagnostic counters, not timing controls.

## Try it on this Mac

Open **http://127.0.0.1:8765** while the local preview server is running. It displays the generated native touchscreen skin and connects its controls to the actual C++ VST2 processor, compiled for this Mac. Click **Record test loop**, **Add overdub**, and **Render / listen**. Use the on-screen buttons, knobs, and timing pages; try **Save project snapshot** followed by **Reload snapshot**. Recalled audio starts stopped.

This is an offline test host: audio advances when you render or press a control. It generates a pulsed stereo tone instead of recording a microphone. The browser plays the resulting audio with visual frames sampled from the C++ engine at 100 ms intervals; its Waveform, Cassette and Meters selector uses the same image strips as the native skin. It does not emulate Akai firmware, hardware CPU load, audio routing, touchscreen behavior, or physical Q-Links. The browser adds touch hit areas, rotary knob gestures and live values. Drag a knob rim in a circle, or drag up/down through its centre; Shift gives fine adjustment, double-click restores its default, and arrow keys adjust it in steps. Home/End select the range limits. Knob artwork and readouts follow during the drag; the offline engine receives the final value on release. The preview simulates MPC transport, Record/Overdub state and sequence cycle positions; Host Sync defaults On. The MIDI test controls send actual VST2 note/CC events for testing assignments; send CC 0 then 127 when testing trigger buttons.

Restart the preview from this directory:

```sh
python3 tools/preview_server.py
```

If the Mac library needs rebuilding:

```sh
python3 build.py preview
python3 tools/preview_server.py
```

## Included

- The existing RetroLooper C++ engine: stereo record, play/stop, overdub, mute, four undo/redo levels, clear, stop now, and sync to start.
- **120 seconds maximum first-pass recording**, selected for these devices' memory budget. Overdubbing can continue indefinitely. Recording closes automatically at the limit.
- A touch-first interface across five pages: Looper, Mixer, Timing, Pads and MIDI. Version 0.1.6 uses the supplied Akai concept/sprite artwork: a cream cassette with rainbow stripes, neon red/green/amber/cyan/purple transport, cyan-ring knobs, dark metal panels, and rainbow RetroLooper branding. The cassette and transport glyphs are extracted from the supplied images; captions, bezels and knobs are rebuilt at native resolution. Dynamic native values use the Akai host's Roboto font.
- Primary transport buttons are **240 × 88 pixels**; settings targets are at least **56 pixels** high. Control labels and live values are at least 22 pixels. The home page shows host sequence length, Wave/Tape/Meter selection and /2/x1/x2; Record Arm, Loop On/Off, Overdub, Stop Now, Clear, Undo/Redo, Mute and Export; Threshold, Loop HP, Loop LP and Loop Level. Stereo input/output meters remain visible in every mode. Mixer, Timing, Pads and MIDI hold the other controls.
- The native skin and browser preview share one geometry manifest, including every hit area, image strip and readout. **Fit / 100%** in the preview lets you inspect the layout at its native 1280 × 628 plugin-area size. The surrounding Akai host uses the rest of the display; [Akai lists the MPC One+ display as 1280 × 800](https://support.akaipro.com/en/support/solutions/articles/69000831945/).
- **Waveform / Cassette / Meters** selector on the Looper page, also assigned to a Q-Link and saved in the host project. Waveform shows the complete loop's signed min/max envelope and a moving playhead. Cassette uses the supplied textured shell, a live 48-column cyan waveform, amber playhead, 192-frame circular hubs, independent supply/take-up rotation at equal linear tape speed, smooth reel faces without tape lines, tape paths or guide rollers, and the reference cassette's side-A graphic. Meters retains the shared focus ballistics, slow peak hold and 2.2-second clip latch in the reference's green/yellow/red LED colors. Stereo input/output peak and RMS strips are always visible on the Looper page. The focus meter uses the main app's shared `TrackMeterBallistics` class. Reels turn in the same direction, continue across loop boundaries and while muted, and freeze when stopped. Tape packs use a long virtual tape and keep a small visual reserve in long sessions; they never impose a tape-end condition or change the audio. Clearing the loop clears its waveform. Existing v0.1.0 projects open with Waveform selected.
- Input gain (0–4), loop level, pan, master level, stereo/left/right/off input, channel/off loop output, overdub feedback, loop crossfade, HP/LP filters and threshold start. MPC supplies monitoring, tempo and click; retired standalone controls remain only as compatible legacy parameter slots.
- Host tempo/transport following, time-signature following, Immediate / Bar / Loop timing, per-action timing overrides, Record Mode, Mute Mode, and Play / Overdub after recording.
- Live Reverse, stopped Half/Double **length**, Trim/Restore one bar, and Set Bars (1–64, within 120 seconds). Half/Double match the main plugin's bar-length editing; they are not tape-speed controls.
- **All 52 visible MIDI targets are MIDI-learnable and host-automatable**, including transport, levels, pan, routing, HP/LP filters, launch mode, timing, view, stopped edits, WAV slots/import/export and mapping controls themselves. On MIDI, select a target with its knob/Q-Link or Prev/Next, choose a note gesture, enable Learn and send a note or CC. CC controls span the parameter's entire range (0–127); CC triggers fire once on a 0–63 to 64–127 rising edge. Notes trigger actions, toggle on/off settings, cycle options/integers and step continuous controls by 1/127 of their range. Tap-only notes fire on note-on; double tap and hold need note-off. One CC or note+gesture owns one target per instance; relearning moves that assignment. Notes/CCs accept all MIDI channels. Defaults remain notes 36–42 and CC 20–26 for Record, Play/Stop, Overdub, Mute, Undo, Redo and Clear. Readouts are display-only. Host page tabs remain host navigation.
- Lossless stereo float audio, controls, and all MIDI bindings in version-2 host project chunks. Version-2 projects with 167 parameters from v0.1.3/0.1.4 retain audio, controls and mappings. Version-1 projects from v0.1.0–0.1.2 migrate their controls and original seven-action bindings. Checksummed state rejects malformed/truncated payloads. Undo history is not serialized. Opening a project restores loops stopped.
- WAV export writes stereo PCM24 to the next available slot in the `Exports` folder beside the plug-in, while playing or stopped. Import accepts mono/stereo PCM16/24/32 or IEEE float32 WAV and resamples off the audio callback; legacy files next to the `.so` remain importable. Import and destructive length edits require the loop to be stopped.

## Hardware adaptations and limits

The native skin recreates all three visual views using parameter-driven PNG filmstrips. The complete 256-bin engine envelope is folded into 48 signed waveform columns (33 amplitude steps), rather than running the desktop's custom renderer. Visual data updates at up to 10 Hz; only changed frames in the selected view plus the always-visible stereo meters are reported to the host. Knobs use 128-frame strips and explicit native vertical touch dragging; unused generated images are removed, including the retired crossfader. The conservative all-images-cached memory estimate is recorded in `preview/resource-audit.json`; this includes the five page backgrounds and controls as well as the three views. Actual UI/CPU and memory use still need a device check.

Alternate desktop themes, keyboard shortcuts, editable text names, desktop file pickers, MIDI clock output, and additional desktop audio buses are not reproduced. Use the Akai host for hardware routing and track names. The main plugin's retired Ableton Link control has no Link runtime to port; this version follows the host clock.

The shared engine still contains eight internal track slots for source consistency, but only track 1 is addressable here. Version **0.2.2** removes the two-minute startup allocation. MPC locks process allocations into physical RAM, so the older assumption that unused history buffers would stay uncommitted was incorrect. Empty instances now prepare only engine/scratch storage. Once the host supplies a valid sequence length, a worker reserves live stereo audio and all four undo/redo levels for that length and the selected /2, x1 or x2 ratio. Capture waits for preparation and then starts at a sequence boundary; allocation never runs in the capture callback. At 44.1 kHz, the audio/history payload is approximately **13.5 MiB for a two-bar, four-second take at 120 BPM**, versus roughly **404 MiB reserved at startup by the old build**. These figures exclude the engine, graphics and MPC itself. Longer takes still need proportionally more memory, up to the existing 120-second limit. Background growth checks available RAM and leaves 128 MiB for the host. Storage is reused within an instance and released when the instance is removed; Clear retains undo/reusable storage. Explicit legacy free-running mode still prepares the two-minute profile because it has no sequence length. This beta accepts 44.1/48 kHz.

Version 0.1.7 uses MPC OS 3.x knob data version 5 so native touch-drag direction is explicit. This package targets MPC OS 3.x (including the tested MPC One+ 3.10); it is not marked MPC OS 2.x compatible. The v0.1.6 legacy-format package remains available separately. This structural check does not establish compatibility with every Force firmware. **Loading, touch/Q-Link behavior, sound, realtime deadline, project persistence and memory pressure still require testing on both of your actual devices.** The initial MPC One+ checks are listed below; Force validation and CPU benchmarking remain pending.

## Device package

`dist/RetroLooper-One-0.3.6-mpc-armv7.zip` is the current MPC OS 3.x beta package, including the Akai sprite-based UI and complete MIDI mapping. It contains the ARM library, native skin, Q-Link maps, project metadata, checksums, and the community project's install/uninstall scripts. Use your existing community plugin installer to review/install the zip, or follow the included `INSTALL.md`. The installer backs up `MPC.settings` and restarts the MPC host. Do not install the Mac preview `.dylib` on the devices. This folder does not automatically install to hardware.

Start hardware testing with one empty instance in a disposable project: verify monitor/routing, make a short stereo take, dub/undo/redo, stop and save/reopen, then test quantization with the host transport and Q-Links. Check CPU/memory before extending to long takes or several instances. Report the exact Force and MPC One+ firmware versions with those results.

## Rebuild and verify

Build requirements: Python 3.9+, Clang/Xcode command-line tools for native tests, Pillow for skin generation, and **Zig 0.13.0** for cross-compilation. The parent repository supplies the engine, shared meter-ballistics header, fonts and Signalsmith headers. No JUCE or Steinberg SDK is needed for this target.

The community tools are pinned to `open-mpc/mpc-vst-plugins` commit `0dc17120cd7e952e95eeecdd820ac9e495a75ee9`. Clone/check out that revision and pass its path:

```sh
python3 build.py test --sanitize
python3 build.py arm --zig /path/to/zig
python3 build.py skin --kit /path/to/mpc-vst-plugins
python3 build.py package --kit /path/to/mpc-vst-plugins
```

Cross target: ARMv7 Cortex-A17 hard-float, glibc 2.28 minimum, exported `VSTPluginMain` only. Dependencies and symbol versions are checked by the community catalog checker. MPC One+ library loading and the isolated ARM audio test are verified below; full live routing, Force loading and performance testing remain pending.

`build.py` copies the current shared sources into `build/engine/`, applies asserted adaptations for the 120-second cap, sequence-sized track-1 allocation, off-callback storage preparation, track-1-only stretcher preparation and ARMv7 denormal handling, and records source hashes and exact adaptations in `build/source-manifest.json`. The main app's meter-ballistics header is copied unchanged into the build. Parent files are read only. `params.lock` prevents changes to existing automation/skin indices. Mac, AU and VST3 builds are unchanged.

## Attribution

The VST2 ABI declarations, native skin tooling and package installer come from the MIT-licensed [mpc-vst-plugins project](https://github.com/sd88me/mpc-vst-plugins) at the revision above; see `vendor/mpc-vst-plugins-LICENSE`. Signalsmith Stretch and Signalsmith Linear retain their upstream MIT licenses, included in the package. RetroLooper source/artwork retain the parent project's ownership; this port does not relicense them.

Official Akai support does not provide a general third-party standalone-plugin install route; this beta targets the existing community setup you confirmed.

## MPC One+ test status (4 November 2026)

Version 0.1.7 was installed directly on the MPC One+ running Hakai 3.10.0.49, with the previous version and settings backed up. All 188 installed files match the release, saved loop files were preserved, and other plugin registrations are unchanged. MPC restarted and its log confirms initialising 0.1.7.0; a device screenshot confirms the full editor renders. A separate process on this MPC passed synthetic stereo recording/playback, overdub/undo, stop/resume with Host Sync On and valid host clocks both stopped and playing. This did not drive ALSA or change the user's project. Physical touch/Q-Link feel, real-source monitoring/routing, MIDI into the MPC effect host, project recall and CPU benchmarking remain unverified. See `diagnostics/update-017-native-tests.log` and `diagnostics/update-017.json`.

To feed internal instruments and external inputs to a single looper, route the instrument track and a monitored external-input Audio track to the same Submix (e.g. Sub 1), and place the one RetroLooper insert on that Submix. Route the Submix to the main outputs. Verify RetroLooper's input meters move before recording.

## Native controls in v0.1.7

On the MPC/Force touchscreen, drag up/down to turn a knob; Q-Links keep the same parameter mapping. Rotation uses 128 frames. Double-tap opens Akai's native knob overlay. The Mac browser also offers circular mouse dragging, Shift fine adjustment and double-click reset; those browser gestures do not replace Akai's native firmware gestures. Existing project parameter indices, MIDI assignments and the 120-second/four-undo recording profile are unchanged.

## Recording fix in v0.1.8

A native host trigger ignores a second press of the same button within 250 ms, matching the pinned community wrapper. One physical MPC touch can arrive as two high values 30–140 ms apart; previously the second could close the recording after the first started it. Releases still pass, the next deliberate tap works, and mapped MIDI events retain sample-offset timing. This guard covers all momentary host buttons.

RetroLooper is a stereo channel insert. It records the left/right buffers supplied by the channel it is inserted on and returns monitoring/loop playback to that same channel's left/right output buffers. The host controls that channel's destination. No direct hardware device or MPC master output is opened. The output option is now labelled **Channel** (previously Main); its parameter index and saved value are unchanged. Input defaults to Stereo, output to Channel, and Host Sync stays On.

## Historical v0.1.9 behavior (superseded by v0.3.0 above)

The primary workflow is a stereo insert on an MPC Audio track. Host Sync defaults On. **Capture Control = MPC Rec** is the default: the VST host's Record/Overdub flag arms an empty take at the next sequence-loop boundary, captures one loop at the selected length, and continues overdubbing while the host remains in Record. Leaving Record returns to playback. MPC Stop stops the loop; MPC Play resumes it at the host position. The plugin's local buttons can enable/mute the loop or arm capture but never start the MPC transport. Timing's Manual capture option keeps plugin-button arming while still following MPC Play/Stop. The legacy Host Sync Off parameter remains for older projects and offline diagnostic tests; it is no longer a front-panel toggle.

The host's valid VST cycle positions supply sequence-loop length. Timing offers **/2, x1, x2** (default x1); these select the next take length. A 2x take continues across a normal MPC sequence wrap. Existing audio is not destructively resized by changing the ratio; Tools retains explicit stopped-loop length edits. Partial sequence loop regions use the reported loop-region length. If cycle positions are unavailable the plugin waits and displays WAIT MPC LENGTH; it never guesses four bars. Takes over the agreed 120-second limit are refused. Native Stop during a take closes it early. Keep MPC looping for takes longer than one sequence pass.

In Host Sync mode the channel's incoming stereo buffers pass through at unity, so MPC channel monitoring, gain, mute and output routing remain with MPC. Plugin input selection/gain applies to captured audio; loop level/master/pan/output apply to the stored loop. The plugin's own monitor and metronome controls are retained only for legacy free-running operation and are replaced by CHANNEL/MPC CLICK indicators on the panel. The plugin does not turn the MPC's metronome on/off or bypass the MPC's monitor setting.

An insert captures exactly the stereo audio MPC supplies: VST2 does not expose separate existing-clip/live-input streams or per-channel monitor/record-arm settings here. Use the MPC channel's monitoring/routing so it feeds the audio you want layered. RetroLooper does not feed its own loop output back into its recorder. Whether this firmware exposes the global Record flag per armed audio track, and whether both physical REC and OVERDUB set it, must be verified on the device; the Manual capture option is available without independent playback timing.

Validated using the pinned community MPC transport protocol (tools/docs/MIDI_TIMING.md): loop flags, PPQ cycle positions, real-host jitter tolerance and non-interruption at normal wraps. The saved-project format accepts v0.1.8's 169 parameters/51 MIDI targets and appends the new controls without moving prior indices.

## v0.1.10: physical REC first take and smooth progress

Press the **MPC's physical REC** to arm the first take. MPC Play/OVERDUB alone cannot create a take from empty. Armed capture waits for the next MPC sequence boundary and records the selected /2, x1 or x2 length; later overdubs follow the MPC recording flag automatically. The plugin's own REC ARM/MIDI Record can also arm explicitly. VST2 combines physical REC and OVERDUB into one flag, so the physical first-take distinction uses the small **RetroLooper MPC Buttons** companion in `dist/RetroLooper-MPC-Buttons-0.1.1-mpc-armv7.zip`. Install it alongside the plugin; Timing shows **REC BUTTON READY**. Without it, the on-screen/MIDI Record fallback is available but physical first-take arming is not. This supersedes the automatic empty-take arming described for v0.1.9.

The companion observes the controller's Private input after MPC reads it. It preserves every message and error unchanged, does not remap buttons, writes no MIDI and accesses no audio. It exposes an atomic REC-press counter to the plugin. The shared community addin installer extends the existing preload setting, backs it up, reloads systemd and restarts MPC. Removal is available through `/data/mpc-addins/retrolooper-buttons/uninstall.sh` and does not delete plugin loops. The controller map is documented by [Hakai](https://github.com/henning/Hakai-MPC) and the passive observer approach by [Skipback](https://github.com/sd88me/mpc-addin-skipback); those applications are not installed by this package.

Playhead strips now have 192 positions, shared in vertically tiled segments to stay within native texture-height limits. Position and the 192-frame cassette wheels invalidate at about 30 Hz; text, waveform and meters keep their lower cadence. This does not request full editor rebuilds at 30 Hz. First-pass progress advances against the target capture length. The MPC One+ user confirmed native transport/Overdub response and sequence length in v0.1.9. Physical REC differentiation and the new 30 Hz hardware feel require the corresponding device check.

## v0.1.11: value-driven native animation

MPC's live value cache now includes the visual parameters, matching the pinned community wrapper. They remain display-only in the processor: writes from host automation cannot change visual/engine state. The playhead and cassette wheels publish near 30 Hz, and their strips contain 192 positions. Clock, binding and sequence labels now publish changing normalized values to invalidate their native value labels. Periodic full editor rebuilds are removed; UpdateDisplay is reserved for state/message changes after control interaction settles. This follows the community wrapper's documented value-driven widget path and avoids the preset-popup/Q-Link interruption caused by periodic UpdateDisplay calls. Native hardware smoothness still requires direct observation.

Companion 0.1.1 has a strict five-symbol export audit. The isolated private-controller pass-through test and native REC BUTTON READY check pass, and MPC starts stably with the corrected helper. The early 0.1.0 companion is quarantined under dist/retired and must not be installed.
