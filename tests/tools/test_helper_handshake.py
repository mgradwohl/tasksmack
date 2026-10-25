#!/usr/bin/env python3
"""Tests for tools/helper-handshake.py (#2723).

Its Hello must be byte for byte the frame the C++ encoder writes: the fuzz corpus's hello seed,
which test_BrokerProtocol.cpp decodes and a protocol change has to update, is that frame for
request 1. So a protocol bump that changes Hello fails here until the script learns it.
"""

import importlib.util
import struct
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools" / "helper-handshake.py"
spec = importlib.util.spec_from_file_location("helper_handshake", SCRIPT)
handshake = importlib.util.module_from_spec(spec)
spec.loader.exec_module(handshake)

HELLO_SEED = REPO / "tests" / "fuzz" / "corpus" / "fuzz_broker_protocol" / "hello"


class EncodingTest(unittest.TestCase):
    def test_the_hello_is_the_frame_the_cpp_encoder_writes(self) -> None:
        version = handshake.protocol_version()
        self.assertEqual(handshake.encode_hello(version, 1), HELLO_SEED.read_bytes())

    def test_a_version_without_a_hello_encoding_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            handshake.encode_hello(max(handshake.HELLO_PAYLOADS) + 1, 1)


class ReplyTest(unittest.TestCase):
    def reply(self, kind: int = 2, request_id: int = 1, accepted: int = 1) -> bytes:
        return handshake.frame(2, kind, request_id, struct.pack("<HB", 2, accepted))

    def test_an_accepted_reply_is_read(self) -> None:
        self.assertEqual(handshake.decode_hello_reply(self.reply(), 1), (2, True))
        self.assertEqual(handshake.decode_hello_reply(self.reply(accepted=0), 1), (2, False))

    def test_anything_but_the_reply_to_this_hello_is_refused(self) -> None:
        for bad in (self.reply(kind=63), self.reply(request_id=2), self.reply(accepted=2), self.reply()[:-1], b""):
            with self.subTest(bad=bad):
                with self.assertRaises(ValueError):
                    handshake.decode_hello_reply(bad, 1)


if __name__ == "__main__":
    sys.exit(unittest.main())
