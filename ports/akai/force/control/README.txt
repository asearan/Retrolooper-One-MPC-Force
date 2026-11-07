RetroLooper One Controls (Force) 0.1.0 — hardware test build

For RetroLooper One 0.1.0 on Akai Force 3.10, GNU build ID:
154ae9752e0f3c6490912a703b0dfe4b668b343d
Host executable SHA256:
7bbbd8050bfe67db4d320df0ab39f5c3194210f9bbd225d8415f0c4c5f43300d

This is a background integration helper, not a second insert effect. Install
it alongside the RetroLooper One plugin. Never substitute the MPC Buttons
helper. Its ten exported functions use the retrolooper_force_ namespace and
accept only the Force plugin ID RlF1. Unknown firmware fails closed.

The exact application/window/engine/project and Audio track graphs were
validated read-only on the user's Force. Eighteen instruction/type guards
match the copied host image. Root discovery scans at most the first 32 MiB
of ordinary heap at startup; the MPC One+ singleton address is not used.

Native track selection, arm and monitor changes use the notifying native
property setters on the host UI queue. The audio callback uses immutable
published status and does not call these setters or read process-memory files.
Capture cues disarm only the owning selected Audio track, preventing a second
native timeline recording. Only selected RetroLooper One tracks receive
live input; other RetroLooper One tracks use Monitor OFF and retain loop
playback. Insert effects before the looper are recorded into each layer.

Physical buttons use Force private CC messages, rather than MPC note events.
Native transport messages pass unchanged. Touch/Pads remains the default
capture source, with Force's transport providing the clock and start/stop.

Looper controls use sixteen pads in the lower two rows while the editor is
visible. P1-P8 are the bottom row, left to right. P9-P16 are the row above.
The upper 48 pads are passed unchanged. Closing the editor or entering native
mute/performance modes releases all 64 pads to Force. Existing native RGB
messages supply the actual device/header and restorable lower-row colours;
no device model number is guessed. Queued events expire across mode changes.
Each instance retains its own assignments in the project chunk.

ARM isolated tests passed on Force with test-owned model memory and fake ALSA:
selection/monitor ordering, arm filtering, editor ownership, mode handoff,
all lower-row pad identities, RGB restoration, split/running MIDI, pressure,
byte/error passthrough, stale events and bounded/invalid model rejection.
The production .so also passed synthetic audio tests executed on Force for
/2, x1 and x2 timing, four instances, threshold, Reverse and touch capture.
Live input/output, touch, pad/LED and Arrange-loop tests are still pending.
Save projects before installation: installing the helper requires a host app
restart. The helper is installed on the provided Force. Startup, native UI-queue
readiness and native RGB learning are verified; physical actions still need
the saved-project hardware test.

Read-only status files (no audio or project names):
/tmp/retrolooper-force-track-state.json
/tmp/retrolooper-force-pad-state.json

The queue ABI derives from the MIT-licensed Twisted Cable community helper.
See native-ui-LICENSE and mpc-vst-plugins-LICENSE. Protocol facts were checked
against the following source; this package uses an original parser and does
not copy its implementation:
https://github.com/TheKikGen/kikpad/blob/master/mod_kikpad_MPCForce.h
https://github.com/sd88me/plugin-manager-usb-for-open-mpc
