import importlib.util
import struct
import sys
import unittest
from pathlib import Path

MODULE_PATH = Path(__file__).parents[1] / "hid_cmd.py"
SPEC = importlib.util.spec_from_file_location("hid_cmd", MODULE_PATH)
hid_cmd = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = hid_cmd
SPEC.loader.exec_module(hid_cmd)


class HidCodecTests(unittest.TestCase):
    def test_default_temperature(self):
        packet = hid_cmd.build_tcal_request(1, 3, "start")
        self.assertEqual(
            packet[:7], bytes((254, 1, 0xC8, 3, 1, 0x00, 0x80))
        )
        self.assertEqual(struct.unpack_from("<h", packet, 5)[0], -32768)

    def test_custom_temperature_and_stop_zero(self):
        packet = hid_cmd.build_tcal_request(2, 0xFF, "start", "23.45")
        self.assertEqual(packet[3:5], bytes((0xFF, 1)))
        self.assertEqual(struct.unpack_from("<h", packet, 5)[0], 2345)
        stop = hid_cmd.build_tcal_request(3, 0, "stop")
        self.assertEqual(stop[3:5], bytes((0, 2)))
        self.assertEqual(struct.unpack_from("<h", stop, 5)[0], 0)

    def test_masks_and_all_sixteen_nibbles(self):
        raw = bytearray(16)
        raw[:8] = bytes((251, 9, 0xC8, 8, 0x03, 0x80, 0x01, 0x80))
        expected = tuple(range(16))
        for tracker, value in enumerate(expected):
            raw[8 + tracker // 2] |= value << (4 if tracker & 1 else 0)
        ack = hid_cmd.decode_ack(bytes(raw))
        self.assertEqual(ack.considered_mask, 0x8003)
        self.assertEqual(ack.reply_mask, 0x8001)
        self.assertEqual(ack.tracker_status, expected)

    def test_no_tracker_immediate_ack(self):
        raw = bytearray(16)
        raw[:4] = bytes((251, 4, 0xC8, 4))
        raw[8:] = b"\xff" * 8
        ack = hid_cmd.decode_ack(bytes(raw))
        self.assertEqual(ack.considered_mask, 0)
        self.assertEqual(ack.reply_mask, 0)
        self.assertTrue(all(v == hid_cmd.NOT_CONSIDERED for v in ack.tracker_status))

    def test_no_response_warning(self):
        raw = bytearray(16)
        raw[:8] = bytes((251, 5, 0xC8, 8, 1, 0, 0, 0))
        raw[8:] = b"\xff" * 8
        raw[8] = 0xFE
        text = hid_cmd.format_ack(hid_cmd.decode_ack(bytes(raw)))
        self.assertIn("NO_RESPONSE", text)
        self.assertIn("result is unknown", text)
        self.assertIn("STOP or ABORT", text)

    def test_started_ack_is_not_reported_as_no_response(self):
        raw = bytearray(16)
        raw[:8] = bytes(
            (251, 6, 0xC8, hid_cmd.STATUS_STARTED, 1, 0, 0, 0)
        )
        raw[8:] = b"\xff" * 8
        raw[8] = 0xFE
        text = hid_cmd.format_ack(hid_cmd.decode_ack(bytes(raw)))
        self.assertIn("overall=7", text)
        self.assertNotIn("WARNING", text)
        self.assertNotIn("result is unknown", text)

    def test_coalesced_started_and_final_ack_are_both_preserved(self):
        started = bytearray(16)
        started[:8] = bytes(
            (251, 7, 0xC8, hid_cmd.STATUS_STARTED, 1, 0, 0, 0)
        )
        started[8:] = b"\xff" * 8
        started[8] = 0xFE

        final = bytearray(16)
        final[:8] = bytes((251, 7, 0xC8, 0, 1, 0, 1, 0))
        final[8:] = b"\xff" * 8
        final[8] = 0xF0

        class FakeDevice:
            def __init__(self):
                self.reports = [bytes(started + final + bytearray(32))]

            def read(self, _size, _timeout):
                return self.reports.pop(0) if self.reports else b""

        client = hid_cmd.Client.__new__(hid_cmd.Client)
        client.device = FakeDevice()
        client.pending_acks = []

        first = client.wait_ack(7, 0.1)
        second = client.wait_ack(7, 0.1)
        self.assertEqual(first.overall, hid_cmd.STATUS_STARTED)
        self.assertEqual(second.overall, 0)
        self.assertEqual(second.reply_mask, 1)


if __name__ == "__main__":
    unittest.main()
