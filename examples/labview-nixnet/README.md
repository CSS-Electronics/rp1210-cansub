# LabVIEW example: CANsub + NI-XNET, read and send by DBC signal

A LabVIEW VI that reads and sends CAN signals by DBC name through a CANsub, using the RP1210 driver and NI's free NI-XNET driver for the DBC encoding (no NI hardware needed). No Python, no webCAN, no byte packing in LabVIEW.

```
CANsub <--> CANSUB32.dll <--> LabVIEW VI <--> NI-XNET Conversion sessions (DBC) <--> signal values
```

The VI uses two CSS Electronics extensions in the driver that exchange frames in NI-XNET's raw format. RP1210 applications never call them, so they change nothing for them.

```c
long WINAPI CANSUB_ReadXnetFrames(short nClientID, unsigned char *pBuffer, long nBufferSize, long *pFrameCount);
long WINAPI CANSUB_WriteXnetFrames(short nClientID, const unsigned char *pBuffer, long nBufferSize, long *pFrameCount);
```

- Use them on a generic CAN client (`CAN:Baud=...`). J1939 clients get `ERR_INVALID_PROTOCOL`. `pFrameCount` may be NULL.
- `CANSUB_ReadXnetFrames` replaces `RP1210_ReadMessage` (set filters first). It never blocks: It returns the bytes written, 0 when nothing is waiting, or a negative RP1210 error code. The buffer must hold at least 80 bytes (one CAN FD frame); 64 KB per call is plenty. Timestamps are NI-XNET's (100 ns since 1601 UTC). With echo on, own frames carry NI-XNET's transmit-echo flag.
- `CANSUB_WriteXnetFrames` sends the frames in the buffer as XNET Convert (Signal to Frame Raw) produces them. It returns the number of frames sent or a negative RP1210 error code. It checks the whole buffer first and sends nothing if a frame is malformed or not classic CAN (data or remote, up to 8 bytes).

Status: Early stage. Verified with LabVIEW 2026 Q3 (64-bit) and NI-XNET 26.3. 32-bit LabVIEW, LabVIEW 2021 itself and CAN FD are untested.

## Files

| File | What |
|---|---|
| `cansub_rp1210_xnet.vi` | The example VI (LabVIEW 2026) |
| `cansub_rp1210_xnet_lv2021.vi` | Same VI saved for LabVIEW 2021 |
| `rp1210_labview.h` | The DLL functions the VI uses, as a C header for LabVIEW's import wizard |
| `labview-demo.dbc` | Demo DBC, 500 kbit/s: `EngineData` (0x100) and `VehicleData` (0x101) from the rig, `RigCommand` (0x200) from LabVIEW |
| `tools/xnet_raw_test.py` | The same calls in Python, to check the DLL and NI-XNET without LabVIEW |
| `tools/demo_feeder.py` | Optional demo rig on another CANsub channel: Sends `EngineData` and `VehicleData`, prints the `RigCommand` values it receives |

Both VIs load `CANSUB32.dll` and, by default, `labview-demo.dbc` from their own folder. The release zip already has `CANSUB32.dll` (64-bit) and `CANSUB32.ini` here. When building from source, copy them from `build\x64\`.

## Requirements

- A CANsub with API 05.00
- LabVIEW 64-bit. For 32-bit LabVIEW, use the 32-bit `CANSUB32.dll` from the top of the release zip instead.
- NI-XNET with LabVIEW support (free, via NI Package Manager). Restart LabVIEW after installing it.

The driver needs no installation for this: The VI loads `CANSUB32.dll` from its own folder.

## Run it

1. Connect the CANsub to the bus. For a bench test, wire CH1 to CH2 (terminated) and run the demo rig on CH2 (`pip install python-can-cansub cantools`). It sends `EngineData` and `VehicleData`, and prints the `RigCommand` values it receives from the VI:
   ```
   python tools\demo_feeder.py --address <device-id>-usb.local --channel 2 --bitrate 500000 --dbc labview-demo.dbc
   ```
2. Open `cansub_rp1210_xnet.vi` and set the front panel:
   - `Device ID`: 1 = first CANsub CH1, 2 = CH2
   - `Protocol`: `CAN:Baud=500` (your bit rate in kbit/s)
   - `DBC file`: Full path to your DBC. Leave it empty to use `labview-demo.dbc` beside the VI
   - `Signals`: Signals to decode as `Frame.Signal`, e.g. `VehicleData.VehicleSpeed`
   - `Send signals` and `Send values`: Signals to send as `Frame.Signal`, one value each. Signals of the same frame that you leave out get their DBC default
3. Press Run. `Signal values` update and the chart shows the signal selected by `Chart signal #`.
4. Switch `Send` on to send every `Send period (s)`. `Frames sent` shows the result of the last send. Values you change while running go out with the next send.
5. Press STOP to disconnect and release the channel.

Only send frames that no other node on the bus sends, such as `RigCommand` in the demo.

To check without LabVIEW (`pip install nixnet`): `python tools\xnet_raw_test.py --dbc labview-demo.dbc --device 1 --baud 500`

## Troubleshooting

| Symptom | Cause |
|---|---|
| `Client ID` 142 | No CANsub found. Run `cansub_discover.exe` and check that UDP port 5353 (mDNS) is not blocked |
| `Client ID` 135 | Channel in use: Close webCAN or any other client on that channel |
| `Client ID` 134 | No device for that Device ID |
| Values stay 0 | No traffic, wrong bit rate, or signal names not given as `Frame.Signal` |
| `Frames sent` negative | An RP1210 error code, negated. 159: Nothing sent |
| LabVIEW cannot find the library | `CANSUB32.dll` is not beside the VI, or its bitness does not match LabVIEW |

A `Client ID` above 127 is an RP1210 error code. A negative `Bytes read` is the same code, negated.

## How the VI is built

To rebuild it or add the calls to your own VI. All DLL calls use a Call Library Function Node with library `CANSUB32.dll` and calling convention **stdcall (WINAPI)**:

| Block | Function | Return | Parameters, in order |
|---|---|---|---|
| Connect | `RP1210_ClientConnect` | I16 | `hwnd` pointer-sized, `device` I16, `protocol` C string, `tx` I32, `rx` I32, `appPacketizing` I16 |
| Filters pass | `RP1210_SendCommand` | I16 | `command` I16, `client` I16, `data` C string, `size` I16 |
| Read frames | `CANSUB_ReadXnetFrames` | I32 | `client` I16, `buffer` U8 array (minimum size = `size`), `size` I32, `count` I32 pointer |
| Send frames | `CANSUB_WriteXnetFrames` | I32 | `client` I16, `buffer` U8 array (Constant), `size` I32, `count` I32 pointer (wire a 0: Each parameter needs a wire on one side) |
| Disconnect | `RP1210_ClientDisconnect` | I16 | `client` I16 |

Each node has **Specify path on diagram** ticked and gets `CANSUB32.dll` beside the VI (Current VI's Path, Strip Path, Build Path), chained through the `path in` and `path out` terminals. That way the VI stores no absolute path and never picks up another copy of the DLL.

1. **Before the loop:** Connect (`hwnd`, `tx`, `rx`, `appPacketizing` = 0; returns the client ID), then Filters pass (`command` 3, empty `data`, `size` 0: Without it no frames pass), then two **XNET Create Session** (Generic, mode Conversion): One with the `Signals` list, one with the `Send signals` list. The DBC is `DBC file`, or `labview-demo.dbc` beside the VI when that is empty (Empty String/Path?, Select).
2. **In a While Loop** with a Stop button and a 50 ms wait:
   - Read frames (`size` 65536, returns bytes written), **Array Subset** of the buffer to that length, then **XNET Convert (Frame Raw to Signal)**.
   - **Elapsed Time** (auto reset, target `Send period (s)`) AND `Send` selects a Case structure. Its True case runs **XNET Convert (Signal to Frame Raw)** on `Send values`, then Send frames with the frame bytes and their **Array Size**.
3. **After the loop:** Disconnect, **XNET Clear**, Simple Error Handler. The send session is cleared automatically when the VI stops.

## Planned

- Timestamped output per signal, for logging

Contributions are welcome.
