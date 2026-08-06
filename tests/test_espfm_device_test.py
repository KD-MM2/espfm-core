#!/usr/bin/env python3
"""
Unit tests for pure-logic functions in espfm_device_test.py.

These tests require NO hardware and NO network. Phase 1 covers pure
Python helpers: the .env credential loader, CLI arg parsing, and CoAP
status-code formatting. Phase 2 covers the discovery/list/snapshot run
functions by patching espfm_device_test.do_request to return canned
CoAP responses. Phase 3 covers the wifi/status, wifi/scan, ds18b20/scan,
and control read-only run functions the same way. Phase 4 covers the
create phase run functions (free-pin discovery, fan, source, curve,
schedule, manual-temp, and ds18b20 config) the same way. Phase 5 covers
the item-level read run functions (fan, source, curve, schedule get) and
the error-path GET the same way. Phase 6 covers the update-phase run
functions (fan, source, curve, schedule PUT with restore) and the control
restore / out-of-range error paths the same way. Phase 7 covers the
hostname update (test + restore) and the cleanup-delete run functions
(fan, source, curve, schedule DELETE) plus the shared _status_case helper
the same way. Phase 8 covers the destructive-phase run functions (config
import, system reboot, config verify, and wifi connect) plus the
wait_for_device helper the same way. Phase 9 covers the report renderers:
render_body (per-endpoint markdown sections) and write_report (the full
report document).
"""

import io
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest.mock import patch

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))

import espfm_pb2 as pb

from espfm_device_test import (
    code_str,
    load_env_file,
    parse_args,
    RunState,
    CaseResult,
    COAP_GET,
    COAP_POST,
    COAP_PUT,
    COAP_DELETE,
    run_fans_list,
    discover_live_fan,
    run_sources_list,
    run_curves_list,
    run_schedules_list,
    run_config_snapshot,
    run_wifi_status,
    run_wifi_scan,
    run_ds18b20_scan,
    run_control_originals,
    discover_free_pins,
    run_create_fan,
    run_create_source,
    run_create_curve,
    run_create_schedule,
    run_manual_temp,
    run_ds18b20_config,
    run_fan_get,
    run_source_get,
    run_curve_get,
    run_schedule_get,
    run_error_path_get,
    run_fan_update,
    run_source_update,
    run_curve_update,
    run_schedule_update,
    run_control_restore,
    run_control_error,
    _status_case,
    run_hostname_test,
    run_hostname_restore,
    run_delete_fan,
    run_delete_source,
    run_delete_curve,
    run_delete_schedule,
    wait_for_device,
    run_config_import,
    run_system_reboot,
    run_config_verify,
    run_wifi_connect,
    render_body,
    write_report,
)


def _make_state():
    """Build a RunState with a throwaway report path (no device I/O)."""
    return RunState("192.168.0.50", 5683, "test_report.md")


def _make_case(
    surface,
    verdict="PASS",
    response_summary="resp",
    note=None,
    method=COAP_GET,
    request_summary="none",
    expected_status="2.05",
    actual_status="2.05 Content",
):
    """Build a CaseResult with the full 10-arg constructor (note defaults to None)."""
    return CaseResult(
        label=surface,
        surface=surface,
        method=method,
        path=surface,
        request_summary=request_summary,
        expected_status=expected_status,
        actual_status=actual_status,
        response_summary=response_summary,
        verdict=verdict,
        note=note,
    )


# ============================================================
# load_env_file
# ============================================================


class TestLoadEnvFile(unittest.TestCase):
    """Tests for load_env_file — loads KEY=VALUE pairs from a .env file."""

    def _write_env_file(self, content):
        """Write content to a temp .env file and return its path."""
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        path = os.path.join(tmp.name, ".env")
        with open(path, "w", encoding="utf-8") as f:
            f.write(content)
        return path

    @patch.dict(os.environ, {}, clear=True)
    def test_surrounding_whitespace_stripped(self):
        """Keys and values with surrounding whitespace are stripped."""
        path = self._write_env_file(
            "  KEY1  =  value1  \n\tKEY2\t=\tvalue2\n"
        )
        load_env_file(path)
        self.assertEqual(os.environ.get("KEY1"), "value1")
        self.assertEqual(os.environ.get("KEY2"), "value2")

    @patch.dict(os.environ, {}, clear=True)
    def test_surrounding_double_quote_pair_removed(self):
        """One matching pair of surrounding double quotes is removed."""
        path = self._write_env_file('WIFI_SSID = "MyNet"\n')
        load_env_file(path)
        self.assertEqual(os.environ.get("WIFI_SSID"), "MyNet")

    @patch.dict(os.environ, {}, clear=True)
    def test_value_with_single_quote_left_intact(self):
        """A lone leading quote (no matching trailing quote) is kept."""
        path = self._write_env_file('PARTIAL = "abc\n')
        load_env_file(path)
        self.assertEqual(os.environ.get("PARTIAL"), '"abc')

    @patch.dict(os.environ, {}, clear=True)
    def test_blank_and_comment_lines_ignored(self):
        """Blank lines and lines starting with # contribute no env vars."""
        path = self._write_env_file(
            "# full-line comment\n\nKEY=value\n   \n# another comment\n"
        )
        load_env_file(path)
        self.assertEqual(os.environ.get("KEY"), "value")
        self.assertEqual(set(os.environ.keys()), {"KEY"})

    @patch.dict(os.environ, {}, clear=True)
    def test_line_without_equals_ignored(self):
        """A line with no '=' introduces no env var."""
        path = self._write_env_file("KEY=value\nNOSEPARATOR\nOTHER=thing\n")
        load_env_file(path)
        self.assertEqual(os.environ.get("KEY"), "value")
        self.assertEqual(os.environ.get("OTHER"), "thing")
        self.assertNotIn("NOSEPARATOR", os.environ)

    @patch.dict(os.environ, {}, clear=True)
    def test_missing_file_is_noop(self):
        """A missing file path introduces no env vars and raises no error."""
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        missing = os.path.join(tmp.name, "does_not_exist.env")
        load_env_file(missing)  # must not raise
        self.assertEqual(os.environ, {})


# ============================================================
# parse_args
# ============================================================


class TestParseArgs(unittest.TestCase):
    """Tests for parse_args — CLI defaults, positionals, and flag precedence."""

    def test_defaults(self):
        args = parse_args([])
        self.assertEqual(args.host, "192.168.0.28")
        self.assertEqual(args.port, 5683)
        self.assertEqual(args.output, "tools/espfm_device_test_report.md")

    def test_positional_host_and_port(self):
        args = parse_args(["10.0.0.5", "1234"])
        self.assertEqual(args.host, "10.0.0.5")
        self.assertEqual(args.port, 1234)

    def test_flags_override_positionals(self):
        args = parse_args(["10.0.0.5", "1234", "--host", "10.1.1.1", "--port", "9999"])
        self.assertEqual(args.host, "10.1.1.1")
        self.assertEqual(args.port, 9999)

    def test_flags_without_positionals(self):
        args = parse_args(["--host", "10.2.2.2", "--port", "7777"])
        self.assertEqual(args.host, "10.2.2.2")
        self.assertEqual(args.port, 7777)

    def test_host_flag_only(self):
        args = parse_args(["--host", "10.3.3.3"])
        self.assertEqual(args.host, "10.3.3.3")
        self.assertEqual(args.port, 5683)


# ============================================================
# code_str
# ============================================================


class TestCodeStr(unittest.TestCase):
    """Tests for code_str — formats CoAP status codes for display."""

    def test_none_is_timeout(self):
        self.assertEqual(code_str(None), "TIMEOUT")

    def test_known_code_from_map(self):
        self.assertEqual(code_str(0x45), "2.05 Content")
        self.assertEqual(code_str(0x84), "4.04 Not Found")

    def test_unknown_code_class_and_detail(self):
        self.assertEqual(code_str(0x60), "3.00")
        self.assertEqual(code_str(0x61), "3.01")


# ============================================================
# run_fans_list
# ============================================================


class TestRunFansList(unittest.TestCase):
    """Tests for run_fans_list — GET /fans decodes a FanList into state.fan_list."""

    @patch("espfm_device_test.do_request")
    def test_2_05_decodes_fan_list(self, mock_request):
        fan_list = pb.FanList()
        fan = fan_list.fans.add()
        fan.id = 0
        fan.name = "gpu"
        fan.mode = 0
        fan.duty = 50
        fan.rpm = 0
        fan.enabled = True
        fan.pwm_gpio = 22
        fan.tach_gpio = 23
        fan.source_id = 0
        fan.curve_id = 0
        fan.schedule_id = 0
        fan.group_id = 0
        fan.alarm = 0
        mock_request.return_value = (0x45, fan_list.SerializeToString())

        state = _make_state()
        run_fans_list(state)

        self.assertIsNotNone(state.fan_list)
        self.assertEqual(len(state.fan_list.fans), 1)
        self.assertEqual(state.fan_list.fans[0].id, 0)
        self.assertEqual(state.fan_list.fans[0].pwm_gpio, 22)
        self.assertEqual(state.fan_list.fans[0].tach_gpio, 23)
        mock_request.assert_called_once_with(COAP_GET, "/fans")
        result = state.results[-1]
        self.assertEqual(result.label, "GET /fans")
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.expected_status, "2.05")
        self.assertEqual(result.actual_status, "2.05 Content")
        self.assertIn("pwm_gpio=22", result.response_summary)
        self.assertIn("tach_gpio=23", result.response_summary)


# ============================================================
# discover_live_fan
# ============================================================


class TestDiscoverLiveFan(unittest.TestCase):
    """Tests for discover_live_fan — selects the pwm22/tach23 fan or falls back to id 0."""

    def _fan(self, id_, pwm, tach):
        fan = pb.FanInfo()
        fan.id = id_
        fan.name = f"fan-{id_}"
        fan.pwm_gpio = pwm
        fan.tach_gpio = tach
        return fan

    def test_matching_fan_sets_live_fan_id(self):
        fan_list = pb.FanList()
        fan_list.fans.append(self._fan(7, 22, 23))
        fan_list.fans.append(self._fan(1, 15, 16))
        state = _make_state()
        state.fan_list = fan_list

        discover_live_fan(state)

        self.assertEqual(state.live_fan_id, 7)
        result = state.results[-1]
        self.assertEqual(result.label, "discover_live_fan")
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.expected_status, "2.05")
        self.assertEqual(result.actual_status, "2.05")
        self.assertEqual(result.surface, "GET /fans")

    def test_no_matching_fan_keeps_live_fan_id_zero(self):
        fan_list = pb.FanList()
        fan_list.fans.append(self._fan(3, 15, 16))
        fan_list.fans.append(self._fan(4, 18, 19))
        state = _make_state()
        state.fan_list = fan_list
        state.live_fan_id = 5  # must be reset to the id-0 fallback

        discover_live_fan(state)

        self.assertEqual(state.live_fan_id, 0)
        result = state.results[-1]
        self.assertEqual(result.label, "discover_live_fan")
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.expected_status, "n/a")
        self.assertEqual(result.actual_status, "n/a")
        self.assertIsNotNone(result.note)
        self.assertIn("R11", result.note)

    def test_no_fan_list_available_falls_back_to_zero(self):
        state = _make_state()
        state.fan_list = None
        state.live_fan_id = 5

        discover_live_fan(state)

        self.assertEqual(state.live_fan_id, 0)
        result = state.results[-1]
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.expected_status, "n/a")
        self.assertEqual(result.actual_status, "n/a")


# ============================================================
# run_sources_list
# ============================================================


class TestRunSourcesList(unittest.TestCase):
    """Tests for run_sources_list — decodes a SourceList and sets manual_source_id."""

    @patch("espfm_device_test.do_request")
    def test_2_05_decodes_source_list_and_sets_manual_source_id(self, mock_request):
        source_list = pb.SourceList()
        ntc = source_list.sources.add()
        ntc.id = 0
        ntc.name = "ntc"
        ntc.type = 0
        ntc.status = 1
        ntc.temp_c = 25.0
        ntc.gpio = 34
        manual = source_list.sources.add()
        manual.id = 3
        manual.name = "manual"
        manual.type = pb.SOURCE_TYPE_MANUAL
        manual.status = 1
        manual.temp_c = 20.0
        manual.gpio = 255
        mock_request.return_value = (0x45, source_list.SerializeToString())

        state = _make_state()
        run_sources_list(state)

        self.assertIsNotNone(state.source_list)
        self.assertEqual(len(state.source_list.sources), 2)
        self.assertEqual(state.manual_source_id, 3)
        mock_request.assert_called_once_with(COAP_GET, "/sources")
        result = state.results[-1]
        self.assertEqual(result.label, "GET /sources")
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.expected_status, "2.05")
        self.assertEqual(result.actual_status, "2.05 Content")


# ============================================================
# run_curves_list
# ============================================================


class TestRunCurvesList(unittest.TestCase):
    """Tests for run_curves_list — decodes a CurveList and sets first_curve_id."""

    @patch("espfm_device_test.do_request")
    def test_2_05_decodes_curve_list_and_sets_first_curve_id(self, mock_request):
        curve_list = pb.CurveList()
        curve = curve_list.curves.add()
        curve.id = 5
        curve.name = "gpu-temp"
        point = curve.points.add()
        point.temp_c = 30.0
        point.duty = 20
        mock_request.return_value = (0x45, curve_list.SerializeToString())

        state = _make_state()
        run_curves_list(state)

        self.assertIsNotNone(state.curve_list)
        self.assertEqual(len(state.curve_list.curves), 1)
        self.assertEqual(state.first_curve_id, 5)
        mock_request.assert_called_once_with(COAP_GET, "/curves")
        result = state.results[-1]
        self.assertEqual(result.label, "GET /curves")
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.expected_status, "2.05")
        self.assertEqual(result.actual_status, "2.05 Content")
        self.assertIn("points=1", result.response_summary)


# ============================================================
# run_schedules_list
# ============================================================


class TestRunSchedulesList(unittest.TestCase):
    """Tests for run_schedules_list — decodes a ScheduleList into state.schedule_list."""

    @patch("espfm_device_test.do_request")
    def test_2_05_decodes_schedule_list(self, mock_request):
        schedule_list = pb.ScheduleList()
        sched = schedule_list.schedules.add()
        sched.id = 1
        sched.fan_id = 0
        sched.duty = 50
        sched.start_min = 480
        sched.end_min = 1080
        sched.enabled = True
        sched.name = "day"
        mock_request.return_value = (0x45, schedule_list.SerializeToString())

        state = _make_state()
        run_schedules_list(state)

        self.assertIsNotNone(state.schedule_list)
        self.assertEqual(len(state.schedule_list.schedules), 1)
        self.assertEqual(state.schedule_list.schedules[0].duty, 50)
        mock_request.assert_called_once_with(COAP_GET, "/schedules")
        result = state.results[-1]
        self.assertEqual(result.label, "GET /schedules")
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.expected_status, "2.05")
        self.assertEqual(result.actual_status, "2.05 Content")

    @patch("espfm_device_test.do_request")
    def test_2_05_empty_schedule_list(self, mock_request):
        schedule_list = pb.ScheduleList()
        mock_request.return_value = (0x45, schedule_list.SerializeToString())

        state = _make_state()
        run_schedules_list(state)

        self.assertIsNotNone(state.schedule_list)
        self.assertEqual(len(state.schedule_list.schedules), 0)
        result = state.results[-1]
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.response_summary, "")


# ============================================================
# run_config_snapshot
# ============================================================


class TestRunConfigSnapshot(unittest.TestCase):
    """Tests for run_config_snapshot — decodes a ConfigFile into state.pre_run_config."""

    @patch("espfm_device_test.do_request")
    def test_2_05_decodes_config_file(self, mock_request):
        config = pb.ConfigFile()
        config.version = "3.0"
        fan = config.fans.fans.add()
        fan.id = 0
        fan.name = "gpu"
        fan.pwm_gpio = 22
        fan.tach_gpio = 23
        src = config.sources.sources.add()
        src.id = 0
        src.type = pb.SOURCE_TYPE_MANUAL
        curve = config.curves.curves.add()
        curve.id = 0
        curve.name = "gpu-temp"
        mock_request.return_value = (0x45, config.SerializeToString())

        state = _make_state()
        run_config_snapshot(state)

        self.assertIsNotNone(state.pre_run_config)
        self.assertEqual(state.pre_run_config.version, "3.0")
        self.assertEqual(len(state.pre_run_config.fans.fans), 1)
        self.assertEqual(len(state.pre_run_config.sources.sources), 1)
        self.assertEqual(len(state.pre_run_config.curves.curves), 1)
        self.assertEqual(len(state.pre_run_config.schedules.schedules), 0)
        mock_request.assert_called_once_with(COAP_GET, "/config")
        result = state.results[-1]
        self.assertEqual(result.label, "GET /config")
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.expected_status, "2.05")
        self.assertEqual(result.actual_status, "2.05 Content")
        self.assertEqual(
            result.response_summary,
            "version='3.0', fans=1, sources=1, curves=1, schedules=0",
        )


# ============================================================
# Decode-error FAIL paths
# ============================================================


class TestDecodeErrorPaths(unittest.TestCase):
    """A 2.05 response with an undecodable payload records FAIL without crashing."""

    @patch("espfm_device_test.do_request")
    def test_fans_list_garbage_payload_fails_cleanly(self, mock_request):
        mock_request.return_value = (0x45, b"\xff\xff\xff not a protobuf FanList")

        state = _make_state()
        run_fans_list(state)  # must not raise

        self.assertIsNone(state.fan_list)
        result = state.results[-1]
        self.assertEqual(result.label, "GET /fans")
        self.assertEqual(result.verdict, "FAIL")
        self.assertEqual(result.expected_status, "2.05")
        self.assertEqual(result.actual_status, "2.05 Content")
        self.assertTrue(result.response_summary.startswith("decode error:"))


# ============================================================
# run_wifi_status
# ============================================================


class TestRunWifiStatus(unittest.TestCase):
    """Tests for run_wifi_status — GET /wifi/status decodes WifiStatus."""

    @patch("espfm_device_test.do_request")
    def test_2_05_decodes_wifi_status(self, mock_request):
        status = pb.WifiStatus()
        status.sta_connected = True
        status.sta_ip = "192.168.0.28"
        status.ap_ip = "192.168.4.1"
        mock_request.return_value = (0x45, status.SerializeToString())

        state = _make_state()
        run_wifi_status(state)

        mock_request.assert_called_once_with(COAP_GET, "/wifi/status")
        result = state.results[-1]
        self.assertEqual(result.label, "GET /wifi/status")
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.expected_status, "2.05")
        self.assertEqual(result.actual_status, "2.05 Content")
        self.assertIn("sta_connected=True", result.response_summary)
        self.assertIn("sta_ip='192.168.0.28'", result.response_summary)
        self.assertIn("ap_ip='192.168.4.1'", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_timeout_marks_fail(self, mock_request):
        mock_request.return_value = (None, None)

        state = _make_state()
        run_wifi_status(state)

        result = state.results[-1]
        self.assertEqual(result.verdict, "FAIL")
        self.assertEqual(result.actual_status, "TIMEOUT")
        self.assertEqual(result.response_summary, "TIMEOUT")


# ============================================================
# run_wifi_scan
# ============================================================


class TestRunWifiScan(unittest.TestCase):
    """Tests for run_wifi_scan — GET /wifi/scan with a 10 s timeout."""

    @patch("espfm_device_test.do_request")
    def test_2_05_decodes_ap_list(self, mock_request):
        scan = pb.WifiScanResult()
        ap = scan.aps.add()
        ap.ssid = "HomeNet"
        ap.rssi = -45
        ap.channel = 6
        ap.authmode = 3
        mock_request.return_value = (0x45, scan.SerializeToString())

        state = _make_state()
        run_wifi_scan(state)

        mock_request.assert_called_once_with(COAP_GET, "/wifi/scan", timeout=10)
        result = state.results[-1]
        self.assertEqual(result.label, "GET /wifi/scan")
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.expected_status, "2.05")
        self.assertEqual(result.actual_status, "2.05 Content")
        self.assertIn("ssid='HomeNet'", result.response_summary)
        self.assertIn("rssi=-45", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_2_05_empty_ap_list(self, mock_request):
        scan = pb.WifiScanResult()
        mock_request.return_value = (0x45, scan.SerializeToString())

        state = _make_state()
        run_wifi_scan(state)

        result = state.results[-1]
        self.assertEqual(result.verdict, "PASS")
        self.assertIn("empty AP list", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_timeout_marks_fail(self, mock_request):
        mock_request.return_value = (None, None)

        state = _make_state()
        run_wifi_scan(state)

        result = state.results[-1]
        self.assertEqual(result.verdict, "FAIL")
        self.assertEqual(result.actual_status, "TIMEOUT")


# ============================================================
# run_ds18b20_scan
# ============================================================


class TestRunDs18b20Scan(unittest.TestCase):
    """Tests for run_ds18b20_scan — 2.05 decodes; 5.03 is a valid outcome."""

    @patch("espfm_device_test.do_request")
    def test_2_05_decodes_scan_response(self, mock_request):
        scan = pb.Ds18b20ScanResponse()
        scan.device_count = 1
        dev = scan.devices.add()
        dev.index = 0
        dev.rom_code = 0x28ABCD
        dev.temp_c = 25.5
        mock_request.return_value = (0x45, scan.SerializeToString())

        state = _make_state()
        run_ds18b20_scan(state)

        mock_request.assert_called_once_with(COAP_GET, "/ds18b20/scan")
        result = state.results[-1]
        self.assertEqual(result.label, "GET /ds18b20/scan")
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.expected_status, "2.05 or 5.03")
        self.assertIn("device_count=1", result.response_summary)
        self.assertIn("temp_c=25.5", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_5_03_no_bus_is_valid_pass(self, mock_request):
        mock_request.return_value = (0xA3, b"")

        state = _make_state()
        run_ds18b20_scan(state)

        result = state.results[-1]
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.actual_status, "5.03")
        self.assertIn("no DS18B20 bus", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_other_code_marks_fail(self, mock_request):
        mock_request.return_value = (0x80, b"")  # 4.00

        state = _make_state()
        run_ds18b20_scan(state)

        result = state.results[-1]
        self.assertEqual(result.verdict, "FAIL")
        self.assertEqual(result.actual_status, "4.00 Bad Request")


# ============================================================
# run_control_originals
# ============================================================


class TestRunControlOriginals(unittest.TestCase):
    """Tests for run_control_originals — captures ControlConfig originals."""

    @patch("espfm_device_test.do_request")
    def test_2_05_decodes_and_stores_originals(self, mock_request):
        control = pb.ControlConfig()
        control.hysteresis = 3
        control.ramp_up = 10
        control.ramp_down = 3
        control.failsafe_policy = pb.FAILSAFE_SAFE_DUTY
        control.safe_duty = 50
        mock_request.return_value = (0x45, control.SerializeToString())

        state = _make_state()
        run_control_originals(state)

        mock_request.assert_called_once_with(COAP_GET, "/control")
        self.assertIsNotNone(state.control_originals)
        self.assertEqual(state.control_originals.hysteresis, 3)
        self.assertEqual(state.control_originals.ramp_up, 10)
        self.assertEqual(state.control_originals.ramp_down, 3)
        self.assertEqual(state.control_originals.failsafe_policy, pb.FAILSAFE_SAFE_DUTY)
        self.assertEqual(state.control_originals.safe_duty, 50)
        result = state.results[-1]
        self.assertEqual(result.label, "GET /control")
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.expected_status, "2.05")
        self.assertIn("hysteresis=3", result.response_summary)
        self.assertIn("safe_duty=50", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_5_03_control_unset_marks_fail(self, mock_request):
        mock_request.return_value = (0xA3, b"")

        state = _make_state()
        run_control_originals(state)

        result = state.results[-1]
        self.assertEqual(result.verdict, "FAIL")
        self.assertEqual(result.actual_status, "5.03")
        self.assertIn("control unset", result.response_summary)
        self.assertIsNone(state.control_originals)

    @patch("espfm_device_test.do_request")
    def test_decode_error_marks_fail(self, mock_request):
        mock_request.return_value = (0x45, b"\xff\xff\xff not a ControlConfig")

        state = _make_state()
        run_control_originals(state)

        result = state.results[-1]
        self.assertEqual(result.verdict, "FAIL")
        self.assertIsNone(state.control_originals)
        self.assertTrue(result.response_summary.startswith("decode error:"))


# ============================================================
# discover_free_pins
# ============================================================


class TestDiscoverFreePins(unittest.TestCase):
    """Tests for discover_free_pins — selects free pwm/tach/DS pins (R21)."""

    def test_free_pair_and_ds_pin_selected(self):
        fan_list = pb.FanList()
        fan = fan_list.fans.add()
        fan.id = 0
        fan.name = "gpu"
        fan.pwm_gpio = 22
        fan.tach_gpio = 23

        source_list = pb.SourceList()
        ntc = source_list.sources.add()
        ntc.id = 0
        ntc.name = "ntc"
        ntc.gpio = 34
        manual = source_list.sources.add()
        manual.id = 3
        manual.name = "manual"
        manual.gpio = 255  # "none" — excluded from the used set

        state = _make_state()
        state.fan_list = fan_list
        state.source_list = source_list
        discover_free_pins(state)

        self.assertEqual(state.free_fan_pwm, 2)
        self.assertEqual(state.free_fan_tach, 4)
        self.assertEqual(state.free_ds_pin, 5)
        result = state.results[-1]
        self.assertEqual(result.label, "discover_free_pins")
        self.assertEqual(result.surface, "GET /fans + GET /sources")
        self.assertEqual(result.expected_status, "n/a")
        self.assertEqual(result.actual_status, "n/a")
        self.assertEqual(result.verdict, "PASS")
        self.assertIn("used_gpio=[22, 23, 34]", result.response_summary)
        self.assertIn(
            "free_fan_pwm=2, free_fan_tach=4, free_ds_pin=5",
            result.response_summary,
        )

    def test_tach_255_when_no_second_pin_free(self):
        fan_list = pb.FanList()
        # Occupy every claimable pin except 2 (the reserved table already skips
        # 1,3,6-11,16,17), so pin 2 is the only free pwm and no tach is free ->
        # the tach=255 fallback fires.
        idx = 0
        for pin in range(1, 41):
            if pin == 2 or pin in {1, 3, 6, 7, 8, 9, 10, 11, 16, 17}:
                continue
            fan = fan_list.fans.add()
            fan.id = idx
            fan.name = f"fan-{idx}"
            fan.pwm_gpio = pin
            fan.tach_gpio = 255
            idx += 1

        state = _make_state()
        state.fan_list = fan_list
        discover_free_pins(state)

        self.assertEqual(state.free_fan_pwm, 2)
        self.assertEqual(state.free_fan_tach, 255)
        self.assertIsNone(state.free_ds_pin)
        result = state.results[-1]
        self.assertEqual(result.verdict, "PASS")
        self.assertIn(
            "free_fan_pwm=2, free_fan_tach=255, free_ds_pin=None",
            result.response_summary,
        )

    def test_no_free_pins_sets_none(self):
        fan_list = pb.FanList()
        for idx, pin in enumerate(range(1, 41)):
            fan = fan_list.fans.add()
            fan.id = idx
            fan.name = f"fan-{idx}"
            fan.pwm_gpio = pin
            fan.tach_gpio = 255

        state = _make_state()
        state.fan_list = fan_list
        discover_free_pins(state)

        self.assertIsNone(state.free_fan_pwm)
        self.assertIsNone(state.free_fan_tach)
        self.assertIsNone(state.free_ds_pin)
        result = state.results[-1]
        self.assertEqual(result.verdict, "PASS")
        self.assertIn("free_fan_pwm=None", result.response_summary)

    def test_empty_lists_pick_lowest_free_pins(self):
        state = _make_state()
        state.fan_list = pb.FanList()
        state.source_list = pb.SourceList()
        discover_free_pins(state)

        self.assertEqual(state.free_fan_pwm, 2)
        self.assertEqual(state.free_fan_tach, 4)
        self.assertEqual(state.free_ds_pin, 5)
        result = state.results[-1]
        self.assertEqual(result.verdict, "PASS")
        self.assertIn("used_gpio=[(none)]", result.response_summary)


# ============================================================
# run_create_fan
# ============================================================


class TestRunCreateFan(unittest.TestCase):
    """Tests for run_create_fan — POST /fans create or NOT TESTED (R22-R24)."""

    @patch("espfm_device_test.do_request")
    def test_no_free_pair_marks_not_tested(self, mock_request):
        state = _make_state()
        state.free_fan_pwm = None

        run_create_fan(state)

        mock_request.assert_not_called()
        self.assertIsNone(state.created_fan_id)
        result = state.results[-1]
        self.assertEqual(result.label, "POST /fans")
        self.assertEqual(result.surface, "POST /fans")
        self.assertEqual(result.verdict, "NOT TESTED")
        self.assertEqual(result.actual_status, "NOT TESTED")
        self.assertEqual(result.note, "no free pwm/tach pair available")

    @patch("espfm_device_test.do_request")
    def test_2_01_sets_created_fan_id(self, mock_request):
        fan_info = pb.FanInfo()
        fan_info.id = 9
        fan_info.name = "test-fan"
        fan_info.pwm_gpio = 1
        fan_info.tach_gpio = 2
        mock_request.return_value = (0x41, fan_info.SerializeToString())

        state = _make_state()
        state.free_fan_pwm = 1
        state.free_fan_tach = 2
        run_create_fan(state)

        self.assertEqual(state.created_fan_id, 9)
        mock_request.assert_called_once()
        call_args = mock_request.call_args[0]
        self.assertEqual(call_args[0], COAP_POST)
        self.assertEqual(call_args[1], "/fans")
        req = call_args[2]
        self.assertEqual(req.pwm_gpio, 1)
        self.assertEqual(req.tach_gpio, 2)
        self.assertEqual(req.name, "test-fan")
        result = state.results[-1]
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.expected_status, "2.01")
        self.assertEqual(result.actual_status, "2.01 Created")
        self.assertIn(
            "FanCreateRequest{pwm_gpio=1, tach_gpio=2",
            result.request_summary,
        )
        self.assertIn("id=9", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_4_00_status_response_marks_fail(self, mock_request):
        st = pb.StatusResponse()
        st.ok = False
        st.error_code = 7
        st.error_msg = "gpio in use"
        mock_request.return_value = (0x80, st.SerializeToString())

        state = _make_state()
        state.free_fan_pwm = 1
        state.free_fan_tach = 2
        run_create_fan(state)

        self.assertIsNone(state.created_fan_id)
        result = state.results[-1]
        self.assertEqual(result.verdict, "FAIL")
        self.assertEqual(result.actual_status, "4.00")
        self.assertIn(
            "ok=False, error_code=7, error_msg='gpio in use'",
            result.response_summary,
        )

    @patch("espfm_device_test.do_request")
    def test_timeout_marks_fail(self, mock_request):
        mock_request.return_value = (None, None)

        state = _make_state()
        state.free_fan_pwm = 1
        state.free_fan_tach = 2
        run_create_fan(state)

        self.assertIsNone(state.created_fan_id)
        result = state.results[-1]
        self.assertEqual(result.verdict, "FAIL")
        self.assertEqual(result.actual_status, "TIMEOUT")
        self.assertEqual(result.response_summary, "TIMEOUT")


# ============================================================
# run_create_source
# ============================================================


class TestRunCreateSource(unittest.TestCase):
    """Tests for run_create_source — POST /sources create or FAIL (R25)."""

    @patch("espfm_device_test.do_request")
    def test_2_01_sets_created_source_id(self, mock_request):
        src_info = pb.SourceInfo()
        src_info.id = 4
        src_info.name = "test-source"
        src_info.type = pb.SOURCE_TYPE_MANUAL
        src_info.gpio = 255
        mock_request.return_value = (0x41, src_info.SerializeToString())

        state = _make_state()
        run_create_source(state)

        self.assertEqual(state.created_source_id, 4)
        mock_request.assert_called_once()
        req = mock_request.call_args[0][2]
        self.assertEqual(req.type, pb.SOURCE_TYPE_MANUAL)
        self.assertEqual(req.name, "test-source")
        self.assertEqual(req.gpio, 255)
        result = state.results[-1]
        self.assertEqual(result.label, "POST /sources")
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.expected_status, "2.01")
        self.assertEqual(result.actual_status, "2.01 Created")
        self.assertIn("SOURCE_TYPE_MANUAL", result.request_summary)
        self.assertIn("id=4", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_4_00_status_response_marks_fail(self, mock_request):
        st = pb.StatusResponse()
        st.ok = False
        st.error_code = 5
        st.error_msg = "source add failed"
        mock_request.return_value = (0x80, st.SerializeToString())

        state = _make_state()
        run_create_source(state)

        self.assertIsNone(state.created_source_id)
        result = state.results[-1]
        self.assertEqual(result.verdict, "FAIL")
        self.assertEqual(result.actual_status, "4.00 Bad Request")
        self.assertIn(
            "ok=False, error_code=5, error_msg='source add failed'",
            result.response_summary,
        )

    @patch("espfm_device_test.do_request")
    def test_timeout_marks_fail(self, mock_request):
        mock_request.return_value = (None, None)

        state = _make_state()
        run_create_source(state)

        self.assertIsNone(state.created_source_id)
        result = state.results[-1]
        self.assertEqual(result.verdict, "FAIL")
        self.assertEqual(result.actual_status, "TIMEOUT")


# ============================================================
# run_create_curve
# ============================================================


class TestRunCreateCurve(unittest.TestCase):
    """Tests for run_create_curve — POST /curves create or FAIL (R26)."""

    @patch("espfm_device_test.do_request")
    def test_2_01_sets_created_curve_id(self, mock_request):
        curve_info = pb.CurveInfo()
        curve_info.id = 6
        curve_info.name = "test-curve"
        mock_request.return_value = (0x41, curve_info.SerializeToString())

        state = _make_state()
        run_create_curve(state)

        self.assertEqual(state.created_curve_id, 6)
        mock_request.assert_called_once()
        req = mock_request.call_args[0][2]
        self.assertEqual(len(req.points), 5)
        self.assertEqual(
            [(p.temp_c, p.duty) for p in req.points],
            [(25.0, 10), (35.0, 30), (45.0, 50), (55.0, 70), (65.0, 90)],
        )
        result = state.results[-1]
        self.assertEqual(result.label, "POST /curves")
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.actual_status, "2.01 Created")
        self.assertIn("points=5", result.request_summary)
        self.assertIn("id=6", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_4_00_status_response_marks_fail(self, mock_request):
        st = pb.StatusResponse()
        st.ok = False
        st.error_code = 6
        st.error_msg = "curve add failed"
        mock_request.return_value = (0x80, st.SerializeToString())

        state = _make_state()
        run_create_curve(state)

        self.assertIsNone(state.created_curve_id)
        result = state.results[-1]
        self.assertEqual(result.verdict, "FAIL")
        self.assertEqual(result.actual_status, "4.00 Bad Request")
        self.assertIn("error_code=6", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_timeout_marks_fail(self, mock_request):
        mock_request.return_value = (None, None)

        state = _make_state()
        run_create_curve(state)

        self.assertIsNone(state.created_curve_id)
        result = state.results[-1]
        self.assertEqual(result.verdict, "FAIL")
        self.assertEqual(result.actual_status, "TIMEOUT")


# ============================================================
# run_create_schedule
# ============================================================


class TestRunCreateSchedule(unittest.TestCase):
    """Tests for run_create_schedule — POST /schedules on the live fan (R27)."""

    @patch("espfm_device_test.do_request")
    def test_2_01_sets_created_schedule_id_using_live_fan_id(self, mock_request):
        sched_info = pb.ScheduleInfo()
        sched_info.id = 8
        sched_info.fan_id = 3
        mock_request.return_value = (0x41, sched_info.SerializeToString())

        state = _make_state()
        state.live_fan_id = 3
        run_create_schedule(state)

        self.assertEqual(state.created_schedule_id, 8)
        mock_request.assert_called_once()
        req = mock_request.call_args[0][2]
        self.assertEqual(req.fan_id, 3)
        self.assertEqual(req.duty, 50)
        self.assertEqual(req.start_min, 600)
        self.assertEqual(req.end_min, 1080)
        self.assertTrue(req.enabled)
        self.assertEqual(req.name, "test-schedule")
        result = state.results[-1]
        self.assertEqual(result.label, "POST /schedules")
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.actual_status, "2.01 Created")
        self.assertIn("fan_id=3", result.request_summary)
        self.assertIn("id=8", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_4_00_status_response_marks_fail(self, mock_request):
        st = pb.StatusResponse()
        st.ok = False
        st.error_code = 8
        st.error_msg = "schedule add failed"
        mock_request.return_value = (0x80, st.SerializeToString())

        state = _make_state()
        state.live_fan_id = 3
        run_create_schedule(state)

        self.assertIsNone(state.created_schedule_id)
        result = state.results[-1]
        self.assertEqual(result.verdict, "FAIL")
        self.assertEqual(result.actual_status, "4.00 Bad Request")
        self.assertIn("error_code=8", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_timeout_marks_fail(self, mock_request):
        mock_request.return_value = (None, None)

        state = _make_state()
        state.live_fan_id = 3
        run_create_schedule(state)

        self.assertIsNone(state.created_schedule_id)
        result = state.results[-1]
        self.assertEqual(result.verdict, "FAIL")
        self.assertEqual(result.actual_status, "TIMEOUT")


# ============================================================
# run_manual_temp
# ============================================================


class TestRunManualTemp(unittest.TestCase):
    """Tests for run_manual_temp — POST /sources/temp with the manual source (R28)."""

    def _source_list_with_manual(self, manual_id):
        """Build a SourceList containing one SOURCE_TYPE_MANUAL source."""
        source_list = pb.SourceList()
        manual = source_list.sources.add()
        manual.id = manual_id
        manual.name = "manual"
        manual.type = pb.SOURCE_TYPE_MANUAL
        manual.gpio = 255
        return source_list

    @patch("espfm_device_test.do_request")
    def test_live_manual_source_id_used_and_passes(self, mock_request):
        st = pb.StatusResponse()
        st.ok = True
        mock_request.return_value = (0x44, st.SerializeToString())

        state = _make_state()
        state.source_list = self._source_list_with_manual(3)
        state.manual_source_id = 3
        state.created_source_id = 99  # must not override the live manual id
        run_manual_temp(state)

        mock_request.assert_called_once()
        req = mock_request.call_args[0][2]
        self.assertEqual(req.id, 3)
        self.assertEqual(req.temp_c, 20.0)
        result = state.results[-1]
        self.assertEqual(result.label, "POST /sources/temp")
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.expected_status, "2.04")
        self.assertEqual(result.actual_status, "2.04 Changed")
        self.assertIn("ManualTempRequest{id=3", result.request_summary)
        self.assertIn("ok=True", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_created_source_id_fallback_used(self, mock_request):
        st = pb.StatusResponse()
        st.ok = True
        mock_request.return_value = (0x44, st.SerializeToString())

        state = _make_state()
        state.source_list = None
        state.created_source_id = 4
        run_manual_temp(state)

        mock_request.assert_called_once()
        req = mock_request.call_args[0][2]
        self.assertEqual(req.id, 4)
        result = state.results[-1]
        self.assertEqual(result.verdict, "PASS")

    @patch("espfm_device_test.do_request")
    def test_no_manual_id_marks_not_tested(self, mock_request):
        state = _make_state()
        state.source_list = None
        state.created_source_id = None
        run_manual_temp(state)

        mock_request.assert_not_called()
        self.assertEqual(len(state.results), 1)
        result = state.results[-1]
        self.assertEqual(result.label, "POST /sources/temp")
        self.assertEqual(result.verdict, "NOT TESTED")
        self.assertEqual(result.actual_status, "NOT TESTED")
        self.assertEqual(result.note, "no manual source id available")

    @patch("espfm_device_test.do_request")
    def test_2_04_ok_false_marks_fail(self, mock_request):
        st = pb.StatusResponse()
        st.ok = False
        st.error_msg = "boom"
        mock_request.return_value = (0x44, st.SerializeToString())

        state = _make_state()
        state.source_list = self._source_list_with_manual(3)
        state.manual_source_id = 3
        run_manual_temp(state)

        result = state.results[-1]
        self.assertEqual(result.verdict, "FAIL")
        self.assertEqual(result.actual_status, "2.04 Changed")
        self.assertIn("ok=False, error_msg='boom'", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_4_04_source_not_found_marks_fail(self, mock_request):
        st = pb.StatusResponse()
        st.ok = False
        st.error_code = 404
        st.error_msg = "source not found"
        mock_request.return_value = (0x84, st.SerializeToString())

        state = _make_state()
        state.source_list = self._source_list_with_manual(3)
        state.manual_source_id = 3
        run_manual_temp(state)

        result = state.results[-1]
        self.assertEqual(result.verdict, "FAIL")
        self.assertEqual(result.actual_status, "4.04 Not Found")
        self.assertIn(
            "ok=False, error_code=404, error_msg='source not found'",
            result.response_summary,
        )


# ============================================================
# run_ds18b20_config
# ============================================================


class TestRunDs18b20Config(unittest.TestCase):
    """Tests for run_ds18b20_config — POST /ds18b20/config (R29-R30)."""

    @patch("espfm_device_test.do_request")
    def test_no_free_pin_marks_not_tested(self, mock_request):
        state = _make_state()
        state.free_ds_pin = None

        run_ds18b20_config(state)

        mock_request.assert_not_called()
        result = state.results[-1]
        self.assertEqual(result.label, "POST /ds18b20/config")
        self.assertEqual(result.surface, "POST /ds18b20/config")
        self.assertEqual(result.verdict, "NOT TESTED")
        self.assertEqual(result.actual_status, "NOT TESTED")
        self.assertEqual(result.note, "no free GPIO pin available")

    @patch("espfm_device_test.do_request")
    def test_2_04_ok_true_passes(self, mock_request):
        st = pb.StatusResponse()
        st.ok = True
        mock_request.return_value = (0x44, st.SerializeToString())

        state = _make_state()
        state.free_ds_pin = 3
        run_ds18b20_config(state)

        mock_request.assert_called_once()
        req = mock_request.call_args[0][2]
        self.assertEqual(req.gpio, 3)
        result = state.results[-1]
        self.assertEqual(result.label, "POST /ds18b20/config")
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.expected_status, "2.04")
        self.assertEqual(result.actual_status, "2.04 Changed")
        self.assertIn("Ds18b20ConfigRequest{gpio=3}", result.request_summary)
        self.assertIn("ok=True", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_4_00_status_response_marks_fail(self, mock_request):
        st = pb.StatusResponse()
        st.ok = False
        st.error_code = 1
        st.error_msg = "init fail"
        mock_request.return_value = (0x80, st.SerializeToString())

        state = _make_state()
        state.free_ds_pin = 3
        run_ds18b20_config(state)

        result = state.results[-1]
        self.assertEqual(result.verdict, "FAIL")
        self.assertEqual(result.actual_status, "4.00 Bad Request")
        self.assertIn(
            "ok=False, error_code=1, error_msg='init fail'",
            result.response_summary,
        )


# ============================================================
# run_fan_get
# ============================================================


class TestRunFanGet(unittest.TestCase):
    """Tests for run_fan_get — GET /fans/{id} verifies the FanInfo id (R31)."""

    @patch("espfm_device_test.do_request")
    def test_2_05_matching_id_passes(self, mock_request):
        fan = pb.FanInfo()
        fan.id = 7
        fan.name = "gpu"
        fan.pwm_gpio = 22
        fan.tach_gpio = 23
        mock_request.return_value = (0x45, fan.SerializeToString())

        state = _make_state()
        state.live_fan_id = 7
        run_fan_get(state)

        mock_request.assert_called_once_with(COAP_GET, "/fans/7")
        result = state.results[-1]
        self.assertEqual(result.label, "GET /fans/7")
        self.assertEqual(result.surface, "/fans/7")
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.expected_status, "2.05")
        self.assertEqual(result.actual_status, "2.05 Content")
        self.assertIn("id=7", result.response_summary)
        self.assertIn("pwm_gpio=22", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_2_05_mismatched_id_marks_fail(self, mock_request):
        fan = pb.FanInfo()
        fan.id = 99
        fan.name = "other"
        mock_request.return_value = (0x45, fan.SerializeToString())

        state = _make_state()
        state.live_fan_id = 7
        run_fan_get(state)

        result = state.results[-1]
        self.assertEqual(result.verdict, "FAIL")
        self.assertEqual(result.actual_status, "2.05 Content")
        self.assertEqual(
            result.response_summary, "id mismatch: got 99, expected 7"
        )


# ============================================================
# run_source_get
# ============================================================


class TestRunSourceGet(unittest.TestCase):
    """Tests for run_source_get — GET /sources/{id} verifies SourceInfo (R32)."""

    @patch("espfm_device_test.do_request")
    def test_2_05_matching_id_passes(self, mock_request):
        src = pb.SourceInfo()
        src.id = 3
        src.name = "manual"
        src.type = pb.SOURCE_TYPE_MANUAL
        src.status = 1
        src.temp_c = 20.0
        src.gpio = 255
        mock_request.return_value = (0x45, src.SerializeToString())

        state = _make_state()
        state.manual_source_id = 3
        run_source_get(state)

        mock_request.assert_called_once_with(COAP_GET, "/sources/3")
        result = state.results[-1]
        self.assertEqual(result.label, "/sources/3")
        self.assertEqual(result.surface, "/sources/3")
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.expected_status, "2.05")
        self.assertEqual(result.actual_status, "2.05 Content")
        self.assertIn("id=3", result.response_summary)
        self.assertIn("name='manual'", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_no_source_id_marks_not_tested(self, mock_request):
        state = _make_state()  # manual_source_id defaults to None
        run_source_get(state)

        mock_request.assert_not_called()
        self.assertEqual(len(state.results), 1)
        result = state.results[-1]
        self.assertEqual(result.label, "GET /sources/{id}")
        self.assertEqual(result.surface, "GET /sources/{id}")
        self.assertEqual(result.path, "GET /sources/{id}")
        self.assertEqual(result.verdict, "NOT TESTED")
        self.assertEqual(result.expected_status, "2.05")
        self.assertEqual(result.actual_status, "NOT TESTED")
        self.assertEqual(result.note, "no pre-existing source id available")


# ============================================================
# run_curve_get
# ============================================================


class TestRunCurveGet(unittest.TestCase):
    """Tests for run_curve_get — GET /curves/{id} verifies CurveInfo (R33)."""

    @patch("espfm_device_test.do_request")
    def test_2_05_matching_id_passes(self, mock_request):
        curve = pb.CurveInfo()
        curve.id = 5
        curve.name = "gpu-temp"
        point = curve.points.add()
        point.temp_c = 30.0
        point.duty = 20
        mock_request.return_value = (0x45, curve.SerializeToString())

        state = _make_state()
        state.first_curve_id = 5
        run_curve_get(state)

        mock_request.assert_called_once_with(COAP_GET, "/curves/5")
        result = state.results[-1]
        self.assertEqual(result.label, "/curves/5")
        self.assertEqual(result.surface, "/curves/5")
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.expected_status, "2.05")
        self.assertEqual(result.actual_status, "2.05 Content")
        self.assertIn("id=5", result.response_summary)
        self.assertIn("points=1", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_no_curve_id_marks_not_tested(self, mock_request):
        state = _make_state()  # first_curve_id defaults to None
        run_curve_get(state)

        mock_request.assert_not_called()
        self.assertEqual(len(state.results), 1)
        result = state.results[-1]
        self.assertEqual(result.label, "GET /curves/{id}")
        self.assertEqual(result.surface, "GET /curves/{id}")
        self.assertEqual(result.path, "GET /curves/{id}")
        self.assertEqual(result.verdict, "NOT TESTED")
        self.assertEqual(result.expected_status, "2.05")
        self.assertEqual(result.actual_status, "NOT TESTED")
        self.assertEqual(result.note, "no pre-existing curve id available")


# ============================================================
# run_schedule_get
# ============================================================


class TestRunScheduleGet(unittest.TestCase):
    """Tests for run_schedule_get — GET /schedules/{id} verifies ScheduleInfo (R34)."""

    @patch("espfm_device_test.do_request")
    def test_2_05_matching_id_passes(self, mock_request):
        sched = pb.ScheduleInfo()
        sched.id = 8
        sched.fan_id = 3
        sched.duty = 50
        sched.start_min = 600
        sched.end_min = 1080
        sched.enabled = True
        sched.name = "test-schedule"
        mock_request.return_value = (0x45, sched.SerializeToString())

        state = _make_state()
        state.created_schedule_id = 8
        run_schedule_get(state)

        mock_request.assert_called_once_with(COAP_GET, "/schedules/8")
        result = state.results[-1]
        self.assertEqual(result.label, "/schedules/8")
        self.assertEqual(result.surface, "/schedules/8")
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.expected_status, "2.05")
        self.assertEqual(result.actual_status, "2.05 Content")
        self.assertIn("id=8", result.response_summary)
        self.assertIn("name='test-schedule'", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_no_schedule_id_marks_not_tested(self, mock_request):
        state = _make_state()  # created_schedule_id defaults to None
        run_schedule_get(state)

        mock_request.assert_not_called()
        self.assertEqual(len(state.results), 1)
        result = state.results[-1]
        self.assertEqual(result.label, "GET /schedules/{id}")
        self.assertEqual(result.surface, "GET /schedules/{id}")
        self.assertEqual(result.path, "GET /schedules/{id}")
        self.assertEqual(result.verdict, "NOT TESTED")
        self.assertEqual(result.expected_status, "2.05")
        self.assertEqual(result.actual_status, "NOT TESTED")
        self.assertEqual(result.note, "no test schedule created")


# ============================================================
# run_error_path_get
# ============================================================


class TestRunErrorPathGet(unittest.TestCase):
    """Tests for run_error_path_get — GET /fans/7 records the 4.04 error (R35)."""

    @patch("espfm_device_test.do_request")
    def test_4_04_not_found_is_expected_pass(self, mock_request):
        mock_request.return_value = (0x84, b"")

        state = _make_state()
        run_error_path_get(state)

        mock_request.assert_called_once_with(COAP_GET, "/fans/7")
        result = state.results[-1]
        self.assertEqual(result.label, "GET /fans/7")
        self.assertEqual(result.surface, "GET /fans/7")
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.expected_status, "4.04")
        self.assertEqual(result.actual_status, "4.04")
        self.assertEqual(
            result.response_summary, "4.04 Not Found (unallocated slot)"
        )

    @patch("espfm_device_test.do_request")
    def test_2_05_fan_present_marks_fail(self, mock_request):
        fan = pb.FanInfo()
        fan.id = 7
        fan.name = "unexpected"
        mock_request.return_value = (0x45, fan.SerializeToString())

        state = _make_state()
        run_error_path_get(state)

        result = state.results[-1]
        self.assertEqual(result.verdict, "FAIL")
        self.assertEqual(result.actual_status, "2.05 Content")
        self.assertTrue(
            result.response_summary.startswith("unexpected fan present:")
        )
        self.assertIn("id=7", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_timeout_marks_fail(self, mock_request):
        mock_request.return_value = (None, None)

        state = _make_state()
        run_error_path_get(state)

        result = state.results[-1]
        self.assertEqual(result.verdict, "FAIL")
        self.assertEqual(result.actual_status, "TIMEOUT")
        self.assertEqual(result.response_summary, "TIMEOUT")


# ============================================================
# run_fan_update
# ============================================================


class TestRunFanUpdate(unittest.TestCase):
    """Tests for run_fan_update — PUT /fans/{id} duty update + restore (R36/R40)."""

    @patch("espfm_device_test.do_request")
    def test_update_and_restore_pass(self, mock_request):
        original = pb.FanInfo()
        original.id = 7
        original.duty = 50
        updated = pb.FanInfo()
        updated.id = 7
        updated.duty = 40
        restored = pb.FanInfo()
        restored.id = 7
        restored.duty = 50
        mock_request.side_effect = [
            (0x45, original.SerializeToString()),
            (0x44, updated.SerializeToString()),
            (0x44, restored.SerializeToString()),
        ]

        state = _make_state()
        state.live_fan_id = 7
        run_fan_update(state)

        self.assertEqual(mock_request.call_count, 3)
        self.assertEqual(mock_request.call_args_list[0][0][0], COAP_GET)
        self.assertEqual(mock_request.call_args_list[0][0][1], "/fans/7")
        update_req = mock_request.call_args_list[1][0][2]
        self.assertEqual(update_req.id, 7)
        self.assertEqual(update_req.duty, 40)
        restore_req = mock_request.call_args_list[2][0][2]
        self.assertEqual(restore_req.id, 7)
        self.assertEqual(restore_req.duty, 50)  # original duty restored

        self.assertEqual(len(state.results), 2)
        update_result = state.results[0]
        self.assertEqual(update_result.label, "PUT /fans/7")
        self.assertEqual(update_result.surface, "PUT /fans/7")
        self.assertEqual(update_result.verdict, "PASS")
        self.assertEqual(update_result.expected_status, "2.04")
        self.assertEqual(update_result.actual_status, "2.04 Changed")
        self.assertIn("FanUpdateRequest{id=7, duty=40}", update_result.request_summary)
        self.assertIn("duty=40", update_result.response_summary)
        restore_result = state.results[1]
        self.assertEqual(restore_result.label, "PUT /fans/7 (restore)")
        self.assertEqual(restore_result.verdict, "PASS")
        self.assertEqual(restore_result.actual_status, "2.04 Changed")
        self.assertIn("FanUpdateRequest{id=7, duty=50}", restore_result.request_summary)
        self.assertIn("duty=50", restore_result.response_summary)


# ============================================================
# run_source_update
# ============================================================


class TestRunSourceUpdate(unittest.TestCase):
    """Tests for run_source_update — PUT /sources/{id} rename + restore (R37/R40)."""

    @patch("espfm_device_test.do_request")
    def test_update_and_restore_pass(self, mock_request):
        original = pb.SourceInfo()
        original.id = 3
        original.name = "gpu-manual"
        updated = pb.SourceInfo()
        updated.id = 3
        updated.name = "gpu-manual-test"
        restored = pb.SourceInfo()
        restored.id = 3
        restored.name = "gpu-manual"
        mock_request.side_effect = [
            (0x45, original.SerializeToString()),
            (0x44, updated.SerializeToString()),
            (0x44, restored.SerializeToString()),
        ]

        state = _make_state()
        state.manual_source_id = 3
        run_source_update(state)

        self.assertEqual(mock_request.call_count, 3)
        self.assertEqual(mock_request.call_args_list[0][0][0], COAP_GET)
        self.assertEqual(mock_request.call_args_list[0][0][1], "/sources/3")
        update_req = mock_request.call_args_list[1][0][2]
        self.assertEqual(update_req.id, 3)
        self.assertEqual(update_req.name, "gpu-manual-test")
        restore_req = mock_request.call_args_list[2][0][2]
        self.assertEqual(restore_req.id, 3)
        self.assertEqual(restore_req.name, "gpu-manual")  # original name restored

        self.assertEqual(len(state.results), 2)
        update_result = state.results[0]
        self.assertEqual(update_result.label, "PUT /sources/3")
        self.assertEqual(update_result.verdict, "PASS")
        self.assertEqual(update_result.expected_status, "2.04")
        self.assertEqual(update_result.actual_status, "2.04 Changed")
        self.assertIn("name='gpu-manual-test'", update_result.response_summary)
        restore_result = state.results[1]
        self.assertEqual(restore_result.label, "PUT /sources/3 (restore)")
        self.assertEqual(restore_result.verdict, "PASS")
        self.assertIn("name='gpu-manual'", restore_result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_no_source_id_marks_not_tested(self, mock_request):
        state = _make_state()  # manual_source_id defaults to None
        run_source_update(state)

        mock_request.assert_not_called()
        self.assertEqual(len(state.results), 1)
        result = state.results[-1]
        self.assertEqual(result.label, "PUT /sources/{id}")
        self.assertEqual(result.surface, "PUT /sources/{id}")
        self.assertEqual(result.verdict, "NOT TESTED")
        self.assertEqual(result.expected_status, "2.04")
        self.assertEqual(result.actual_status, "NOT TESTED")
        self.assertEqual(result.note, "no pre-existing source id available")


# ============================================================
# run_curve_update
# ============================================================


class TestRunCurveUpdate(unittest.TestCase):
    """Tests for run_curve_update — PUT /curves/{id} rename + full-replace restore (R38/R40)."""

    @patch("espfm_device_test.do_request")
    def test_update_and_restore_pass(self, mock_request):
        original = pb.CurveInfo()
        original.id = 5
        original.name = "gpu-temp"
        point = original.points.add()
        point.temp_c = 30.0
        point.duty = 20
        point = original.points.add()
        point.temp_c = 40.0
        point.duty = 40

        updated = pb.CurveInfo()
        updated.id = 5
        updated.name = "gpu-temp-test"

        restored = pb.CurveInfo()
        restored.id = 5
        restored.name = "gpu-temp"
        point = restored.points.add()
        point.temp_c = 30.0
        point.duty = 20
        point = restored.points.add()
        point.temp_c = 40.0
        point.duty = 40
        mock_request.side_effect = [
            (0x45, original.SerializeToString()),
            (0x44, updated.SerializeToString()),
            (0x44, restored.SerializeToString()),
        ]

        state = _make_state()
        state.first_curve_id = 5
        run_curve_update(state)

        self.assertEqual(mock_request.call_count, 3)
        self.assertEqual(mock_request.call_args_list[0][0][1], "/curves/5")
        restore_req = mock_request.call_args_list[2][0][2]
        self.assertEqual(restore_req.id, 5)
        self.assertEqual(restore_req.name, "gpu-temp")  # original name restored
        self.assertEqual(len(restore_req.points), 2)
        self.assertEqual(
            [(p.temp_c, p.duty) for p in restore_req.points],
            [(30.0, 20), (40.0, 40)],
        )

        self.assertEqual(len(state.results), 2)
        update_result = state.results[0]
        self.assertEqual(update_result.label, "PUT /curves/5")
        self.assertEqual(update_result.verdict, "PASS")
        self.assertEqual(update_result.expected_status, "2.04")
        self.assertEqual(update_result.actual_status, "2.04 Changed")
        self.assertIn("name='gpu-temp-test'", update_result.response_summary)
        restore_result = state.results[1]
        self.assertEqual(restore_result.label, "PUT /curves/5 (restore)")
        self.assertEqual(restore_result.verdict, "PASS")
        self.assertIn("name='gpu-temp'", restore_result.response_summary)
        self.assertIn("points=2", restore_result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_no_curve_id_marks_not_tested(self, mock_request):
        state = _make_state()  # first_curve_id defaults to None
        run_curve_update(state)

        mock_request.assert_not_called()
        self.assertEqual(len(state.results), 1)
        result = state.results[-1]
        self.assertEqual(result.label, "PUT /curves/{id}")
        self.assertEqual(result.verdict, "NOT TESTED")
        self.assertEqual(result.expected_status, "2.04")
        self.assertEqual(result.actual_status, "NOT TESTED")
        self.assertEqual(result.note, "no pre-existing curve id available")


# ============================================================
# run_schedule_update
# ============================================================


class TestRunScheduleUpdate(unittest.TestCase):
    """Tests for run_schedule_update — PUT /schedules/{id} duty update + restore (R39/R40)."""

    @patch("espfm_device_test.do_request")
    def test_update_and_restore_pass(self, mock_request):
        original = pb.ScheduleInfo()
        original.id = 8
        original.duty = 50
        updated = pb.ScheduleInfo()
        updated.id = 8
        updated.duty = 80
        restored = pb.ScheduleInfo()
        restored.id = 8
        restored.duty = 50
        mock_request.side_effect = [
            (0x45, original.SerializeToString()),
            (0x44, updated.SerializeToString()),
            (0x44, restored.SerializeToString()),
        ]

        state = _make_state()
        state.created_schedule_id = 8
        run_schedule_update(state)

        self.assertEqual(mock_request.call_count, 3)
        self.assertEqual(mock_request.call_args_list[0][0][1], "/schedules/8")
        update_req = mock_request.call_args_list[1][0][2]
        self.assertEqual(update_req.id, 8)
        self.assertEqual(update_req.duty, 80)
        restore_req = mock_request.call_args_list[2][0][2]
        self.assertEqual(restore_req.id, 8)
        self.assertEqual(restore_req.duty, 50)  # original duty restored

        self.assertEqual(len(state.results), 2)
        update_result = state.results[0]
        self.assertEqual(update_result.label, "PUT /schedules/8")
        self.assertEqual(update_result.verdict, "PASS")
        self.assertEqual(update_result.expected_status, "2.04")
        self.assertEqual(update_result.actual_status, "2.04 Changed")
        self.assertIn("duty=80", update_result.response_summary)
        restore_result = state.results[1]
        self.assertEqual(restore_result.label, "PUT /schedules/8 (restore)")
        self.assertEqual(restore_result.verdict, "PASS")
        self.assertIn("duty=50", restore_result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_no_schedule_id_marks_not_tested(self, mock_request):
        state = _make_state()  # created_schedule_id defaults to None
        run_schedule_update(state)

        mock_request.assert_not_called()
        self.assertEqual(len(state.results), 1)
        result = state.results[-1]
        self.assertEqual(result.label, "PUT /schedules/{id}")
        self.assertEqual(result.verdict, "NOT TESTED")
        self.assertEqual(result.expected_status, "2.04")
        self.assertEqual(result.actual_status, "NOT TESTED")
        self.assertEqual(result.note, "no test schedule created")


# ============================================================
# run_control_restore
# ============================================================


class TestRunControlRestore(unittest.TestCase):
    """Tests for run_control_restore — PUT /control restores originals (R41)."""

    def _control_originals(self):
        """Build a ControlConfig holding the live-device originals (3/10/3/SAFE/50)."""
        control = pb.ControlConfig()
        control.hysteresis = 3
        control.ramp_up = 10
        control.ramp_down = 3
        control.failsafe_policy = pb.FAILSAFE_SAFE_DUTY
        control.safe_duty = 50
        return control

    @patch("espfm_device_test.do_request")
    def test_restore_originals_passes(self, mock_request):
        st = pb.StatusResponse()
        st.ok = True
        mock_request.return_value = (0x44, st.SerializeToString())

        state = _make_state()
        state.control_originals = self._control_originals()
        run_control_restore(state)

        self.assertEqual(mock_request.call_count, 1)
        call_args = mock_request.call_args[0]
        self.assertEqual(call_args[0], COAP_PUT)
        self.assertEqual(call_args[1], "/control")
        req = call_args[2]
        self.assertEqual(req.hysteresis, 3)
        self.assertEqual(req.ramp_up, 10)
        self.assertEqual(req.ramp_down, 3)
        self.assertEqual(req.failsafe_policy, pb.FAILSAFE_SAFE_DUTY)
        self.assertEqual(req.safe_duty, 50)
        result = state.results[-1]
        self.assertEqual(result.label, "PUT /control")
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.expected_status, "2.04")
        self.assertEqual(result.actual_status, "2.04 Changed")
        self.assertIn("hysteresis=3", result.request_summary)
        self.assertIn("ok=True", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_no_originals_marks_not_tested(self, mock_request):
        state = _make_state()  # control_originals defaults to None
        run_control_restore(state)

        mock_request.assert_not_called()
        self.assertEqual(len(state.results), 1)
        result = state.results[-1]
        self.assertEqual(result.label, "PUT /control")
        self.assertEqual(result.verdict, "NOT TESTED")
        self.assertEqual(result.actual_status, "NOT TESTED")
        self.assertEqual(result.note, "no control originals captured")


# ============================================================
# run_control_error
# ============================================================


class TestRunControlError(unittest.TestCase):
    """Tests for run_control_error — PUT /control out-of-range validation (R42)."""

    def _control_originals(self):
        """Build a ControlConfig holding the live-device originals (3/10/3/SAFE/50)."""
        control = pb.ControlConfig()
        control.hysteresis = 3
        control.ramp_up = 10
        control.ramp_down = 3
        control.failsafe_policy = pb.FAILSAFE_SAFE_DUTY
        control.safe_duty = 50
        return control

    @patch("espfm_device_test.do_request")
    def test_0x80_error_response_passes(self, mock_request):
        st = pb.StatusResponse()
        st.ok = False
        st.error_code = 7
        st.error_msg = "hysteresis out of range"
        mock_request.return_value = (0x80, st.SerializeToString())

        state = _make_state()
        state.control_originals = self._control_originals()
        run_control_error(state)

        self.assertEqual(mock_request.call_count, 1)
        call_args = mock_request.call_args[0]
        self.assertEqual(call_args[0], COAP_PUT)
        self.assertEqual(call_args[1], "/control")
        req = call_args[2]
        self.assertEqual(req.hysteresis, 150)
        self.assertEqual(req.ramp_up, 10)
        result = state.results[-1]
        self.assertEqual(result.label, "PUT /control (out-of-range)")
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.expected_status, "4.00")
        self.assertEqual(result.actual_status, "4.00")
        self.assertIn("hysteresis=150", result.request_summary)
        self.assertIn("ok=False", result.response_summary)
        self.assertIn("error_msg='hysteresis out of range'", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_0x44_unexpectedly_accepted_marks_fail(self, mock_request):
        st = pb.StatusResponse()
        st.ok = True
        mock_request.return_value = (0x44, st.SerializeToString())

        state = _make_state()
        state.control_originals = self._control_originals()
        run_control_error(state)

        result = state.results[-1]
        self.assertEqual(result.verdict, "FAIL")
        self.assertEqual(result.actual_status, "2.04 Changed")
        self.assertIn("unexpectedly accepted", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_no_originals_marks_not_tested(self, mock_request):
        state = _make_state()  # control_originals defaults to None
        run_control_error(state)

        mock_request.assert_not_called()
        self.assertEqual(len(state.results), 1)
        result = state.results[-1]
        self.assertEqual(result.label, "PUT /control (out-of-range)")
        self.assertEqual(result.surface, "PUT /control (out-of-range)")
        self.assertEqual(result.verdict, "NOT TESTED")
        self.assertEqual(result.expected_status, "4.00")
        self.assertEqual(result.actual_status, "NOT TESTED")
        self.assertEqual(result.note, "no control originals captured")


# ============================================================
# _status_case
# ============================================================


class TestStatusCase(unittest.TestCase):
    """Tests for _status_case — builds a CaseResult from a StatusResponse (R43-R48)."""

    def _status(self, ok=True, error_code=0, error_msg=""):
        """Build a StatusResponse message with the given fields."""
        st = pb.StatusResponse()
        st.ok = ok
        st.error_code = error_code
        st.error_msg = error_msg
        return st

    def test_2_02_ok_true_passes(self):
        """2.02 Deleted with StatusResponse.ok=True records PASS."""
        st = self._status(ok=True)
        result = _status_case(
            "DELETE /fans/3",
            "DELETE /fans/3",
            "/fans/3",
            "2.02",
            COAP_DELETE,
            0x42,
            st.SerializeToString(),
            "none",
        )
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.expected_status, "2.02")
        self.assertEqual(result.actual_status, "2.02 Deleted")
        self.assertEqual(result.method, COAP_DELETE)
        self.assertIn("ok=True", result.response_summary)

    def test_2_04_ok_true_passes(self):
        """2.04 Changed with StatusResponse.ok=True records PASS."""
        st = self._status(ok=True)
        result = _status_case(
            "PUT /system/hostname",
            "PUT /system/hostname",
            "/system/hostname",
            "2.04",
            COAP_PUT,
            0x44,
            st.SerializeToString(),
            "HostnameRequest{hostname='espfm-test'}",
        )
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.actual_status, "2.04 Changed")
        self.assertEqual(result.method, COAP_PUT)
        self.assertIn("ok=True", result.response_summary)

    def test_2_04_ok_false_marks_fail(self):
        """2.04 Changed with StatusResponse.ok=False records FAIL."""
        st = self._status(ok=False, error_code=7, error_msg="mdns fail")
        result = _status_case(
            "PUT /system/hostname",
            "PUT /system/hostname",
            "/system/hostname",
            "2.04",
            COAP_PUT,
            0x44,
            st.SerializeToString(),
            "HostnameRequest{hostname='espfm-test'}",
        )
        self.assertEqual(result.verdict, "FAIL")
        self.assertIn(
            "ok=False, error_code=7, error_msg='mdns fail'",
            result.response_summary,
        )

    def test_4_00_status_response_marks_fail(self):
        """4.00 with a decodable StatusResponse records FAIL via _status_summary."""
        st = self._status(ok=False, error_code=1, error_msg="bad request")
        result = _status_case(
            "DELETE /fans/3",
            "DELETE /fans/3",
            "/fans/3",
            "2.02",
            COAP_DELETE,
            0x80,
            st.SerializeToString(),
            "none",
        )
        self.assertEqual(result.verdict, "FAIL")
        self.assertEqual(result.actual_status, "4.00 Bad Request")
        self.assertIn("ok=False", result.response_summary)
        self.assertIn("error_msg='bad request'", result.response_summary)

    def test_timeout_marks_fail(self):
        """A timeout (None, None) records FAIL with actual_status TIMEOUT."""
        result = _status_case(
            "PUT /system/hostname",
            "PUT /system/hostname",
            "/system/hostname",
            "2.04",
            COAP_PUT,
            None,
            None,
            "none",
        )
        self.assertEqual(result.verdict, "FAIL")
        self.assertEqual(result.actual_status, "TIMEOUT")
        self.assertEqual(result.response_summary, "TIMEOUT")

    def test_2_04_undecodable_payload_fails_cleanly(self):
        """A 2.04 payload that is not a StatusResponse records FAIL without crashing."""
        result = _status_case(
            "PUT /system/hostname",
            "PUT /system/hostname",
            "/system/hostname",
            "2.04",
            COAP_PUT,
            0x44,
            b"\xff\xff\xff not a StatusResponse",
            "none",
        )
        self.assertEqual(result.verdict, "FAIL")
        self.assertTrue(result.response_summary.startswith("decode error:"))


# ============================================================
# run_hostname_test
# ============================================================


class TestRunHostnameTest(unittest.TestCase):
    """Tests for run_hostname_test — PUT /system/hostname with the test hostname (R43)."""

    @patch("espfm_device_test.do_request")
    def test_2_04_ok_true_passes(self, mock_request):
        st = pb.StatusResponse()
        st.ok = True
        mock_request.return_value = (0x44, st.SerializeToString())

        state = _make_state()
        run_hostname_test(state)

        mock_request.assert_called_once()
        call_args = mock_request.call_args[0]
        self.assertEqual(call_args[0], COAP_PUT)
        self.assertEqual(call_args[1], "/system/hostname")
        req = call_args[2]
        self.assertEqual(req.hostname, "espfm-test")
        result = state.results[-1]
        self.assertEqual(result.label, "PUT /system/hostname")
        self.assertEqual(result.surface, "PUT /system/hostname")
        self.assertEqual(result.path, "/system/hostname")
        self.assertEqual(result.method, COAP_PUT)
        self.assertEqual(result.expected_status, "2.04")
        self.assertEqual(result.actual_status, "2.04 Changed")
        self.assertEqual(result.verdict, "PASS")
        self.assertIn("HostnameRequest{hostname='espfm-test'}", result.request_summary)
        self.assertIn("ok=True", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_2_04_ok_false_marks_fail(self, mock_request):
        st = pb.StatusResponse()
        st.ok = False
        st.error_code = 7
        st.error_msg = "mdns fail"
        mock_request.return_value = (0x44, st.SerializeToString())

        state = _make_state()
        run_hostname_test(state)

        result = state.results[-1]
        self.assertEqual(result.verdict, "FAIL")
        self.assertEqual(result.actual_status, "2.04 Changed")
        self.assertIn(
            "ok=False, error_code=7, error_msg='mdns fail'",
            result.response_summary,
        )

    @patch("espfm_device_test.do_request")
    def test_timeout_marks_fail(self, mock_request):
        mock_request.return_value = (None, None)

        state = _make_state()
        run_hostname_test(state)

        result = state.results[-1]
        self.assertEqual(result.verdict, "FAIL")
        self.assertEqual(result.actual_status, "TIMEOUT")
        self.assertEqual(result.response_summary, "TIMEOUT")


# ============================================================
# run_hostname_restore
# ============================================================


class TestRunHostnameRestore(unittest.TestCase):
    """Tests for run_hostname_restore — restores the original hostname (R44)."""

    @patch("espfm_device_test.do_request")
    def test_original_hostname_set_restores_and_passes(self, mock_request):
        st = pb.StatusResponse()
        st.ok = True
        mock_request.return_value = (0x44, st.SerializeToString())

        state = _make_state()
        state.original_hostname = "espfm-c425"
        run_hostname_restore(state)

        mock_request.assert_called_once()
        call_args = mock_request.call_args[0]
        self.assertEqual(call_args[0], COAP_PUT)
        self.assertEqual(call_args[1], "/system/hostname")
        req = call_args[2]
        self.assertEqual(req.hostname, "espfm-c425")
        result = state.results[-1]
        self.assertEqual(result.label, "PUT /system/hostname (restore)")
        self.assertEqual(result.surface, "PUT /system/hostname (restore)")
        self.assertEqual(result.path, "/system/hostname")
        self.assertEqual(result.method, COAP_PUT)
        self.assertEqual(result.expected_status, "2.04")
        self.assertEqual(result.actual_status, "2.04 Changed")
        self.assertEqual(result.verdict, "PASS")
        self.assertIn("HostnameRequest{hostname='espfm-c425'}", result.request_summary)
        self.assertIn("ok=True", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_no_original_hostname_marks_not_tested(self, mock_request):
        state = _make_state()  # original_hostname defaults to None
        run_hostname_restore(state)

        mock_request.assert_not_called()
        self.assertEqual(len(state.results), 1)
        result = state.results[-1]
        self.assertEqual(result.label, "PUT /system/hostname (restore)")
        self.assertEqual(result.surface, "PUT /system/hostname (restore)")
        self.assertEqual(result.method, COAP_PUT)
        self.assertEqual(result.verdict, "NOT TESTED")
        self.assertEqual(result.expected_status, "2.04")
        self.assertEqual(result.actual_status, "NOT TESTED")
        self.assertEqual(result.note, "no original hostname captured")


# ============================================================
# run_delete_fan
# ============================================================


class TestRunDeleteFan(unittest.TestCase):
    """Tests for run_delete_fan — DELETE /fans/{created_id} (R45)."""

    @patch("espfm_device_test.do_request")
    def test_2_02_deletes_created_fan(self, mock_request):
        st = pb.StatusResponse()
        st.ok = True
        mock_request.return_value = (0x42, st.SerializeToString())

        state = _make_state()
        state.created_fan_id = 3
        state.live_fan_id = 0  # the pre-existing fan must never be deleted
        run_delete_fan(state)

        mock_request.assert_called_once()
        call_args = mock_request.call_args[0]
        self.assertEqual(call_args[0], COAP_DELETE)
        self.assertEqual(call_args[1], "/fans/3")
        result = state.results[-1]
        self.assertEqual(result.label, "DELETE /fans/3")
        self.assertEqual(result.surface, "DELETE /fans/3")
        self.assertEqual(result.path, "/fans/3")
        self.assertEqual(result.method, COAP_DELETE)
        self.assertEqual(result.expected_status, "2.02")
        self.assertEqual(result.actual_status, "2.02 Deleted")
        self.assertEqual(result.verdict, "PASS")
        self.assertIn("ok=True", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_no_created_fan_marks_not_tested(self, mock_request):
        state = _make_state()  # created_fan_id defaults to None
        run_delete_fan(state)

        mock_request.assert_not_called()
        self.assertEqual(len(state.results), 1)
        result = state.results[-1]
        self.assertEqual(result.label, "DELETE /fans/{created_id}")
        self.assertEqual(result.surface, "DELETE /fans/{created_id}")
        self.assertEqual(result.method, COAP_DELETE)
        self.assertEqual(result.verdict, "NOT TESTED")
        self.assertEqual(result.expected_status, "2.02")
        self.assertEqual(result.actual_status, "NOT TESTED")
        self.assertEqual(result.note, "no test fan created")

    @patch("espfm_device_test.do_request")
    def test_collision_with_live_fan_marks_not_tested(self, mock_request):
        state = _make_state()
        state.created_fan_id = 0
        state.live_fan_id = 0  # test fan reused the pre-existing fan's slot
        run_delete_fan(state)

        mock_request.assert_not_called()
        self.assertEqual(len(state.results), 1)
        result = state.results[-1]
        self.assertEqual(result.verdict, "NOT TESTED")
        self.assertEqual(result.actual_status, "NOT TESTED")
        self.assertIn("reused the pre-existing fan slot", result.note)


# ============================================================
# run_delete_source
# ============================================================


class TestRunDeleteSource(unittest.TestCase):
    """Tests for run_delete_source — DELETE /sources/{created_id} (R46)."""

    @patch("espfm_device_test.do_request")
    def test_2_02_deletes_created_source(self, mock_request):
        st = pb.StatusResponse()
        st.ok = True
        mock_request.return_value = (0x42, st.SerializeToString())

        state = _make_state()
        state.created_source_id = 2
        state.manual_source_id = 0  # the pre-existing source must never be deleted
        run_delete_source(state)

        mock_request.assert_called_once()
        call_args = mock_request.call_args[0]
        self.assertEqual(call_args[0], COAP_DELETE)
        self.assertEqual(call_args[1], "/sources/2")
        result = state.results[-1]
        self.assertEqual(result.label, "DELETE /sources/2")
        self.assertEqual(result.surface, "DELETE /sources/2")
        self.assertEqual(result.path, "/sources/2")
        self.assertEqual(result.method, COAP_DELETE)
        self.assertEqual(result.expected_status, "2.02")
        self.assertEqual(result.actual_status, "2.02 Deleted")
        self.assertEqual(result.verdict, "PASS")
        self.assertIn("ok=True", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_no_created_source_marks_not_tested(self, mock_request):
        state = _make_state()  # created_source_id defaults to None
        run_delete_source(state)

        mock_request.assert_not_called()
        self.assertEqual(len(state.results), 1)
        result = state.results[-1]
        self.assertEqual(result.label, "DELETE /sources/{created_id}")
        self.assertEqual(result.surface, "DELETE /sources/{created_id}")
        self.assertEqual(result.method, COAP_DELETE)
        self.assertEqual(result.verdict, "NOT TESTED")
        self.assertEqual(result.expected_status, "2.02")
        self.assertEqual(result.actual_status, "NOT TESTED")
        self.assertEqual(result.note, "no test source created")

    @patch("espfm_device_test.do_request")
    def test_collision_with_pre_existing_source_marks_not_tested(self, mock_request):
        state = _make_state()
        state.created_source_id = 0
        state.manual_source_id = 0  # test source reused the pre-existing slot
        run_delete_source(state)

        mock_request.assert_not_called()
        self.assertEqual(len(state.results), 1)
        result = state.results[-1]
        self.assertEqual(result.verdict, "NOT TESTED")
        self.assertEqual(result.actual_status, "NOT TESTED")
        self.assertIn("reused the pre-existing source slot", result.note)


# ============================================================
# run_delete_curve
# ============================================================


class TestRunDeleteCurve(unittest.TestCase):
    """Tests for run_delete_curve — DELETE /curves/{created_id} (R47)."""

    @patch("espfm_device_test.do_request")
    def test_2_02_deletes_created_curve(self, mock_request):
        st = pb.StatusResponse()
        st.ok = True
        mock_request.return_value = (0x42, st.SerializeToString())

        state = _make_state()
        state.created_curve_id = 4
        state.first_curve_id = 0  # the pre-existing curve must never be deleted
        run_delete_curve(state)

        mock_request.assert_called_once()
        call_args = mock_request.call_args[0]
        self.assertEqual(call_args[0], COAP_DELETE)
        self.assertEqual(call_args[1], "/curves/4")
        result = state.results[-1]
        self.assertEqual(result.label, "DELETE /curves/4")
        self.assertEqual(result.surface, "DELETE /curves/4")
        self.assertEqual(result.path, "/curves/4")
        self.assertEqual(result.method, COAP_DELETE)
        self.assertEqual(result.expected_status, "2.02")
        self.assertEqual(result.actual_status, "2.02 Deleted")
        self.assertEqual(result.verdict, "PASS")
        self.assertIn("ok=True", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_no_created_curve_marks_not_tested(self, mock_request):
        state = _make_state()  # created_curve_id defaults to None
        run_delete_curve(state)

        mock_request.assert_not_called()
        self.assertEqual(len(state.results), 1)
        result = state.results[-1]
        self.assertEqual(result.label, "DELETE /curves/{created_id}")
        self.assertEqual(result.surface, "DELETE /curves/{created_id}")
        self.assertEqual(result.method, COAP_DELETE)
        self.assertEqual(result.verdict, "NOT TESTED")
        self.assertEqual(result.expected_status, "2.02")
        self.assertEqual(result.actual_status, "NOT TESTED")
        self.assertEqual(result.note, "no test curve created")

    @patch("espfm_device_test.do_request")
    def test_collision_with_pre_existing_curve_marks_not_tested(self, mock_request):
        state = _make_state()
        state.created_curve_id = 0
        state.first_curve_id = 0  # test curve reused the pre-existing slot
        run_delete_curve(state)

        mock_request.assert_not_called()
        self.assertEqual(len(state.results), 1)
        result = state.results[-1]
        self.assertEqual(result.verdict, "NOT TESTED")
        self.assertEqual(result.actual_status, "NOT TESTED")
        self.assertIn("reused the pre-existing curve slot", result.note)


# ============================================================
# run_delete_schedule
# ============================================================


class TestRunDeleteSchedule(unittest.TestCase):
    """Tests for run_delete_schedule — DELETE /schedules/{created_id} (R48)."""

    @patch("espfm_device_test.do_request")
    def test_2_02_deletes_created_schedule(self, mock_request):
        st = pb.StatusResponse()
        st.ok = True
        mock_request.return_value = (0x42, st.SerializeToString())

        state = _make_state()
        state.created_schedule_id = 5
        run_delete_schedule(state)

        mock_request.assert_called_once()
        call_args = mock_request.call_args[0]
        self.assertEqual(call_args[0], COAP_DELETE)
        self.assertEqual(call_args[1], "/schedules/5")
        result = state.results[-1]
        self.assertEqual(result.label, "DELETE /schedules/5")
        self.assertEqual(result.surface, "DELETE /schedules/5")
        self.assertEqual(result.path, "/schedules/5")
        self.assertEqual(result.method, COAP_DELETE)
        self.assertEqual(result.expected_status, "2.02")
        self.assertEqual(result.actual_status, "2.02 Deleted")
        self.assertEqual(result.verdict, "PASS")
        self.assertIn("ok=True", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_no_created_schedule_marks_not_tested(self, mock_request):
        state = _make_state()  # created_schedule_id defaults to None
        run_delete_schedule(state)

        mock_request.assert_not_called()
        self.assertEqual(len(state.results), 1)
        result = state.results[-1]
        self.assertEqual(result.label, "DELETE /schedules/{created_id}")
        self.assertEqual(result.surface, "DELETE /schedules/{created_id}")
        self.assertEqual(result.method, COAP_DELETE)
        self.assertEqual(result.verdict, "NOT TESTED")
        self.assertEqual(result.expected_status, "2.02")
        self.assertEqual(result.actual_status, "NOT TESTED")
        self.assertEqual(result.note, "no test schedule created")


# ============================================================
# wait_for_device
# ============================================================


class TestWaitForDevice(unittest.TestCase):
    """Tests for wait_for_device — re-probes GET /system/info until the device returns (R50)."""

    @patch("espfm_device_test.do_request", side_effect=[(None, None), (0x45, b"")])
    @patch("espfm_device_test.time.sleep")
    def test_returns_true_when_device_responds_on_second_probe(self, mock_sleep, mock_request):
        """A failed first probe followed by a responding probe returns True."""
        state = _make_state()
        self.assertTrue(wait_for_device(state, timeout_s=10))
        self.assertEqual(mock_request.call_count, 2)
        mock_request.assert_called_with(COAP_GET, "/system/info", timeout=2)

    @patch("espfm_device_test.do_request", return_value=(None, None))
    @patch("espfm_device_test.time.monotonic", side_effect=[0.0, 0.0, 60.0, 60.0])
    @patch("espfm_device_test.time.sleep")
    def test_all_probes_fail_returns_false(self, mock_sleep, mock_monotonic, mock_request):
        """A device that never responds within the window returns False."""
        state = _make_state()
        self.assertFalse(wait_for_device(state, timeout_s=60))
        mock_request.assert_called_with(COAP_GET, "/system/info", timeout=2)

    @patch(
        "espfm_device_test.do_request",
        side_effect=[ConnectionResetError("reset"), (0x45, b"")],
    )
    @patch("espfm_device_test.time.sleep")
    def test_probe_exception_treated_as_failed_probe(self, mock_sleep, mock_request):
        """A ConnectionResetError during a probe is a failed probe, not a crash."""
        state = _make_state()
        self.assertTrue(wait_for_device(state, timeout_s=10))
        self.assertEqual(mock_request.call_count, 2)


# ============================================================
# run_config_import
# ============================================================


class TestRunConfigImport(unittest.TestCase):
    """Tests for run_config_import — POST /config re-imports the snapshot (R51/R50)."""

    def _pre_run_config(self):
        """Build a ConfigFile snapshot: 1 fan / 1 source / 1 curve / 0 schedules."""
        config = pb.ConfigFile()
        config.version = "3.0"
        fan = config.fans.fans.add()
        fan.id = 0
        fan.name = "gpu"
        fan.enabled = True
        fan.inverted = True
        fan.pwm_gpio = 22
        fan.tach_gpio = 23
        fan.source_id = 0
        fan.curve_id = 0
        fan.schedule_id = 0
        fan.group_id = 0
        src = config.sources.sources.add()
        src.id = 0
        src.name = "ntc"
        src.type = 0
        src.gpio = 34
        src.ds18b20_rom_code = 0
        curve = config.curves.curves.add()
        curve.id = 0
        curve.name = "gpu-temp"
        point = curve.points.add()
        point.temp_c = 30.0
        point.duty = 20
        return config

    @patch("espfm_device_test.do_request")
    @patch("espfm_device_test.time.sleep")
    def test_2_04_ok_and_device_returns_passes(self, mock_sleep, mock_request):
        st = pb.StatusResponse()
        st.ok = True
        info = pb.SystemInfo()
        info.version = "3.0"
        mock_request.side_effect = [
            (0x44, st.SerializeToString()),
            (0x45, info.SerializeToString()),
        ]

        state = _make_state()
        state.pre_run_config = self._pre_run_config()
        run_config_import(state)

        self.assertEqual(mock_request.call_count, 2)
        # First call is POST /config carrying an exact snapshot copy.
        call_args = mock_request.call_args_list[0][0]
        self.assertEqual(call_args[0], COAP_POST)
        self.assertEqual(call_args[1], "/config")
        req = call_args[2]
        self.assertEqual(req.version, "3.0")
        self.assertEqual(len(req.fans.fans), 1)
        self.assertEqual(len(req.sources.sources), 1)
        self.assertEqual(len(req.curves.curves), 1)
        self.assertEqual(len(req.schedules.schedules), 0)
        # Second call is the wait_for_device probe.
        self.assertEqual(mock_request.call_args_list[1][0][0], COAP_GET)
        self.assertEqual(mock_request.call_args_list[1][0][1], "/system/info")

        result = state.results[-1]
        self.assertEqual(result.label, "POST /config")
        self.assertEqual(result.surface, "POST /config")
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.expected_status, "2.04")
        self.assertEqual(result.actual_status, "2.04 Changed")
        self.assertIn("ConfigFile{version='3.0'", result.request_summary)
        self.assertIn("device returned after reboot", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_no_pre_run_config_marks_not_tested(self, mock_request):
        state = _make_state()  # pre_run_config defaults to None
        run_config_import(state)

        mock_request.assert_not_called()
        self.assertEqual(len(state.results), 1)
        result = state.results[-1]
        self.assertEqual(result.label, "POST /config")
        self.assertEqual(result.surface, "POST /config")
        self.assertEqual(result.verdict, "NOT TESTED")
        self.assertEqual(result.actual_status, "NOT TESTED")
        self.assertEqual(result.note, "no pre-run config snapshot")


# ============================================================
# run_system_reboot
# ============================================================


class TestRunSystemReboot(unittest.TestCase):
    """Tests for run_system_reboot — POST /system/reboot (R52/R53/R50)."""

    @patch("espfm_device_test.do_request")
    @patch("espfm_device_test.time.sleep")
    def test_2_04_ok_and_device_returns_passes(self, mock_sleep, mock_request):
        st = pb.StatusResponse()
        st.ok = True
        info = pb.SystemInfo()
        info.version = "3.0"
        mock_request.side_effect = [
            (0x44, st.SerializeToString()),
            (0x45, info.SerializeToString()),
        ]

        state = _make_state()
        run_system_reboot(state)

        self.assertEqual(mock_request.call_count, 2)
        call_args = mock_request.call_args_list[0][0]
        self.assertEqual(call_args[0], COAP_POST)
        self.assertEqual(call_args[1], "/system/reboot")
        self.assertEqual(mock_request.call_args_list[1][0][0], COAP_GET)
        self.assertEqual(mock_request.call_args_list[1][0][1], "/system/info")
        result = state.results[-1]
        self.assertEqual(result.label, "POST /system/reboot")
        self.assertEqual(result.surface, "POST /system/reboot")
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.expected_status, "2.04 or 5.03")
        self.assertEqual(result.actual_status, "2.04 Changed")
        self.assertIn("device returned", result.response_summary)

    @patch("espfm_device_test.do_request")
    @patch("espfm_device_test.time.sleep")
    def test_5_03_reboot_pending_passes(self, mock_sleep, mock_request):
        st = pb.StatusResponse()
        st.ok = False
        st.error_msg = "reboot pending"
        info = pb.SystemInfo()
        info.version = "3.0"
        mock_request.side_effect = [
            (0xA3, st.SerializeToString()),
            (0x45, info.SerializeToString()),
        ]

        state = _make_state()
        run_system_reboot(state)

        result = state.results[-1]
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.actual_status, "5.03")
        self.assertIn("error_msg='reboot pending'", result.response_summary)
        self.assertIn("device returned after pending reboot", result.response_summary)


# ============================================================
# run_config_verify
# ============================================================


class TestRunConfigVerify(unittest.TestCase):
    """Tests for run_config_verify — GET /config matches the snapshot (R55)."""

    def _pre_run_config(self):
        """Build a ConfigFile snapshot: 1 fan / 1 source / 1 curve / 0 schedules."""
        config = pb.ConfigFile()
        config.version = "3.0"
        fan = config.fans.fans.add()
        fan.id = 0
        fan.name = "gpu"
        fan.enabled = True
        fan.inverted = True
        fan.pwm_gpio = 22
        fan.tach_gpio = 23
        fan.source_id = 0
        fan.curve_id = 0
        fan.schedule_id = 0
        fan.group_id = 0
        src = config.sources.sources.add()
        src.id = 0
        src.name = "ntc"
        src.type = 0
        src.gpio = 34
        src.ds18b20_rom_code = 0
        curve = config.curves.curves.add()
        curve.id = 0
        curve.name = "gpu-temp"
        point = curve.points.add()
        point.temp_c = 30.0
        point.duty = 20
        return config

    @patch("espfm_device_test.do_request")
    def test_matching_config_passes(self, mock_request):
        live = self._pre_run_config()
        mock_request.return_value = (0x45, live.SerializeToString())

        state = _make_state()
        state.pre_run_config = self._pre_run_config()
        run_config_verify(state)

        mock_request.assert_called_once_with(COAP_GET, "/config")
        result = state.results[-1]
        self.assertEqual(result.label, "GET /config (verify)")
        self.assertEqual(result.surface, "GET /config (verify)")
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.expected_status, "2.05")
        self.assertEqual(result.actual_status, "2.05 Content")
        self.assertIn("fans=1", result.response_summary)
        self.assertIn("sources=1", result.response_summary)
        self.assertIn("curves=1", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_live_fields_not_compared_passes(self, mock_request):
        live = self._pre_run_config()
        live.fans.fans[0].rpm = 1200
        live.fans.fans[0].mode = 1
        live.fans.fans[0].duty = 80
        live.fans.fans[0].alarm = 1
        live.sources.sources[0].status = 1
        live.sources.sources[0].temp_c = 41.5
        mock_request.return_value = (0x45, live.SerializeToString())

        state = _make_state()
        state.pre_run_config = self._pre_run_config()
        run_config_verify(state)

        result = state.results[-1]
        self.assertEqual(result.verdict, "PASS")

    @patch("espfm_device_test.do_request")
    def test_different_fan_count_marks_fail(self, mock_request):
        live = self._pre_run_config()
        extra = live.fans.fans.add()
        extra.id = 1
        extra.name = "second"
        mock_request.return_value = (0x45, live.SerializeToString())

        state = _make_state()
        state.pre_run_config = self._pre_run_config()
        run_config_verify(state)

        result = state.results[-1]
        self.assertEqual(result.verdict, "FAIL")
        self.assertIn("content mismatch vs pre-run snapshot", result.response_summary)

    @patch("espfm_device_test.do_request")
    def test_no_pre_run_config_marks_not_tested(self, mock_request):
        state = _make_state()  # pre_run_config defaults to None
        run_config_verify(state)

        mock_request.assert_not_called()
        self.assertEqual(len(state.results), 1)
        result = state.results[-1]
        self.assertEqual(result.label, "GET /config (verify)")
        self.assertEqual(result.surface, "GET /config (verify)")
        self.assertEqual(result.verdict, "NOT TESTED")
        self.assertEqual(result.actual_status, "NOT TESTED")
        self.assertEqual(result.note, "no pre-run config snapshot")


# ============================================================
# run_wifi_connect
# ============================================================


class TestRunWifiConnect(unittest.TestCase):
    """Tests for run_wifi_connect — env credentials, 2.04/5.03/TIMEOUT valid (R54/R49)."""

    @patch("espfm_device_test.do_request")
    @patch.dict(
        os.environ, {"WIFI_SSID": "MyNet", "WIFI_PASSWORD": "secret"}, clear=True
    )
    def test_env_credentials_sent_and_2_04_passes(self, mock_request):
        st = pb.StatusResponse()
        st.ok = True
        mock_request.return_value = (0x44, st.SerializeToString())

        state = _make_state()
        buf = io.StringIO()
        with redirect_stdout(buf):
            run_wifi_connect(state)

        mock_request.assert_called_once()
        call_args = mock_request.call_args[0]
        self.assertEqual(call_args[0], COAP_POST)
        self.assertEqual(call_args[1], "/wifi/connect")
        req = call_args[2]
        self.assertEqual(req.ssid, "MyNet")
        self.assertEqual(req.password, "secret")
        result = state.results[-1]
        self.assertEqual(result.label, "POST /wifi/connect")
        self.assertEqual(result.surface, "POST /wifi/connect")
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.expected_status, "2.04 or 5.03 or TIMEOUT")
        self.assertEqual(result.actual_status, "2.04 Changed")
        self.assertIn("ssid='MyNet'", result.request_summary)
        self.assertIn("password='secret'", result.request_summary)
        # Credentials must never be printed to stdout — only in request_summary.
        self.assertNotIn("secret", buf.getvalue())
        self.assertNotIn("MyNet", buf.getvalue())

    @patch("espfm_device_test.do_request")
    @patch.dict(os.environ, {}, clear=True)
    def test_empty_credentials_5_03_passes(self, mock_request):
        st = pb.StatusResponse()
        st.ok = False
        st.error_msg = "set config fail"
        mock_request.return_value = (0xA3, st.SerializeToString())

        state = _make_state()
        run_wifi_connect(state)

        mock_request.assert_called_once()
        req = mock_request.call_args[0][2]
        self.assertEqual(req.ssid, "")
        self.assertEqual(req.password, "")
        result = state.results[-1]
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.actual_status, "5.03")
        self.assertIn("empty credentials", result.request_summary)

    @patch("espfm_device_test.do_request")
    @patch.dict(os.environ, {}, clear=True)
    def test_empty_credentials_timeout_passes(self, mock_request):
        mock_request.return_value = (None, None)

        state = _make_state()
        run_wifi_connect(state)

        result = state.results[-1]
        self.assertEqual(result.verdict, "PASS")
        self.assertEqual(result.actual_status, "TIMEOUT")
        self.assertIn("empty credentials", result.request_summary)

    @patch("espfm_device_test.do_request")
    @patch.dict(
        os.environ, {"WIFI_SSID": "MyNet", "WIFI_PASSWORD": "secret"}, clear=True
    )
    def test_other_code_marks_fail(self, mock_request):
        mock_request.return_value = (0x80, b"")  # 4.00

        state = _make_state()
        run_wifi_connect(state)

        result = state.results[-1]
        self.assertEqual(result.verdict, "FAIL")
        self.assertEqual(result.actual_status, "4.00 Bad Request")


# ============================================================
# render_body
# ============================================================


class TestRenderBody(unittest.TestCase):
    """Tests for render_body — groups results by surface into markdown sections."""

    def test_headings_in_first_seen_surface_order(self):
        """Two records on one surface plus one on another render each surface once."""
        state = _make_state()
        state.results = [
            _make_case("POST /fans"),
            _make_case("GET /fans/0"),
            _make_case("GET /fans/0"),
        ]

        lines = render_body(state)

        headings = [line for line in lines if line.startswith("### ")]
        self.assertEqual(headings, ["### POST /fans", "### GET /fans/0"])

    def test_bullets_include_all_record_fields(self):
        """A PASS record with a response summary renders the full bullet block."""
        state = _make_state()
        state.results = [
            _make_case("GET /fans/0", verdict="PASS", response_summary="id=0")
        ]

        body = "\n".join(render_body(state))

        self.assertIn("- Request: none", body)
        self.assertIn("- Expected: 2.05", body)
        self.assertIn("- Actual: 2.05 Content", body)
        self.assertIn("- Response: id=0", body)
        self.assertIn("- Verdict: PASS", body)

    def test_empty_response_summary_renders_none(self):
        """An empty response summary renders as 'none'."""
        state = _make_state()
        state.results = [_make_case("GET /fans/0", response_summary="")]

        body = "\n".join(render_body(state))

        self.assertIn("- Response: none", body)

    def test_all_pass_group_verdict_pass(self):
        """A surface where every record is PASS renders '**Endpoint result: PASS**'."""
        state = _make_state()
        state.results = [
            _make_case("GET /fans/0", verdict="PASS"),
            _make_case("GET /fans/0", verdict="PASS"),
        ]

        body = "\n".join(render_body(state))

        self.assertIn("- **Endpoint result: PASS**", body)

    def test_all_not_tested_group_verdict_not_tested(self):
        """A surface where every record is NOT TESTED renders NOT TESTED."""
        state = _make_state()
        state.results = [
            _make_case("GET /fans/0", verdict="NOT TESTED", note="skipped"),
            _make_case("GET /fans/0", verdict="NOT TESTED", note="skipped"),
        ]

        body = "\n".join(render_body(state))

        self.assertIn("- **Endpoint result: NOT TESTED**", body)

    def test_note_rendered_after_verdict(self):
        """A note on a record appears after the verdict text."""
        state = _make_state()
        state.results = [
            _make_case("GET /fans/0", verdict="NOT TESTED", note="no id")
        ]

        body = "\n".join(render_body(state))

        self.assertIn("- Verdict: NOT TESTED (no id)", body)

    def test_mixed_pass_fail_group_verdict_fail(self):
        """A surface with PASS and FAIL records renders FAIL."""
        state = _make_state()
        state.results = [
            _make_case("GET /fans/0", verdict="PASS"),
            _make_case("GET /fans/0", verdict="FAIL"),
        ]

        body = "\n".join(render_body(state))

        self.assertIn("- **Endpoint result: FAIL**", body)

    def test_mixed_pass_not_tested_group_verdict_fail(self):
        """A surface with PASS and NOT TESTED records renders FAIL (not PASS)."""
        state = _make_state()
        state.results = [
            _make_case("GET /fans/0", verdict="PASS"),
            _make_case("GET /fans/0", verdict="NOT TESTED"),
        ]

        body = "\n".join(render_body(state))

        self.assertIn("- **Endpoint result: FAIL**", body)


# ============================================================
# write_report
# ============================================================


class TestWriteReport(unittest.TestCase):
    """Tests for write_report — writes the full report document to output_path."""

    def test_writes_header_toc_and_rendered_body(self):
        """The report contains the title, Target IP, TOC, and rendered body."""
        info = pb.SystemInfo()
        info.hostname = "espfm-c425"
        info.version = "3.0"

        state = _make_state()
        state.system_info = info
        state.results = [
            _make_case("GET /fans/0", verdict="PASS", response_summary="id=0")
        ]

        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        state.output_path = os.path.join(tmp.name, "report.md")

        write_report(state)

        with open(state.output_path, "r", encoding="utf-8") as f:
            content = f.read()

        self.assertIn("# ESPFM Live-Device CoAP Integration Test Report", content)
        self.assertIn("- Target IP: `192.168.0.50`", content)
        self.assertIn("- Device hostname: `espfm-c425`", content)
        self.assertIn("- Firmware version: `3.0`", content)
        self.assertIn("## Endpoint Table of Contents", content)
        self.assertIn("## Per-Endpoint Results", content)
        self.assertIn("### GET /fans/0", content)
        self.assertIn("- **Endpoint result: PASS**", content)


if __name__ == "__main__":
    unittest.main()
