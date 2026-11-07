# Validation

Copyright © 2026 IAMTHEBAND LLC, Retroloop USA.

## MPC One+ release

Plugin 0.3.6 and helper 0.2.7 were exercised on MPC One+ 3.10.0.49. Hardware checks include clean loop playback/overdubs, selected-track input routing, avoiding duplicate native timeline capture, native pad colours/actions, Track Mute/Pad Mute handoff, returning looper controls, steady assignment knobs and editor-driven native track selection. The displayed effect, selected native track and pad owner matched; the MPC application remained stable.

Automated checks include sanitized Mac host/DSP tests, isolated ARM audio/ownership/monitor/controller tests, exact reverse PCM and threshold timing, native skin/schema/layout/resource audits, strict helper export checks and catalog package validation. The release archives preserve the tested production binary hashes; packaging adds metadata and notices without changing DSP code.

## Force test build

The exact production library passed a synthetic C host on the Force. Sanitized core tests, isolated ARM helper assertions, ABI/type guards, layout/schema and package checks passed. Installation/startup/native UI readiness and RGB learning were checked. Live Arrange timing/phase, input/output routing, editor-driven track actions and physical lower-row pad behaviour still need functional hardware testing. No verified tested.json entry is claimed for Force 0.1.0.

## Limits

Two-minute maximum, one stereo loop per insert, four undo levels. Only the listed firmware images are validated. Gen2 is unsupported. WAV export exists; automatic native timeline placement is pending. Insert effects are printed only when placed before the looper. Catalog CPU bench metadata is not supplied; no CPU performance claim is made.
