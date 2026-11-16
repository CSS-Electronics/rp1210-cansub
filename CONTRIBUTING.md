# Contributing

Issues and pull requests are welcome. This is an early-stage project, so please say in an issue what you are trying to do, with which application and which CANsub API version, before starting on a large change.

## Build

Needs Windows, Visual Studio 2022 or later with the C++ workload, and CMake 3.27 or later.

```
git clone -c core.longpaths=true --recurse-submodules https://github.com/CSS-Electronics/rp1210-cansub
cd rp1210-cansub
build.bat
```

`core.longpaths` is needed because some paths inside the SDK's bundled mbedTLS are longer than Windows allows by default.

`build.bat` builds both architectures with the CMake presets and fills `dist\`. To work on one architecture:

```
cmake --preset x64
cmake --build --preset x64
ctest --preset x64
```

Binaries land in `build\x64\` (or `build\x86\`).

## Tests

**Unit tests** need no hardware and run in CI on every push: `ctest --preset x64` and `ctest --preset x86`.

| Test | What |
|---|---|
| `test_glue` | Conversions between libcansub and the SDK: frames, error types, result codes, hosts, API version, bit timing |
| `test_mdns` | mDNS parsing, including malformed and hostile input |
| `test_j1939` | J1939 identifiers, transport protocol and address claim, driven by an injected clock |
| `test_connect_params` | The `RP1210_ClientConnect` protocol string: bit rates, CAN FD and sample point options, listen-only |

**Hardware tests** need a CANsub with CH1 and CH2 wired together (terminated), running a firmware with API 05.00:

```
build\x64\test_bus.exe <device-id>-eth.local
pip install pytest python-can-cansub
pytest tests\hardware -v --arch x64 --host <device-id>-eth.local
```

The pytest suites load `build\<arch>\CANSUB32.dll`, so the Python bitness must match: 64-bit Python for `--arch x64`, 32-bit Python for `--arch x86`. They drive CH1 through the driver and check the result on CH2 with python-can-cansub. Please run them on both architectures for a change to the driver or to libcansub, and say in the pull request which device and API version you used.

## Code rules

- **No exceptions across the DLL boundary.** The driver runs inside someone else's process. Every fallible call returns a status code, and no C++ exception may escape an exported function.
- **Only `libcansub/src/sdk/` and `libcansub/src/bit_timing.cpp` include SDK headers.** The driver and tools use only the `cansub::` API in `libcansub/include/`. Conversions between SDK and libcansub types go in `libcansub/src/sdk/convert.hpp`.
- **The SDK is not modified here.** It is a submodule pinned to a release tag. Report SDK issues in its own repository.
- Keep the RP1210 behaviour traceable to the specification: when the driver departs from it, or from a reference implementation, say why in a comment.
- Match the surrounding style (4 spaces, braces on their own line, `snake_case`).

## RP1210 and J1939 specifications

The RP1210 specification (TMC/ATA) and SAE J1939 are copyrighted and not included. Reference behaviour was cross-checked against open-source implementations such as [dfieschko/RP1210](https://github.com/dfieschko/RP1210) and [python-can-j1939](https://github.com/juergenH87/python-can-j1939).
