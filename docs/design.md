# Design notes

How the driver and `libcansub` are put together, for contributors. For using the driver, see the [README](../README.md).

## The SDK does the protocol work

TLS, HTTP, the WebSocket and the CANsub frame codec come from [c-cpp-sdk-cansub](https://github.com/CSS-Electronics/c-cpp-sdk-cansub), pinned as a submodule and built unmodified.

The SDK is *sans-IO*: it never touches a socket, has no threads or timers, and leaves JSON to the caller. `libcansub` supplies those:

- `src/sdk/session.cpp`: two TCP connections per channel (WebSocket and REST), one I/O thread running the ready-flag loop described in the SDK header, and the timeouts the SDK leaves to the caller (connect, request, open, close).
- Calls on an SDK context are serialized by one mutex. A thread that sends a frame also writes the resulting bytes itself, so J1939 transport-protocol pacing does not wait for the I/O thread.
- A close first lets the device send what it has queued. After a second it calls `cansub_close_force()`, so the device drops frames nothing will ever acknowledge. Without that, the next open would find the channel busy.
- `src/bus.cpp`: the REST sequence that opens a channel (API version, channel state, `/phy`, filters, clock).

## The SDK stays behind libcansub's API

`cansub::bus`, `cansub::discover` and their types are plain C++ with no SDK types in them. The driver never touches the SDK directly. Only `libcansub/src/sdk/` and `bit_timing.cpp` include SDK headers, and all conversions between SDK and libcansub types are in `src/sdk/convert.hpp`. An SDK update therefore only concerns `libcansub/src/sdk/`.

## Status codes, never exceptions

Every fallible call returns a `status_t`. An RP1210 DLL is loaded into a third-party process and must not let a C++ exception unwind into it.

## TLS: nothing to install

The SDK compiles the CANsub root certificate in and uses it as the only trust anchor, as python-can-cansub does. The Windows certificate store is never read or modified.

| Address the device by | Chain verified against embedded root | Certificate name checked |
|---|---|---|
| `1a8788d1-usb.local` | yes | yes |
| `10.158.45.1` | yes | no, an IP carries no name to check |

Prefer the name: the device certificate is issued for `<device-id>-{usb,eth}.local`, so only the name form detects that you reached the wrong device. IPv6 is not supported.

## The device is discovered, not configured

A CANsub advertises itself as a `_cansub._tcp` service over mDNS and DNS-SD, with TXT records giving the REST API version and the number of CAN channels.

- `cansub::discover` browses on every IPv4 interface, because a USB-attached device sits on its own subnet that is never the default route.
- The advertisement carries the device's address, so nothing depends on Windows resolving a `.local` name (often the weak point with VPN adapters).
- The parser reads unsolicited network packets inside someone else's process, so `test_mdns` covers hostile input as well as the happy path.

This matters most for RP1210. Real RP1210 adapters are enumerated by their own driver, so vendor INIs name a *kind* of adapter, never a serial number. Discovery lets the CANsub be described the same way, so one INI works on every machine.

## Bit timing comes from the SDK

`cansub_calculate_bittiming()` gives the requested nominal and data bit rates at the requested sample points (80 % unless the connect string sets `SamplePoint=` or `DataSamplePoint=`), with different registers from the device's factory values (250 kbit/s is {1, 255, 64, 64} rather than {4, 63, 16, 16}). Both are valid, and frames pass between channels using each.

The device always runs in CAN FD mode, so the phy endpoint always gets a data phase. A classic channel (no `DataBaud=`) gets 1 Mbit/s as a placeholder, and libcansub refuses to send CAN FD frames on it. FD frames from the bus still arrive on any channel; `RP1210_ReadMessage` skips them, since the RP1210 message has no way to mark a frame as CAN FD, and the NI-XNET exports deliver them.

## Packaging

- Everything is linked with the static C runtime (`/MT`), so the DLL needs no Visual C++ runtime on the target machine.
- Both builds are named `CANSUB32.dll`, as RP1210 requires. `install.bat` puts the 32-bit one in `SysWOW64` and the 64-bit one in `System32`, and Windows sends each application to the folder of its own bitness.
- `RP121032.ini` is not shipped: it may already list other vendors' drivers, so `install.bat` only creates it when it is missing.
