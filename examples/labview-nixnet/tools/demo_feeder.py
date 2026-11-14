"""Bench traffic source: transmit DBC-encoded demo frames from a CANsub channel.

For a CH1 <-> CH2 loopback on one CANsub.2 (terminated cable between the two DB9s):
run this on CH2 while LabVIEW reads CH1. Signals follow smooth, recognisable
waveforms so LabVIEW charts clearly show live data (not a frozen value).

It transmits every DBC message except those sent by the LABVIEW node (RigCommand), and
prints the decoded RigCommand values it receives whenever they change, so you can see what
the VI sends.

  python tools\\demo_feeder.py --address <device-id>-usb.local --channel 2 --bitrate 500000 --dbc labview-demo.dbc
"""
import argparse
import math
import time

import can
import cantools


def value(sig, t, phase):
    lo = sig.minimum if sig.minimum is not None else 0.0
    hi = sig.maximum if sig.maximum is not None else 100.0
    return lo + (hi - lo) * (0.5 + 0.45 * math.sin(2 * math.pi * t / 8.0 + phase))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--address", required=True)
    ap.add_argument("--channel", type=int, default=2)
    ap.add_argument("--bitrate", type=int, default=500_000)
    ap.add_argument("--dbc", required=True)
    ap.add_argument("--period", type=float, default=0.02, help="seconds between frame bursts (default 50 Hz)")
    a = ap.parse_args()

    db = cantools.database.load_file(a.dbc)
    tx = [m for m in db.messages if "LABVIEW" not in m.senders]
    rx = {m.frame_id: m for m in db.messages if "LABVIEW" in m.senders}
    print(f"Transmitting {len(tx)} messages every {a.period}s on {a.address} CH{a.channel}. Ctrl+C to stop.")
    with can.Bus(interface="cansub", address=a.address, channel=a.channel, bitrate=a.bitrate) as bus:
        t0 = time.time()
        last = {}
        try:
            while True:
                t = time.time() - t0
                while (got := bus.recv(timeout=0)) is not None:
                    m = rx.get(got.arbitration_id)
                    if m is not None and (vals := m.decode(got.data)) != last.get(m.name):
                        last[m.name] = vals
                        print(f"{t:8.2f}s received {m.name}: {vals}", flush=True)
                for i, msg in enumerate(tx):
                    vals = {s.name: value(s, t, i + j * 0.7) for j, s in enumerate(msg.signals)}
                    data = msg.encode(vals, strict=False)
                    bus.send(can.Message(arbitration_id=msg.frame_id, is_extended_id=msg.is_extended_frame,
                                         data=data))
                time.sleep(a.period)
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
