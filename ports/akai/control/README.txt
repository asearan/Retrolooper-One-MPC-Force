RetroLooper One Controls (MPC) and Track companion 0.2.7

For RetroLooper One 0.3.6 on the MPC One+ with fingerprinted MPC 3.10.0.49.
It observes physical REC (channel 1 note 73) and OVERDUB (note 80), preserves
native transport bytes/mappings, and reads/writes no audio. Physical REC still has its
own atomic counter, distinct from MPC's combined recording flag.

The worker finds the native Project and UI queue by validated type signatures.
UI callbacks follow the current Audio track's real insert chain to its exact
RetroLooper AEffect. No editor-focus or saved-project selection heuristic is used.
The audio callback reads only published atomic status and the persistent Project's
selected-track pointer; it performs no file/network I/O or native model changes.

When capture is cued, native REC ARM is switched Off on that selected owning
Audio track using MPC's filtered, notifying property setter on the UI thread.
Touch/pad capture requests wait for preparation. Physical REC/OVERDUB alone
do not change native track arm or monitoring. With a missing/stale/unknown bridge, the plugin waits to
capture and its loops keep playing. REC ARM stays Off until manually changed.
Other tracks' arm settings are not targeted.

Native input monitoring follows the selected Audio track among tracks containing
RetroLooper. On selection or a capture cue, that track is prepared with Monitor
IN; the other RetroLooper Audio tracks use Monitor OFF. Saved MERGE settings are accepted. A manual OFF/AUTO/MERGE choice
on the stable selected track is retained until a new selection or capture cue.
Tracks without RetroLooper and their monitor settings are not targeted. Input
ports, output routes, native timeline clips and loop buffers are unchanged.

The pinned native modes are OFF=0, IN=1, AUTO=2, MERGE=3. Version 0.2.3 corrects
the IN/AUTO mapping used in 0.2.1/0.2.2. With native REC ARM Off, preparing AUTO
instead of IN could remove the input exactly when capture was cued. The routing
regression uses the human-confirmed raw IN value to guard against that mistake.

Opening a RetroLooper insert editor selects its owning native Audio track.
The helper reads the live touchscreen overlay child list and EditEffectSession,
then validates its AudioProgram, insert slot, VST identity and unique ownership
in the current Project's TrackPool. Selection uses MPC's own notifying property
setter on the UI thread. It happens once when an editor opens or changes
instance, without repeatedly overriding a later manual track selection.
Hidden editors, other plugins and deleted/ambiguous owners never select a track.

Pad Bank A controls the visible RetroLooper instance, including a second insert
on the same Audio track. MAIN with no editor uses the selected Audio track's
first RetroLooper. Viewing a different plugin releases the looper pad grid.
Bank A is initialized at startup; explicit bank button messages change banks.
Per-instance assignments are stored in the plugin project chunk. The plugin's
MIDI Learn can assign a physical Bank A pad directly; ordinary external MIDI
bindings remain separate. Pads and touchscreen controls use matching colours.
Queued Record/Play/Overdub/Mute blink, and active modes remain bright.

Version 0.2.6 reads the native EngineMode and active popup, including changes
made on the touchscreen. Bank A belongs to RetroLooper on MAIN or while the
insert editor is open from MAIN, Track Mixer, Track View or FX Mode. Native
Track Mute, Pad Mute, Step Sequencer, Browser, MIDI Control, pad performance
overlays, other modes and unknown UI contexts retain their native pads and
colours. MAIN and the insert editor restore the looper grid automatically.
Queued pad actions are invalidated across mode changes. Loop playback and
recording are unaffected by this pad-ownership handoff.

Mode metadata is read only by the MIDI and LED workers. The audio callback
uses expiring atomic context data; it performs no native memory-file reads.

Only owned pad attacks/pressure have their final velocity byte changed to zero
before MPC receives them. Status, note, byte count, split reads and running
status are preserved, avoiding a duplicate native pad action. Every other bank,
track, native transport button, input byte and error remains untouched.

The helper observes the MPC controller outputs' pad-colour messages
to learn its actual SysEx model/header and restorable native colours. A worker
updates only these known RGB pad messages, and restores the cached MPC colours
when looper control is inactive. No ALSA writes occur on the audio callback.
If the native output/header is unavailable, input assignments still work and
the Pads page reports LED WAIT. There is no guessed SysEx device/model number.

Protocol facts were cross-checked against Hakai-MPC's controller chart and
TheKikGen's MPC control-surface documentation; the parser/queue/feedback code
is original. Protocol reference:
https://github.com/henning/Hakai-MPC
https://github.com/TheKikGen/kikpad/blob/master/mod_kikpad_MPC.h
https://github.com/TheKikGen/kikpad/blob/master/mod_kikpad_MPCForce.h

The UI callback validates the live Project-owned TrackPool and every relevant
insert owner before changes. It closes old live-input paths before enabling the
new one, using AudioProgramConnector's recursive-mutex, notifying enum setter.
The audio callback only receives immutable routing/input-ready status; it never
calls native model setters or removes the channel's incoming timeline audio.

The native bridge is restricted to GNU build ID
bc141062ea5ea5be619d98366f423c8102574365, plus code/type fingerprints.
Force and other firmware builds require separate validation. A changed firmware
fails closed instead of using unchecked native addresses.

Read-only diagnostic: /tmp/retrolooper-track-state.json (selection, arm, instance
addresses, monitor state/count and UI-thread flag; no audio or project names).
Track status also reports editor_open, editor_fx and editor_selections.
Pad status: /tmp/retrolooper-pad-state.json (bank, last raw pad identity,
mapped pad, selected effect, native LED readiness, mode and popup IDs; no audio).

The UI queue ABI/checks are adapted from Twisted Cable's MIT-licensed community
helper at commit 922b4054e37a2a1b46177744261e2ba0d5a919d6:
https://github.com/sd88me/plugin-manager-usb-for-open-mpc
See native-ui-LICENSE. Other protocol references:
https://github.com/henning/Hakai-MPC
https://github.com/sd88me/mpc-addin-skipback

Installation uses mpc-vst-plugins' addin installer, preserving existing preload
entries and backing up settings. Save the project before the required MPC app
restart. The library exports exactly ten audited controller/track/pad functions;
compiler/runtime helpers remain hidden. Isolated model/parser/controller tests pass. The existing native monitor and
selected-track capture paths passed the live four-track test with 0.2.3. The new
0.2.5 Bank A colours, physical Pad 14 view switching and steady touchscreen
assignment-knob dragging were confirmed on the MPC. Version 0.2.6 passed
isolated ARM mode/input/LED handoff tests. Live Track Mute, Pad Mute and the
visible RetroLooper editor mode IDs were read and confirmed before packaging;
the installed handoff still requires the saved-project hardware test.

Version 0.2.6's live tests subsequently confirmed native Track Mute colours and
actions, Pad Mute colours, and restored looper colours / Pad 14 view switching.
Version 0.2.7 passed isolated ARM editor-owner/selection/pad tests and the existing
monitoring and controller regressions. Its exact read-only owner resolver and
firmware code guards were verified on the live MPC. Native automatic selection
still needs the saved-project installation test.

The One+ live MIDI trace verified separate button-LED and pad-RGB output streams
and native RGB model 0x56. The helper selects the stream by learned RGB frames,
not by assuming Private/Public metadata labels or hardcoding that model.
