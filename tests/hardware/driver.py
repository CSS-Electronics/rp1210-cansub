"""The ctypes binding to CANSUB32.dll, plus the RP1210 constants the tests assert against.

Shared by test_rp1210.py and test_rp1210_j1939.py so the two suites cannot drift apart on the
function signatures. Fixtures live in conftest.py; protocol helpers live in j1939.py.
"""

import ctypes
import struct
from ctypes import POINTER, c_char_p, c_int32, c_long, c_short, c_void_p, create_string_buffer

NO_ERRORS = 0

ERR_INVALID_CLIENT_ID = 129
ERR_MESSAGE_TOO_LONG = 141
ERR_INVALID_DEVICE = 134
ERR_DEVICE_IN_USE = 135
ERR_INVALID_PROTOCOL = 136
ERR_MESSAGE_NOT_SENT = 159
ERR_INVALID_COMMAND = 144
ERR_HARDWARE_NOT_RESPONDING = 142
ERR_COMMAND_NOT_SUPPORTED = 143
ERR_ADDRESS_CLAIM_FAILED = 146
ERR_COULD_NOT_TX_ADDRESS_CLAIMED = 152
ERR_ADDRESS_LOST = 153
ERR_ADDRESS_NEVER_CLAIMED = 157
ERR_MAX_FILTERS_EXCEEDED = 161
ERR_CANNOT_CLAIM_BROADCAST_ADDRESS = 225
ERR_OUT_OF_ADDRESS_RESOURCES = 226
ERR_ADDRESS_RELEASE_FAILED = 227
ERR_CAN_BAUD_SET_NONSTANDARD = 454
ERR_J1939_BAUD_SET_NONSTANDARD = 457
ERR_CANNOT_DETERMINE_BAUD_RATE = 603

CMD_RESET_DEVICE = 0
CMD_SET_ALL_FILTERS_STATES_TO_PASS = 3
CMD_SET_MESSAGE_FILTERING_FOR_J1939 = 4
CMD_SET_MESSAGE_FILTERING_FOR_CAN = 5
CMD_ECHO_TRANSMITTED_MESSAGES = 16
CMD_SET_ALL_FILTERS_STATES_TO_DISCARD = 17
CMD_PROTECT_J1939_ADDRESS = 19
CMD_SET_BROADCAST_FOR_J1939 = 22
CMD_SET_J1939_FILTER_TYPE = 25
CMD_SET_CAN_FILTER_TYPE = 26
CMD_SET_J1939_INTERPACKET_TIME = 27
CMD_RELEASE_J1939_ADDRESS = 31
CMD_SET_J1939_BAUD = 37
CMD_FLUSH_TX_RX_BUFFERS = 39
CMD_GET_PROTOCOL_CONNECTION_SPEED = 45
CMD_SET_CAN_BAUD = 47
CMD_SET_BLOCKTIMEOUT = 215

BLOCKING_IO = 1
NON_BLOCKING_IO = 0

STANDARD_CAN = 0x00
EXTENDED_CAN = 0x01

FILTER_INCLUSIVE = 0x00
FILTER_EXCLUSIVE = 0x01

ECHO_OFF = 0x00
ECHO_ON = 0x01


class Driver:
    """Thin ctypes binding, with the signatures the RP1210E specification defines."""

    def __init__(self, path):
        self.dll = ctypes.WinDLL(path)

        self.dll.RP1210_ClientConnect.argtypes = [c_long, c_short, c_char_p, c_long, c_long, c_short]
        self.dll.RP1210_ClientConnect.restype = c_short
        self.dll.RP1210_ClientDisconnect.argtypes = [c_short]
        self.dll.RP1210_ClientDisconnect.restype = c_short
        self.dll.RP1210_SendMessage.argtypes = [c_short, c_char_p, c_short, c_short, c_short]
        self.dll.RP1210_SendMessage.restype = c_short
        self.dll.RP1210_ReadMessage.argtypes = [c_short, c_char_p, c_short, c_short]
        self.dll.RP1210_ReadMessage.restype = c_short
        self.dll.RP1210_SendCommand.argtypes = [c_short, c_short, c_char_p, c_short]
        self.dll.RP1210_SendCommand.restype = c_short
        self.dll.RP1210_ReadVersion.argtypes = [c_char_p] * 4
        self.dll.RP1210_ReadVersion.restype = None
        self.dll.RP1210_ReadDetailedVersion.argtypes = [c_short, c_char_p, c_char_p, c_char_p]
        self.dll.RP1210_ReadDetailedVersion.restype = c_short
        self.dll.RP1210_GetErrorMsg.argtypes = [c_short, c_char_p]
        self.dll.RP1210_GetErrorMsg.restype = c_short
        self.dll.RP1210_GetLastErrorMsg.argtypes = [c_short, POINTER(c_int32), c_char_p, c_short]
        self.dll.RP1210_GetLastErrorMsg.restype = c_short
        self.dll.RP1210_GetHardwareStatus.argtypes = [c_short, c_char_p, c_short, c_short]
        self.dll.RP1210_GetHardwareStatus.restype = c_short
        self.dll.RP1210_Ioctl.argtypes = [c_short, c_long, c_void_p, c_void_p]
        self.dll.RP1210_Ioctl.restype = c_short

        # CSS Electronics extension for LabVIEW + NI-XNET, not part of RP1210.
        self.dll.CANSUB_ReadXnetFrames.argtypes = [c_short, c_char_p, c_long, POINTER(c_long)]
        self.dll.CANSUB_ReadXnetFrames.restype = c_long
        self.dll.CANSUB_WriteXnetFrames.argtypes = [c_short, c_char_p, c_long, POINTER(c_long)]
        self.dll.CANSUB_WriteXnetFrames.restype = c_long

    def connect(self, device=1, protocol=b"CAN:Baud=250"):
        return self.dll.RP1210_ClientConnect(0, device, protocol, 0, 0, 0)

    def connect_raw(self, device, protocol, app_packetizing=0):
        """ClientConnect with control over nIsAppPacketizingIncomingMsgs."""
        return self.dll.RP1210_ClientConnect(0, device, protocol, 0, 0, app_packetizing)

    def disconnect(self, client):
        return self.dll.RP1210_ClientDisconnect(client)

    def error_text(self, code):
        buf = create_string_buffer(80)
        self.dll.RP1210_GetErrorMsg(code, buf)
        return buf.value.decode(errors="replace")

    def command(self, number, client, data=b""):
        return self.dll.RP1210_SendCommand(number, client, data, len(data))

    def send(self, client, can_id, payload, extended=False):
        if extended:
            body = bytes([EXTENDED_CAN]) + struct.pack(">I", can_id) + payload
        else:
            body = bytes([STANDARD_CAN]) + struct.pack(">H", can_id) + payload
        return self.dll.RP1210_SendMessage(client, body, len(body), 0, 0)

    def read_xnet(self, client, size=65536):
        """CANSUB_ReadXnetFrames: (result, frame count, the bytes written)."""
        buf = create_string_buffer(size)
        count = c_long(-1)
        result = self.dll.CANSUB_ReadXnetFrames(client, buf, size, ctypes.byref(count))
        return result, count.value, buf.raw[:max(result, 0)]

    def write_xnet(self, client, raw):
        """CANSUB_WriteXnetFrames: (result, frame count)."""
        count = c_long(-1)
        result = self.dll.CANSUB_WriteXnetFrames(client, raw, len(raw), ctypes.byref(count))
        return result, count.value

    def send_body(self, client, body):
        """Sends an already-formatted message body, for the J1939 layout."""
        return self.dll.RP1210_SendMessage(client, body, len(body), 0, 0)

    def read(self, client, blocking=NON_BLOCKING_IO, size=64):
        buf = create_string_buffer(size)
        count = self.dll.RP1210_ReadMessage(client, buf, size, blocking)
        return count, buf.raw[:count] if count > 0 else b""

    def connection_speed(self, client):
        buf = create_string_buffer(17)
        rc = self.dll.RP1210_SendCommand(CMD_GET_PROTOCOL_CONNECTION_SPEED, client, buf, 17)
        return rc, buf.value.decode(errors="replace")


# NI-XNET raw frame format (CANSUB_ReadXnetFrames): little-endian 24-byte base unit, then any payload
# over 8 bytes padded to a multiple of 8.
XNET_BASE = struct.Struct("<QIBBBB8s")
XNET_ID_EXTENDED = 0x20000000
XNET_TYPE_CAN_DATA = 0x00
XNET_TYPE_CAN_REMOTE = 0x01
XNET_TYPE_CANFD_DATA = 0x10
XNET_FLAG_TRANSMIT_ECHO = 0x80
XNET_EPOCH_OFFSET = 116444736000000000  # 100 ns intervals from 1601 to 1970
CANSUB_XNET_FRAME_MAX = 80


def parse_xnet(raw):
    """Splits CANSUB_ReadXnetFrames output into dicts, checking the units add up exactly."""
    frames = []
    pos = 0
    while pos < len(raw):
        ts, ident, ftype, flags, info, length, base = XNET_BASE.unpack_from(raw, pos)
        pos += XNET_BASE.size
        extra = ((length - 8 + 7) & ~7) if length > 8 else 0
        data = base[:min(length, 8)] + raw[pos:pos + max(length - 8, 0)]
        pos += extra
        frames.append({"timestamp": ts, "id": ident & 0x1FFFFFFF,
                       "extended": bool(ident & XNET_ID_EXTENDED), "type": ftype,
                       "flags": flags, "info": info, "data": data})
    assert pos == len(raw), "units do not add up to the byte count returned"
    return frames


def build_xnet(ident, data, extended=False, ftype=XNET_TYPE_CAN_DATA, length=None):
    """One frame in NI-XNET raw format, as XNET Convert (Signal to Frame Raw) produces it: timestamp
    and flags zero. length overrides the payload length byte, for malformed-input tests."""
    length = len(data) if length is None else length
    ident = (ident | XNET_ID_EXTENDED) if extended else ident
    base = XNET_BASE.pack(0, ident, ftype, 0, 0, length, data[:8].ljust(8, b"\0"))
    rest = data[8:]
    return base + rest.ljust((len(rest) + 7) & ~7, b"\0")
