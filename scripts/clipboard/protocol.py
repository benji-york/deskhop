"""DHC2: fixed, canonical frames; no text payload ever enters an exception."""
from enum import IntEnum
import struct
import zlib

MAX_TEXT = 1024
FRAME_BYTES = 64
MAGIC = b'DHC2'
HELLO, REQUEST, BEGIN, DATA, END, PING = range(6)


class Status(IntEnum):
    OK = 0
    EMPTY = 1
    NON_TEXT = 2
    OVERSIZE = 3
    UNSUPPORTED = 4
    UNAVAILABLE = 5


class ProtocolError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise ProtocolError(message)


def wipe(value):
    if isinstance(value, bytearray):
        for i in range(len(value)):
            value[i] = 0


def supported(data):
    return all(c in (9, 10) or 32 <= c <= 126 for c in data)


def classify(data):
    """Fixtures/native response validation; never silently normalize/truncate."""
    if data is None:
        return Status.NON_TEXT
    if len(data) > MAX_TEXT:
        return Status.OVERSIZE
    if not data:
        return Status.EMPTY
    return Status.OK if supported(data) else Status.UNSUPPORTED


def frame(opcode):
    value = bytearray(FRAME_BYTES)
    value[:4] = MAGIC
    value[4] = opcode
    return value


def header(value, opcode, used):
    require(len(value) == FRAME_BYTES and value[:4] == MAGIC and value[4] == opcode,
            'invalid clipboard frame')
    require(not any(value[5:8]) and not any(value[8 + used:]),
            'noncanonical clipboard frame')


def hello(value, boot, helper):
    header(value, HELLO, 16)
    require(struct.unpack_from('<QQ', value, 8) == (boot, helper),
            'clipboard session differs from preflight')


def request(value):
    header(value, REQUEST, 40)
    binding = struct.unpack_from('<QQQQQ', value, 8)
    require(all(binding), 'zero clipboard request identity')
    return binding


def ping(helper):
    value = frame(PING)
    struct.pack_into('<Q', value, 8, helper)
    return value


def responses(binding, status, data):
    """Yield one mutable frame at a time, wiping each when caller advances/closes."""
    require(len(binding) == 5 and all(0 < n < 2**64 for n in binding), 'invalid binding')
    require(isinstance(status, Status), 'invalid clipboard status')
    require(status == Status.OK and classify(data) == Status.OK or
            status != Status.OK and not data, 'invalid clipboard result')
    value = frame(BEGIN)
    try:
        struct.pack_into('<QQQQQBHI', value, 8, *binding, status, len(data),
                         zlib.crc32(data) if data else 0)
        yield value
    finally:
        wipe(value)
    if status != Status.OK:
        return
    for offset in range(0, len(data), 40):
        count = min(40, len(data) - offset)
        value = frame(DATA)
        try:
            struct.pack_into('<QHB', value, 8, binding[3], offset, count)
            value[19:19 + count] = memoryview(data)[offset:offset + count]
            yield value
        finally:
            wipe(value)
    value = frame(END)
    try:
        struct.pack_into('<Q', value, 8, binding[3])
        yield value
    finally:
        wipe(value)
