"""Fixtures shared by the CAN and J1939 suites.

The rig is one CANsub with CH1 and CH2 wired together. The driver always drives CH1 through
DeviceID 1; python-can-cansub always watches CH2 as raw CAN, so a frame the driver puts on the wire
is judged by an independent implementation rather than by the driver itself.

Both sides have to point at the same device, and they get their host from different places: the DLL
reads DeviceParams out of CANSUB32.ini, while python-can takes it as an argument. Rather than ask
whoever runs the tests to keep two files in step, the ini_pointed_at_device fixture rewrites the
*build output* copy from --host, leaving the versioned INI (which finds the device by discovery and
must stay that way) alone.
"""

import ctypes
import os
import re
import time
from ctypes import c_void_p

import pytest

try:
    import can
except ImportError:  # reported as a skip by the ch2 fixture
    can = None

from driver import Driver

DEVICE_CH1 = 1
DEVICE_CH2 = 2

DEFAULT_HOST = os.environ.get("CANSUB_HOST")


def pytest_addoption(parser):
    parser.addoption("--arch", default="x64", choices=["x86", "x64"],
                     help="which build of CANSUB32.dll to load")
    parser.addoption("--host", default=DEFAULT_HOST,
                     help="CANsub hostname or IP, e.g. <device-id>-eth.local (or set CANSUB_HOST)")


@pytest.fixture(scope="session")
def device_host(pytestconfig):
    host = pytestconfig.getoption("--host")

    if not host:
        pytest.exit("give the CANsub with --host <device-id>-eth.local (or set CANSUB_HOST)", returncode=4)

    return host


@pytest.fixture(scope="session")
def build_dir(pytestconfig):
    arch = pytestconfig.getoption("--arch")
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    return os.path.join(root, "build", arch)


@pytest.fixture(scope="session", autouse=True)
def ini_pointed_at_device(build_dir, device_host):
    """Points the built CANSUB32.ini at --host.

    This edits a build artefact, never the versioned INI: that one is shipped as it is, and a test
    run must not quietly repoint it at whatever is on this desk.
    """
    path = os.path.join(build_dir, "CANSUB32.ini")

    if not os.path.isfile(path):
        pytest.skip(f"not built: {path}")

    with open(path, encoding="utf-8") as f:
        text = f.read()

    # Pin every device entry to --host, whichever form the shipped file uses. The shipped INI is
    # auto-detecting, which is right for a customer but wrong for a test: this desk has more than
    # one CANsub, and the DLL and the python-can side have to agree on which one they are using.
    # The auto-detection tests opt back into discovery deliberately.
    patched = re.sub(r"^(DeviceParams=)(?:host=[^,]+|index=\d+)(,channel=\d+)\s*$",
                     r"\g<1>host=" + device_host + r"\g<2>", text, flags=re.MULTILINE)

    if patched != text:
        with open(path, "w", encoding="utf-8") as f:
            f.write(patched)

    return path


@pytest.fixture(scope="session")
def driver(pytestconfig, build_dir, ini_pointed_at_device):
    arch = pytestconfig.getoption("--arch")
    bits = 64 if ctypes.sizeof(c_void_p) == 8 else 32

    if (arch == "x64") != (bits == 64):
        pytest.skip(f"{arch} DLL needs {'64' if arch == 'x64' else '32'}-bit Python")

    path = os.path.join(build_dir, "CANSUB32.dll")

    if not os.path.isfile(path):
        pytest.skip(f"not built: {path}")

    return Driver(path)


def open_bus(host, channel, bitrate=250000, attempts=6, delay=0.4, data_bitrate=None):
    """Opens a python-can-cansub bus, retrying while the device frees a socket.

    The device permits one WebSocket per channel and takes a moment to release it after a close.
    Several J1939 tests reopen the channel to change bit rate, so a bare open would race.
    """
    last = None

    for _ in range(attempts):
        try:
            kwargs = {"data_bitrate": data_bitrate} if data_bitrate else {}
            return can.Bus(interface="cansub", channel=f"{host}@{channel}", bitrate=bitrate,
                           **kwargs)
        except Exception as exc:  # noqa: BLE001 - python-can wraps several transport errors
            last = exc
            time.sleep(delay)

    raise last


@pytest.fixture(scope="session", autouse=True)
def hardware_available(device_host):
    """Fails fast and clearly when CH1 is already held by something else.

    Without this, a stale socket from a crashed run or a still-open webCAN tab shows up as
    ERR_HARDWARE_NOT_RESPONDING from ClientConnect and looks like a driver fault.
    """
    if can is None:
        return

    try:
        import python_can_cansub  # noqa: F401
    except ImportError:
        return

    try:
        # As patient as the driver itself: bus::open polls up to 2 s for the channel to report
        # "stopped", and the device can take a moment longer than that to release it after a
        # previous run. A tighter window here turns a normal wind-down into a failed session.
        bus = open_bus(device_host, DEVICE_CH1, attempts=10, delay=0.5)
    except Exception as exc:  # noqa: BLE001
        pytest.exit(
            f"cannot reach {device_host} CH1: {exc}\n"
            "Check the device is connected, that --host matches it, and that nothing else "
            "(webCAN, dump.exe, test_bus.exe, a crashed run) is holding the channel.",
            returncode=1,
        )

    bus.shutdown()
    time.sleep(0.3)


@pytest.fixture
def client(driver):
    """A generic CAN client on CH1."""
    cid = driver.connect()

    if cid < 0 or cid > 127:
        pytest.fail(f"ClientConnect failed: {cid} {driver.error_text(cid)}")

    yield cid

    driver.disconnect(cid)


@pytest.fixture
def ch2(device_host):
    """python-can-cansub on the other channel, as the independent check."""
    pytest.importorskip("can")
    pytest.importorskip("python_can_cansub")

    bus = open_bus(device_host, DEVICE_CH2)

    yield bus

    bus.shutdown()
