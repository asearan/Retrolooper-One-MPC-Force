# Build Retrolooper One from source

Use Python 3.9+, Pillow, a C++20 compiler, and Zig 0.13.0. The MPC effect builds Gen1 ARMv7 (glibc 2.28) and Gen2 aarch64 (glibc 2.31). Force stays on the Gen1 target. No JUCE or Steinberg SDK is needed. The native skins use MPC OS 3.x controls.

Clone [OpenMPC's VST tools](https://github.com/open-mpc/mpc-vst-plugins) with submodules and check out `0dc17120cd7e952e95eeecdd820ac9e495a75ee9`. That pin writes `plugin.json` and the MPC OS 3.10.1 zip layout. From this repository's root:

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

The `package` commands make local effect ZIPs for development. Catalog drafts are `.github/workflows/release-mpc.yml` and `release-force.yml`. They call OpenMPC `vst-release.yml` at `0dc17120cd7e952e95eeecdd820ac9e495a75ee9` and open draft releases `mpc-v0.4.0` and `force-v0.3.0`. Publish a draft only after installing that zip on MPC OS 3.10.1. Ship the Controls scripts independently as Device patches. Each native build must be profiled and tested before it is supported. Controls 0.2.13 and 0.2.10 stay pinned to MPC One+ 3.10.0.49 and Force 3.10.

## Trademarks

VST is a registered trademark of Steinberg Media Technologies GmbH.

Audio Units is a trademark of Apple Inc.
