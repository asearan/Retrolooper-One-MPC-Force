# Retrolooper One hardware checks

Use a disposable copy of a project. Save it before each application restart. Back up the existing effect folder, any Loops or Exports, and the existing Controls configuration. Keep other add-ins in place. Check the device and release versions before recording results.

## Standalone effects for MPC 0.3.9 and Force 0.2.10

1. Disable any older Retrolooper Controls and install the matching effect ZIP. After restart, confirm no Retrolooper Controls library is loaded. Add the effect as an Audio track insert and check that the custom GUI loads. Reopen a project from the previous release to check that its instance and saved loop still resolve.
2. Set the track to Monitor MERGE and native REC ARM OFF. Cue a take on Retrolooper's touchscreen, start native Play and feed audio. Confirm clean playback and a clean overdub. Stop with the host, then verify that the looper's own Play/Stop affects its loop while the host plays. Try a touchscreen pad and an assigned Q-Link or MIDI control.
3. Verify the length with a known sequence on MPC and a known Arrange loop on Force. If the host reports no usable cycle range, use Take Bars on Mixer and verify ÷2 and ×2. On Force, check an enabled 8-bar Arrange loop as a specific regression. Check silent Auto Start, threshold triggering, and the resulting phase.
4. Export WAV, reopen it in the host and listen. Confirm no native pad remapping or automatic track selection occurs without Controls.
5. On Force, leave Record Offset at its default Auto and compare a fresh analog take and overdub to the metronome. Verify that the loop stays at the Arrange start and has the expected duration.

## Optional Controls

1. Use the exact supported host build listed in the matching Controls guide. Save the project. Apply the Device patch, verify that the application restarts and status is patched, then reopen the project.
2. Open loopers on different tracks and verify track follow, the matching physical pad grid and selected-track capture. Check that the selected track switches to MERGE, the previous track’s live monitoring turns OFF and its stored loop keeps playing. Verify native pad behavior after leaving the looper for a native mode.
3. On Force with Controls 0.2.10, keep transport playing and tap **CLIP** from the looper opened over MAIN. Check that it blinks, then press a physical pad directly. Confirm the looper stays open and the chosen slot receives the bounce. Arm another bounce, open Matrix manually and select a slot on the touchscreen. Check empty-slot creation, occupied-slot replacement, audible playback and native Undo.
4. On MPC with Controls 0.2.13, stop transport, select an Audio track with an empty destination at bar 1, and tap **EXPORT**. Confirm **BOUNCED TO TRACK** appears and the region plays with the looper stopped. Use MPC Undo and repeat. Check that an occupied destination retains its audio and that the WAV is saved in **RetroLooper Exports**.
5. Save, remove the patch, and verify that the core effect still loads and captures. Confirm the other add-ins still work. Verify patch persistence across a full boot if it will be described as persistent.

Keep a separate result for each firmware, whether Controls was loaded, and which steps passed. Leave the catalog PR untouched until these checks are complete.
