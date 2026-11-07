# Build from source

Copyright © 2026 IAMTHEBAND LLC, Retroloop USA.

Python 3.9+, Pillow, a C++20 compiler and Zig 0.13.0 are required. Mac host tests/preview use clang++; ARM output uses Zig's ARMv7 hard-float/glibc 2.28 target. No JUCE or Steinberg SDK is required.

The skin generator is pinned to open-mpc/mpc-vst-plugins commit 6dd48faa414bd9e3390e5824b0a0e3126f965dbd. Clone that project with submodules and check out the pinned commit. Catalog packaging uses the current checked release tools, recorded in release/BUILD-INFO.json.

~~~sh
python3 -m pip install pillow
cd ports/akai
python3 build.py test --sanitize
python3 build.py arm --zig /path/to/zig-0.13.0/zig
python3 build.py skin --kit /path/to/pinned-mpc-vst-plugins
python3 build.py preview
python3 tools/preview_server.py --port 8765
~~~

Force is built separately, without replacing the MPC binary/helper:

~~~sh
cd ports/akai/force
python3 build_force.py test --sanitize
python3 build_force.py arm --zig /path/to/zig-0.13.0/zig
python3 build_force.py skin --kit /path/to/pinned-mpc-vst-plugins
python3 build_controls.py --zig /path/to/zig-0.13.0/zig
~~~

The shared build copies engine sources into a generated build directory and applies asserted port adaptations. Parent engine sources stay unchanged. The Force generator separately applies its firmware/profile and grid adaptations. Do not change parameter indices/order without reviewing project and automation compatibility.

For catalog-ready packages after building both products:

~~~sh
python3 release/package_catalog.py --kit /path/to/current-mpc-vst-plugins
~~~

Release packages declare their public source/license and exact helper dependency. Native controls are firmware-specific; a new firmware requires validation, not simply changing the version label.
