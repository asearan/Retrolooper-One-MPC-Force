# Testing the standalone builds

Run the sanitized host and audio suites for both builds and validate both effect ZIPs with the OpenMPC catalog checker. Verify both patch scripts against the isolated systemd fixture, including wrong-host rejection, preservation of other preloads and failed-start rollback.

The ARM harness opens the production effect in an isolated process. It checks a host-provided cycle, manual Take Bars, touch capture, overdub and host stop. Run it with and without the patch preloaded on the unsupported test process to check fallback. This does not test the actual Akai UI, native input routing or native clip import.

See [Hardware checks](HARDWARE-TEST.md) for what must pass in the actual Akai app before submitting these versions.
