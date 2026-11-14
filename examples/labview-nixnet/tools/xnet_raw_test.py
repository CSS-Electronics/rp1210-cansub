"""Check CANSUB_ReadXnetFrames: CANsub CH1 -> NI-XNET raw frames -> NI-XNET decode (no byte parsing here).

Run with a Python that has nixnet installed:
  python tools/xnet_raw_test.py --dbc labview-demo.dbc --device 1 --baud 500 --seconds 6
"""
import argparse, ctypes, os, sys, time
from ctypes import POINTER, byref, c_char_p, c_long, c_short, c_void_p, create_string_buffer

import nixnet
from nixnet import _frames, convert, database

ap = argparse.ArgumentParser()
ap.add_argument("--dll", default=os.path.join(os.path.dirname(__file__), "..", "CANSUB32.dll"))
ap.add_argument("--dbc", required=True)
ap.add_argument("--device", type=int, default=1)
ap.add_argument("--baud", type=int, default=500)
ap.add_argument("--seconds", type=float, default=6)
a = ap.parse_args()

dll = ctypes.WinDLL(os.path.abspath(a.dll))
dll.RP1210_ClientConnect.argtypes = [c_void_p, c_short, c_char_p, c_long, c_long, c_short]
dll.RP1210_ClientConnect.restype = c_short
dll.RP1210_SendCommand.argtypes = [c_short, c_short, c_char_p, c_short]
dll.RP1210_SendCommand.restype = c_short
dll.RP1210_ClientDisconnect.argtypes = [c_short]
dll.RP1210_ClientDisconnect.restype = c_short
dll.CANSUB_ReadXnetFrames.argtypes = [c_short, c_char_p, c_long, POINTER(c_long)]
dll.CANSUB_ReadXnetFrames.restype = c_long

signals = []
with database.Database(os.path.abspath(a.dbc)) as db:
    for ck in db.clusters:  # nixnet 1.0 collections yield names, so index by key
        cl = db.clusters[ck]
        for fk in cl.frames:
            signals += [f"{fk}.{sk}" for sk in cl.frames[fk].mux_static_signals]

client = dll.RP1210_ClientConnect(None, a.device, f"CAN:Baud={a.baud}".encode(), 0, 0, 0)
if client < 0 or client > 127:
    sys.exit(f"ClientConnect failed: {client}")
try:
    # Buffer smaller than one worst-case frame must be refused, not overrun.
    small = create_string_buffer(79)
    print("buffer 79 ->", dll.CANSUB_ReadXnetFrames(client, small, 79, None), "(expect -141)")

    assert dll.RP1210_SendCommand(3, client, b"", 0) == 0
    buf = create_string_buffer(65536)
    n = c_long()
    total_frames = total_bytes = calls = 0
    with convert.SignalConversionSinglePointSession(os.path.abspath(a.dbc), "", signals) as conv:
        end = time.time() + a.seconds
        next_print = time.time() + 1
        while time.time() < end:
            r = dll.CANSUB_ReadXnetFrames(client, buf, len(buf), byref(n))
            calls += 1
            if r < 0:
                sys.exit(f"CANSUB_ReadXnetFrames failed: {r}")
            if r:
                raw = buf.raw[:r]
                frames = list(_frames.iterate_frames(raw))
                assert len(frames) == n.value, (len(frames), n.value)
                total_frames += n.value
                total_bytes += r
                values = list(conv._convert_bytes_to_signals(raw))
                if time.time() >= next_print:
                    f = frames[-1]
                    print(f"  {n.value:3d} frames/{r:5d} B this call | last: id=0x{f.identifier:X} type={f.type.name} "
                          f"len={len(f.payload)} ts={f.timestamp}")
                    print("   ", ", ".join(f"{s}={v:.2f}" for s, (_, v) in zip(signals, values)))
                    next_print += 1
            time.sleep(0.02)
    print(f"total {total_frames} frames, {total_bytes} B, {calls} calls, {total_frames / a.seconds:.0f} frames/s")
finally:
    print("disconnect ->", dll.RP1210_ClientDisconnect(client))
