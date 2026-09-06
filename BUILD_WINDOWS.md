# Build Pulse Macro v1.4.6 for Windows

Target: Geometry Dash 2.2081, Geode 5.10.1, Win64.

## Easiest: GitHub Actions

This source includes `.github/workflows/build-win64.yml`.

1. Put this source tree in a GitHub repository.
2. Open **Actions → Build Win64 Geode → Run workflow**.
3. Download the `PulseMacro-v1.4.6-Win64` artifact.
4. Put the generated `.geode` file in Geometry Dash's `geode/mods` directory.

The workflow uses the official `geode-sdk/build-geode-mod` action, Geode SDK v5.10.1, target Win64, Release mode.

## Local Windows build

Install Geode CLI + SDK 5.10.1 and a supported C++ toolchain, then from this folder configure/build in Release. The project expects `GEODE_SDK` to point at your SDK checkout.

Standalone logic tests can be run without the Geode SDK:

```sh
cmake -S . -B build-core -DPULSE_CORE_ONLY=ON -DBUILD_TESTING=ON
cmake --build build-core
ctest --test-dir build-core --output-on-failure
```
