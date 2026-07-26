#!/usr/bin/env python3
"""
Unit tests for pure-logic functions in espfm_shell.py.

These tests require NO hardware and NO network — they exercise only
pure Python helpers and mock-based packet construction.
"""

import os
import struct
import sys
import unittest
from unittest.mock import MagicMock, patch

sys.path.insert(0, os.path.dirname(__file__))

from espfm_shell import (
    COAP_GET,
    COAP_POST,
    CoAPTransport,
    CoapError,
    _error_message,
    _fmt_minutes,
    _parse_flags,
    _parse_points,
    _resolve_enum,
)


# ============================================================
# _parse_flags
# ============================================================


class TestParseFlags(unittest.TestCase):
    """Tests for _parse_flags — parses --key value pairs from token lists."""

    def test_mixed_flags(self):
        result = _parse_flags(["--duty", "50", "--mode", "auto"])
        self.assertEqual(result, {"duty": "50", "mode": "auto"})

    def test_boolean_flag_no_value(self):
        """A flag followed by another flag is treated as boolean 'true'."""
        result = _parse_flags(["--verbose", "--output", "json"])
        self.assertEqual(result, {"verbose": "true", "output": "json"})

    def test_empty_args(self):
        result = _parse_flags([])
        self.assertEqual(result, {})

    def test_single_flag_with_value(self):
        result = _parse_flags(["--name", "fan1"])
        self.assertEqual(result, {"name": "fan1"})

    def test_single_boolean_flag(self):
        result = _parse_flags(["--enabled"])
        self.assertEqual(result, {"enabled": "true"})

    def test_leading_non_flag_tokens_skipped(self):
        """Tokens not starting with '--' are skipped."""
        result = _parse_flags(["list", "--duty", "75"])
        self.assertEqual(result, {"duty": "75"})

    def test_multiple_boolean_flags(self):
        result = _parse_flags(["--a", "--b", "--c"])
        self.assertEqual(result, {"a": "true", "b": "true", "c": "true"})

    def test_flag_at_end_of_list(self):
        """Last token is a flag with no value following — treated as boolean."""
        result = _parse_flags(["--name", "test", "--dry-run"])
        self.assertEqual(result, {"name": "test", "dry-run": "true"})


# ============================================================
# _parse_points
# ============================================================


class TestParsePoints(unittest.TestCase):
    """Tests for _parse_points — parses 'temp:duty,temp:duty,...' strings."""

    def test_valid_multiple_points(self):
        result = _parse_points("30:20,50:50,70:100")
        self.assertEqual(result, [(30.0, 20), (50.0, 50), (70.0, 100)])

    def test_single_point(self):
        result = _parse_points("45:60")
        self.assertEqual(result, [(45.0, 60)])

    def test_float_temperature(self):
        result = _parse_points("25.5:30")
        self.assertEqual(result, [(25.5, 30)])

    def test_empty_string_raises(self):
        with self.assertRaises(ValueError):
            _parse_points("")

    def test_invalid_format_no_colon(self):
        with self.assertRaises(ValueError):
            _parse_points("3020")

    def test_invalid_format_too_many_parts(self):
        with self.assertRaises(ValueError):
            _parse_points("30:20:10")

    def test_non_numeric_temp_raises(self):
        with self.assertRaises(ValueError):
            _parse_points("abc:20")

    def test_non_numeric_duty_raises(self):
        with self.assertRaises(ValueError):
            _parse_points("30:xyz")

    def test_whitespace_handling(self):
        """Whitespace around pairs is stripped."""
        result = _parse_points(" 30:20 , 50:50 ")
        self.assertEqual(result, [(30.0, 20), (50.0, 50)])


# ============================================================
# _resolve_enum
# ============================================================


class TestResolveEnum(unittest.TestCase):
    """Tests for _resolve_enum — resolves string labels or int values."""

    def test_string_label(self):
        mapping = {"manual": 0, "auto": 1}
        self.assertEqual(_resolve_enum("manual", mapping), 0)
        self.assertEqual(_resolve_enum("auto", mapping), 1)

    def test_case_insensitive(self):
        mapping = {"manual": 0, "auto": 1}
        self.assertEqual(_resolve_enum("AUTO", mapping), 1)
        self.assertEqual(_resolve_enum("Manual", mapping), 0)

    def test_integer_string(self):
        mapping = {"manual": 0, "auto": 1}
        self.assertEqual(_resolve_enum("0", mapping), 0)
        self.assertEqual(_resolve_enum("1", mapping), 1)

    def test_invalid_value_raises(self):
        mapping = {"manual": 0, "auto": 1}
        with self.assertRaises(ValueError):
            _resolve_enum("turbo", mapping)

    def test_out_of_range_int_raises(self):
        """An integer not present in the mapping's values should raise."""
        mapping = {"manual": 0, "auto": 1}
        with self.assertRaises(ValueError):
            _resolve_enum("99", mapping)

    def test_full_enum_mapping(self):
        mapping = {"ntc": 0, "ds18b20": 1, "manual": 2, "0": 0, "1": 1, "2": 2}
        self.assertEqual(_resolve_enum("ds18b20", mapping), 1)
        self.assertEqual(_resolve_enum("2", mapping), 2)


# ============================================================
# _fmt_minutes
# ============================================================


class TestFmtMinutes(unittest.TestCase):
    """Tests for _fmt_minutes — converts minutes-since-midnight to HH:MM."""

    def test_midnight(self):
        self.assertEqual(_fmt_minutes(0), "00:00")

    def test_morning(self):
        self.assertEqual(_fmt_minutes(480), "08:00")

    def test_evening(self):
        self.assertEqual(_fmt_minutes(1080), "18:00")

    def test_last_minute_of_day(self):
        self.assertEqual(_fmt_minutes(1439), "23:59")

    def test_one_oclock(self):
        self.assertEqual(_fmt_minutes(60), "01:00")

    def test_thirty_minutes(self):
        self.assertEqual(_fmt_minutes(30), "00:30")

    def test_noon(self):
        self.assertEqual(_fmt_minutes(720), "12:00")


# ============================================================
# _error_message
# ============================================================


class TestErrorMessage(unittest.TestCase):
    """Tests for _error_message — formats CoapError into human-readable text."""

    def test_timeout_code_zero(self):
        e = CoapError(0)
        self.assertIn("timed out", _error_message(e))

    def test_bad_request_4_00(self):
        e = CoapError(0x80, b"")
        self.assertIn("Bad request", _error_message(e))

    def test_not_found_4_04(self):
        e = CoapError(0x84, b"")
        self.assertIn("Not found", _error_message(e))

    def test_service_unavailable_5_03(self):
        e = CoapError(0xA3, b"")
        self.assertIn("Service unavailable", _error_message(e))

    def test_unknown_code(self):
        e = CoapError(0x60)
        result = _error_message(e)
        self.assertIn("CoAP error", result)


# ============================================================
# CoAPTransport packet construction
# ============================================================


class TestCoAPTransportPacket(unittest.TestCase):
    """Tests for CoAPTransport.request — verify raw UDP packet structure."""

    def _make_transport(self):
        t = CoAPTransport("127.0.0.1", 5683, 1.0)
        t._sock = MagicMock()
        # Make recvfrom return a minimal valid CoAP response:
        # version=1, type=ACK(2), token_len=0, code=2.05(0x45), mid=0x0001
        # + payload marker + payload "ok"
        response = bytes([0x60, 0x45, 0x00, 0x01, 0xFF]) + b"ok"
        t._sock.recvfrom.return_value = (response, ("127.0.0.1", 5683))
        return t

    def test_packet_header_version_and_method(self):
        """First byte: version=1 (bits 7-6), type=CON=0 (bits 5-4), token_len=4 (bits 3-0)."""
        t = self._make_transport()
        t.request(COAP_GET, "/fans")

        sent_data = t._sock.sendto.call_args[0][0]
        # Byte 0: ver(2 bits) | type(2 bits) | tkl(4 bits)
        # ver=1 -> 0b01, type=CON=0 -> 0b00, tkl=4 -> 0b0100
        # => 0b01_00_0100 = 0x44
        self.assertEqual(sent_data[0] & 0xC0, 0x40)  # version = 1
        self.assertEqual(sent_data[0] & 0x30, 0x00)   # type = CON (0)
        self.assertEqual(sent_data[0] & 0x0F, 4)      # token length = 4

    def test_packet_method_code(self):
        """Second byte is the CoAP method code."""
        t = self._make_transport()
        t.request(COAP_POST, "/fans")

        sent_data = t._sock.sendto.call_args[0][0]
        self.assertEqual(sent_data[1], COAP_POST)

    def test_packet_token_length(self):
        """Token is 4 bytes (bytes 4-7 of the packet)."""
        t = self._make_transport()
        t.request(COAP_GET, "/fans")

        sent_data = t._sock.sendto.call_args[0][0]
        tkl = sent_data[0] & 0x0F
        self.assertEqual(tkl, 4)
        # Token occupies bytes 4..4+tkl-1
        token = sent_data[4:8]
        self.assertEqual(len(token), 4)

    def test_uri_path_option_single_segment(self):
        """Single path segment 'fans' -> option header delta=11, len=4."""
        t = self._make_transport()
        t.request(COAP_GET, "/fans")

        sent_data = t._sock.sendto.call_args[0][0]
        # After header (4 bytes) + token (4 bytes) = byte 8
        # Option delta=11, length=4 => byte = (11 << 4) | 4 = 0xB4
        self.assertEqual(sent_data[8], (11 << 4) | 4)
        self.assertEqual(sent_data[9:13], b"fans")

    def test_uri_path_option_multi_segment(self):
        """Multi-segment path '/fans/0' -> first option delta=11, subsequent delta=0."""
        t = self._make_transport()
        t.request(COAP_GET, "/fans/0")

        sent_data = t._sock.sendto.call_args[0][0]
        # First option: delta=11, len=4 for "fans"
        self.assertEqual(sent_data[8], (11 << 4) | 4)
        self.assertEqual(sent_data[9:13], b"fans")
        # Second option: delta=0 (same number), len=1 for "0"
        self.assertEqual(sent_data[13], (0 << 4) | 1)
        self.assertEqual(sent_data[14], ord("0"))

    def test_payload_marker_and_body(self):
        """When payload is non-empty, 0xFF marker precedes it."""
        t = self._make_transport()
        t.request(COAP_POST, "/fans", b"\x08test")

        sent_data = t._sock.sendto.call_args[0][0]
        # Find the 0xFF payload marker
        marker_pos = sent_data.index(0xFF)
        self.assertEqual(sent_data[marker_pos + 1:], b"\x08test")

    def test_no_payload_marker_when_empty(self):
        """When payload is empty, no 0xFF marker is appended."""
        t = self._make_transport()
        t.request(COAP_GET, "/fans")

        sent_data = t._sock.sendto.call_args[0][0]
        self.assertNotIn(b"\xff", sent_data)

    def test_sendto_called_with_correct_address(self):
        """sendto is called with the configured host and port."""
        t = self._make_transport()
        t.request(COAP_GET, "/fans")

        addr = t._sock.sendto.call_args[0][1]
        self.assertEqual(addr, ("127.0.0.1", 5683))

    def test_response_code_parsed(self):
        """Response code byte is extracted correctly from the CoAP response."""
        t = self._make_transport()
        code, payload = t.request(COAP_GET, "/fans")

        self.assertEqual(code, 0x45)  # 2.05 Content

    def test_response_payload_extracted(self):
        """Payload after 0xFF marker is returned."""
        t = self._make_transport()
        code, payload = t.request(COAP_GET, "/fans")

        self.assertEqual(payload, b"ok")

    def test_timeout_returns_none(self):
        """On socket.timeout, returns (None, None)."""
        import socket as _socket

        t = self._make_transport()
        t._sock.recvfrom.side_effect = _socket.timeout
        code, payload = t.request(COAP_GET, "/fans")

        self.assertIsNone(code)
        self.assertIsNone(payload)

    def test_not_connected_raises(self):
        """Calling request without connecting raises RuntimeError."""
        t = CoAPTransport("127.0.0.1")
        # _sock is None by default
        with self.assertRaises(RuntimeError):
            t.request(COAP_GET, "/fans")


if __name__ == "__main__":
    unittest.main()
