"""Drives the CANSUB32 RP1210E driver through ctypes and checks it against python-can-cansub.

Needs a CANsub with CH1 and CH2 wired together. The DLL uses CH1 (DeviceID 1 in the INI) and
python-can-cansub listens on CH2, so a frame that leaves the driver is checked by an independent
implementation rather than by the driver itself.

    pytest tests/hardware -v --arch x64 --host <device-id>-eth.local

Bitness must match the interpreter: 64-bit Python can only load the x64 build. The ctypes binding,
the RP1210 constants and the fixtures live in driver.py and conftest.py, shared with the J1939
suite; this module covers the generic CAN protocol.
"""

import ctypes
import struct
import time
from ctypes import c_int32, create_string_buffer

import pytest

try:
    import can
except ImportError:  # reported as a skip by the ch2 fixture
    can = None

from conftest import DEVICE_CH1, DEVICE_CH2
from driver import (
    BLOCKING_IO,
    CMD_ECHO_TRANSMITTED_MESSAGES,
    CMD_FLUSH_TX_RX_BUFFERS,
    CMD_GET_PROTOCOL_CONNECTION_SPEED,
    CMD_PROTECT_J1939_ADDRESS,
    CMD_RELEASE_J1939_ADDRESS,
    CMD_SET_ALL_FILTERS_STATES_TO_DISCARD,
    CMD_SET_ALL_FILTERS_STATES_TO_PASS,
    CMD_SET_BLOCKTIMEOUT,
    CMD_SET_CAN_BAUD,
    CMD_SET_CAN_FILTER_TYPE,
    CMD_SET_J1939_BAUD,
    CMD_SET_MESSAGE_FILTERING_FOR_CAN,
    CMD_SET_MESSAGE_FILTERING_FOR_J1939,
    ERR_COMMAND_NOT_SUPPORTED,
    ERR_DEVICE_IN_USE,
    ERR_INVALID_CLIENT_ID,
    ERR_MESSAGE_TOO_LONG,
    ERR_INVALID_DEVICE,
    ERR_CAN_BAUD_SET_NONSTANDARD,
    ERR_INVALID_PROTOCOL,
    ERR_MESSAGE_NOT_SENT,
    EXTENDED_CAN,
    NON_BLOCKING_IO,
    NO_ERRORS,
    STANDARD_CAN,
    CANSUB_XNET_FRAME_MAX,
    XNET_EPOCH_OFFSET,
    XNET_FLAG_TRANSMIT_ECHO,
    XNET_TYPE_CAN_DATA,
    XNET_TYPE_CAN_REMOTE,
    XNET_TYPE_CANFD_DATA,
    build_xnet,
    parse_xnet,
)

def parse_can(body, echo):
    """Unpacks a CAN message read from the driver (RP1210E section 17.7).

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

    msg_type = body[pos]
    pos += 1

    if msg_type == EXTENDED_CAN:
        can_id = struct.unpack(">I", body[pos:pos + 4])[0]
        pos += 4
    else:
        can_id = struct.unpack(">H", body[pos:pos + 2])[0]
        pos += 2

    return {
        "timestamp": timestamp,
        "echo": echo_byte,
        "extended": msg_type == EXTENDED_CAN,
        "id": can_id,
        "data": body[pos:],
    }



def test_version(driver):
    bufs = [create_string_buffer(16) for _ in range(4)]
    driver.dll.RP1210_ReadVersion(*bufs)
    dll_major, dll_minor, api_major, api_minor = (b.value.decode() for b in bufs)

    assert dll_major and dll_minor
    # Must match the RP1210 key declared in CANSUB32.ini; see the note in rp1210_api.cpp.
    assert (api_major, api_minor) == ("2", "0"), "declared revision is RP1210B"


def test_error_messages(driver):
    assert driver.error_text(ERR_INVALID_PROTOCOL) == "ERR_INVALID_PROTOCOL"
    assert driver.error_text(NO_ERRORS) == "NO_ERRORS"


def test_detailed_version(driver, client):
    api, dll, fw = (create_string_buffer(17) for _ in range(3))

    assert driver.dll.RP1210_ReadDetailedVersion(client, api, dll, fw) == NO_ERRORS
    assert api.value.decode() == "RP1210B", "must match the INI's declared revision"
    assert fw.value.decode(), "firmware version read from the device"


def refuses(driver, expected, **kwargs):
    """Asserts a connect attempt is refused, and cleans up if it wrongly succeeded.

    A connect that should have failed but did not leaves a client holding the channel, and every
    later test then fails with ERR_DEVICE_IN_USE instead of the one real fault. Disconnecting here
    keeps the failure report readable.
    """
    result = driver.connect(**kwargs)

    if 0 <= result <= 127:
        driver.disconnect(result)

    assert result == expected, f"got {result} ({driver.error_text(result)})"


def test_connect_rejects_bad_arguments(driver):
    refuses(driver, ERR_INVALID_PROTOCOL, protocol=b"ISO15765")
    refuses(driver, ERR_INVALID_PROTOCOL, protocol=b"J1708")
    refuses(driver, ERR_INVALID_DEVICE, device=99)


@pytest.mark.parametrize("protocol,speed", [
    # Format 5, kBaud: the plain spelling.
    (b"CAN:Baud=250", "250"),
    (b"CAN:Baud=500", "500"),
    # Format 5, bit rate: RP1210E 14.10.5 allows 1001..1000000, so this is the same request.
    (b"CAN:Baud=250000", "250"),
    (b"CAN:Baud=500000", "500"),
    (b"CAN:Baud=1000000", "1000"),
    # Format 1, which some diagnostic tools send. The bit-timing parameters are ignored in favour
    # of the device's own table, but the connection must succeed: refusing this made every attempt
    # from such a tool fail.
    (b"CAN:Baud=500000,SampleLocation=75,SJW=1,IDSize=29", "500"),
    (b"CAN:Baud=250000,SampleLocation=75,SJW=1,IDSize=29", "250"),
    # Format 2 and 3, the other explicit forms.
    (b"CAN:Baud=250000,PROP_SEG=4,PHASE_SEG1=4,PHASE_SEG2=3,SJW=1,IDSize=29", "250"),
    (b"CAN:Baud=500000,TSEG1=13,TSEG2=2,SampleTimes=1,SJW=1,IDSize=B", "500"),
    # Channel= may appear anywhere in the string and is not ours to interpret: one DeviceID is one
    # CAN channel here, which MultiCANChannels=0 declares.
    (b"CAN:Baud=500,Channel=1", "500"),
])
def test_connect_baud_spellings(driver, protocol, speed):
    """Every spelling of a bit rate the specification allows has to be understood.

    A real customer was blocked for days by this: the Service Tool sends the bit/s form inside a
    Format 1 string, the driver only read the kBaud form, and the result was
    ERR_CAN_BAUD_SET_NONSTANDARD on every attempt with no indication of why.
    """
    cid = driver.connect(protocol=protocol)

    assert 0 <= cid <= 127, f"{protocol!r}: {cid} {driver.error_text(cid)}"

    try:
        rc, reported = driver.connection_speed(cid)
        assert rc == NO_ERRORS
        assert reported == speed, f"{protocol!r} connected at {reported}, expected {speed}"
    finally:
        driver.disconnect(cid)


def test_connect_rejects_rates_the_device_cannot_produce(driver):
    """666 kBaud is in the specification but the CANsub cannot generate it.

    Refused rather than rounded to 500 or 1000: an application that asked for 666 and silently got
    something else would see a bus that does not work and no reason for it.
    """
    refuses(driver, ERR_CAN_BAUD_SET_NONSTANDARD, protocol=b"CAN:Baud=666")
    refuses(driver, ERR_CAN_BAUD_SET_NONSTANDARD, protocol=b"CAN:Baud=666000")
    refuses(driver, ERR_CAN_BAUD_SET_NONSTANDARD, protocol=b"CAN:Baud=333333")
    refuses(driver, ERR_CAN_BAUD_SET_NONSTANDARD, protocol=b"CAN:Baud=0")


def test_protocol_name_is_matched_exactly(driver):
    """The protocol name is compared whole, not as a prefix.

    An earlier version tested only the first three characters, so "CANopen" and friends connected as
    generic CAN and then behaved nothing like the protocol the application asked for.
    """
    refuses(driver, ERR_INVALID_PROTOCOL, protocol=b"CANxyz")
    refuses(driver, ERR_INVALID_PROTOCOL, protocol=b"CANopen")
    refuses(driver, ERR_INVALID_PROTOCOL, protocol=b"J1939xyz")


def write_ini(path, text):
    """Writes the INI and flushes Windows' profile cache.

    GetPrivateProfileString caches the file it last read, so a rewrite from this process can go
    unnoticed by the DLL and a test then exercises the previous contents instead. Passing three
    nulls to WritePrivateProfileString is the documented way to discard that cache.
    """
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)

    ctypes.windll.kernel32.WritePrivateProfileStringA(None, None, None, path.encode())


def test_auto_detected_device(driver, build_dir, device_host):
    """A DeviceParams entry with no host= finds whatever CANsub is attached.

    This is what lets one INI work on every machine, so it is worth proving against real hardware
    rather than trusting the discovery unit tests: the INI is rewritten here with the host removed,
    exactly as the shipped file has it.
    """
    import os
    import re

    path = os.path.join(build_dir, "CANSUB32.ini")

    with open(path, encoding="utf-8") as f:
        original = f.read()

    # Anchored to a line start, so the examples in the file's comments cannot be mistaken for
    # device entries -- which is how an earlier version of this test passed while changing nothing.
    auto, count = re.subn(r"(?m)^DeviceParams=host=[^,]+,channel=(\d+)\s*$",
                          r"DeviceParams=index=1,channel=\1", original)

    assert count >= 1, "no device entry was switched to auto-detection, so nothing was tested"

    try:
        write_ini(path, auto)

        cid = driver.connect(protocol=b"CAN:Baud=250")

        assert 0 <= cid <= 127, f"auto-detect failed: {cid} {driver.error_text(cid)}"

        # And it found a real device, not just any answer: the firmware string comes from it.
        api, dll, fw = (create_string_buffer(17) for _ in range(3))

        assert driver.dll.RP1210_ReadDetailedVersion(cid, api, dll, fw) == NO_ERRORS
        assert fw.value.decode(), "no firmware version, so nothing was really opened"

        driver.disconnect(cid)
    finally:
        write_ini(path, original)


def test_auto_detect_reports_a_missing_device_clearly(driver, build_dir):
    """Asking for the fourth attached CANsub when there is one must say so, not just fail."""
    import os
    import re

    path = os.path.join(build_dir, "CANSUB32.ini")

    with open(path, encoding="utf-8") as f:
        original = f.read()

    auto, count = re.subn(r"(?m)^DeviceParams=host=[^,]+,channel=(\d+)\s*$",
                          r"DeviceParams=index=4,channel=\1", original)

    assert count >= 1, "no device entry was switched to auto-detection, so nothing was tested"

    try:
        write_ini(path, auto)

        result = driver.connect(protocol=b"CAN:Baud=250")

        if 0 <= result <= 127:
            driver.disconnect(result)
            pytest.skip("four or more CANsubs are attached, so this cannot be tested here")

        detail = create_string_buffer(80)
        sub = c_int32(0)

        driver.dll.RP1210_GetLastErrorMsg(result, ctypes.byref(sub), detail, result)
        text = detail.value.decode(errors="replace")

        assert "CANsub number 4" in text or "answered" in text, \
            f"the reason should name the problem, got {text!r}"
    finally:
        write_ini(path, original)


def test_channel_is_exclusive(driver, client):
    """The device permits one client per channel, so a second connect must be refused."""
    assert driver.connect() == ERR_DEVICE_IN_USE


def test_invalid_client_id(driver):
    assert driver.disconnect(77) == ERR_INVALID_CLIENT_ID
    count, _ = driver.read(77)
    assert count == -ERR_INVALID_CLIENT_ID


def test_reconnect_after_disconnect(driver):
    first = driver.connect()
    assert first >= 0
    assert driver.disconnect(first) == NO_ERRORS

    second = driver.connect()
    assert second >= 0, driver.error_text(second)
    driver.disconnect(second)


def test_no_traffic_until_filter_set(driver, client, ch2):
    """RP1210: a client receives nothing until it sets a filter."""
    ch2.send(can.Message(arbitration_id=0x100, data=b"\x01", is_extended_id=False))
    time.sleep(0.3)

    count, _ = driver.read(client)
    assert count == 0, "no filter set, so nothing should reach the client"

    assert driver.command(CMD_SET_ALL_FILTERS_STATES_TO_PASS, client) == NO_ERRORS

    ch2.send(can.Message(arbitration_id=0x100, data=b"\x01", is_extended_id=False))
    deadline = time.time() + 2

    while time.time() < deadline:
        count, body = driver.read(client)
        if count > 0:
            assert parse_can(body, echo=False)["id"] == 0x100
            return
        time.sleep(0.02)

    pytest.fail("nothing received after filters were set to pass")


def test_send_reaches_other_channel(driver, client, ch2):
    payload = b"\x11\x22\x33\x44\x55\x66\x77\x88"

    assert driver.send(client, 0x123, payload) == NO_ERRORS

    msg = ch2.recv(timeout=2)

    assert msg is not None, "python-can-cansub saw nothing on CH2"
    assert msg.arbitration_id == 0x123
    assert not msg.is_extended_id
    assert bytes(msg.data) == payload


def test_send_extended(driver, client, ch2):
    assert driver.send(client, 0x1ABCDEF, b"\xAA\xBB", extended=True) == NO_ERRORS

    msg = ch2.recv(timeout=2)

    assert msg is not None
    assert msg.arbitration_id == 0x1ABCDEF
    assert msg.is_extended_id
    assert bytes(msg.data) == b"\xAA\xBB"


def test_read_layout_without_echo(driver, client, ch2):
    """Echo off: a read is exactly 4 timestamp + 1 type + id + data bytes, with no echo byte."""
    assert driver.command(CMD_SET_ALL_FILTERS_STATES_TO_PASS, client) == NO_ERRORS

    ch2.send(can.Message(arbitration_id=0x321, data=b"\x01\x02\x03", is_extended_id=False))

    count, body = driver.read(client, BLOCKING_IO)

    assert count == 4 + 1 + 2 + 3, f"unexpected length {count}"

    frame = parse_can(body, echo=False)

    assert frame["echo"] is None
    assert frame["id"] == 0x321
    assert frame["data"] == b"\x01\x02\x03"
    assert frame["timestamp"] > 0


def test_read_layout_with_echo(driver, client, ch2):
    """Echo on: every read gains a byte, and an own frame reports 0x01 while a received one is 0x00.

    This is the layout-sensitive case in the specification: the echo byte's presence changes the
    offset of everything after it, so the byte count is asserted and not just the decoded content.
    """
    assert driver.command(CMD_ECHO_TRANSMITTED_MESSAGES, client, bytes([1])) == NO_ERRORS
    assert driver.command(CMD_SET_ALL_FILTERS_STATES_TO_PASS, client) == NO_ERRORS

    driver.command(CMD_FLUSH_TX_RX_BUFFERS, client)

    # Own transmission: echoed back with the flag set.
    assert driver.send(client, 0x111, b"\x42") == NO_ERRORS

    own = None
    deadline = time.time() + 2

    while time.time() < deadline and own is None:
        count, body = driver.read(client)
        if count > 0:
            assert count == 4 + 1 + 1 + 2 + 1, f"unexpected length {count} with echo on"
            frame = parse_can(body, echo=True)
            if frame["id"] == 0x111:
                own = frame
        else:
            time.sleep(0.02)

    assert own is not None, "own frame was not echoed back"
    assert own["echo"] == 0x01, "own frame must report echo byte 0x01"
    assert own["data"] == b"\x42"

    # A frame from the other channel reports 0x00.
    ch2.send(can.Message(arbitration_id=0x222, data=b"\x07", is_extended_id=False))

    other = None
    deadline = time.time() + 2

    while time.time() < deadline and other is None:
        count, body = driver.read(client)
        if count > 0:
            frame = parse_can(body, echo=True)
            if frame["id"] == 0x222:
                other = frame
        else:
            time.sleep(0.02)

    assert other is not None, "frame from CH2 not received"
    assert other["echo"] == 0x00, "frame from elsewhere must report echo byte 0x00"


def test_inclusive_filter(driver, client, ch2):
    assert driver.command(CMD_SET_ALL_FILTERS_STATES_TO_DISCARD, client) == NO_ERRORS

    # One accepted identifier, added the way an application does: one call per id.
    accept = bytes([STANDARD_CAN]) + struct.pack(">H", 0x200)
    assert driver.command(CMD_SET_MESSAGE_FILTERING_FOR_CAN, client, accept) == NO_ERRORS

    ch2.send(can.Message(arbitration_id=0x201, data=b"\x01", is_extended_id=False))
    ch2.send(can.Message(arbitration_id=0x200, data=b"\x02", is_extended_id=False))

    got = []
    deadline = time.time() + 2

    while time.time() < deadline:
        count, body = driver.read(client)
        if count > 0:
            got.append(parse_can(body, echo=False)["id"])
        else:
            time.sleep(0.02)

    assert 0x200 in got, "accepted identifier did not arrive"
    assert 0x201 not in got, "blocked identifier passed the filter"


def test_filter_mask_and_header(driver, client, ch2):
    """RP1210 defines the CAN filter as nine bytes: type, a 4-byte mask, a 4-byte header.

    The mask says which bits matter and the header what they must be, so one call can accept a
    range of identifiers. Reading this as the short "type plus identifier" form would take the mask
    for an identifier and filter against nonsense, which is why the length decides.
    """
    assert driver.command(CMD_SET_ALL_FILTERS_STATES_TO_DISCARD, client) == NO_ERRORS

    # Accept 0x1AB00000..0x1ABFFFFF: the top 12 bits of a 29-bit identifier must match.
    body = bytes([EXTENDED_CAN]) + struct.pack(">I", 0x1FF00000) + struct.pack(">I", 0x1AB00000)
    assert driver.command(CMD_SET_MESSAGE_FILTERING_FOR_CAN, client, body) == NO_ERRORS

    ch2.send(can.Message(arbitration_id=0x1AB12345, data=b"\x01", is_extended_id=True))
    ch2.send(can.Message(arbitration_id=0x1ABFFFFF, data=b"\x02", is_extended_id=True))
    ch2.send(can.Message(arbitration_id=0x1CC12345, data=b"\x03", is_extended_id=True))

    got = []
    deadline = time.time() + 2

    while time.time() < deadline:
        count, body = driver.read(client)
        if count > 0:
            got.append(parse_can(body, echo=False)["id"])
        else:
            time.sleep(0.02)

    assert 0x1AB12345 in got, "an identifier inside the masked range did not arrive"
    assert 0x1ABFFFFF in got, "the top of the masked range did not arrive"
    assert 0x1CC12345 not in got, "an identifier outside the masked range passed"


def test_filter_exclusive_honours_the_mask(driver, client, ch2):
    """An exclusive filter is judged in software, so it has to carry the mask too."""
    assert driver.command(CMD_SET_CAN_FILTER_TYPE, client, bytes([1])) == NO_ERRORS

    body = bytes([EXTENDED_CAN]) + struct.pack(">I", 0x1FF00000) + struct.pack(">I", 0x1AB00000)
    assert driver.command(CMD_SET_MESSAGE_FILTERING_FOR_CAN, client, body) == NO_ERRORS

    ch2.send(can.Message(arbitration_id=0x1AB12345, data=b"\x01", is_extended_id=True))
    ch2.send(can.Message(arbitration_id=0x1CC12345, data=b"\x02", is_extended_id=True))

    got = []
    deadline = time.time() + 2

    while time.time() < deadline:
        count, body = driver.read(client)
        if count > 0:
            got.append(parse_can(body, echo=False)["id"])
        else:
            time.sleep(0.02)

    assert 0x1CC12345 in got, "an identifier outside the blocked range should pass"
    assert 0x1AB12345 not in got, "an identifier inside the blocked range should be dropped"


def test_connection_speed(driver, client):
    buf = create_string_buffer(17)

    assert driver.dll.RP1210_SendCommand(CMD_GET_PROTOCOL_CONNECTION_SPEED, client, buf, 17) == NO_ERRORS
    assert buf.value.decode() == "250"


def test_set_can_baud(driver, client):
    # Byte 0 selects when to change, byte 1 is the baud identifier: '6' is 500k.
    assert driver.command(CMD_SET_CAN_BAUD, client, b"\x00" + b"6") == NO_ERRORS

    buf = create_string_buffer(17)
    driver.dll.RP1210_SendCommand(CMD_GET_PROTOCOL_CONNECTION_SPEED, client, buf, 17)

    assert buf.value.decode() == "500"


def test_block_timeout_is_honoured(driver, client):
    """A blocking read with nothing to read must return 0 after roughly the configured timeout."""
    assert driver.command(CMD_SET_ALL_FILTERS_STATES_TO_DISCARD, client) == NO_ERRORS
    assert driver.command(CMD_SET_BLOCKTIMEOUT, client, struct.pack(">H", 300)) == NO_ERRORS

    start = time.time()
    count, _ = driver.read(client, BLOCKING_IO)
    elapsed = time.time() - start

    assert count == 0
    assert 0.2 < elapsed < 1.5, f"blocking read returned after {elapsed:.2f}s, expected about 0.3s"


def test_hardware_status(driver, client):
    buf = create_string_buffer(18)

    assert driver.dll.RP1210_GetHardwareStatus(client, buf, 18, 0) == NO_ERRORS
    assert buf.raw[0] == 0x01, "hardware should report as present"


def test_j1939_commands_rejected_on_a_can_client(driver, client):
    """The J1939 commands belong to a J1939 client, not to this one.

    The mirror of this test lives in the J1939 suite: the CAN-only commands must be refused there.
    Either test alone would let the driver drift into accepting everything everywhere.
    """
    assert driver.command(CMD_PROTECT_J1939_ADDRESS, client,
                          b"\x80" + b"\x00" * 8 + b"\x00") == ERR_COMMAND_NOT_SUPPORTED
    assert driver.command(CMD_RELEASE_J1939_ADDRESS, client, b"\x80") == ERR_COMMAND_NOT_SUPPORTED
    assert driver.command(CMD_SET_MESSAGE_FILTERING_FOR_J1939, client,
                          b"\x01\x00\xF0\x04\x00\x00\x00") == ERR_COMMAND_NOT_SUPPORTED
    assert driver.command(CMD_SET_J1939_BAUD, client, b"\x00" + b"5") == ERR_COMMAND_NOT_SUPPORTED


# ---------------------------------------------------------------- CANSUB_ReadXnetFrames
#
# The LabVIEW + NI-XNET extension. Not RP1210, so these check it against NI's raw frame format and
# that it reads through the same client state (filters, echo) as RP1210_ReadMessage.


def read_xnet_until(driver, client, wanted, timeout=2):
    """Drains CANSUB_ReadXnetFrames until `wanted` frames have arrived or the time runs out."""
    frames = []
    deadline = time.time() + timeout

    while time.time() < deadline and len(frames) < wanted:
        result, count, raw = driver.read_xnet(client)
        assert result >= 0, f"CANSUB_ReadXnetFrames failed: {result}"
        parsed = parse_xnet(raw)
        assert len(parsed) == count, "frame count does not match the frames written"
        frames += parsed
        if count == 0:
            time.sleep(0.02)

    return frames


def test_xnet_frames_layout(driver, client, ch2):
    """Standard and extended frames, in NI-XNET's raw format, with a wall-clock timestamp."""
    assert driver.command(CMD_SET_ALL_FILTERS_STATES_TO_PASS, client) == NO_ERRORS
    driver.command(CMD_FLUSH_TX_RX_BUFFERS, client)

    ch2.send(can.Message(arbitration_id=0x321, data=b"\x01\x02\x03", is_extended_id=False))
    ch2.send(can.Message(arbitration_id=0x1ABCDEF, data=b"\x11\x22\x33\x44\x55\x66\x77\x88",
                         is_extended_id=True))

    frames = read_xnet_until(driver, client, 2)

    assert [(f["id"], f["extended"], f["data"]) for f in frames] == [
        (0x321, False, b"\x01\x02\x03"),
        (0x1ABCDEF, True, b"\x11\x22\x33\x44\x55\x66\x77\x88"),
    ]

    for f in frames:
        assert f["type"] == XNET_TYPE_CAN_DATA
        assert f["flags"] == 0
        unix = (f["timestamp"] - XNET_EPOCH_OFFSET) / 1e7
        assert abs(unix - time.time()) < 60, "timestamp is not wall-clock time"


def test_xnet_frames_echo_flag(driver, client, ch2):
    """With echo on, an own frame carries NI-XNET's transmit-echo flag; a received one does not."""
    assert driver.command(CMD_ECHO_TRANSMITTED_MESSAGES, client, bytes([1])) == NO_ERRORS
    assert driver.command(CMD_SET_ALL_FILTERS_STATES_TO_PASS, client) == NO_ERRORS
    driver.command(CMD_FLUSH_TX_RX_BUFFERS, client)

    assert driver.send(client, 0x111, b"\x42") == NO_ERRORS
    ch2.send(can.Message(arbitration_id=0x222, data=b"\x07", is_extended_id=False))

    frames = {f["id"]: f for f in read_xnet_until(driver, client, 2)}

    assert frames[0x111]["flags"] == XNET_FLAG_TRANSMIT_ECHO
    assert frames[0x222]["flags"] == 0


def test_xnet_frames_empty_and_bad_arguments(driver, client):
    """Nothing queued reads 0; a buffer below one worst-case frame and a bad client are refused."""
    assert driver.command(CMD_SET_ALL_FILTERS_STATES_TO_DISCARD, client) == NO_ERRORS

    assert driver.read_xnet(client)[:2] == (0, 0)
    assert driver.read_xnet(client, CANSUB_XNET_FRAME_MAX - 1)[0] == -ERR_MESSAGE_TOO_LONG
    assert driver.read_xnet(127)[0] == -ERR_INVALID_CLIENT_ID


def test_xnet_frames_refused_on_j1939(driver):
    """J1939 messages are reassembled, so there is no single CAN frame to hand to NI-XNET."""
    cid = driver.connect(protocol=b"J1939:Baud=250")
    assert 0 <= cid <= 127, driver.error_text(cid)

    try:
        assert driver.read_xnet(cid)[0] == -ERR_INVALID_PROTOCOL
    finally:
        driver.disconnect(cid)


def test_xnet_write_reaches_other_channel(driver, client, ch2):
    """Frames in NI-XNET raw format, standard, extended and remote, arrive on CH2 in order."""
    raw = (build_xnet(0x123, b"\x01\x02\x03")
           + build_xnet(0x1ABCDEF, b"\x11\x22\x33\x44\x55\x66\x77\x88", extended=True)
           + build_xnet(0x456, b"", ftype=XNET_TYPE_CAN_REMOTE, length=2))

    assert driver.write_xnet(client, raw) == (3, 3)

    received = [ch2.recv(timeout=2) for _ in range(3)]
    assert all(m is not None for m in received), "python-can-cansub saw fewer than 3 frames on CH2"

    assert [(m.arbitration_id, m.is_extended_id, m.is_remote_frame) for m in received] == [
        (0x123, False, False), (0x1ABCDEF, True, False), (0x456, False, True)]
    assert bytes(received[0].data) == b"\x01\x02\x03"
    assert bytes(received[1].data) == b"\x11\x22\x33\x44\x55\x66\x77\x88"
    assert received[2].dlc == 2


def test_xnet_write_round_trip_with_echo(driver, client, ch2):
    """With echo on, frames written in NI-XNET format read back in NI-XNET format, flagged as echo.

    ch2 is needed although unused: the device echoes a frame only once another node has
    acknowledged it on the bus."""
    assert driver.command(CMD_ECHO_TRANSMITTED_MESSAGES, client, bytes([1])) == NO_ERRORS
    assert driver.command(CMD_SET_ALL_FILTERS_STATES_TO_PASS, client) == NO_ERRORS
    driver.command(CMD_FLUSH_TX_RX_BUFFERS, client)

    assert driver.write_xnet(client, build_xnet(0x321, b"\xAA\xBB"))[0] == 1

    frames = read_xnet_until(driver, client, 1)

    assert (frames[0]["id"], frames[0]["data"], frames[0]["flags"]) == (
        0x321, b"\xAA\xBB", XNET_FLAG_TRANSMIT_ECHO)


def test_xnet_write_refuses_without_sending(driver, client, ch2):
    """A malformed buffer, CAN FD, or a classic payload over 8 bytes sends nothing at all."""
    good = build_xnet(0x100, b"\x01")

    cases = [
        good + good[:10],                                                # truncated second frame
        good + build_xnet(0x101, bytes(12), ftype=XNET_TYPE_CANFD_DATA),  # CAN FD
        good + build_xnet(0x102, bytes(8), length=9)[:24],               # classic, length over 8
        good + build_xnet(0x103, b"\x01", ftype=0x02),                   # not a CAN frame type
    ]

    for raw in cases:
        assert driver.write_xnet(client, raw) == (-ERR_MESSAGE_NOT_SENT, 0)

    assert ch2.recv(timeout=0.5) is None, "a refused buffer still put a frame on the bus"


def test_xnet_write_empty_and_bad_arguments(driver, client):
    """An empty buffer sends nothing and succeeds; a bad client is refused."""
    assert driver.write_xnet(client, b"") == (0, 0)
    assert driver.write_xnet(127, build_xnet(0x100, b"\x01"))[0] == -ERR_INVALID_CLIENT_ID


def test_xnet_write_refused_on_j1939(driver):
    """J1939 messages go through the J1939 engine, so raw frames are refused."""
    cid = driver.connect(protocol=b"J1939:Baud=250")
    assert 0 <= cid <= 127, driver.error_text(cid)

    try:
        assert driver.write_xnet(cid, build_xnet(0x100, b"\x01"))[0] == -ERR_INVALID_PROTOCOL
    finally:
        driver.disconnect(cid)
