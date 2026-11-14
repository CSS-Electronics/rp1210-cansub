"""The J1939 protocol, driven through the DLL on CH1 and judged on CH2 by python-can-cansub.

The driver is the thing under test; the peer on CH2 is raw CAN, so every frame the driver emits is
decoded by an independent implementation. Byte counts are asserted as well as content, because the
echo byte shifts the layout of everything after it and an off-by-one decodes as plausible garbage.

The transport protocol state machine and the identifier mapping have their own hardware-free tests
in test_j1939.cpp; what is checked here is that the driver puts the right bytes on a real wire.
"""

import time

import pytest

import j1939 as j
from driver import (
    BLOCKING_IO,
    CMD_ECHO_TRANSMITTED_MESSAGES,
    CMD_FLUSH_TX_RX_BUFFERS,
    CMD_PROTECT_J1939_ADDRESS,
    CMD_RELEASE_J1939_ADDRESS,
    CMD_SET_ALL_FILTERS_STATES_TO_DISCARD,
    CMD_SET_ALL_FILTERS_STATES_TO_PASS,
    CMD_SET_BLOCKTIMEOUT,
    CMD_SET_BROADCAST_FOR_J1939,
    CMD_SET_CAN_BAUD,
    CMD_SET_CAN_FILTER_TYPE,
    CMD_SET_J1939_BAUD,
    CMD_SET_J1939_FILTER_TYPE,
    CMD_SET_J1939_INTERPACKET_TIME,
    CMD_SET_MESSAGE_FILTERING_FOR_CAN,
    CMD_SET_MESSAGE_FILTERING_FOR_J1939,
    ERR_ADDRESS_CLAIM_FAILED,
    ERR_ADDRESS_NEVER_CLAIMED,
    ERR_CANNOT_CLAIM_BROADCAST_ADDRESS,
    ERR_CANNOT_DETERMINE_BAUD_RATE,
    ERR_CAN_BAUD_SET_NONSTANDARD,
    ERR_COMMAND_NOT_SUPPORTED,
    ERR_INVALID_COMMAND,
    ERR_J1939_BAUD_SET_NONSTANDARD,
    ERR_MESSAGE_TOO_LONG,
    ERR_OUT_OF_ADDRESS_RESOURCES,
    FILTER_EXCLUSIVE,
    NO_ERRORS,
)

TOOL = 0xF9  # Offboard diagnostic tool 1, the address a service tool normally claims.
PEER = 0x21


@pytest.fixture
def j1939_client(driver):
    """A J1939 client on CH1 at 250k."""
    cid = driver.connect(protocol=b"J1939:Baud=250")

    if cid < 0 or cid > 127:
        pytest.fail(f"ClientConnect failed: {cid} {driver.error_text(cid)}")

    yield cid

    driver.disconnect(cid)


@pytest.fixture
def peer(ch2):
    ecu = j.PeerEcu(ch2, address=PEER)

    yield ecu

    ecu.close()


@pytest.fixture
def claimed(driver, j1939_client, peer):
    """A J1939 client that has protected the service tool address."""
    command = bytes([TOOL]) + j.NAME + bytes([0])

    assert driver.command(CMD_PROTECT_J1939_ADDRESS, j1939_client, command) == NO_ERRORS
    assert driver.command(CMD_SET_ALL_FILTERS_STATES_TO_PASS, j1939_client) == NO_ERRORS

    peer.clear()

    return j1939_client


def read_until(driver, client, predicate, timeout=3.0, echo=False, size=2048):
    """Reads until a message satisfies the predicate, or the time runs out."""
    deadline = time.time() + timeout

    while time.time() < deadline:
        count, body = driver.read(client, size=size)

        if count > 0:
            message = j.parse(body, echo)
            message["count"] = count

            if predicate(message):
                return message
        else:
            time.sleep(0.01)

    return None


# ---------------------------------------------------------------- connect


def test_connect_accepts_the_documented_forms(driver):
    for protocol in (b"J1939", b"J1939:Baud=250", b"J1939:Baud=500", b"J1939:Baud=1000"):
        cid = driver.connect(protocol=protocol)
        assert 0 <= cid <= 127, f"{protocol!r}: {cid} {driver.error_text(cid)}"
        driver.disconnect(cid)


def test_connect_reports_the_speed(driver):
    cid = driver.connect(protocol=b"J1939:Baud=500")
    assert 0 <= cid <= 127

    rc, speed = driver.connection_speed(cid)
    driver.disconnect(cid)

    assert rc == NO_ERRORS
    assert speed == "500"


@pytest.mark.parametrize("protocol,speed", [
    (b"J1939:Baud=250", "250"),
    (b"J1939:Baud=500", "500"),
    # J1939 formats 3 to 5 spell the bit rate in bit/s, the same as the CAN ones.
    (b"J1939:Baud=250000", "250"),
    (b"J1939:Baud=500000", "500"),
    (b"J1939:Baud=500000,SampleLocation=75,SJW=1,IDSize=29", "500"),
    (b"J1939:Baud=250000,PROP_SEG=4,PHASE_SEG1=4,PHASE_SEG2=3,SJW=1,IDSize=29", "250"),
])
def test_j1939_baud_spellings(driver, protocol, speed):
    cid = driver.connect(protocol=protocol)

    assert 0 <= cid <= 127, f"{protocol!r}: {cid} {driver.error_text(cid)}"

    try:
        rc, reported = driver.connection_speed(cid)
        assert rc == NO_ERRORS
        assert reported == speed
    finally:
        driver.disconnect(cid)


def test_unsupported_baud_is_a_j1939_error(driver):
    """666k is legal in RP1210 but the CANsub cannot generate it.

    The J1939 code, not the CAN one: an application that mixes them up gets a misleading message.
    """
    cid = driver.connect(protocol=b"J1939:Baud=666")
    assert cid == ERR_J1939_BAUD_SET_NONSTANDARD

    cid = driver.connect(protocol=b"J1939:Baud=333")
    assert cid == ERR_J1939_BAUD_SET_NONSTANDARD


def test_unknown_tokens_are_ignored(driver):
    """RP1210 lets applications append vendor tokens; an unknown one must not break the connect."""
    cid = driver.connect(protocol=b"J1939:Baud=250,Channel=1")
    assert 0 <= cid <= 127, driver.error_text(cid)
    driver.disconnect(cid)


def test_can_commands_rejected_on_a_j1939_client(driver, j1939_client):
    """The mirror of the J1939-on-a-CAN-client test in test_rp1210.py."""
    assert driver.command(CMD_SET_MESSAGE_FILTERING_FOR_CAN, j1939_client,
                          b"\x01\x00\x00\x01\x00") == ERR_COMMAND_NOT_SUPPORTED
    assert driver.command(CMD_SET_CAN_FILTER_TYPE, j1939_client, b"\x00") == ERR_COMMAND_NOT_SUPPORTED
    assert driver.command(CMD_SET_CAN_BAUD, j1939_client, b"\x00" + b"6") == ERR_COMMAND_NOT_SUPPORTED


def test_broadcast_list_is_refused(driver, j1939_client):
    assert driver.command(CMD_SET_BROADCAST_FOR_J1939, j1939_client,
                          b"\x05") == ERR_COMMAND_NOT_SUPPORTED


# ---------------------------------------------------------------- single frame


def test_single_frame_transmit_pdu2(driver, claimed, peer):
    payload = bytes([0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88])

    assert driver.send_body(claimed, j.pack(0x00F004, payload, priority=3, sa=TOOL)) == NO_ERRORS

    msg = peer.expect(pgn=0x00F004, sa=TOOL)

    assert msg is not None, "nothing arrived on CH2"
    assert msg.arbitration_id == 0x0CF004F9
    assert bytes(msg.data) == payload


def test_single_frame_transmit_pdu2_ignores_destination(driver, claimed, peer):
    """A group-extension message is a broadcast; a destination alongside it must not change the id."""
    assert driver.send_body(claimed, j.pack(0x00F004, b"\x01", priority=3, sa=TOOL,
                                            da=PEER)) == NO_ERRORS

    msg = peer.expect(pgn=0x00F004, sa=TOOL)

    assert msg is not None
    assert msg.arbitration_id == 0x0CF004F9, "destination leaked into a PDU2 identifier"


@pytest.mark.parametrize("priority,expected", [(0, 0x00EF21F9), (6, 0x18EF21F9), (7, 0x1CEF21F9)])
def test_single_frame_transmit_pdu1_priority(driver, claimed, peer, priority, expected):
    peer.clear()

    assert driver.send_body(claimed, j.pack(0x00EF00, b"\xAA\xBB", priority=priority, sa=TOOL,
                                            da=PEER)) == NO_ERRORS

    msg = peer.expect(pgn=0x00EF00, sa=TOOL)

    assert msg is not None
    assert msg.arbitration_id == expected
    assert bytes(msg.data) == b"\xAA\xBB"


def test_single_frame_receive_pdu2(driver, claimed, peer):
    peer.send(0x00F004, bytes(range(1, 9)), priority=3)

    message = read_until(driver, claimed, lambda m: m["pgn"] == 0x00F004)

    assert message is not None
    assert message["count"] == j.read_length(8, echo=False) == 18
    assert message["priority"] == 3
    assert message["sa"] == PEER
    assert message["da"] == 0xFF, "a group-extension message is addressed to everyone"
    assert not message["bam"]
    assert message["data"] == bytes(range(1, 9))


def test_single_frame_receive_pdu1(driver, claimed, peer):
    peer.send(0x00EF00, b"\x01\x02\x03", da=TOOL)

    message = read_until(driver, claimed, lambda m: m["pgn"] == 0x00EF00)

    assert message is not None
    assert message["count"] == j.read_length(3, echo=False) == 13
    assert message["sa"] == PEER
    assert message["da"] == TOOL


def test_zero_length_payload(driver, claimed, peer):
    """The off-by-one catcher: nothing follows the header, so the count is the header exactly."""
    peer.send(0x00EF00, b"", da=TOOL)

    message = read_until(driver, claimed, lambda m: m["pgn"] == 0x00EF00)

    assert message is not None
    assert message["count"] == j.read_length(0, echo=False) == 10
    assert message["data"] == b""


def test_nothing_before_a_filter_is_set(driver, j1939_client, peer):
    """RP1210: a client receives nothing until it sets a filter."""
    peer.send(0x00F004, b"\x01", priority=3)
    time.sleep(0.4)

    count, _ = driver.read(j1939_client, size=2048)
    assert count == 0, "no filter set, so nothing should reach the client"

    assert driver.command(CMD_SET_ALL_FILTERS_STATES_TO_PASS, j1939_client) == NO_ERRORS

    peer.send(0x00F004, b"\x01", priority=3)

    assert read_until(driver, j1939_client, lambda m: m["pgn"] == 0x00F004) is not None


# ---------------------------------------------------------------- echo


def test_echo_layouts(driver, claimed, peer):
    """Echo on adds a byte between the timestamp and the parameter group."""
    assert driver.command(CMD_ECHO_TRANSMITTED_MESSAGES, claimed, bytes([1])) == NO_ERRORS
    assert driver.command(CMD_SET_ALL_FILTERS_STATES_TO_PASS, claimed) == NO_ERRORS
    driver.command(CMD_FLUSH_TX_RX_BUFFERS, claimed)

    assert driver.send_body(claimed, j.pack(0x00F004, b"\x42", priority=3, sa=TOOL)) == NO_ERRORS

    own = read_until(driver, claimed, lambda m: m["pgn"] == 0x00F004 and m["echo"] == 0x01,
                     echo=True)

    assert own is not None, "own message was not echoed back"
    assert own["count"] == j.read_length(1, echo=True) == 12
    assert own["data"] == b"\x42"

    peer.send(0x00FECA, b"\x07", priority=6)

    other = read_until(driver, claimed, lambda m: m["pgn"] == 0x00FECA, echo=True)

    assert other is not None
    assert other["echo"] == 0x00, "a message from elsewhere reports echo byte 0x00"

    assert driver.command(CMD_ECHO_TRANSMITTED_MESSAGES, claimed, bytes([0])) == NO_ERRORS
    assert driver.command(CMD_SET_ALL_FILTERS_STATES_TO_PASS, claimed) == NO_ERRORS

    peer.send(0x00FECA, b"\x08", priority=6)

    without = read_until(driver, claimed, lambda m: m["pgn"] == 0x00FECA, echo=False)

    assert without is not None
    assert without["count"] == j.read_length(1, echo=False) == 11
    assert without["echo"] is None


def test_echo_of_a_broadcast_is_one_message(driver, claimed, peer):
    """The likeliest echo bug: echoing the transport frames instead of the assembled message."""
    assert driver.command(CMD_ECHO_TRANSMITTED_MESSAGES, claimed, bytes([1])) == NO_ERRORS
    assert driver.command(CMD_SET_ALL_FILTERS_STATES_TO_PASS, claimed) == NO_ERRORS
    driver.command(CMD_FLUSH_TX_RX_BUFFERS, claimed)

    payload = bytes(range(1, 21))

    assert driver.send_body(claimed, j.pack(0x00FEF1, payload, sa=TOOL, bam=True)) == NO_ERRORS

    echoes = []
    deadline = time.time() + 4

    while time.time() < deadline:
        count, body = driver.read(claimed, size=2048)

        if count > 0:
            message = j.parse(body, echo=True)

            if message["echo"] == 0x01:
                message["count"] = count
                echoes.append(message)
        else:
            time.sleep(0.02)

    assert len(echoes) == 1, f"expected one assembled echo, got {len(echoes)}"
    assert echoes[0]["count"] == j.read_length(20, echo=True) == 31
    assert echoes[0]["bam"], "reported as carried by broadcast"
    assert echoes[0]["data"] == payload
    assert not any(m["pgn"] in (j.PGN_TP_CM, j.PGN_TP_DT) for m in echoes)


# ---------------------------------------------------------------- broadcast transport


def test_bam_transmit(driver, claimed, peer):
    payload = bytes(range(1, 21))

    assert driver.send_body(claimed, j.pack(0x00FEF1, payload, sa=TOOL, bam=True)) == NO_ERRORS

    frames = peer.collect(4, timeout=5, sa=TOOL)

    assert len(frames) == 4, f"expected an announcement and three packets, got {len(frames)}"
    assert frames[0].arbitration_id == j.can_id(j.PGN_TP_CM, 6, TOOL, 0xFF)
    assert bytes(frames[0].data) == j.tp_bam(0x00FEF1, 20)

    for index, frame in enumerate(frames[1:], start=1):
        assert frame.arbitration_id == j.can_id(j.PGN_TP_DT, 7, TOOL, 0xFF)
        assert frame.data[0] == index

    assert bytes(frames[3].data) == bytes([3]) + payload[14:] + b"\xFF", "last packet padded"

    gaps = j.PeerEcu.gaps(frames[1:])

    assert all(gap >= 40 for gap in gaps), f"packets sent too fast: {gaps}"


def test_bam_interpacket_time(driver, claimed, peer):
    """Command 27 takes a little-endian millisecond count.

    Read big-endian, a request for 10 ms becomes about 46 days, so the transfer would simply never
    finish; the assertion below bounds it so that shows up as a failure and not as a hang.
    """
    assert driver.command(CMD_SET_J1939_INTERPACKET_TIME, claimed,
                          (200).to_bytes(4, "little")) == NO_ERRORS

    payload = bytes(range(1, 21))
    driver.send_body(claimed, j.pack(0x00FEF1, payload, sa=TOOL, bam=True))

    frames = peer.collect(4, timeout=6, sa=TOOL)
    assert len(frames) == 4
    assert all(gap >= 180 for gap in j.PeerEcu.gaps(frames[1:]))

    peer.clear()

    assert driver.command(CMD_SET_J1939_INTERPACKET_TIME, claimed,
                          (10).to_bytes(4, "little")) == NO_ERRORS

    driver.send_body(claimed, j.pack(0x00FEF1, payload, sa=TOOL, bam=True))

    frames = peer.collect(4, timeout=3, sa=TOOL)
    assert len(frames) == 4, "a little-endian 10 ms should finish well inside three seconds"
    assert all(gap <= 60 for gap in j.PeerEcu.gaps(frames[1:]))

    assert driver.command(CMD_SET_J1939_INTERPACKET_TIME, claimed, b"\x0A\x00") == ERR_INVALID_COMMAND


def test_bam_receive(driver, claimed, peer):
    """The timestamp must be the last packet's, so a gap before it has to show."""
    payload = bytes(range(1, 21))

    peer.send_bam(0x00FEF1, payload, gap=0.06)
    peer.send(0x00FF99, b"\x01")  # marker, immediately after the last packet

    message = read_until(driver, claimed, lambda m: m["pgn"] == 0x00FEF1)

    assert message is not None
    assert message["count"] == j.read_length(20, echo=False) == 30
    assert message["sa"] == PEER
    assert message["da"] == 0xFF
    assert message["bam"], "reported as carried by broadcast"
    assert message["data"] == payload

    marker = read_until(driver, claimed, lambda m: m["pgn"] == 0x00FF99)

    assert marker is not None
    assert abs(marker["timestamp"] - message["timestamp"]) <= 60, \
        "the reassembled message should be stamped with its last packet, not its announcement"


def test_transport_frames_never_reach_the_application(driver, claimed, peer):
    peer.send_bam(0x00FEF1, bytes(range(1, 21)), gap=0.055)

    seen = []
    deadline = time.time() + 3

    while time.time() < deadline:
        count, body = driver.read(claimed, size=2048)

        if count > 0:
            seen.append(j.parse(body, echo=False)["pgn"])
        else:
            time.sleep(0.02)

    assert j.PGN_TP_CM not in seen, "connection management leaked to the application"
    assert j.PGN_TP_DT not in seen, "data transfer leaked to the application"
    assert 0x00FEF1 in seen


def test_bam_with_a_missing_packet_is_discarded(driver, claimed, peer):
    peer.send_bam(0x00FEF1, bytes(range(1, 21)), gap=0.055, drop_seq=2)

    time.sleep(1.2)

    assert read_until(driver, claimed, lambda m: m["pgn"] == 0x00FEF1, timeout=0.5) is None
    assert peer.expect_none(pgn=j.PGN_TP_CM, sa=TOOL, timeout=0.5), \
        "a broadcast must never be aborted"


# ---------------------------------------------------------------- connection-mode transport


@pytest.mark.parametrize("chunk", [1, 2, None], ids=["one-at-a-time", "two-at-a-time", "all"])
def test_rts_cts_transmit(driver, claimed, peer, chunk):
    """A peer granting one packet at a time is the common real-world case, not an edge case."""
    import threading

    payload = bytes(range(1, 21))
    result = {}

    def responder():
        result["session"] = peer.respond_to_rts(cts_chunk=chunk, timeout=4)

    thread = threading.Thread(target=responder, daemon=True)
    thread.start()

    time.sleep(0.05)

    assert driver.send_body(claimed, j.pack(0x00EF00, payload, sa=TOOL, da=PEER)) == NO_ERRORS

    thread.join(timeout=8)

    assert result.get("session") is not None, "the transfer did not complete"
    assert result["session"]["size"] == 20
    assert result["session"]["packets"] == 3

    data = [f for f in peer.seen()
            if j.decode_id(f.arbitration_id)["pgn"] == j.PGN_TP_DT
            and j.decode_id(f.arbitration_id)["sa"] == TOOL]

    assert len(data) == 3, f"expected three data packets, got {len(data)}"
    assert [f.data[0] for f in data] == [1, 2, 3]
    assert bytes(data[2].data) == bytes([3]) + payload[14:] + b"\xFF"


def test_rts_cts_receive(driver, claimed, peer):
    payload = bytes(range(1, 21))

    ack = peer.originate_rts(0x00EF00, payload, da=TOOL)

    assert ack is not None, "the driver did not acknowledge the transfer"
    assert bytes(ack.data) == j.tp_eom(0x00EF00, 20)

    # From the log rather than the stream: originate_rts has already read past the clear-to-send.
    control = [f for f in peer.seen()
               if j.decode_id(f.arbitration_id)["pgn"] == j.PGN_TP_CM
               and j.decode_id(f.arbitration_id)["sa"] == TOOL]

    assert any(f.data[0] == j.TP_CTS for f in control), "the driver did not answer the request"

    message = read_until(driver, claimed, lambda m: m["pgn"] == 0x00EF00)

    assert message is not None
    assert message["count"] == j.read_length(20, echo=False) == 30
    assert message["sa"] == PEER
    assert message["da"] == TOOL, "addressed to the address we claimed"
    assert not message["bam"], "carried by connection mode, not broadcast"
    assert message["data"] == payload


def test_rts_for_another_node_is_ignored(driver, claimed, peer):
    peer.send(j.PGN_TP_CM, j.tp_rts(0x00EF00, 20), priority=7, da=0x33)

    assert peer.expect_none(pgn=j.PGN_TP_CM, sa=TOOL, timeout=1.0), \
        "answered a transfer aimed at another node"


def test_transmit_without_a_grant_times_out(driver, claimed, peer):
    """No clear-to-send arrives, so T3 expires and the driver gives up cleanly."""
    payload = bytes(range(1, 21))

    assert driver.send_body(claimed, j.pack(0x00EF00, payload, sa=TOOL, da=PEER)) == NO_ERRORS

    abort = peer.expect(pgn=j.PGN_TP_CM, sa=TOOL, first=j.TP_ABORT, timeout=3)

    assert abort is not None, "no abort after the timeout"
    assert abort.data[1] == j.ABORT_TIMEOUT

    assert peer.expect_none(pgn=j.PGN_TP_DT, sa=TOOL, timeout=0.5), "data sent without a grant"

    # And the driver is usable again straight away.
    peer.clear()
    assert driver.send_body(claimed, j.pack(0x00F004, b"\x01", priority=3, sa=TOOL)) == NO_ERRORS
    assert peer.expect(pgn=0x00F004, sa=TOOL, timeout=2) is not None


def test_transport_needs_a_claimed_address(driver, j1939_client, peer):
    assert driver.command(CMD_SET_ALL_FILTERS_STATES_TO_PASS, j1939_client) == NO_ERRORS

    payload = bytes(range(1, 21))

    assert driver.send_body(j1939_client, j.pack(0x00EF00, payload, sa=TOOL,
                                                 da=PEER)) == ERR_ADDRESS_NEVER_CLAIMED

    # A single frame goes out regardless, which RP1210E section 16.7 requires.
    assert driver.send_body(j1939_client, j.pack(0x00EF00, b"\x01", sa=TOOL, da=PEER)) == NO_ERRORS
    assert peer.expect(pgn=0x00EF00, sa=TOOL, timeout=2) is not None


def test_message_too_long(driver, claimed):
    assert driver.send_body(claimed, j.pack(0x00FEF1, bytes(1786), sa=TOOL,
                                            bam=True)) == ERR_MESSAGE_TOO_LONG


def test_maximum_message(driver, claimed, peer):
    """1785 bytes is the largest the RP1210 J1939 field can express."""
    assert driver.command(CMD_SET_J1939_INTERPACKET_TIME, claimed,
                          (10).to_bytes(4, "little")) == NO_ERRORS

    payload = bytes((i % 251) for i in range(1785))

    assert driver.send_body(claimed, j.pack(0x00FEF1, payload, sa=TOOL, bam=True)) == NO_ERRORS

    frames = peer.collect(256, timeout=30, sa=TOOL)

    assert len(frames) == 256, f"expected 256 frames, got {len(frames)}"
    assert bytes(frames[0].data) == j.tp_bam(0x00FEF1, 1785)

    rebuilt = b"".join(bytes(f.data)[1:] for f in frames[1:])

    assert rebuilt == payload, "the payload did not survive segmentation"


def test_small_buffer_keeps_the_message(driver, claimed, peer):
    """A read into too small a buffer is an error, and the message waits for a bigger one."""
    payload = bytes(range(1, 21))

    peer.send_bam(0x00FEF1, payload, gap=0.055)

    deadline = time.time() + 4
    short = None

    while time.time() < deadline:
        count, _ = driver.read(claimed, size=16)

        if count < 0:
            short = count
            break

        time.sleep(0.02)

    assert short == -ERR_MESSAGE_TOO_LONG, f"expected a too-long error, got {short}"

    message = read_until(driver, claimed, lambda m: m["pgn"] == 0x00FEF1)

    assert message is not None, "the message was lost instead of being kept"
    assert message["data"] == payload


# ---------------------------------------------------------------- address claim


def test_address_claim_is_announced(driver, j1939_client, peer):
    command = bytes([TOOL]) + j.NAME + bytes([0])

    assert driver.command(CMD_PROTECT_J1939_ADDRESS, j1939_client, command) == NO_ERRORS

    claim = peer.expect(pgn=j.PGN_ADDRESS_CLAIM, sa=TOOL)

    assert claim is not None, "no address claim on the wire"
    assert claim.arbitration_id == 0x18EEFFF9
    assert bytes(claim.data) == j.NAME, "the NAME must travel exactly as the application gave it"


def test_address_claim_rejects_broadcast_and_null(driver, j1939_client):
    for address in (0xFF, 0xFE):
        command = bytes([address]) + j.NAME + bytes([0])
        assert driver.command(CMD_PROTECT_J1939_ADDRESS, j1939_client,
                              command) == ERR_CANNOT_CLAIM_BROADCAST_ADDRESS

    assert driver.command(CMD_PROTECT_J1939_ADDRESS, j1939_client,
                          bytes([TOOL]) + j.NAME) == ERR_INVALID_COMMAND


def test_second_address_is_refused(driver, claimed):
    """One address per client, which is what J1939Addresses=1 in the INI declares."""
    assert driver.command(CMD_PROTECT_J1939_ADDRESS, claimed,
                          bytes([0x80]) + j.NAME + bytes([0])) == ERR_OUT_OF_ADDRESS_RESOURCES

    # Re-protecting the same address is idempotent.
    assert driver.command(CMD_PROTECT_J1939_ADDRESS, claimed,
                          bytes([TOOL]) + j.NAME + bytes([0])) == NO_ERRORS


def test_address_lost_to_a_lower_name(driver, claimed, peer):
    peer.claim(TOOL, j.NAME_LOWER)

    cannot = peer.expect(pgn=j.PGN_ADDRESS_CLAIM, sa=j.ADDRESS_NULL, timeout=2)

    assert cannot is not None, "did not concede the address"
    assert cannot.arbitration_id == 0x18EEFFFE
    assert bytes(cannot.data) == j.NAME

    payload = bytes(range(1, 21))

    assert driver.send_body(claimed, j.pack(0x00EF00, payload, sa=TOOL, da=PEER)) != NO_ERRORS


def test_address_defended_against_a_higher_name(driver, claimed, peer):
    peer.claim(TOOL, j.NAME_HIGHER)

    again = peer.expect(pgn=j.PGN_ADDRESS_CLAIM, sa=TOOL, timeout=2)

    assert again is not None, "did not restate the claim"
    assert bytes(again.data) == j.NAME

    # And the client is still usable.
    peer.clear()
    assert driver.send_body(claimed, j.pack(0x00F004, b"\x01", priority=3, sa=TOOL)) == NO_ERRORS
    assert peer.expect(pgn=0x00F004, sa=TOOL, timeout=2) is not None


def test_identical_name_is_our_own_echo(driver, claimed, peer):
    """The same NAME is either our own claim coming back or a duplicate node; conceding is wrong."""
    peer.claim(TOOL, j.NAME)

    assert peer.expect_none(pgn=j.PGN_ADDRESS_CLAIM, sa=j.ADDRESS_NULL, timeout=1.0), \
        "conceded to a node carrying our own NAME"


def test_request_for_address_claim_is_answered(driver, claimed, peer):
    peer.request(j.PGN_ADDRESS_CLAIM)
    assert peer.expect(pgn=j.PGN_ADDRESS_CLAIM, sa=TOOL, timeout=2) is not None

    peer.clear()
    peer.request(j.PGN_ADDRESS_CLAIM, da=TOOL)
    assert peer.expect(pgn=j.PGN_ADDRESS_CLAIM, sa=TOOL, timeout=2) is not None

    peer.clear()
    peer.request(j.PGN_ADDRESS_CLAIM, da=0x33)
    assert peer.expect_none(pgn=j.PGN_ADDRESS_CLAIM, sa=TOOL, timeout=1.0), \
        "answered a request aimed at another node"


def test_request_answered_with_filters_discarded(driver, claimed, peer):
    """Answering is the driver's own duty, so an application filter must not silence it."""
    assert driver.command(CMD_SET_ALL_FILTERS_STATES_TO_DISCARD, claimed) == NO_ERRORS

    peer.clear()
    peer.request(j.PGN_ADDRESS_CLAIM)

    assert peer.expect(pgn=j.PGN_ADDRESS_CLAIM, sa=TOOL, timeout=2) is not None


def test_address_release(driver, claimed, peer):
    assert driver.command(CMD_RELEASE_J1939_ADDRESS, claimed,
                          bytes([0x80])) == ERR_ADDRESS_NEVER_CLAIMED
    assert driver.command(CMD_RELEASE_J1939_ADDRESS, claimed, bytes([TOOL])) == NO_ERRORS

    peer.clear()
    peer.request(j.PGN_ADDRESS_CLAIM)

    assert peer.expect_none(pgn=j.PGN_ADDRESS_CLAIM, sa=TOOL, timeout=1.0), \
        "still answering for a released address"

    assert driver.command(CMD_RELEASE_J1939_ADDRESS, claimed,
                          bytes([TOOL])) == ERR_ADDRESS_NEVER_CLAIMED


# ---------------------------------------------------------------- filtering


def test_filter_by_pgn(driver, j1939_client, peer):
    assert driver.command(CMD_SET_ALL_FILTERS_STATES_TO_DISCARD, j1939_client) == NO_ERRORS

    wanted = bytes([j.FILTER_PGN]) + (0x00F004).to_bytes(3, "little") + b"\x00\x00\x00"
    assert driver.command(CMD_SET_MESSAGE_FILTERING_FOR_J1939, j1939_client, wanted) == NO_ERRORS

    peer.send(0x00F004, b"\x01", priority=3)
    peer.send(0x00F005, b"\x02", priority=3)
    peer.send(0x00FECA, b"\x03", priority=6)
    peer.send(0x00F004, b"\x04", priority=7)

    seen = []
    deadline = time.time() + 2

    while time.time() < deadline:
        count, body = driver.read(j1939_client, size=2048)

        if count > 0:
            seen.append(j.parse(body, echo=False))
        else:
            time.sleep(0.02)

    groups = [m["pgn"] for m in seen]

    assert 0x00F004 in groups
    assert 0x00F005 not in groups
    assert 0x00FECA not in groups
    assert len([g for g in groups if g == 0x00F004]) == 2, \
        "priority is not part of a parameter group filter"


def test_filter_by_source_and_destination(driver, claimed, peer):
    assert driver.command(CMD_SET_ALL_FILTERS_STATES_TO_DISCARD, claimed) == NO_ERRORS

    wanted = bytes([j.FILTER_SOURCE]) + b"\x00\x00\x00" + b"\x00" + bytes([PEER]) + b"\x00"
    assert driver.command(CMD_SET_MESSAGE_FILTERING_FOR_J1939, claimed, wanted) == NO_ERRORS

    peer.send(0x00F004, b"\x01", priority=3)
    peer.send(0x00F004, b"\x02", priority=3, sa=0x33)

    seen = []
    deadline = time.time() + 2

    while time.time() < deadline:
        count, body = driver.read(claimed, size=2048)

        if count > 0:
            seen.append(j.parse(body, echo=False))
        else:
            time.sleep(0.02)

    assert [m["sa"] for m in seen] == [PEER]


def test_exclusive_filter(driver, claimed, peer):
    assert driver.command(CMD_SET_J1939_FILTER_TYPE, claimed, bytes([FILTER_EXCLUSIVE])) == NO_ERRORS

    blocked = bytes([j.FILTER_PGN]) + (0x00F004).to_bytes(3, "little") + b"\x00\x00\x00"
    assert driver.command(CMD_SET_MESSAGE_FILTERING_FOR_J1939, claimed, blocked) == NO_ERRORS

    peer.send(0x00F004, b"\x01", priority=3)
    peer.send(0x00FECA, b"\x02", priority=6)

    seen = []
    deadline = time.time() + 2

    while time.time() < deadline:
        count, body = driver.read(claimed, size=2048)

        if count > 0:
            seen.append(j.parse(body, echo=False)["pgn"])
        else:
            time.sleep(0.02)

    assert 0x00FECA in seen
    assert 0x00F004 not in seen


def test_filter_does_not_break_transport(driver, claimed, peer):
    """A filter on the assembled group must not stop the driver seeing the transport frames.

    If the driver installed a hardware filter for the wanted group, the announcement and data
    packets would be dropped in silicon and this reassembly would never happen.
    """
    assert driver.command(CMD_SET_ALL_FILTERS_STATES_TO_DISCARD, claimed) == NO_ERRORS

    wanted = bytes([j.FILTER_PGN]) + (0x00FEF1).to_bytes(3, "little") + b"\x00\x00\x00"
    assert driver.command(CMD_SET_MESSAGE_FILTERING_FOR_J1939, claimed, wanted) == NO_ERRORS

    payload = bytes(range(1, 21))
    peer.send_bam(0x00FEF1, payload, gap=0.055)

    message = read_until(driver, claimed, lambda m: m["pgn"] == 0x00FEF1, timeout=4)

    assert message is not None, "the wanted group did not survive reassembly under a filter"
    assert message["data"] == payload

    # A group the application did not ask for is reassembled and then dropped, not leaked.
    peer.send_bam(0x00FEF2, payload, gap=0.055)

    assert read_until(driver, claimed, lambda m: m["pgn"] == 0x00FEF2, timeout=2) is None


def test_bad_filter_flags_are_refused(driver, j1939_client):
    bad = bytes([0x02]) + b"\x00\x00\x00\x00\x00\x00"
    assert driver.command(CMD_SET_MESSAGE_FILTERING_FOR_J1939, j1939_client, bad) == ERR_INVALID_COMMAND

    assert driver.command(CMD_SET_MESSAGE_FILTERING_FOR_J1939, j1939_client,
                          b"\x01\x00\x00") == ERR_INVALID_COMMAND


# ---------------------------------------------------------------- commands


def test_set_j1939_baud(driver, j1939_client):
    """Both encodings of the baud identifier are accepted, because applications disagree."""
    assert driver.command(CMD_SET_J1939_BAUD, j1939_client, b"\x00" + b"6") == NO_ERRORS

    rc, speed = driver.connection_speed(j1939_client)
    assert rc == NO_ERRORS and speed == "500"

    assert driver.command(CMD_SET_J1939_BAUD, j1939_client, b"\x00\x05") == NO_ERRORS

    rc, speed = driver.connection_speed(j1939_client)
    assert rc == NO_ERRORS and speed == "250"

    assert driver.command(CMD_SET_J1939_BAUD, j1939_client,
                          b"\x00" + b"9") == ERR_J1939_BAUD_SET_NONSTANDARD
    assert driver.command(CMD_SET_J1939_BAUD, j1939_client, b"\x00") == ERR_INVALID_COMMAND


def test_baud_auto_detects_the_rate(driver, ch2, device_host):
    """"Auto" is a real probe, not a guess: the driver listens and takes the rate that talks.

    Needs traffic to find, which is the honest limitation of doing it properly, so the peer keeps
    the bus busy while the probe runs.
    """
    import threading

    import can

    stop = threading.Event()

    def chatter():
        while not stop.is_set():
            try:
                ch2.send(can.Message(arbitration_id=0x0CF00400, data=b"\x01" * 8,
                                     is_extended_id=True))
            except Exception:  # noqa: BLE001 - the bus closes underneath us at the end
                return

            time.sleep(0.02)

    thread = threading.Thread(target=chatter, daemon=True)
    thread.start()

    try:
        cid = driver.connect(protocol=b"J1939:Baud=Auto")

        assert 0 <= cid <= 127, f"probe failed: {cid} {driver.error_text(cid)}"

        rc, speed = driver.connection_speed(cid)
        driver.disconnect(cid)

        assert rc == NO_ERRORS
        assert speed == "250", "the peer is at 250k, so the probe should settle there"
    finally:
        stop.set()
        thread.join(timeout=2)


def test_baud_auto_gives_up_on_a_silent_bus(driver):
    """Nothing to hear means nothing to report. Better than silently picking the wrong rate."""
    cid = driver.connect(protocol=b"J1939:Baud=Auto")

    if 0 <= cid <= 127:
        driver.disconnect(cid)
        pytest.skip("the bus is not quiet, so this cannot be tested here")

    assert cid == ERR_CANNOT_DETERMINE_BAUD_RATE


def test_can_baud_auto_is_still_refused(driver):
    """The CAN protocol declares CANAutoBaud=FALSE, and has to behave that way."""
    cid = driver.connect(protocol=b"CAN:Baud=Auto")

    if 0 <= cid <= 127:
        driver.disconnect(cid)
        pytest.fail("CAN:Baud=Auto should be refused")

    assert cid == ERR_CAN_BAUD_SET_NONSTANDARD


def test_app_packetizing_passes_transport_frames_through(driver, peer):
    """With nIsAppPacketizingIncomingMsgs set the application owns the transport protocol.

    RP1210E section 14.2 describes the flag as covering inbound and outbound alike, so the driver
    neither reassembles nor segments, and the raw frames become ordinary traffic.
    """
    cid = driver.connect_raw(1, b"J1939:Baud=250", app_packetizing=1)

    assert 0 <= cid <= 127, driver.error_text(cid)

    try:
        assert driver.command(CMD_SET_ALL_FILTERS_STATES_TO_PASS, cid) == NO_ERRORS

        peer.clear()
        peer.send_bam(0x00FEF1, bytes(range(1, 21)), gap=0.03)

        seen = []
        deadline = time.time() + 3

        while time.time() < deadline:
            count, body = driver.read(cid, size=2048)

            if count > 0:
                seen.append(j.parse(body, echo=False)["pgn"])
            else:
                time.sleep(0.02)

        assert j.PGN_TP_CM in seen, "the announcement should reach the application"
        assert j.PGN_TP_DT in seen, "the data packets should reach the application"
        assert 0x00FEF1 not in seen, "the driver should not have reassembled anything"

        # And it will not segment on the way out either.
        assert driver.send_body(cid, j.pack(0x00FEF1, bytes(range(1, 21)), sa=TOOL,
                                            bam=True)) == ERR_MESSAGE_TOO_LONG
    finally:
        driver.disconnect(cid)


def test_repeated_sessions_do_not_leak(driver, claimed, peer):
    """The cheapest test for "it worked for ten minutes and then stopped".

    Connect churn and transport sessions are where handles and session slots leak, and a leak shows
    up as the tenth exchange behaving differently from the first.
    """
    import threading

    payload = bytes(range(1, 21))

    for round_number in range(10):
        peer.clear(settle=0.05)

        # A broadcast out.
        assert driver.send_body(claimed, j.pack(0x00FEF1, payload, sa=TOOL, bam=True)) == NO_ERRORS

        frames = peer.collect(4, timeout=5, sa=TOOL, pgn=None)
        assert len(frames) >= 4, f"round {round_number}: broadcast stalled"

        # A broadcast in.
        peer.send_bam(0x00FEF2, payload, gap=0.03)

        message = read_until(driver, claimed, lambda m: m["pgn"] == 0x00FEF2, timeout=4)
        assert message is not None, f"round {round_number}: inbound broadcast lost"
        assert message["data"] == payload

        # A connection-mode transfer out, with the peer answering.
        result = {}

        def responder():
            result["session"] = peer.respond_to_rts(cts_chunk=2, timeout=4)

        thread = threading.Thread(target=responder, daemon=True)
        thread.start()
        time.sleep(0.05)

        assert driver.send_body(claimed, j.pack(0x00EF00, payload, sa=TOOL, da=PEER)) == NO_ERRORS

        thread.join(timeout=8)
        assert result.get("session") is not None, f"round {round_number}: transfer did not complete"

    # Still answering for its address after all that.
    peer.clear()
    peer.request(j.PGN_ADDRESS_CLAIM)
    assert peer.expect(pgn=j.PGN_ADDRESS_CLAIM, sa=TOOL, timeout=2) is not None


def test_block_timeout_is_honoured(driver, j1939_client):
    assert driver.command(CMD_SET_ALL_FILTERS_STATES_TO_DISCARD, j1939_client) == NO_ERRORS
    assert driver.command(CMD_SET_BLOCKTIMEOUT, j1939_client, (300).to_bytes(2, "big")) == NO_ERRORS

    start = time.time()
    count, _ = driver.read(j1939_client, BLOCKING_IO, size=2048)
    elapsed = time.time() - start

    assert count == 0
    assert 0.2 < elapsed < 1.5, f"blocking read returned after {elapsed:.2f}s, expected about 0.3s"
