# Build Retrolooper One from source

Use Python 3.9+, Pillow, a C++20 compiler, and Zig 0.13.0. The effects target ARMv7 hard-float Linux with glibc 2.28. No JUCE or Steinberg SDK is needed. The native skins use MPC OS 3.x controls.

Clone [OpenMPC's VST tools](https://github.com/open-mpc/mpc-vst-plugins) with submodules and check out `6dd48faa414bd9e3390e5824b0a0e3126f965dbd` for the skin generator. From this repository's root:

```sh
python3 ports/akai/build.py test --sanitize
python3 ports/akai/build.py arm --zig /path/to/zig
python3 ports/akai/build.py package --kit /path/to/pinned-mpc-vst-plugins

python3 ports/akai/force/build_force.py test --sanitize
python3 ports/akai/force/build_force.py arm --zig /path/to/zig
python3 ports/akai/force/build_force.py package --kit /path/to/pinned-mpc-vst-plugins
```

The two products use the same core source, with their existing VST IDs and separate skins. The shared build copies engine files into a generated directory and applies checked port adaptations there. Preserve parameter order for project and automation compatibility.

The MPC ARM command also produces its stripped Controls library. To build or rebuild the Controls libraries independently and package them as optional Device patches:

```sh
python3 ports/akai/build.py controls --zig /path/to/zig
python3 ports/akai/force/build_controls.py --zig /path/to/zig
python3 ports/akai/device-patches/build_patches.py --out /path/to/patch-output
```

The `package` commands make local effect ZIPs for development. Catalog release ZIPs should use the current OpenMPC release tooling with the public source, license and portable validation options. Ship the Controls scripts independently as Device patches. Each native build must be profiled and tested before it is supported.

## Trademarks

VST is a registered trademark of Steinberg Media Technologies GmbH.

Audio Units is a trademark of Apple Inc.
