"""CAN FD and the other opt-in connect options (DataBaud=, SamplePoint=, DataSamplePoint=,
ListenOnly=), checked against python-can-cansub on CH2.

RP1210 itself has no CAN FD, so FD frames travel only through the NI-XNET exports
(CANSUB_ReadXnetFrames / CANSUB_WriteXnetFrames). RP1210_ReadMessage skips them.

    pytest tests/hardware -v --arch x64 --host <device-id>-eth.local
"""

import time

import pytest

try:
    import can
except ImportError:  # reported as a skip by the fixtures
    can = None

from conftest import DEVICE_CH2, open_bus
from driver import (
    CMD_FLUSH_TX_RX_BUFFERS,
    CMD_SET_ALL_FILTERS_STATES_TO_PASS,
    CMD_SET_CAN_BAUD,
    ERR_CAN_BAUD_SET_NONSTANDARD,
    ERR_INVALID_PROTOCOL,
    ERR_MESSAGE_NOT_SENT,
    ERR_MESSAGE_TOO_LONG,
    NO_ERRORS,
    XNET_TYPE_CAN_DATA,
    XNET_TYPE_CANFD_DATA,
    XNET_TYPE_CANFDBRS_DATA,
    build_xnet,
    parse_xnet,
)

RATES = [(500, 2000), (1000, 4000)]


def connect(driver, protocol):
    cid = driver.connect(protocol=protocol)

    assert 0 <= cid <= 127, f"{protocol!r}: {cid} {driver.error_text(cid)}"
    assert driver.command(CMD_SET_ALL_FILTERS_STATES_TO_PASS, cid) == NO_ERRORS
    driver.command(CMD_FLUSH_TX_RX_BUFFERS, cid)

    return cid


def refuses(driver, expected, protocol):
    result = driver.connect(protocol=protocol)

    if 0 <= result <= 127:
        driver.disconnect(result)

    assert result == expected, f"{protocol!r}: got {result} ({driver.error_text(result)})"


def read_xnet_until(driver, client, wanted, timeout=2):
    frames = []
    deadline = time.time() + timeout

    while time.time() < deadline and len(frames) < wanted:
        result, count, raw = driver.read_xnet(client)
        assert result >= 0, f"CANSUB_ReadXnetFrames failed: {result}"
        frames += parse_xnet(raw)
        if count == 0:
            time.sleep(0.02)

    return frames


@pytest.fixture
def fd_bus(device_host):
    """Opens python-can-cansub on CH2 at the given rates; closed after the test."""
    pytest.importorskip("python_can_cansub")
    opened = []

    def make(nominal_k, data_k):
        bus = open_bus(device_host, DEVICE_CH2, bitrate=nominal_k * 1000,
                       data_bitrate=data_k * 1000)
        opened.append(bus)
        return bus

    yield make

    for bus in opened:
        bus.shutdown()


# ---------------------------------------------------------------- connect options


@pytest.mark.parametrize("protocol", [
    b"CAN:Baud=500,DataBaud=2000",
    b"CAN:Baud=500000,DataBaud=2000000",
    b"CAN:Baud=1000,DataBaud=4000",
    b"CAN:Baud=500,DataBaud=2000,SamplePoint=87.5,DataSamplePoint=75",
    b"CAN:Baud=250,SamplePoint=87.5",
    b"CAN:Baud=500,ListenOnly=1",
])
def test_connect_options_accepted(driver, protocol):
    cid = driver.connect(protocol=protocol)

    assert 0 <= cid <= 127, f"{protocol!r}: {cid} {driver.error_text(cid)}"
    driver.disconnect(cid)


def test_connect_options_refused(driver):
    """Bad bit timing is a baud error; an option the protocol does not offer is a protocol error."""
    for protocol in [
        b"CAN:Baud=1000,DataBaud=500",            # data rate below nominal
        b"CAN:Baud=500,DataBaud=2M",              # not a number
        b"CAN:Baud=500,SamplePoint=100",          # not inside the bit
        b"CAN:Baud=500,DataSamplePoint=75",       # without DataBaud=
    ]:
        refuses(driver, ERR_CAN_BAUD_SET_NONSTANDARD, protocol)

    refuses(driver, ERR_INVALID_PROTOCOL, b"CAN:Baud=500,ListenOnly=maybe")
    refuses(driver, ERR_INVALID_PROTOCOL, b"J1939:Baud=250,DataBaud=2000")
    refuses(driver, ERR_INVALID_PROTOCOL, b"J1939:Baud=250,ListenOnly=1")


def test_set_can_baud_keeps_the_channel_when_refused(driver):
    """SET_CAN_BAUD to a nominal rate above the client's data rate is refused before the channel is
    closed, so the client keeps working at its old rate."""
    cid = connect(driver, b"CAN:Baud=250,DataBaud=500")

    try:
        # '7' is 1000k, above the 500k data rate.
        assert driver.command(CMD_SET_CAN_BAUD, cid, b"\x00" + b"7") == ERR_CAN_BAUD_SET_NONSTANDARD
        assert driver.connection_speed(cid) == (NO_ERRORS, "250")
        assert driver.read_xnet(cid)[0] >= 0, "the client no longer works"
    finally:
        driver.disconnect(cid)


# ---------------------------------------------------------------- CAN FD through the XNET exports


@pytest.mark.parametrize("nominal,data", RATES)
def test_fd_write_reaches_other_channel(driver, fd_bus, nominal, data):
    """FD frames with and without BRS, 12 and 64 bytes, standard and extended, plus a classic frame,
    arrive intact on CH2 in order."""
    ch2 = fd_bus(nominal, data)
    cid = connect(driver, f"CAN:Baud={nominal},DataBaud={data}".encode())

    try:
        payload64 = bytes(range(64))
        raw = (build_xnet(0x18FF1234, payload64, extended=True, ftype=XNET_TYPE_CANFDBRS_DATA)
               + build_xnet(0x123, bytes(range(12)), ftype=XNET_TYPE_CANFD_DATA)
               + build_xnet(0x1ABCDEF, payload64[:8], extended=True))

        assert driver.write_xnet(cid, raw) == (3, 3)

        received = [ch2.recv(timeout=2) for _ in range(3)]
        assert all(m is not None for m in received), "CH2 saw fewer than 3 frames"

        assert [(m.arbitration_id, m.is_extended_id, m.is_fd, m.bitrate_switch, bytes(m.data))
                for m in received] == [
            (0x18FF1234, True, True, True, payload64),
            (0x123, False, True, False, bytes(range(12))),
            (0x1ABCDEF, True, False, False, payload64[:8]),
        ]
    finally:
        driver.disconnect(cid)


@pytest.mark.parametrize("nominal,data", RATES)
def test_fd_read_from_other_channel(driver, fd_bus, nominal, data):
    """FD frames sent by CH2 read back as NI-XNET CAN FD / CAN FD+BRS frames."""
    ch2 = fd_bus(nominal, data)
    cid = connect(driver, f"CAN:Baud={nominal},DataBaud={data}".encode())

    try:
        ch2.send(can.Message(arbitration_id=0x18FEF100, data=bytes(range(64)), is_extended_id=True,
                             is_fd=True, bitrate_switch=True))
        ch2.send(can.Message(arbitration_id=0x456, data=bytes(range(20)), is_extended_id=False,
                             is_fd=True, bitrate_switch=False))
        ch2.send(can.Message(arbitration_id=0x457, data=b"\x01\x02", is_extended_id=False))

        frames = read_xnet_until(driver, cid, 3)

        assert [(f["id"], f["extended"], f["type"], f["data"]) for f in frames] == [
            (0x18FEF100, True, XNET_TYPE_CANFDBRS_DATA, bytes(range(64))),
            (0x456, False, XNET_TYPE_CANFD_DATA, bytes(range(20))),
            (0x457, False, XNET_TYPE_CAN_DATA, b"\x01\x02"),
        ]
    finally:
        driver.disconnect(cid)


def test_fd_write_refuses_without_sending(driver, fd_bus):
    """An FD frame with a length CAN FD cannot carry refuses the whole buffer."""
    ch2 = fd_bus(500, 2000)
    cid = connect(driver, b"CAN:Baud=500,DataBaud=2000")

    try:
        good = build_xnet(0x100, b"\x01")
        bad = build_xnet(0x101, bytes(10), ftype=XNET_TYPE_CANFD_DATA)

        assert driver.write_xnet(cid, good + bad) == (-ERR_MESSAGE_NOT_SENT, 0)
        assert ch2.recv(timeout=0.5) is None, "a refused buffer still put a frame on the bus"
    finally:
        driver.disconnect(cid)


def test_read_message_skips_fd_frames(driver, fd_bus):
    """RP1210_ReadMessage has no way to flag an FD frame, so it skips them and returns the next
    classic frame. The XNET read on the same client still sees both."""
    ch2 = fd_bus(500, 2000)
    cid = connect(driver, b"CAN:Baud=500,DataBaud=2000")

    try:
        ch2.send(can.Message(arbitration_id=0x200, data=bytes(12), is_extended_id=False, is_fd=True))
        ch2.send(can.Message(arbitration_id=0x201, data=b"\x05", is_extended_id=False))
        time.sleep(0.3)

        count, body = driver.read(cid, size=64)

        assert count > 0, f"no classic frame read: {count}"
        # Timestamp (4), type (1), standard identifier (2), data.
        assert body[5:7] == b"\x02\x01" and body[7:] == b"\x05"
        assert driver.read(cid)[0] == 0, "the FD frame was not skipped"
    finally:
        driver.disconnect(cid)


def test_read_message_keeps_a_frame_that_does_not_fit(driver, client, ch2):
    """A buffer too small for the next frame returns ERR_MESSAGE_TOO_LONG, and a retry with a bigger
    buffer gets that same frame rather than the one after it."""
    assert driver.command(CMD_SET_ALL_FILTERS_STATES_TO_PASS, client) == NO_ERRORS
    driver.command(CMD_FLUSH_TX_RX_BUFFERS, client)

    ch2.send(can.Message(arbitration_id=0x302, data=bytes(range(8)), is_extended_id=False))
    ch2.send(can.Message(arbitration_id=0x303, data=b"\x01", is_extended_id=False))
    time.sleep(0.3)

    count, _ = driver.read(client, size=8)
    assert count == -ERR_MESSAGE_TOO_LONG

    count, body = driver.read(client, size=64)
    assert count == 4 + 1 + 2 + 8 and body[5:7] == b"\x03\x02", "the held frame was lost"

    count, body = driver.read(client, size=64)
    assert count > 0 and body[5:7] == b"\x03\x03"


# ---------------------------------------------------------------- listen only


def test_listen_only_receives_but_never_sends(driver, ch2):
    """A listen-only client sees the bus, and both send paths refuse without touching it."""
    cid = connect(driver, b"CAN:Baud=250,ListenOnly=1")

    try:
        assert driver.send(cid, 0x111, b"\x01") == ERR_MESSAGE_NOT_SENT
        assert driver.write_xnet(cid, build_xnet(0x112, b"\x01")) == (-ERR_MESSAGE_NOT_SENT, 0)
        assert ch2.recv(timeout=0.5) is None, "a listen-only client put a frame on the bus"

        # Nothing acknowledges CH2's frame, so the device repeats it until the driver reads one.
        ch2.send(can.Message(arbitration_id=0x222, data=b"\x07", is_extended_id=False))

        frames = read_xnet_until(driver, cid, 1)

        assert frames and frames[0]["id"] == 0x222 and frames[0]["data"] == b"\x07"
    finally:
        driver.disconnect(cid)
