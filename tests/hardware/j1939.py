"""J1939 helpers for the hardware suite: message layouts, frame builders, and a peer ECU.

Kept separate from the tests so the layout knowledge lives in one place. The peer is driven by
python-can-cansub on CH2 and speaks raw CAN, so everything the driver puts on the wire is judged by
an independent implementation rather than by the driver itself.
"""

import struct
import threading
import time

ADDRESS_NULL = 0xFE
ADDRESS_GLOBAL = 0xFF

PGN_REQUEST = 0x00EA00
PGN_TP_DT = 0x00EB00
PGN_TP_CM = 0x00EC00
PGN_ADDRESS_CLAIM = 0x00EE00

TP_RTS = 16
TP_CTS = 17
TP_EOM_ACK = 19
TP_BAM = 32
TP_ABORT = 255

ABORT_BUSY = 1
ABORT_RESOURCES = 2
ABORT_TIMEOUT = 3
ABORT_CTS_WHILE_SENDING = 4
ABORT_OTHER = 250

FILTER_PGN = 0x01
FILTER_SOURCE = 0x04
FILTER_DESTINATION = 0x08

HOW_BAM = 0x80

# python-can-j1939's own test NAME: identity 1234567, manufacturer 666, function 201, industry
# group 5, not arbitrary address capable. Reusing it means the contention vectors are shared with a
# tested implementation.
NAME = bytes([135, 214, 82, 83, 130, 201, 254, 82])
NAME_LOWER = bytes([135, 214, 82, 83, 130, 111, 254, 82])
NAME_HIGHER = bytes([135, 214, 82, 83, 130, 222, 254, 82])


def can_id(pgn, priority, sa, da=ADDRESS_GLOBAL):
    """Builds a 29-bit identifier. PDU1 puts the destination in the PS byte, PDU2 the group."""
    pf = (pgn >> 8) & 0xFF
    pages = (pgn >> 16) & 0x03
    ps = da if pf < 0xF0 else (pgn & 0xFF)

    return ((priority & 7) << 26) | (pages << 24) | (pf << 16) | (ps << 8) | sa


def decode_id(value):
    pf = (value >> 16) & 0xFF
    pages = (value >> 24) & 0x03
    ps = (value >> 8) & 0xFF

    if pf < 0xF0:
        return {"pgn": (pages << 16) | (pf << 8), "priority": (value >> 26) & 7,
                "sa": value & 0xFF, "da": ps}

    return {"pgn": (pages << 16) | (pf << 8) | ps, "priority": (value >> 26) & 7,
            "sa": value & 0xFF, "da": ADDRESS_GLOBAL}


def pack(pgn, data=b"", priority=6, sa=0, da=ADDRESS_GLOBAL, bam=False):
    """An RP1210_SendMessage body: PGN (3, little endian), How/Priority, source, destination, data."""
    how = (HOW_BAM if bam else 0x00) | (priority & 0x07)

    return struct.pack("<I", pgn)[:3] + bytes([how, sa, da]) + data


def parse(body, echo):
    """Unpacks a J1939 message read from the driver (RP1210E section 17.6).

    The echo byte is only present when echo mode is on, so the caller has to say which layout to
    expect. That is the whole point of testing it: an off-by-one here decodes as plausible garbage.
    """
    pos = 0
    timestamp = struct.unpack(">I", body[pos:pos + 4])[0]
    pos += 4

    echo_byte = None
    if echo:
        echo_byte = body[pos]
        pos += 1

    pgn = body[pos] | (body[pos + 1] << 8) | (body[pos + 2] << 16)
    pos += 3

    how = body[pos]
    pos += 1

    sa = body[pos]
    pos += 1

    da = body[pos]
    pos += 1

    assert how & 0x78 == 0, f"reserved How/Priority bits set: 0x{how:02X}"

    return {
        "timestamp": timestamp,
        "echo": echo_byte,
        "pgn": pgn,
        "how": how,
        "priority": how & 0x07,
        "bam": bool(how & HOW_BAM),
        "sa": sa,
        "da": da,
        "data": body[pos:],
    }


def read_length(payload_len, echo):
    """The exact byte count a read of this payload must return."""
    return 4 + (1 if echo else 0) + 3 + 1 + 1 + 1 + payload_len


def _cm(control, a, b, c, d, pgn):
    return bytes([control, a, b, c, d]) + struct.pack("<I", pgn)[:3]


def tp_bam(pgn, size):
    return _cm(TP_BAM, size & 0xFF, (size >> 8) & 0xFF, packets_for(size), 0xFF, pgn)


def tp_rts(pgn, size, max_per_cts=0xFF):
    return _cm(TP_RTS, size & 0xFF, (size >> 8) & 0xFF, packets_for(size), max_per_cts, pgn)


def tp_cts(pgn, count, next_seq):
    return _cm(TP_CTS, count, next_seq, 0xFF, 0xFF, pgn)


def tp_eom(pgn, size):
    return _cm(TP_EOM_ACK, size & 0xFF, (size >> 8) & 0xFF, packets_for(size), 0xFF, pgn)


def tp_abort(pgn, reason):
    return _cm(TP_ABORT, reason, 0xFF, 0xFF, 0xFF, pgn)


def tp_dt(seq, chunk):
    return bytes([seq]) + bytes(chunk) + b"\xFF" * (7 - len(chunk))


def packets_for(size):
    return (size + 6) // 7


def chunks(data):
    return [data[i:i + 7] for i in range(0, len(data), 7)]


class PeerEcu:
    """An ECU on CH2, for the driver to talk to.

    Runs a reader thread so device timestamps survive for the spacing assertions, and offers just
    enough transport protocol to act as the other end of a transfer.
    """

    def __init__(self, bus, address=0x21, name=NAME):
        self.bus = bus
        self.address = address
        self.name = name
        self._frames = []
        self._cursor = 0
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._read, daemon=True)
        self._thread.start()

    def _read(self):
        while not self._stop.is_set():
            msg = self.bus.recv(timeout=0.1)

            if msg is not None and msg.is_extended_id:
                with self._lock:
                    self._frames.append(msg)

    def close(self):
        self._stop.set()
        self._thread.join(timeout=2)

    # ---------------------------------------------------------------- receiving

    def clear(self, settle=0.2):
        """Discards everything seen so far, including frames still in flight.

        The settle delay matters: without it a frame sent just before the call lands just after it,
        and the next expect() matches something from the previous step of the test.
        """
        time.sleep(settle)

        with self._lock:
            self._frames.clear()
            self._cursor = 0

    def seen(self):
        with self._lock:
            return list(self._frames)

    def expect(self, pgn=None, sa=None, da=None, first=None, timeout=2.0):
        """Waits for the next matching frame and consumes it.

        Consuming is the point. A scan that always restarts at the beginning returns the same frame
        over and over, so a caller waiting for three data packets in turn is satisfied three times
        by the first one -- which makes a responder race ahead of the driver and produces failures
        that look like driver bugs.
        """
        deadline = time.time() + timeout

        while True:
            with self._lock:
                while self._cursor < len(self._frames):
                    msg = self._frames[self._cursor]
                    self._cursor += 1
                    info = decode_id(msg.arbitration_id)

                    if pgn is not None and info["pgn"] != pgn:
                        continue
                    if sa is not None and info["sa"] != sa:
                        continue
                    if da is not None and info["da"] != da:
                        continue
                    if first is not None and (len(msg.data) == 0 or msg.data[0] != first):
                        continue

                    return msg

            if time.time() >= deadline:
                return None

            time.sleep(0.005)

    def expect_none(self, timeout=0.5, **match):
        return self.expect(timeout=timeout, **match) is None

    def collect(self, count, timeout=10.0, pgn=None, sa=None):
        """Waits for at least count matching frames and returns them in order."""
        deadline = time.time() + timeout

        def matching():
            out = []

            for msg in self._frames:
                info = decode_id(msg.arbitration_id)

                if pgn is not None and info["pgn"] != pgn:
                    continue
                if sa is not None and info["sa"] != sa:
                    continue

                out.append(msg)

            return out

        while time.time() < deadline:
            with self._lock:
                found = matching()

                if len(found) >= count:
                    return found[:count]

            time.sleep(0.01)

        with self._lock:
            return matching()

    @staticmethod
    def gaps(frames):
        """Milliseconds between consecutive frames, from the device timestamps."""
        return [(b.timestamp - a.timestamp) * 1000.0 for a, b in zip(frames, frames[1:])]

    # ---------------------------------------------------------------- sending

    def send(self, pgn, data=b"", priority=6, sa=None, da=ADDRESS_GLOBAL):
        import can

        self.bus.send(can.Message(
            arbitration_id=can_id(pgn, priority, self.address if sa is None else sa, da),
            data=bytes(data), is_extended_id=True))

    def send_bam(self, pgn, data, gap=0.055, drop_seq=None, order=None):
        self.send(PGN_TP_CM, tp_bam(pgn, len(data)), priority=7, da=ADDRESS_GLOBAL)

        parts = list(enumerate(chunks(data), start=1))

        if order is not None:
            parts = [parts[i] for i in order]

        for seq, chunk in parts:
            if drop_seq is not None and seq == drop_seq:
                continue

            time.sleep(gap)
            self.send(PGN_TP_DT, tp_dt(seq, chunk), priority=7, da=ADDRESS_GLOBAL)

    def originate_rts(self, pgn, data, da, expect_cts=True, drop_seq=None):
        """Acts as the sender of a connection-mode transfer; the driver is the receiver."""
        self.send(PGN_TP_CM, tp_rts(pgn, len(data)), priority=7, da=da)

        if not expect_cts:
            return None

        parts = list(enumerate(chunks(data), start=1))

        for seq, chunk in parts:
            if drop_seq is not None and seq == drop_seq:
                continue

            self.send(PGN_TP_DT, tp_dt(seq, chunk), priority=7, da=da)
            time.sleep(0.005)

        return self.expect(pgn=PGN_TP_CM, sa=da, first=TP_EOM_ACK)

    def respond_to_rts(self, cts_chunk=None, hold_first=False, abort_reason=None, drop_eom=False,
                       timeout=2.0):
        """Acts as the receiver of a connection-mode transfer; the driver is the sender.

        cts_chunk grants that many packets at a time, so a value of 1 reproduces the one-at-a-time
        behaviour real ECUs commonly use.
        """
        rts = self.expect(pgn=PGN_TP_CM, first=TP_RTS, timeout=timeout)

        if rts is None:
            return None

        info = decode_id(rts.arbitration_id)
        peer = info["sa"]
        size = rts.data[1] | (rts.data[2] << 8)
        packets = rts.data[3]
        pgn = rts.data[5] | (rts.data[6] << 8) | (rts.data[7] << 16)

        if abort_reason is not None:
            self.send(PGN_TP_CM, tp_abort(pgn, abort_reason), priority=7, da=peer)
            return None

        if hold_first:
            self.send(PGN_TP_CM, tp_cts(pgn, 0, 0xFF), priority=7, da=peer)
            time.sleep(0.3)

        granted = cts_chunk or packets
        next_seq = 1

        while next_seq <= packets:
            count = min(granted, packets - next_seq + 1)
            self.send(PGN_TP_CM, tp_cts(pgn, count, next_seq), priority=7, da=peer)

            for _ in range(count):
                if self.expect(pgn=PGN_TP_DT, sa=peer, timeout=timeout) is None:
                    return None

            next_seq += count

        if not drop_eom:
            self.send(PGN_TP_CM, tp_eom(pgn, size), priority=7, da=peer)

        return {"pgn": pgn, "size": size, "packets": packets}

    def claim(self, address, name=NAME):
        self.send(PGN_ADDRESS_CLAIM, name, priority=6, sa=address, da=ADDRESS_GLOBAL)

    def request(self, pgn, da=ADDRESS_GLOBAL):
        self.send(PGN_REQUEST, struct.pack("<I", pgn)[:3], priority=6, da=da)
