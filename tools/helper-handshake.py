#!/usr/bin/env python3
"""
tools/helper-handshake.py -- The installed helper answers TaskSmack's handshake through pkexec (#2723).

Usage:
    helper-handshake.py [--helper PATH] [--no-pkexec] [--timeout SECONDS]

Starts the helper the way PkexecBroker does (src/Platform/Linux/PkexecBroker.cpp): `pkexec <helper>
--broker` with one end of a SOCK_SEQPACKET socketpair as its stdin, sends a Hello of this checkout's
protocol version over the other end, and expects an accepted HelloReply. Then it closes the socket
and expects the helper to exit 0, as it does when TaskSmack closes it. CI runs it against the
installed .deb with a polkit rule that answers YES for the runner, so no password dialog is needed;
the dialog itself stays a manual check. --no-pkexec starts the helper directly (as the tests do).

The protocol version is read from src/Platform/BrokerProtocol.h beside this script, and the Hello
is encoded as that header says. tests/tools/test_helper_handshake.py checks the encoding against
the fuzz corpus's hello seed, which the C++ protocol tests decode, so the two can't drift apart.
"""

import argparse
import re
import socket
import struct
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
PROTOCOL_HEADER = REPO / "src" / "Platform" / "BrokerProtocol.h"
INSTALLED_HELPER = "/usr/libexec/tasksmack/tasksmack-helper"
HELLO = 1
HELLO_REPLY = 2
# u16 version, u16 kind, u32 request id: the header after the length.
HEADER = struct.Struct("<HHI")

# Each protocol version's Hello payload (BrokerProtocol.h's encodeRequest()). A new version that
# changes Hello adds its row; one missing fails loudly rather than send a Hello the helper refuses.
HELLO_PAYLOADS = {
    2: lambda version: struct.pack("<H", version),
    3: lambda version: struct.pack("<HB", version, 0),  # forSession off (#2728)
    4: lambda version: struct.pack("<HB", version, 0),  # Hello unchanged; ReadProcessCounters added (#2728)
}


def protocol_version(header: Path = PROTOCOL_HEADER) -> int:
    """PROTOCOL_VERSION as BrokerProtocol.h declares it."""
    match = re.search(r"PROTOCOL_VERSION\s*=\s*(\d+)\s*;", header.read_text(encoding="utf-8"))
    if match is None:
        raise ValueError(f"no PROTOCOL_VERSION in {header}")
    return int(match.group(1))


def frame(version: int, kind: int, request_id: int, payload: bytes) -> bytes:
    body = HEADER.pack(version, kind, request_id) + payload
    return struct.pack("<I", len(body)) + body


def encode_hello(version: int, request_id: int) -> bytes:
    if version not in HELLO_PAYLOADS:
        raise ValueError(f"protocol version {version} has no Hello encoding here; add it to HELLO_PAYLOADS")
    return frame(version, HELLO, request_id, HELLO_PAYLOADS[version](version))


def decode_hello_reply(data: bytes, request_id: int) -> tuple[int, bool]:
    """The helper's version and whether it accepted ours; a ValueError for anything but that reply."""
    if len(data) < 4 + HEADER.size:
        raise ValueError(f"a frame of {len(data)} bytes is too short")
    (length,) = struct.unpack_from("<I", data)
    if length != len(data) - 4:
        raise ValueError(f"the frame says {length} bytes follow, {len(data) - 4} do")
    version, kind, answered = HEADER.unpack_from(data, 4)
    if kind != HELLO_REPLY:
        raise ValueError(f"expected a HelloReply (kind {HELLO_REPLY}), got kind {kind}")
    if answered != request_id:
        raise ValueError(f"the reply answers request {answered}, not {request_id}")
    payload = data[4 + HEADER.size :]
    if len(payload) != 3 or payload[2] > 1:
        raise ValueError("a HelloReply's payload is its version and an accepted flag")
    (reply_version,) = struct.unpack_from("<H", payload)
    return reply_version, payload[2] == 1


def handshake(helper: str, use_pkexec: bool, timeout: float) -> int:
    version = protocol_version()
    ours, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    command = (["pkexec"] if use_pkexec else []) + [helper, "--broker"]
    print(f"Starting {' '.join(command)} (protocol {version})")
    process = subprocess.Popen(command, stdin=theirs.fileno(), close_fds=True)
    theirs.close()
    try:
        ours.settimeout(timeout)
        ours.sendall(encode_hello(version, 1))
        try:
            reply = ours.recv(4096)
        except TimeoutError:
            print(f"No reply within {timeout} s", file=sys.stderr)
            return 1
        if not reply:
            print(f"The helper closed the socket without answering (exit {process.wait(timeout)})", file=sys.stderr)
            return 1
        reply_version, accepted = decode_hello_reply(reply, 1)
        if not accepted:
            print(f"The helper speaks protocol {reply_version} and refused version {version}", file=sys.stderr)
            return 1
        print(f"Handshake accepted: the helper speaks protocol {reply_version}")
    finally:
        ours.close()
    try:
        status = process.wait(timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        print("The helper didn't exit once its socket was closed", file=sys.stderr)
        return 1
    if status != 0:
        print(f"The helper exited {status} after the socket was closed", file=sys.stderr)
        return 1
    print("The helper exited 0 once its socket was closed")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    parser.add_argument("--helper", default=INSTALLED_HELPER, help=f"the helper to start (default: {INSTALLED_HELPER})")
    parser.add_argument("--no-pkexec", action="store_true", help="start the helper directly, not through pkexec")
    parser.add_argument("--timeout", type=float, default=60.0, help="seconds to wait for the reply and the exit")
    args = parser.parse_args(argv[1:])
    return handshake(args.helper, not args.no_pkexec, args.timeout)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
