# RP1210 driver for CANsub

An RP1210 driver DLL (`CANSUB32.dll`, 32-bit and 64-bit) for the CANsub CAN interfaces by CSS Electronics ([CANsub.2](https://www.csselectronics.com/products/can-fd-usb-interface-ethernet-cansub-2), [CANsub.4](https://www.csselectronics.com/products/can-bus-interface-usb-ethernet-cansub-4)). It lets Windows diagnostic and service tools that support RP1210 use a CANsub as their CAN gateway, over USB or Ethernet.

- Generic **CAN** and **J1939**, including the J1939 transport protocol (up to 1785 bytes) and address claim
- Finds the attached CANsub automatically (mDNS), so one INI works on every machine
- Built on the official [C/C++ SDK for CANsub](https://github.com/CSS-Electronics/c-cpp-sdk-cansub)
- Includes a [LabVIEW example](examples/labview-nixnet/) that reads and sends CAN signals by DBC name via NI-XNET

## Quick start

No build needed.

1. Make sure your CANsub runs a firmware with **API 05.00**. The API version of each firmware release is listed in the [CANsub Docs](https://canlogger.csselectronics.com/cansub-docs/) changelog.
2. Download `rp1210-cansub-<version>.zip` from [Releases](https://github.com/CSS-Electronics/rp1210-cansub/releases) and unzip it.
3. Test without installing:
   ```
   cansub_discover.exe                    rem list the CANsub devices on the network
   rp1210_connect.exe                     rem DeviceID 1, CAN:Baud=250, 32-bit driver
   x64\rp1210_connect.exe 1 CAN:Baud=500  rem 64-bit driver
   rp1210_connect.exe 1 J1939:Baud=250    rem claim an address and list what answers
   ```
4. Right-click `install.bat` and choose Run as administrator. It installs the 32-bit and 64-bit driver (see [Install details](#install-details)).
5. Restart your application and pick "RP1210B CSS Electronics" as the gateway.

## Install details

- `install.bat` copies the 32-bit DLL to `%WINDIR%\SysWOW64`, the 64-bit DLL to `%WINDIR%\System32` and `CANSUB32.ini` to `%WINDIR%` (keeping the old one as `.backup`). After that, the driver reads `%WINDIR%\CANSUB32.ini`, not the copy in the zip.
- It registers the driver in `%WINDIR%\RP121032.ini` only if that file does not exist. If it does (other RP1210 drivers are installed), add `CANSUB32` to its line by hand, e.g. `APIImplementations=DGDPA5MA,CANSUB32` (no spaces).
- `uninstall.bat` deletes only the DLLs. To deregister fully, remove `CANSUB32` from `RP121032.ini`.

## Devices and bit rates

- DeviceID 1-4 are CAN channels 1-4 of the first CANsub found (in device-id order), 5-8 of the second, up to four devices.
- With several devices on a shared network, that order changes when devices come and go. Pin a DeviceID to one device in `CANSUB32.ini` instead, e.g. `DeviceParams=host=97622d1f-eth.local,channel=1` (or its IP address). This also works where mDNS is blocked.
- Bit rates: 125, 250, 500 and 1000 kbit/s (`CAN:Baud=500` or `CAN:Baud=500000`). `J1939:Baud=Auto` needs traffic on the bus.

## Troubleshooting

- `cansub_discover.exe` finds nothing: Check that the CANsub is powered and connected, and that a firewall does not block UDP port 5353 (mDNS). A device marked "API NOT SUPPORTED" needs a firmware update.
- `rp1210_connect.exe` fails: It prints the likely cause and the RP1210 error text.
- The application reports an error such as "gateway not responding": Record the driver's call trace and attach it to your issue or support request. Start the application from a command prompt with
  ```
  set CANSUB_RP1210_LOG=C:\Users\Public\cansub-rp1210.log
  ```
  or set `DebugLevel=2` and `DebugFile=C:\Users\Public\cansub-rp1210.log` under `[VendorInformation]` in `%WINDIR%\CANSUB32.ini`.

## Examples

| Example | What |
|---|---|
| [`examples/labview-nixnet`](examples/labview-nixnet/) | LabVIEW reads and sends CAN signals by DBC name through the driver, via NI-XNET (no NI hardware needed) |

## Build from source

Needs Windows, Visual Studio 2022 or later with the C++ workload, and CMake 3.27 or later.

```
git clone -c core.longpaths=true --recurse-submodules https://github.com/CSS-Electronics/rp1210-cansub
cd rp1210-cansub
build.bat
```

`build.bat` builds x86 and x64 into `build\x86\` and `build\x64\`, then assembles `dist\`, the same folder as the release zip. For one architecture: `cmake --preset x64`, `cmake --build --preset x64`, `ctest --preset x64`. Hardware tests and code rules are in [CONTRIBUTING.md](CONTRIBUTING.md).

## Layout

```
src/ include/ ini/             the RP1210 driver, including the J1939 layer (src/j1939*)
libcansub/                     connection layer on top of the SDK, with a plain C++ API
tools/                         rp1210_connect, cansub_discover, install.bat, uninstall.bat
tests/unit/                    hardware-free tests (run in CI)
tests/hardware/                tests against a CANsub with CH1 and CH2 wired together
examples/labview-nixnet/       LabVIEW example
docs/design.md                 design notes for contributors
third_party/c-cpp-sdk-cansub/  the official SDK (git submodule), unmodified
```

## Limitations

- Windows only
- Generic CAN and J1939 only (no ISO15765 or CAN FD)
- One client per CAN channel: Close webCAN or other tools on that channel first
- mTLS is not supported

## Status and support

Early stage: The driver passes our hardware tests (CAN and J1939, 32-bit and 64-bit), but has been used with few applications so far. Test it on your own setup before relying on it, in particular for ECU flash programming, which our tests do not cover.

Questions, problems and feedback are welcome: Open an [issue](https://github.com/CSS-Electronics/rp1210-cansub/issues) or contact [CSS Electronics](https://www.csselectronics.com). Contributions too, see [CONTRIBUTING.md](CONTRIBUTING.md).

## License

MIT, see [`LICENSE`](LICENSE). Third-party notices: [`NOTICE.md`](NOTICE.md).
