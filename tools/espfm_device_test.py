#!/usr/bin/env python3
"""
ESPFM Live-Device CoAP Integration Test — drives every CoAP endpoint on a live
ESPFM device and writes a markdown report of the results.

Usage:
    python espfm_device_test.py [HOST [PORT]]
    python espfm_device_test.py --host 192.168.0.28 --port 5683 --output tools/espfm_device_test_report.md

Dependencies:
    - protobuf: required for the generated espfm_pb2 message classes
      (install with `pip install protobuf`)
    - repo tools/ modules: espfm_shell (CoAPTransport, COAP_CODES, CoapError,
      COAP_GET/POST/PUT/DELETE) and espfm_pb2 (generated protobuf messages)
    - aiocoap: NOT required - the script uses the repo's raw-UDP CoAPTransport
    - python-dotenv: NOT required - WiFi credentials are loaded from the
      repo-root .env by the built-in loader in load_env_file()
"""

from __future__ import annotations

import argparse
import datetime
import os
import socket
import sys
import time

# Make tools/ importable so espfm_pb2 and espfm_shell can be imported directly.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import espfm_pb2 as pb
from espfm_shell import (
    CoAPTransport,
    COAP_CODES,
    CoapError,
    COAP_GET,
    COAP_POST,
    COAP_PUT,
    COAP_DELETE,
)

# ============================================================
# Constants
# ============================================================

DEFAULT_HOST = "192.168.0.28"
DEFAULT_PORT = 5683
DEFAULT_OUTPUT = "tools/espfm_device_test_report.md"

# GPIOs reserved by the ESP32 chip (UART console, flash SPI, PSRAM SPI) —
# the same table f_gpio.c stamps into the registry as 0xFFFFFFFF. Free-pin
# discovery must skip these or POST /fans / POST /ds18b20/config hit a
# "GPIO is reserved" 4.00 (f_gpio_claim rejects them).
RESERVED_PINS = {1, 3, 6, 7, 8, 9, 10, 11, 16, 17}

# Module-visible CoAP transport, created in main() after the reachability probe.
transport: CoAPTransport | None = None


def load_env_file(path: str | None = None) -> None:
    """Load KEY=VALUE pairs from a .env file into the process environment.

    When path is None, use the repo root's .env (two directories above this
    file). A missing or unreadable file is a no-op. Blank lines, lines
    starting with '#', and lines without '=' are skipped. Keys and values are
    stripped; one matching pair of surrounding double quotes is removed from
    the value. No third-party dependency (python-dotenv is not used).
    """
    if path is None:
        path = os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))), ".env"
        )
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                stripped = line.strip()
                if not stripped or stripped.startswith("#"):
                    continue
                if "=" not in stripped:
                    continue
                key, _, value = stripped.partition("=")
                key = key.strip()
                if not key:
                    continue
                value = value.strip()
                if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
                    value = value[1:-1]
                os.environ[key] = value
    except (OSError, UnicodeDecodeError):
        return


def code_str(code: int | None) -> str:
    """Format a CoAP status code for display."""
    if code is None:
        return "TIMEOUT"
    return COAP_CODES.get(code, f"{code >> 5}.{code & 0x1F:02d}")


class CaseResult:
    """One captured CoAP test case."""

    def __init__(
        self,
        label: str,
        surface: str,
        method: int,
        path: str,
        request_summary: str,
        expected_status: str,
        actual_status: str,
        response_summary: str,
        verdict: str,
        note: str | None = None,
    ) -> None:
        self.label = label
        self.surface = surface
        self.method = method
        self.path = path
        self.request_summary = request_summary
        self.expected_status = expected_status
        self.actual_status = actual_status
        self.response_summary = response_summary
        self.verdict = verdict
        self.note = note


class RunState:
    """Mutable state threaded through every run function."""

    def __init__(self, host: str, port: int, output_path: str) -> None:
        self.host = host
        self.port = port
        self.output_path = output_path
        self.system_info: pb.SystemInfo | None = None
        self.fan_list: pb.FanList | None = None
        self.source_list: pb.SourceList | None = None
        self.curve_list: pb.CurveList | None = None
        self.schedule_list: pb.ScheduleList | None = None
        self.live_fan_id: int = 0
        self.manual_source_id: int | None = None
        self.first_curve_id: int | None = None
        self.created_fan_id: int | None = None
        self.created_source_id: int | None = None
        self.created_curve_id: int | None = None
        self.created_schedule_id: int | None = None
        self.free_fan_pwm: int | None = None
        self.free_fan_tach: int | None = None
        self.free_ds_pin: int | None = None
        self.pre_run_config: pb.ConfigFile | None = None
        self.control_originals: pb.ControlConfig | None = None
        self.original_hostname: str | None = None
        self.results: list[CaseResult] = []


def probe_device(host: str, port: int) -> bool:
    """Probe reachability by sending GET /system/info twice (2s timeout)."""
    probe = CoAPTransport(host, port, timeout=2)
    probe.connect()
    try:
        for _ in range(2):
            code, _ = probe.request(COAP_GET, "/system/info")
            if code is not None:
                return True
        return False
    except Exception:
        return False
    finally:
        probe.close()


def do_request(
    method: int, path: str, msg: object | None = None, timeout: float | None = None
) -> tuple[int | None, bytes | None]:
    """Send a CoAP request, returning (code, payload_bytes).

    Never raises on non-2.x codes. Returns (None, None) on timeout.
    When timeout is given, temporarily extends the socket timeout.
    """
    assert transport is not None, "do_request called before transport created"
    payload = msg.SerializeToString() if msg is not None else b""
    if timeout is not None:
        sock = transport._sock
        old_sock_timeout = sock.gettimeout()
        old_transport_timeout = transport.timeout
        sock.settimeout(timeout)
        transport.timeout = timeout
        try:
            return transport.request(method, path, payload)
        finally:
            sock.settimeout(old_sock_timeout)
            transport.timeout = old_transport_timeout
    return transport.request(method, path, payload)


def run_system_info(state: RunState) -> None:
    """Capture GET /system/info identity into state."""
    code, payload = do_request(COAP_GET, "/system/info")
    if code == 0x45:
        try:
            info = pb.SystemInfo()
            info.ParseFromString(payload)
        except Exception as exc:
            state.results.append(
                CaseResult(
                    label="GET /system/info",
                    surface="GET /system/info",
                    method=COAP_GET,
                    path="/system/info",
                    request_summary="none",
                    expected_status="2.05",
                    actual_status=code_str(code),
                    response_summary=f"decode error: {exc}",
                    verdict="FAIL",
                )
            )
            return
        state.system_info = info
        state.original_hostname = info.hostname
        summary = (
            f"version={info.version}, uptime_s={info.uptime_s}, "
            f"heap_free={info.heap_free}, fan_count={info.fan_count}, "
            f"source_count={info.source_count}, curve_count={info.curve_count}, "
            f"schedule_count={info.schedule_count}, hostname={info.hostname}"
        )
        verdict = "PASS"
    else:
        if code is None:
            summary = "TIMEOUT"
        else:
            summary = payload.hex() if payload else "(empty payload)"
        verdict = "FAIL"
    state.results.append(
        CaseResult(
            label="GET /system/info",
            surface="GET /system/info",
            method=COAP_GET,
            path="/system/info",
            request_summary="none",
            expected_status="2.05",
            actual_status=code_str(code),
            response_summary=summary,
            verdict=verdict,
        )
    )


def _fan_summary(fan: pb.FanInfo) -> str:
    """One-line summary of a FanInfo message."""
    return (
        f"id={fan.id}, name={fan.name!r}, mode={fan.mode}, duty={fan.duty}, "
        f"rpm={fan.rpm}, pwm_gpio={fan.pwm_gpio}, tach_gpio={fan.tach_gpio}, "
        f"source_id={fan.source_id}, curve_id={fan.curve_id}, "
        f"schedule_id={fan.schedule_id}, group_id={fan.group_id}, "
        f"enabled={fan.enabled}, alarm={fan.alarm}"
    )


def run_fans_list(state: RunState) -> None:
    """Capture GET /fans as a decoded FanList into state.fan_list."""
    code, payload = do_request(COAP_GET, "/fans")
    if code == 0x45:
        try:
            fan_list = pb.FanList()
            fan_list.ParseFromString(payload)
        except Exception as exc:
            state.results.append(
                CaseResult(
                    label="GET /fans",
                    surface="GET /fans",
                    method=COAP_GET,
                    path="/fans",
                    request_summary="none",
                    expected_status="2.05",
                    actual_status=code_str(code),
                    response_summary=f"decode error: {exc}",
                    verdict="FAIL",
                )
            )
            return
        state.fan_list = fan_list
        summary = "; ".join(f"[{_fan_summary(f)}]" for f in fan_list.fans)
        verdict = "PASS"
    else:
        if code is None:
            summary = "TIMEOUT"
        else:
            summary = payload.hex() if payload else "(empty payload)"
        verdict = "FAIL"
    state.results.append(
        CaseResult(
            label="GET /fans",
            surface="GET /fans",
            method=COAP_GET,
            path="/fans",
            request_summary="none",
            expected_status="2.05",
            actual_status=code_str(code),
            response_summary=summary,
            verdict=verdict,
        )
    )


def discover_live_fan(state: RunState) -> None:
    """Select the live fan: first fan with pwm_gpio==22 and tach_gpio==23.

    Falls back to fan id 0 (R11) and prints the actual fan list when no
    pwm22/tach23 fan is present.
    """
    found = None
    if state.fan_list is not None:
        for fan in state.fan_list.fans:
            if fan.pwm_gpio == 22 and fan.tach_gpio == 23:
                found = fan
                break
    if found is not None:
        state.live_fan_id = found.id
        state.results.append(
            CaseResult(
                label="discover_live_fan",
                surface="GET /fans",
                method=COAP_GET,
                path="/fans",
                request_summary="none",
                expected_status="2.05",
                actual_status="2.05",
                response_summary=_fan_summary(found),
                verdict="PASS",
            )
        )
        return
    # No pwm22/tach23 fan — print the actual list and use the id-0 fallback.
    lines: list[str] = []
    if state.fan_list is not None:
        for fan in state.fan_list.fans:
            line = (
                f"id={fan.id}, name={fan.name!r}, "
                f"pwm_gpio={fan.pwm_gpio}, tach_gpio={fan.tach_gpio}"
            )
            lines.append(line)
            print(f"discover_live_fan: fan {line}")
    else:
        print("discover_live_fan: no fan list available (GET /fans failed)")
    state.live_fan_id = 0
    state.results.append(
        CaseResult(
            label="discover_live_fan",
            surface="GET /fans",
            method=COAP_GET,
            path="/fans",
            request_summary="none",
            expected_status="n/a",
            actual_status="n/a",
            response_summary="; ".join(lines) if lines else "(empty fan list)",
            verdict="PASS",
            note="no pwm22/tach23 fan found; using fan id 0 fallback (R11)",
        )
    )


def _source_summary(src: pb.SourceInfo) -> str:
    """One-line summary of a SourceInfo message."""
    return (
        f"id={src.id}, name={src.name!r}, type={src.type}, status={src.status}, "
        f"temp_c={src.temp_c}, gpio={src.gpio}"
    )


def run_sources_list(state: RunState) -> None:
    """Capture GET /sources as a decoded SourceList into state.source_list.

    Also sets state.manual_source_id to the id of the first
    SOURCE_TYPE_MANUAL source (live: 0); if none exists it stays at the
    default and phase 4 handles the fallback.
    """
    code, payload = do_request(COAP_GET, "/sources")
    if code == 0x45:
        try:
            source_list = pb.SourceList()
            source_list.ParseFromString(payload)
        except Exception as exc:
            state.results.append(
                CaseResult(
                    label="GET /sources",
                    surface="GET /sources",
                    method=COAP_GET,
                    path="/sources",
                    request_summary="none",
                    expected_status="2.05",
                    actual_status=code_str(code),
                    response_summary=f"decode error: {exc}",
                    verdict="FAIL",
                )
            )
            return
        state.source_list = source_list
        summary = "; ".join(f"[{_source_summary(s)}]" for s in source_list.sources)
        for src in source_list.sources:
            if src.type == pb.SOURCE_TYPE_MANUAL:
                state.manual_source_id = src.id
                break
        verdict = "PASS"
    else:
        if code is None:
            summary = "TIMEOUT"
        else:
            summary = payload.hex() if payload else "(empty payload)"
        verdict = "FAIL"
    state.results.append(
        CaseResult(
            label="GET /sources",
            surface="GET /sources",
            method=COAP_GET,
            path="/sources",
            request_summary="none",
            expected_status="2.05",
            actual_status=code_str(code),
            response_summary=summary,
            verdict=verdict,
        )
    )


def _curve_summary(curve: pb.CurveInfo) -> str:
    """One-line summary of a CurveInfo message (id, name, point count)."""
    return f"id={curve.id}, name={curve.name!r}, points={len(curve.points)}"


def run_curves_list(state: RunState) -> None:
    """Capture GET /curves as a decoded CurveList into state.curve_list.

    Also sets state.first_curve_id to the id of the first curve (live: 0).
    """
    code, payload = do_request(COAP_GET, "/curves")
    if code == 0x45:
        try:
            curve_list = pb.CurveList()
            curve_list.ParseFromString(payload)
        except Exception as exc:
            state.results.append(
                CaseResult(
                    label="GET /curves",
                    surface="GET /curves",
                    method=COAP_GET,
                    path="/curves",
                    request_summary="none",
                    expected_status="2.05",
                    actual_status=code_str(code),
                    response_summary=f"decode error: {exc}",
                    verdict="FAIL",
                )
            )
            return
        state.curve_list = curve_list
        summary = "; ".join(f"[{_curve_summary(c)}]" for c in curve_list.curves)
        if curve_list.curves:
            state.first_curve_id = curve_list.curves[0].id
        verdict = "PASS"
    else:
        if code is None:
            summary = "TIMEOUT"
        else:
            summary = payload.hex() if payload else "(empty payload)"
        verdict = "FAIL"
    state.results.append(
        CaseResult(
            label="GET /curves",
            surface="GET /curves",
            method=COAP_GET,
            path="/curves",
            request_summary="none",
            expected_status="2.05",
            actual_status=code_str(code),
            response_summary=summary,
            verdict=verdict,
        )
    )


def _schedule_summary(sched: pb.ScheduleInfo) -> str:
    """One-line summary of a ScheduleInfo message."""
    return (
        f"id={sched.id}, fan_id={sched.fan_id}, duty={sched.duty}, "
        f"start_min={sched.start_min}, end_min={sched.end_min}, "
        f"enabled={sched.enabled}, name={sched.name!r}"
    )


def run_schedules_list(state: RunState) -> None:
    """Capture GET /schedules as a decoded ScheduleList into state.schedule_list."""
    code, payload = do_request(COAP_GET, "/schedules")
    if code == 0x45:
        try:
            schedule_list = pb.ScheduleList()
            schedule_list.ParseFromString(payload)
        except Exception as exc:
            state.results.append(
                CaseResult(
                    label="GET /schedules",
                    surface="GET /schedules",
                    method=COAP_GET,
                    path="/schedules",
                    request_summary="none",
                    expected_status="2.05",
                    actual_status=code_str(code),
                    response_summary=f"decode error: {exc}",
                    verdict="FAIL",
                )
            )
            return
        state.schedule_list = schedule_list
        summary = "; ".join(
            f"[{_schedule_summary(s)}]" for s in schedule_list.schedules
        )
        verdict = "PASS"
    else:
        if code is None:
            summary = "TIMEOUT"
        else:
            summary = payload.hex() if payload else "(empty payload)"
        verdict = "FAIL"
    state.results.append(
        CaseResult(
            label="GET /schedules",
            surface="GET /schedules",
            method=COAP_GET,
            path="/schedules",
            request_summary="none",
            expected_status="2.05",
            actual_status=code_str(code),
            response_summary=summary,
            verdict=verdict,
        )
    )


def run_config_snapshot(state: RunState) -> None:
    """Capture GET /config as a decoded ConfigFile into state.pre_run_config.

    The payload is a single CoAP datagram on this device; no Block2
    handling is implemented.
    """
    code, payload = do_request(COAP_GET, "/config")
    if code == 0x45:
        try:
            config = pb.ConfigFile()
            config.ParseFromString(payload)
        except Exception as exc:
            state.results.append(
                CaseResult(
                    label="GET /config",
                    surface="GET /config",
                    method=COAP_GET,
                    path="/config",
                    request_summary="none",
                    expected_status="2.05",
                    actual_status=code_str(code),
                    response_summary=f"decode error: {exc}",
                    verdict="FAIL",
                )
            )
            return
        state.pre_run_config = config
        summary = (
            f"version={config.version!r}, fans={len(config.fans.fans)}, "
            f"sources={len(config.sources.sources)}, "
            f"curves={len(config.curves.curves)}, "
            f"schedules={len(config.schedules.schedules)}"
        )
        verdict = "PASS"
    else:
        if code is None:
            summary = "TIMEOUT"
        else:
            summary = payload.hex() if payload else "(empty payload)"
        verdict = "FAIL"
    state.results.append(
        CaseResult(
            label="GET /config",
            surface="GET /config",
            method=COAP_GET,
            path="/config",
            request_summary="none",
            expected_status="2.05",
            actual_status=code_str(code),
            response_summary=summary,
            verdict=verdict,
        )
    )


def run_wifi_status(state: RunState) -> None:
    """Capture GET /wifi/status as a decoded WifiStatus (R16)."""
    code, payload = do_request(COAP_GET, "/wifi/status")
    if code == 0x45:
        try:
            status = pb.WifiStatus()
            status.ParseFromString(payload)
        except Exception as exc:
            state.results.append(
                CaseResult(
                    label="GET /wifi/status",
                    surface="GET /wifi/status",
                    method=COAP_GET,
                    path="/wifi/status",
                    request_summary="none",
                    expected_status="2.05",
                    actual_status=code_str(code),
                    response_summary=f"decode error: {exc}",
                    verdict="FAIL",
                )
            )
            return
        summary = (
            f"sta_connected={status.sta_connected}, "
            f"sta_ip={status.sta_ip!r}, ap_ip={status.ap_ip!r}"
        )
        verdict = "PASS"
    else:
        if code is None:
            summary = "TIMEOUT"
        else:
            summary = payload.hex() if payload else "(empty payload)"
        verdict = "FAIL"
    state.results.append(
        CaseResult(
            label="GET /wifi/status",
            surface="GET /wifi/status",
            method=COAP_GET,
            path="/wifi/status",
            request_summary="none",
            expected_status="2.05",
            actual_status=code_str(code),
            response_summary=summary,
            verdict=verdict,
        )
    )


def run_wifi_scan(state: RunState) -> None:
    """Capture GET /wifi/scan with a raised 10 s transport timeout (R17).

    The scan blocks ~3.5 s on device; timeout=10 mirrors the
    ESPFMClient.wifi_scan() pattern in espfm_shell.py.
    """
    code, payload = do_request(COAP_GET, "/wifi/scan", timeout=10)
    if code == 0x45:
        try:
            scan_result = pb.WifiScanResult()
            scan_result.ParseFromString(payload)
        except Exception as exc:
            state.results.append(
                CaseResult(
                    label="GET /wifi/scan",
                    surface="GET /wifi/scan",
                    method=COAP_GET,
                    path="/wifi/scan",
                    request_summary="none",
                    expected_status="2.05",
                    actual_status=code_str(code),
                    response_summary=f"decode error: {exc}",
                    verdict="FAIL",
                )
            )
            return
        aps = [
            f"[ssid={ap.ssid!r}, rssi={ap.rssi}, channel={ap.channel}, "
            f"authmode={ap.authmode}]"
            for ap in scan_result.aps
        ]
        summary = "; ".join(aps) if aps else "(empty AP list)"
        verdict = "PASS"
    else:
        if code is None:
            summary = "TIMEOUT (no response within 10 s)"
        else:
            summary = payload.hex() if payload else "(empty payload)"
        verdict = "FAIL"
    state.results.append(
        CaseResult(
            label="GET /wifi/scan",
            surface="GET /wifi/scan",
            method=COAP_GET,
            path="/wifi/scan",
            request_summary="none",
            expected_status="2.05",
            actual_status=code_str(code),
            response_summary=summary,
            verdict=verdict,
        )
    )


def run_ds18b20_scan(state: RunState) -> None:
    """Capture GET /ds18b20/scan as a decoded Ds18b20ScanResponse (R18).

    A 5.03 (0xA3) response — no DS18B20 bus configured on the live device —
    is recorded as a valid outcome (R19).
    """
    code, payload = do_request(COAP_GET, "/ds18b20/scan")
    actual = code_str(code)
    if code == 0x45:
        try:
            scan = pb.Ds18b20ScanResponse()
            scan.ParseFromString(payload)
        except Exception as exc:
            state.results.append(
                CaseResult(
                    label="GET /ds18b20/scan",
                    surface="GET /ds18b20/scan",
                    method=COAP_GET,
                    path="/ds18b20/scan",
                    request_summary="none",
                    expected_status="2.05 or 5.03",
                    actual_status=actual,
                    response_summary=f"decode error: {exc}",
                    verdict="FAIL",
                )
            )
            return
        devices = [
            f"[index={dev.index}, rom_code={dev.rom_code}, temp_c={dev.temp_c}]"
            for dev in scan.devices
        ]
        summary = f"device_count={scan.device_count}"
        if devices:
            summary += "; " + "; ".join(devices)
        verdict = "PASS"
    elif code == 0xA3:
        summary = "5.03 Service Unavailable (no DS18B20 bus)"
        actual = "5.03"
        verdict = "PASS"
    else:
        if code is None:
            summary = "TIMEOUT"
        else:
            summary = payload.hex() if payload else "(empty payload)"
        verdict = "FAIL"
    state.results.append(
        CaseResult(
            label="GET /ds18b20/scan",
            surface="GET /ds18b20/scan",
            method=COAP_GET,
            path="/ds18b20/scan",
            request_summary="none",
            expected_status="2.05 or 5.03",
            actual_status=actual,
            response_summary=summary,
            verdict=verdict,
        )
    )


def run_control_originals(state: RunState) -> None:
    """Capture GET /control as a decoded ControlConfig (R20).

    The decoded object is stored on state.control_originals so later phases
    can restore the pre-run tunables. A 5.03 (control unset) is a FAIL
    because the originals cannot be captured.
    """
    code, payload = do_request(COAP_GET, "/control")
    actual = code_str(code)
    if code == 0x45:
        try:
            control = pb.ControlConfig()
            control.ParseFromString(payload)
        except Exception as exc:
            state.results.append(
                CaseResult(
                    label="GET /control",
                    surface="GET /control",
                    method=COAP_GET,
                    path="/control",
                    request_summary="none",
                    expected_status="2.05",
                    actual_status=actual,
                    response_summary=f"decode error: {exc}",
                    verdict="FAIL",
                )
            )
            return
        state.control_originals = control
        summary = (
            f"hysteresis={control.hysteresis}, ramp_up={control.ramp_up}, "
            f"ramp_down={control.ramp_down}, "
            f"failsafe_policy={control.failsafe_policy}, "
            f"safe_duty={control.safe_duty}"
        )
        verdict = "PASS"
    elif code == 0xA3:
        summary = "5.03 control unset"
        actual = "5.03"
        verdict = "FAIL"
    else:
        if code is None:
            summary = "TIMEOUT"
        else:
            summary = payload.hex() if payload else "(empty payload)"
        verdict = "FAIL"
    state.results.append(
        CaseResult(
            label="GET /control",
            surface="GET /control",
            method=COAP_GET,
            path="/control",
            request_summary="none",
            expected_status="2.05",
            actual_status=actual,
            response_summary=summary,
            verdict=verdict,
        )
    )


def discover_free_pins(state: RunState) -> None:
    """Select free GPIO pins for the test fan and the DS18B20 bus (R21).

    Collects every in-use pin from fan_list (pwm_gpio, tach_gpio) and
    source_list (gpio), excluding 255 ("none"), plus the chip-reserved pins
    (UART/flash/PSRAM) that f_gpio_claim rejects. Picks the first free pwm pin
    from 1..40, its first free tach pin (255 when no second pin is free), and
    a separate free pin for the DS18B20 bus (None when none is free).
    """
    used: set[int] = set(RESERVED_PINS)
    if state.fan_list is not None:
        for fan in state.fan_list.fans:
            if fan.pwm_gpio != 255:
                used.add(fan.pwm_gpio)
            if fan.tach_gpio != 255:
                used.add(fan.tach_gpio)
    if state.source_list is not None:
        for src in state.source_list.sources:
            if src.gpio != 255:
                used.add(src.gpio)

    free_fan_pwm: int | None = None
    free_fan_tach: int | None = None
    for pwm in range(1, 41):
        if pwm in used:
            continue
        tach: int | None = None
        for cand in range(1, 41):
            if cand in used or cand == pwm:
                continue
            tach = cand
            break
        free_fan_pwm = pwm
        free_fan_tach = tach if tach is not None else 255
        break
    state.free_fan_pwm = free_fan_pwm
    state.free_fan_tach = free_fan_tach

    free_ds_pin: int | None = None
    for pin in range(1, 41):
        if pin in used:
            continue
        if pin == free_fan_pwm:
            continue
        if free_fan_tach != 255 and pin == free_fan_tach:
            continue
        free_ds_pin = pin
        break
    state.free_ds_pin = free_ds_pin

    # Report only config-in-use pins (not the reserved table) in the summary.
    used_config = {p for p in used if p not in RESERVED_PINS}
    used_summary = ", ".join(str(p) for p in sorted(used_config)) if used_config else "(none)"
    state.results.append(
        CaseResult(
            label="discover_free_pins",
            surface="GET /fans + GET /sources",
            method=COAP_GET,
            path="/fans + /sources",
            request_summary="none",
            expected_status="n/a",
            actual_status="n/a",
            response_summary=(
                f"used_gpio=[{used_summary}], "
                f"free_fan_pwm={free_fan_pwm}, free_fan_tach={free_fan_tach}, "
                f"free_ds_pin={free_ds_pin}"
            ),
            verdict="PASS",
        )
    )


def _status_summary(payload: bytes | None) -> str:
    """Decode a StatusResponse payload into ok/error_code/error_msg, else hex."""
    if not payload:
        return "(empty payload)"
    try:
        st = pb.StatusResponse()
        st.ParseFromString(payload)
        return f"ok={st.ok}, error_code={st.error_code}, error_msg={st.error_msg!r}"
    except Exception:
        return payload.hex()


def run_create_fan(state: RunState) -> None:
    """Create the test fan on the discovered free pwm/tach pair (R22/R23/R24)."""
    if state.free_fan_pwm is None:
        state.results.append(
            CaseResult(
                label="POST /fans",
                surface="POST /fans",
                method=COAP_POST,
                path="/fans",
                request_summary="none",
                expected_status="2.01",
                actual_status="NOT TESTED",
                response_summary="(skipped)",
                verdict="NOT TESTED",
                note="no free pwm/tach pair available",
            )
        )
        return
    req = pb.FanCreateRequest(
        pwm_gpio=state.free_fan_pwm,
        tach_gpio=state.free_fan_tach,
        name="test-fan",
    )
    req_summary = (
        f"FanCreateRequest{{pwm_gpio={req.pwm_gpio}, "
        f"tach_gpio={req.tach_gpio}, name={req.name!r}}}"
    )
    code, payload = do_request(COAP_POST, "/fans", req)
    summary = ""
    actual = code_str(code)
    verdict = "FAIL"
    if code == 0x41:
        try:
            fan = pb.FanInfo()
            fan.ParseFromString(payload)
        except Exception as exc:
            summary = f"decode error: {exc}"
        else:
            state.created_fan_id = fan.id
            summary = _fan_summary(fan)
            verdict = "PASS"
    elif code == 0x80:
        actual = "4.00"
        summary = _status_summary(payload)
    elif code is None:
        actual = "TIMEOUT"
        summary = "TIMEOUT"
    else:
        summary = payload.hex() if payload else "(empty payload)"
    state.results.append(
        CaseResult(
            label="POST /fans",
            surface="POST /fans",
            method=COAP_POST,
            path="/fans",
            request_summary=req_summary,
            expected_status="2.01",
            actual_status=actual,
            response_summary=summary,
            verdict=verdict,
        )
    )


def run_create_source(state: RunState) -> None:
    """Create the test manual source with gpio=255 (R25)."""
    req = pb.SourceCreateRequest(
        type=pb.SOURCE_TYPE_MANUAL, name="test-source", gpio=255
    )
    req_summary = (
        f"SourceCreateRequest{{type={req.type} (SOURCE_TYPE_MANUAL), "
        f"name={req.name!r}, gpio={req.gpio}}}"
    )
    code, payload = do_request(COAP_POST, "/sources", req)
    summary = ""
    actual = code_str(code)
    verdict = "FAIL"
    if code == 0x41:
        try:
            src = pb.SourceInfo()
            src.ParseFromString(payload)
        except Exception as exc:
            summary = f"decode error: {exc}"
        else:
            state.created_source_id = src.id
            summary = _source_summary(src)
            verdict = "PASS"
    elif code == 0x80:
        summary = _status_summary(payload)
    elif code is None:
        actual = "TIMEOUT"
        summary = "TIMEOUT"
    else:
        summary = payload.hex() if payload else "(empty payload)"
    state.results.append(
        CaseResult(
            label="POST /sources",
            surface="POST /sources",
            method=COAP_POST,
            path="/sources",
            request_summary=req_summary,
            expected_status="2.01",
            actual_status=actual,
            response_summary=summary,
            verdict=verdict,
        )
    )


def run_create_curve(state: RunState) -> None:
    """Create the test curve with 5 points (R26)."""
    req = pb.CurveCreateRequest(name="test-curve")
    for temp_c, duty in ((25.0, 10), (35.0, 30), (45.0, 50), (55.0, 70), (65.0, 90)):
        point = req.points.add()
        point.temp_c = temp_c
        point.duty = duty
    req_summary = f"CurveCreateRequest{{name={req.name!r}, points={len(req.points)}}}"
    code, payload = do_request(COAP_POST, "/curves", req)
    summary = ""
    actual = code_str(code)
    verdict = "FAIL"
    if code == 0x41:
        try:
            curve = pb.CurveInfo()
            curve.ParseFromString(payload)
        except Exception as exc:
            summary = f"decode error: {exc}"
        else:
            state.created_curve_id = curve.id
            summary = _curve_summary(curve)
            verdict = "PASS"
    elif code == 0x80:
        summary = _status_summary(payload)
    elif code is None:
        actual = "TIMEOUT"
        summary = "TIMEOUT"
    else:
        summary = payload.hex() if payload else "(empty payload)"
    state.results.append(
        CaseResult(
            label="POST /curves",
            surface="POST /curves",
            method=COAP_POST,
            path="/curves",
            request_summary=req_summary,
            expected_status="2.01",
            actual_status=actual,
            response_summary=summary,
            verdict=verdict,
        )
    )


def run_create_schedule(state: RunState) -> None:
    """Create the test schedule on the live fan (R27)."""
    req = pb.ScheduleCreateRequest(
        fan_id=state.live_fan_id,
        duty=50,
        start_min=600,
        end_min=1080,
        enabled=True,
        name="test-schedule",
    )
    req_summary = (
        f"ScheduleCreateRequest{{fan_id={req.fan_id}, duty={req.duty}, "
        f"start_min={req.start_min}, end_min={req.end_min}, "
        f"enabled={req.enabled}, name={req.name!r}}}"
    )
    code, payload = do_request(COAP_POST, "/schedules", req)
    summary = ""
    actual = code_str(code)
    verdict = "FAIL"
    if code == 0x41:
        try:
            sched = pb.ScheduleInfo()
            sched.ParseFromString(payload)
        except Exception as exc:
            summary = f"decode error: {exc}"
        else:
            state.created_schedule_id = sched.id
            summary = _schedule_summary(sched)
            verdict = "PASS"
    elif code == 0x80:
        summary = _status_summary(payload)
    elif code is None:
        actual = "TIMEOUT"
        summary = "TIMEOUT"
    else:
        summary = payload.hex() if payload else "(empty payload)"
    state.results.append(
        CaseResult(
            label="POST /schedules",
            surface="POST /schedules",
            method=COAP_POST,
            path="/schedules",
            request_summary=req_summary,
            expected_status="2.01",
            actual_status=actual,
            response_summary=summary,
            verdict=verdict,
        )
    )


def run_manual_temp(state: RunState) -> None:
    """Set the manual source temperature (R28).

    Uses the live manual source id when one was discovered, else the manual
    source created in run_create_source, else records NOT TESTED.
    """
    manual_id: int | None = None
    if state.source_list is not None:
        for src in state.source_list.sources:
            if src.type == pb.SOURCE_TYPE_MANUAL:
                manual_id = state.manual_source_id
                break
    if manual_id is None and state.created_source_id is not None:
        manual_id = state.created_source_id
    if manual_id is None:
        state.results.append(
            CaseResult(
                label="POST /sources/temp",
                surface="POST /sources/temp",
                method=COAP_POST,
                path="/sources/temp",
                request_summary="none",
                expected_status="2.04",
                actual_status="NOT TESTED",
                response_summary="(skipped)",
                verdict="NOT TESTED",
                note="no manual source id available",
            )
        )
        return
    req = pb.ManualTempRequest(id=manual_id, temp_c=20.0)
    req_summary = f"ManualTempRequest{{id={req.id}, temp_c={req.temp_c}}}"
    code, payload = do_request(COAP_POST, "/sources/temp", req)
    summary = ""
    actual = code_str(code)
    verdict = "FAIL"
    if code == 0x44:
        try:
            st = pb.StatusResponse()
            st.ParseFromString(payload)
        except Exception as exc:
            summary = f"decode error: {exc}"
        else:
            summary = f"ok={st.ok}"
            if st.error_msg:
                summary += f", error_msg={st.error_msg!r}"
            verdict = "PASS" if st.ok else "FAIL"
    elif code in (0x84, 0x80):
        summary = _status_summary(payload)
    elif code is None:
        actual = "TIMEOUT"
        summary = "TIMEOUT"
    else:
        summary = payload.hex() if payload else "(empty payload)"
    state.results.append(
        CaseResult(
            label="POST /sources/temp",
            surface="POST /sources/temp",
            method=COAP_POST,
            path="/sources/temp",
            request_summary=req_summary,
            expected_status="2.04",
            actual_status=actual,
            response_summary=summary,
            verdict=verdict,
        )
    )


def run_ds18b20_config(state: RunState) -> None:
    """Configure the DS18B20 bus on the free DS pin (R29/R30)."""
    if state.free_ds_pin is None:
        state.results.append(
            CaseResult(
                label="POST /ds18b20/config",
                surface="POST /ds18b20/config",
                method=COAP_POST,
                path="/ds18b20/config",
                request_summary="none",
                expected_status="2.04",
                actual_status="NOT TESTED",
                response_summary="(skipped)",
                verdict="NOT TESTED",
                note="no free GPIO pin available",
            )
        )
        return
    req = pb.Ds18b20ConfigRequest(gpio=state.free_ds_pin)
    req_summary = f"Ds18b20ConfigRequest{{gpio={req.gpio}}}"
    code, payload = do_request(COAP_POST, "/ds18b20/config", req)
    summary = ""
    actual = code_str(code)
    verdict = "FAIL"
    if code == 0x44:
        try:
            st = pb.StatusResponse()
            st.ParseFromString(payload)
        except Exception as exc:
            summary = f"decode error: {exc}"
        else:
            summary = f"ok={st.ok}"
            if st.error_msg:
                summary += f", error_msg={st.error_msg!r}"
            verdict = "PASS" if st.ok else "FAIL"
    elif code == 0x80:
        summary = _status_summary(payload)
    elif code is None:
        actual = "TIMEOUT"
        summary = "TIMEOUT"
    else:
        summary = payload.hex() if payload else "(empty payload)"
    state.results.append(
        CaseResult(
            label="POST /ds18b20/config",
            surface="POST /ds18b20/config",
            method=COAP_POST,
            path="/ds18b20/config",
            request_summary=req_summary,
            expected_status="2.04",
            actual_status=actual,
            response_summary=summary,
            verdict=verdict,
        )
    )


# ============================================================
# Endpoint table of contents — every CoAP surface under test
# ============================================================

ENDPOINT_TOC: list[dict[str, str]] = [
    {
        "method": "GET",
        "path": "/system/info",
        "usage": "Read system identity, uptime, heap, and entity counts",
        "request": "none",
        "expected_response": "2.05 SystemInfo{version, uptime_s, heap_free, fan_count, source_count, curve_count, schedule_count, hostname}",
    },
    {
        "method": "PUT",
        "path": "/system/hostname",
        "usage": "Set the device hostname (persists to NVS)",
        "request": 'HostnameRequest{hostname: "espfm-test"}',
        "expected_response": "2.04 StatusResponse{ok=true}; 4.04 path mismatch; 4.00 undecodable/mdns fail",
    },
    {
        "method": "POST",
        "path": "/system/reboot",
        "usage": "Reboot the device after ~2 s",
        "request": "none",
        "expected_response": '2.04 StatusResponse{ok=true}; 5.03 "reboot pending"',
    },
    {
        "method": "GET",
        "path": "/fans",
        "usage": "List all fans",
        "request": "none",
        "expected_response": "2.05 FanList{repeated FanInfo}",
    },
    {
        "method": "POST",
        "path": "/fans",
        "usage": "Create a fan",
        "request": 'FanCreateRequest{pwm_gpio: <free>, tach_gpio: <free or 255>, name: "test-fan"}',
        "expected_response": "2.01 FanInfo{id, pwm_gpio, tach_gpio, name}; 4.00 StatusResponse{ok=false} on claim/constraint failure",
    },
    {
        "method": "GET",
        "path": "/fans/{id}",
        "usage": "Read one fan",
        "request": "none",
        "expected_response": "2.05 FanInfo{id}; 4.04 if unallocated",
    },
    {
        "method": "PUT",
        "path": "/fans/{id}",
        "usage": "Update fan fields",
        "request": "FanUpdateRequest{id, duty: <test value>}",
        "expected_response": "2.04 FanInfo{id, duty}; 4.04 not found; 4.00 StatusResponse on GPIO-swap failure",
    },
    {
        "method": "DELETE",
        "path": "/fans/{id}",
        "usage": "Delete a fan",
        "request": "none",
        "expected_response": "2.02 StatusResponse{ok=true}; 4.04 not found",
    },
    {
        "method": "GET",
        "path": "/sources",
        "usage": "List all sources",
        "request": "none",
        "expected_response": "2.05 SourceList{repeated SourceInfo}",
    },
    {
        "method": "POST",
        "path": "/sources",
        "usage": "Create a source",
        "request": 'SourceCreateRequest{type: SOURCE_TYPE_MANUAL, name: "test-source", gpio: 255}',
        "expected_response": "2.01 SourceInfo{id, name, type}; 4.00 StatusResponse on add failure",
    },
    {
        "method": "POST",
        "path": "/sources/temp",
        "usage": "Set a manual source temperature",
        "request": "ManualTempRequest{id: <manual source id>, temp_c: 20.0}",
        "expected_response": "2.04 StatusResponse{ok=true}; 4.04 source not found; 4.00 on fail",
    },
    {
        "method": "GET",
        "path": "/sources/{id}",
        "usage": "Read one source",
        "request": "none",
        "expected_response": "2.05 SourceInfo{id}; 4.04",
    },
    {
        "method": "PUT",
        "path": "/sources/{id}",
        "usage": "Rename a source",
        "request": "SourceUpdateRequest{id, name: <test name>}",
        "expected_response": "2.04 SourceInfo{id, name}; 4.04/4.00",
    },
    {
        "method": "DELETE",
        "path": "/sources/{id}",
        "usage": "Delete a source",
        "request": "none",
        "expected_response": "2.02 StatusResponse{ok=true}; 4.04",
    },
    {
        "method": "GET",
        "path": "/curves",
        "usage": "List all curves",
        "request": "none",
        "expected_response": "2.05 CurveList{repeated CurveInfo}",
    },
    {
        "method": "POST",
        "path": "/curves",
        "usage": "Create a curve",
        "request": 'CurveCreateRequest{name: "test-curve", points: [2-10 CurvePoint{temp_c, duty}]}',
        "expected_response": "2.01 CurveInfo{id, name, points}; 4.00 undecodable/upsert fail",
    },
    {
        "method": "GET",
        "path": "/curves/{id}",
        "usage": "Read one curve",
        "request": "none",
        "expected_response": "2.05 CurveInfo{id}; 4.04",
    },
    {
        "method": "PUT",
        "path": "/curves/{id}",
        "usage": "Update curve name and/or points",
        "request": "CurveUpdateRequest{id, name, points}",
        "expected_response": "2.04 CurveInfo{id, name, points}; 4.04/4.00",
    },
    {
        "method": "DELETE",
        "path": "/curves/{id}",
        "usage": "Delete a curve",
        "request": "none",
        "expected_response": "2.02 StatusResponse{ok=true}; 4.04",
    },
    {
        "method": "GET",
        "path": "/schedules",
        "usage": "List all schedules",
        "request": "none",
        "expected_response": "2.05 ScheduleList{repeated ScheduleInfo}",
    },
    {
        "method": "POST",
        "path": "/schedules",
        "usage": "Create a schedule",
        "request": 'ScheduleCreateRequest{fan_id: <live fan id>, duty: 50, start_min: 600, end_min: 1080, enabled: true, name: "test-schedule"}',
        "expected_response": "2.01 ScheduleInfo{id}; 4.00 on add failure",
    },
    {
        "method": "GET",
        "path": "/schedules/{id}",
        "usage": "Read one schedule",
        "request": "none",
        "expected_response": "2.05 ScheduleInfo{id}; 4.04",
    },
    {
        "method": "PUT",
        "path": "/schedules/{id}",
        "usage": "Update schedule fields",
        "request": "ScheduleUpdateRequest{id, duty: <test value>}",
        "expected_response": "2.04 ScheduleInfo{id, duty}; 4.04/4.00",
    },
    {
        "method": "DELETE",
        "path": "/schedules/{id}",
        "usage": "Delete a schedule",
        "request": "none",
        "expected_response": "2.02 StatusResponse{ok=true}; 4.04",
    },
    {
        "method": "GET",
        "path": "/config",
        "usage": "Export the full config",
        "request": "none",
        "expected_response": "2.05 ConfigFile{version, fans, sources, curves, schedules}",
    },
    {
        "method": "POST",
        "path": "/config",
        "usage": "Import a full config (reboots after ~2 s)",
        "request": "ConfigFile{<pre-run snapshot>}",
        "expected_response": "2.04 StatusResponse{ok=true}; 4.00 validation fail; 5.00 persist fail",
    },
    {
        "method": "GET",
        "path": "/control",
        "usage": "Read control tunables",
        "request": "none",
        "expected_response": "2.05 ControlConfig{hysteresis, ramp_up, ramp_down, failsafe_policy, safe_duty}; 5.03 control unset",
    },
    {
        "method": "PUT",
        "path": "/control",
        "usage": "Set control tunables",
        "request": "ControlConfig{hysteresis, ramp_up, ramp_down, failsafe_policy, safe_duty}",
        "expected_response": "2.04 StatusResponse{ok=true}; 4.00 range/decode fail; 5.03 control unset",
    },
    {
        "method": "GET",
        "path": "/ds18b20/scan",
        "usage": "Scan the DS18B20 bus",
        "request": "none",
        "expected_response": "2.05 Ds18b20ScanResponse{devices, device_count}; 5.03 no bus; 5.00 scan fail",
    },
    {
        "method": "POST",
        "path": "/ds18b20/config",
        "usage": "Configure the DS18B20 bus GPIO",
        "request": "Ds18b20ConfigRequest{gpio: <free pin>}",
        "expected_response": "2.04 StatusResponse{ok=true}; 4.00 init/claim fail",
    },
    {
        "method": "GET",
        "path": "/wifi/scan",
        "usage": "Scan nearby WiFi APs (~3.5 s)",
        "request": "none",
        "expected_response": "2.05 WifiScanResult{repeated WifiApRecord}; 5.03 scan fail",
    },
    {
        "method": "GET",
        "path": "/wifi/status",
        "usage": "Read STA/AP status",
        "request": "none",
        "expected_response": "2.05 WifiStatus{sta_connected, sta_ip, ap_ip}",
    },
    {
        "method": "POST",
        "path": "/wifi/connect",
        "usage": "Connect to a WiFi network",
        "request": 'WifiConnectRequest{ssid: "<WIFI_SSID>", password: "<WIFI_PASSWORD>"}',
        "expected_response": "2.04 StatusResponse{ok=true}; 5.03 set-config fail; or timeout",
    },
]


def render_body(state: RunState) -> list[str]:
    """Render one markdown section per tested endpoint from captured results.

    Groups state.results by surface (method + resolved path, first-seen order)
    and emits a `### METHOD path` heading plus a bullet per captured record with
    the request data sent, the actual responded data (CoAP code + decoded fields
    or raw hex), and the verdict. All actual-response values come from the
    captured CaseResult fields — nothing is hand-edited (R56, R57).
    """
    lines: list[str] = []
    seen: set[str] = set()
    for result in state.results:
        if result.surface in seen:
            continue
        seen.add(result.surface)
        group = [r for r in state.results if r.surface == result.surface]
        lines.append(f"### {result.surface}")
        lines.append("")
        for r in group:
            note = f" ({r.note})" if r.note else ""
            lines.append(f"- Request: {r.request_summary}")
            lines.append(f"  - Expected: {r.expected_status}")
            lines.append(f"  - Actual: {r.actual_status}")
            lines.append(f"  - Response: {r.response_summary or 'none'}")
            lines.append(f"  - Verdict: {r.verdict}{note}")
        # Surface verdict (plan Step 9.1): PASS only when every record is PASS;
        # NOT TESTED only when every record is NOT TESTED; otherwise FAIL.
        if all(r.verdict == "PASS" for r in group):
            surface_verdict = "PASS"
        elif all(r.verdict == "NOT TESTED" for r in group):
            surface_verdict = "NOT TESTED"
        else:
            surface_verdict = "FAIL"
        lines.append(f"- **Endpoint result: {surface_verdict}**")
        lines.append("")
    return lines


def write_report(state: RunState) -> None:
    """Write the report: device-identity header, TOC, and per-endpoint body."""
    lines: list[str] = []
    lines.append("# ESPFM Live-Device CoAP Integration Test Report")
    lines.append("")
    hostname = state.system_info.hostname if state.system_info else "unknown"
    version = state.system_info.version if state.system_info else "unknown"
    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    lines.append(f"- Target IP: `{state.host}`")
    lines.append(f"- Device hostname: `{hostname}`")
    lines.append(f"- Firmware version: `{version}`")
    lines.append(f"- Run timestamp: `{now}`")
    lines.append("")
    lines.append("## Endpoint Table of Contents")
    lines.append("")
    for entry in ENDPOINT_TOC:
        lines.append(f"- **{entry['method']} {entry['path']}** — {entry['usage']}")
        lines.append(f"  - Request: {entry['request']}")
        lines.append(f"  - Expected: {entry['expected_response']}")
    lines.append("")
    lines.append("## Per-Endpoint Results")
    lines.append("")
    lines.extend(render_body(state))
    lines.append("")

    with open(state.output_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


def parse_args(argv: list[str]) -> argparse.Namespace:
    """Parse CLI args. --host/--port flags take precedence over positionals."""
    parser = argparse.ArgumentParser(
        description="ESPFM live-device CoAP integration test"
    )
    parser.add_argument(
        "host", nargs="?", default=DEFAULT_HOST,
        help=f"Device IP (default: {DEFAULT_HOST})",
    )
    parser.add_argument(
        "port", nargs="?", type=int, default=DEFAULT_PORT,
        help=f"CoAP port (default: {DEFAULT_PORT})",
    )
    parser.add_argument(
        "--host", dest="host_flag", default=None,
        help="Device IP (overrides positional host)",
    )
    parser.add_argument(
        "--port", dest="port_flag", type=int, default=None,
        help="CoAP port (overrides positional port)",
    )
    parser.add_argument(
        "--output", default=DEFAULT_OUTPUT,
        help=f"Report output path (default: {DEFAULT_OUTPUT})",
    )
    args = parser.parse_args(argv)

    if args.host_flag is not None:
        args.host = args.host_flag
    if args.port_flag is not None:
        args.port = args.port_flag
    return args


# ============================================================
# Phase 5 — item-level reads and error-path GET (R31-R35)
# ============================================================


def run_fan_get(state: RunState) -> None:
    """Read GET /fans/{live_fan_id} and verify the id matches (R31)."""
    path = f"/fans/{state.live_fan_id}"
    code, payload = do_request(COAP_GET, path)
    summary = ""
    actual = code_str(code)
    verdict = "FAIL"
    if code == 0x45:
        try:
            fan = pb.FanInfo()
            fan.ParseFromString(payload)
        except Exception as exc:
            summary = f"decode error: {exc}"
        else:
            if fan.id == state.live_fan_id:
                summary = _fan_summary(fan)
                verdict = "PASS"
            else:
                summary = f"id mismatch: got {fan.id}, expected {state.live_fan_id}"
    elif code is None:
        actual = "TIMEOUT"
        summary = "TIMEOUT"
    else:
        summary = payload.hex() if payload else "(empty payload)"
    state.results.append(
        CaseResult(
            label=f"GET /fans/{state.live_fan_id}",
            surface=path,
            method=COAP_GET,
            path=path,
            request_summary="none",
            expected_status="2.05",
            actual_status=actual,
            response_summary=summary,
            verdict=verdict,
        )
    )


def run_source_get(state: RunState) -> None:
    """Read GET /sources/{manual_source_id} and verify the id matches (R32)."""
    if state.manual_source_id is None:
        state.results.append(
            CaseResult(
                label="GET /sources/{id}",
                surface="GET /sources/{id}",
                method=COAP_GET,
                path="GET /sources/{id}",
                request_summary="none",
                expected_status="2.05",
                actual_status="NOT TESTED",
                response_summary="",
                verdict="NOT TESTED",
                note="no pre-existing source id available",
            )
        )
        return
    path = f"/sources/{state.manual_source_id}"
    code, payload = do_request(COAP_GET, path)
    summary = ""
    actual = code_str(code)
    verdict = "FAIL"
    if code == 0x45:
        try:
            src = pb.SourceInfo()
            src.ParseFromString(payload)
        except Exception as exc:
            summary = f"decode error: {exc}"
        else:
            if src.id == state.manual_source_id:
                summary = _source_summary(src)
                verdict = "PASS"
            else:
                summary = f"id mismatch: got {src.id}, expected {state.manual_source_id}"
    elif code is None:
        actual = "TIMEOUT"
        summary = "TIMEOUT"
    else:
        summary = payload.hex() if payload else "(empty payload)"
    state.results.append(
        CaseResult(
            label=path,
            surface=path,
            method=COAP_GET,
            path=path,
            request_summary="none",
            expected_status="2.05",
            actual_status=actual,
            response_summary=summary,
            verdict=verdict,
        )
    )


def run_curve_get(state: RunState) -> None:
    """Read GET /curves/{first_curve_id} and verify the id matches (R33)."""
    if state.first_curve_id is None:
        state.results.append(
            CaseResult(
                label="GET /curves/{id}",
                surface="GET /curves/{id}",
                method=COAP_GET,
                path="GET /curves/{id}",
                request_summary="none",
                expected_status="2.05",
                actual_status="NOT TESTED",
                response_summary="",
                verdict="NOT TESTED",
                note="no pre-existing curve id available",
            )
        )
        return
    path = f"/curves/{state.first_curve_id}"
    code, payload = do_request(COAP_GET, path)
    summary = ""
    actual = code_str(code)
    verdict = "FAIL"
    if code == 0x45:
        try:
            curve = pb.CurveInfo()
            curve.ParseFromString(payload)
        except Exception as exc:
            summary = f"decode error: {exc}"
        else:
            if curve.id == state.first_curve_id:
                summary = _curve_summary(curve)
                verdict = "PASS"
            else:
                summary = f"id mismatch: got {curve.id}, expected {state.first_curve_id}"
    elif code is None:
        actual = "TIMEOUT"
        summary = "TIMEOUT"
    else:
        summary = payload.hex() if payload else "(empty payload)"
    state.results.append(
        CaseResult(
            label=path,
            surface=path,
            method=COAP_GET,
            path=path,
            request_summary="none",
            expected_status="2.05",
            actual_status=actual,
            response_summary=summary,
            verdict=verdict,
        )
    )


def run_schedule_get(state: RunState) -> None:
    """Read GET /schedules/{created_schedule_id} and verify the id matches (R34)."""
    if state.created_schedule_id is None:
        state.results.append(
            CaseResult(
                label="GET /schedules/{id}",
                surface="GET /schedules/{id}",
                method=COAP_GET,
                path="GET /schedules/{id}",
                request_summary="none",
                expected_status="2.05",
                actual_status="NOT TESTED",
                response_summary="",
                verdict="NOT TESTED",
                note="no test schedule created",
            )
        )
        return
    path = f"/schedules/{state.created_schedule_id}"
    code, payload = do_request(COAP_GET, path)
    summary = ""
    actual = code_str(code)
    verdict = "FAIL"
    if code == 0x45:
        try:
            sched = pb.ScheduleInfo()
            sched.ParseFromString(payload)
        except Exception as exc:
            summary = f"decode error: {exc}"
        else:
            if sched.id == state.created_schedule_id:
                summary = _schedule_summary(sched)
                verdict = "PASS"
            else:
                summary = (
                    f"id mismatch: got {sched.id}, expected {state.created_schedule_id}"
                )
    elif code is None:
        actual = "TIMEOUT"
        summary = "TIMEOUT"
    else:
        summary = payload.hex() if payload else "(empty payload)"
    state.results.append(
        CaseResult(
            label=path,
            surface=path,
            method=COAP_GET,
            path=path,
            request_summary="none",
            expected_status="2.05",
            actual_status=actual,
            response_summary=summary,
            verdict=verdict,
        )
    )


def run_error_path_get(state: RunState) -> None:
    """Read GET /fans/7 and record the 4.04 Not Found expected error (R35)."""
    code, payload = do_request(COAP_GET, "/fans/7")
    summary = ""
    actual = code_str(code)
    verdict = "FAIL"
    if code == 0x84:
        actual = "4.04"
        summary = "4.04 Not Found (unallocated slot)"
        verdict = "PASS"
    elif code == 0x45:
        try:
            fan = pb.FanInfo()
            fan.ParseFromString(payload)
        except Exception as exc:
            summary = f"decode error: {exc}"
        else:
            summary = f"unexpected fan present: {_fan_summary(fan)}"
    elif code is None:
        actual = "TIMEOUT"
        summary = "TIMEOUT"
    else:
        summary = payload.hex() if payload else "(empty payload)"
    state.results.append(
        CaseResult(
            label="GET /fans/7",
            surface="GET /fans/7",
            method=COAP_GET,
            path="/fans/7",
            request_summary="none",
            expected_status="4.04",
            actual_status=actual,
            response_summary=summary,
            verdict=verdict,
        )
    )


# ============================================================
# Phase 6 — update phase (fan, source, curve, schedule, control) (R36-R42)
# ============================================================


def run_fan_update(state: RunState) -> None:
    """PUT /fans/{live_fan_id}: set duty=40, verify, then restore (R36/R40)."""
    path = f"/fans/{state.live_fan_id}"
    surface = f"PUT /fans/{state.live_fan_id}"

    # Capture the original duty from GET /fans/{id}.
    original_duty: int | None = None
    code, payload = do_request(COAP_GET, path)
    if code == 0x45:
        try:
            fan = pb.FanInfo()
            fan.ParseFromString(payload)
        except Exception:
            fan = None
        else:
            original_duty = fan.duty

    # Update duty to 40.
    req = pb.FanUpdateRequest(id=state.live_fan_id, duty=40)
    req_summary = f"FanUpdateRequest{{id={req.id}, duty={req.duty}}}"
    code, payload = do_request(COAP_PUT, path, req)
    summary = ""
    actual = code_str(code)
    verdict = "FAIL"
    if code == 0x44:
        try:
            fan = pb.FanInfo()
            fan.ParseFromString(payload)
        except Exception as exc:
            summary = f"decode error: {exc}"
        else:
            summary = _fan_summary(fan)
            verdict = "PASS" if fan.duty == 40 else "FAIL"
            if fan.duty != 40:
                summary = f"duty mismatch: got {fan.duty}, expected 40; {summary}"
    elif code in (0x80, 0x84):
        summary = _status_summary(payload)
    elif code is None:
        actual = "TIMEOUT"
        summary = "TIMEOUT"
    else:
        summary = payload.hex() if payload else "(empty payload)"
    state.results.append(
        CaseResult(
            label=surface,
            surface=surface,
            method=COAP_PUT,
            path=path,
            request_summary=req_summary,
            expected_status="2.04",
            actual_status=actual,
            response_summary=summary,
            verdict=verdict,
        )
    )

    # Restore the original duty (R40).
    if original_duty is None:
        state.results.append(
            CaseResult(
                label=f"{surface} (restore)",
                surface=surface,
                method=COAP_PUT,
                path=path,
                request_summary="none",
                expected_status="2.04",
                actual_status="NOT TESTED",
                response_summary="(skipped)",
                verdict="NOT TESTED",
                note="original duty could not be captured",
            )
        )
        return
    restore = pb.FanUpdateRequest(id=state.live_fan_id, duty=original_duty)
    restore_summary = f"FanUpdateRequest{{id={restore.id}, duty={restore.duty}}}"
    code, payload = do_request(COAP_PUT, path, restore)
    summary = ""
    actual = code_str(code)
    verdict = "FAIL"
    if code == 0x44:
        try:
            fan = pb.FanInfo()
            fan.ParseFromString(payload)
        except Exception as exc:
            summary = f"decode error: {exc}"
        else:
            summary = _fan_summary(fan)
            verdict = "PASS" if fan.duty == original_duty else "FAIL"
            if fan.duty != original_duty:
                summary = (
                    f"duty mismatch: got {fan.duty}, expected {original_duty}; "
                    f"{summary}"
                )
    elif code in (0x80, 0x84):
        summary = _status_summary(payload)
    elif code is None:
        actual = "TIMEOUT"
        summary = "TIMEOUT"
    else:
        summary = payload.hex() if payload else "(empty payload)"
    state.results.append(
        CaseResult(
            label=f"{surface} (restore)",
            surface=surface,
            method=COAP_PUT,
            path=path,
            request_summary=restore_summary,
            expected_status="2.04",
            actual_status=actual,
            response_summary=summary,
            verdict=verdict,
        )
    )


def run_source_update(state: RunState) -> None:
    """PUT /sources/{id}: rename to gpu-manual-test, verify, restore (R37/R40)."""
    if state.manual_source_id is None:
        state.results.append(
            CaseResult(
                label="PUT /sources/{id}",
                surface="PUT /sources/{id}",
                method=COAP_PUT,
                path="/sources/{id}",
                request_summary="none",
                expected_status="2.04",
                actual_status="NOT TESTED",
                response_summary="",
                verdict="NOT TESTED",
                note="no pre-existing source id available",
            )
        )
        return
    path = f"/sources/{state.manual_source_id}"
    surface = f"PUT /sources/{state.manual_source_id}"

    # Capture the original name from GET /sources/{id}.
    original_name: str | None = None
    code, payload = do_request(COAP_GET, path)
    if code == 0x45:
        try:
            src = pb.SourceInfo()
            src.ParseFromString(payload)
        except Exception:
            src = None
        else:
            original_name = src.name

    # Rename to gpu-manual-test.
    req = pb.SourceUpdateRequest(id=state.manual_source_id, name="gpu-manual-test")
    req_summary = f"SourceUpdateRequest{{id={req.id}, name={req.name!r}}}"
    code, payload = do_request(COAP_PUT, path, req)
    summary = ""
    actual = code_str(code)
    verdict = "FAIL"
    if code == 0x44:
        try:
            src = pb.SourceInfo()
            src.ParseFromString(payload)
        except Exception as exc:
            summary = f"decode error: {exc}"
        else:
            summary = _source_summary(src)
            verdict = "PASS" if src.name == "gpu-manual-test" else "FAIL"
            if src.name != "gpu-manual-test":
                summary = (
                    f"name mismatch: got {src.name!r}, expected 'gpu-manual-test'; "
                    f"{summary}"
                )
    elif code in (0x80, 0x84):
        summary = _status_summary(payload)
    elif code is None:
        actual = "TIMEOUT"
        summary = "TIMEOUT"
    else:
        summary = payload.hex() if payload else "(empty payload)"
    state.results.append(
        CaseResult(
            label=surface,
            surface=surface,
            method=COAP_PUT,
            path=path,
            request_summary=req_summary,
            expected_status="2.04",
            actual_status=actual,
            response_summary=summary,
            verdict=verdict,
        )
    )

    # Restore the original name (R40).
    if original_name is None:
        state.results.append(
            CaseResult(
                label=f"{surface} (restore)",
                surface=surface,
                method=COAP_PUT,
                path=path,
                request_summary="none",
                expected_status="2.04",
                actual_status="NOT TESTED",
                response_summary="(skipped)",
                verdict="NOT TESTED",
                note="original name could not be captured",
            )
        )
        return
    restore = pb.SourceUpdateRequest(id=state.manual_source_id, name=original_name)
    restore_summary = f"SourceUpdateRequest{{id={restore.id}, name={restore.name!r}}}"
    code, payload = do_request(COAP_PUT, path, restore)
    summary = ""
    actual = code_str(code)
    verdict = "FAIL"
    if code == 0x44:
        try:
            src = pb.SourceInfo()
            src.ParseFromString(payload)
        except Exception as exc:
            summary = f"decode error: {exc}"
        else:
            summary = _source_summary(src)
            verdict = "PASS" if src.name == original_name else "FAIL"
            if src.name != original_name:
                summary = (
                    f"name mismatch: got {src.name!r}, expected {original_name!r}; "
                    f"{summary}"
                )
    elif code in (0x80, 0x84):
        summary = _status_summary(payload)
    elif code is None:
        actual = "TIMEOUT"
        summary = "TIMEOUT"
    else:
        summary = payload.hex() if payload else "(empty payload)"
    state.results.append(
        CaseResult(
            label=f"{surface} (restore)",
            surface=surface,
            method=COAP_PUT,
            path=path,
            request_summary=restore_summary,
            expected_status="2.04",
            actual_status=actual,
            response_summary=summary,
            verdict=verdict,
        )
    )


def run_curve_update(state: RunState) -> None:
    """PUT /curves/{id}: rename to gpu-temp-test, verify, full-replace restore (R38/R40)."""
    if state.first_curve_id is None:
        state.results.append(
            CaseResult(
                label="PUT /curves/{id}",
                surface="PUT /curves/{id}",
                method=COAP_PUT,
                path="/curves/{id}",
                request_summary="none",
                expected_status="2.04",
                actual_status="NOT TESTED",
                response_summary="",
                verdict="NOT TESTED",
                note="no pre-existing curve id available",
            )
        )
        return
    path = f"/curves/{state.first_curve_id}"
    surface = f"PUT /curves/{state.first_curve_id}"

    # Capture the original name and points from GET /curves/{id}.
    original_curve: pb.CurveInfo | None = None
    code, payload = do_request(COAP_GET, path)
    if code == 0x45:
        try:
            curve = pb.CurveInfo()
            curve.ParseFromString(payload)
        except Exception:
            curve = None
        else:
            original_curve = curve

    # Rename to gpu-temp-test.
    req = pb.CurveUpdateRequest(id=state.first_curve_id, name="gpu-temp-test")
    req_summary = (
        f"CurveUpdateRequest{{id={req.id}, name={req.name!r}, "
        f"points={len(req.points)}}}"
    )
    code, payload = do_request(COAP_PUT, path, req)
    summary = ""
    actual = code_str(code)
    verdict = "FAIL"
    if code == 0x44:
        try:
            curve = pb.CurveInfo()
            curve.ParseFromString(payload)
        except Exception as exc:
            summary = f"decode error: {exc}"
        else:
            summary = _curve_summary(curve)
            verdict = "PASS" if curve.name == "gpu-temp-test" else "FAIL"
            if curve.name != "gpu-temp-test":
                summary = (
                    f"name mismatch: got {curve.name!r}, expected 'gpu-temp-test'; "
                    f"{summary}"
                )
    elif code in (0x80, 0x84):
        summary = _status_summary(payload)
    elif code is None:
        actual = "TIMEOUT"
        summary = "TIMEOUT"
    else:
        summary = payload.hex() if payload else "(empty payload)"
    state.results.append(
        CaseResult(
            label=surface,
            surface=surface,
            method=COAP_PUT,
            path=path,
            request_summary=req_summary,
            expected_status="2.04",
            actual_status=actual,
            response_summary=summary,
            verdict=verdict,
        )
    )

    # Restore via full-replace upsert: original name + original points (R40).
    if original_curve is None:
        state.results.append(
            CaseResult(
                label=f"{surface} (restore)",
                surface=surface,
                method=COAP_PUT,
                path=path,
                request_summary="none",
                expected_status="2.04",
                actual_status="NOT TESTED",
                response_summary="(skipped)",
                verdict="NOT TESTED",
                note="original curve could not be captured",
            )
        )
        return
    restore = pb.CurveUpdateRequest(id=state.first_curve_id, name=original_curve.name)
    for point in original_curve.points:
        restored_point = restore.points.add()
        restored_point.temp_c = point.temp_c
        restored_point.duty = point.duty
    restore_summary = (
        f"CurveUpdateRequest{{id={restore.id}, name={restore.name!r}, "
        f"points={len(restore.points)}}}"
    )
    code, payload = do_request(COAP_PUT, path, restore)
    summary = ""
    actual = code_str(code)
    verdict = "FAIL"
    if code == 0x44:
        try:
            curve = pb.CurveInfo()
            curve.ParseFromString(payload)
        except Exception as exc:
            summary = f"decode error: {exc}"
        else:
            points_match = len(curve.points) == len(original_curve.points) and all(
                cp.temp_c == op.temp_c and cp.duty == op.duty
                for cp, op in zip(curve.points, original_curve.points)
            )
            summary = _curve_summary(curve)
            if curve.name == original_curve.name and points_match:
                verdict = "PASS"
            else:
                verdict = "FAIL"
                summary = (
                    f"restore mismatch: name={curve.name!r} expected "
                    f"{original_curve.name!r}, points={len(curve.points)} expected "
                    f"{len(original_curve.points)}; {summary}"
                )
    elif code in (0x80, 0x84):
        summary = _status_summary(payload)
    elif code is None:
        actual = "TIMEOUT"
        summary = "TIMEOUT"
    else:
        summary = payload.hex() if payload else "(empty payload)"
    state.results.append(
        CaseResult(
            label=f"{surface} (restore)",
            surface=surface,
            method=COAP_PUT,
            path=path,
            request_summary=restore_summary,
            expected_status="2.04",
            actual_status=actual,
            response_summary=summary,
            verdict=verdict,
        )
    )


def run_schedule_update(state: RunState) -> None:
    """PUT /schedules/{id}: set duty=80, verify, restore (R39/R40)."""
    if state.created_schedule_id is None:
        state.results.append(
            CaseResult(
                label="PUT /schedules/{id}",
                surface="PUT /schedules/{id}",
                method=COAP_PUT,
                path="/schedules/{id}",
                request_summary="none",
                expected_status="2.04",
                actual_status="NOT TESTED",
                response_summary="",
                verdict="NOT TESTED",
                note="no test schedule created",
            )
        )
        return
    path = f"/schedules/{state.created_schedule_id}"
    surface = f"PUT /schedules/{state.created_schedule_id}"

    # Capture the original duty from GET /schedules/{id}.
    original_duty: int | None = None
    code, payload = do_request(COAP_GET, path)
    if code == 0x45:
        try:
            sched = pb.ScheduleInfo()
            sched.ParseFromString(payload)
        except Exception:
            sched = None
        else:
            original_duty = sched.duty

    # Update duty to 80.
    req = pb.ScheduleUpdateRequest(id=state.created_schedule_id, duty=80)
    req_summary = f"ScheduleUpdateRequest{{id={req.id}, duty={req.duty}}}"
    code, payload = do_request(COAP_PUT, path, req)
    summary = ""
    actual = code_str(code)
    verdict = "FAIL"
    if code == 0x44:
        try:
            sched = pb.ScheduleInfo()
            sched.ParseFromString(payload)
        except Exception as exc:
            summary = f"decode error: {exc}"
        else:
            summary = _schedule_summary(sched)
            verdict = "PASS" if sched.duty == 80 else "FAIL"
            if sched.duty != 80:
                summary = f"duty mismatch: got {sched.duty}, expected 80; {summary}"
    elif code in (0x80, 0x84):
        summary = _status_summary(payload)
    elif code is None:
        actual = "TIMEOUT"
        summary = "TIMEOUT"
    else:
        summary = payload.hex() if payload else "(empty payload)"
    state.results.append(
        CaseResult(
            label=surface,
            surface=surface,
            method=COAP_PUT,
            path=path,
            request_summary=req_summary,
            expected_status="2.04",
            actual_status=actual,
            response_summary=summary,
            verdict=verdict,
        )
    )

    # Restore the original duty (R40).
    if original_duty is None:
        state.results.append(
            CaseResult(
                label=f"{surface} (restore)",
                surface=surface,
                method=COAP_PUT,
                path=path,
                request_summary="none",
                expected_status="2.04",
                actual_status="NOT TESTED",
                response_summary="(skipped)",
                verdict="NOT TESTED",
                note="original duty could not be captured",
            )
        )
        return
    restore = pb.ScheduleUpdateRequest(id=state.created_schedule_id, duty=original_duty)
    restore_summary = f"ScheduleUpdateRequest{{id={restore.id}, duty={restore.duty}}}"
    code, payload = do_request(COAP_PUT, path, restore)
    summary = ""
    actual = code_str(code)
    verdict = "FAIL"
    if code == 0x44:
        try:
            sched = pb.ScheduleInfo()
            sched.ParseFromString(payload)
        except Exception as exc:
            summary = f"decode error: {exc}"
        else:
            summary = _schedule_summary(sched)
            verdict = "PASS" if sched.duty == original_duty else "FAIL"
            if sched.duty != original_duty:
                summary = (
                    f"duty mismatch: got {sched.duty}, expected {original_duty}; "
                    f"{summary}"
                )
    elif code in (0x80, 0x84):
        summary = _status_summary(payload)
    elif code is None:
        actual = "TIMEOUT"
        summary = "TIMEOUT"
    else:
        summary = payload.hex() if payload else "(empty payload)"
    state.results.append(
        CaseResult(
            label=f"{surface} (restore)",
            surface=surface,
            method=COAP_PUT,
            path=path,
            request_summary=restore_summary,
            expected_status="2.04",
            actual_status=actual,
            response_summary=summary,
            verdict=verdict,
        )
    )


def run_control_restore(state: RunState) -> None:
    """PUT /control: restore the recorded control originals (R41)."""
    if state.control_originals is None:
        state.results.append(
            CaseResult(
                label="PUT /control",
                surface="PUT /control",
                method=COAP_PUT,
                path="/control",
                request_summary="none",
                expected_status="2.04",
                actual_status="NOT TESTED",
                response_summary="",
                verdict="NOT TESTED",
                note="no control originals captured",
            )
        )
        return
    req = pb.ControlConfig()
    req.hysteresis = state.control_originals.hysteresis
    req.ramp_up = state.control_originals.ramp_up
    req.ramp_down = state.control_originals.ramp_down
    req.failsafe_policy = state.control_originals.failsafe_policy
    req.safe_duty = state.control_originals.safe_duty
    req_summary = (
        f"ControlConfig{{hysteresis={req.hysteresis}, ramp_up={req.ramp_up}, "
        f"ramp_down={req.ramp_down}, failsafe_policy={req.failsafe_policy}, "
        f"safe_duty={req.safe_duty}}}"
    )
    code, payload = do_request(COAP_PUT, "/control", req)
    summary = ""
    actual = code_str(code)
    verdict = "FAIL"
    if code == 0x44:
        try:
            st = pb.StatusResponse()
            st.ParseFromString(payload)
        except Exception as exc:
            summary = f"decode error: {exc}"
        else:
            summary = _status_summary(payload)
            verdict = "PASS" if st.ok else "FAIL"
    elif code in (0x80, 0x84):
        summary = _status_summary(payload)
    elif code is None:
        actual = "TIMEOUT"
        summary = "TIMEOUT"
    else:
        summary = payload.hex() if payload else "(empty payload)"
    state.results.append(
        CaseResult(
            label="PUT /control",
            surface="PUT /control",
            method=COAP_PUT,
            path="/control",
            request_summary=req_summary,
            expected_status="2.04",
            actual_status=actual,
            response_summary=summary,
            verdict=verdict,
        )
    )


def run_control_error(state: RunState) -> None:
    """PUT /control with hysteresis=150, expecting a 4.00 error (R42)."""
    if state.control_originals is None:
        state.results.append(
            CaseResult(
                label="PUT /control (out-of-range)",
                surface="PUT /control (out-of-range)",
                method=COAP_PUT,
                path="/control",
                request_summary="none",
                expected_status="4.00",
                actual_status="NOT TESTED",
                response_summary="",
                verdict="NOT TESTED",
                note="no control originals captured",
            )
        )
        return
    req = pb.ControlConfig(hysteresis=150)
    req.ramp_up = state.control_originals.ramp_up
    req.ramp_down = state.control_originals.ramp_down
    req.failsafe_policy = state.control_originals.failsafe_policy
    req.safe_duty = state.control_originals.safe_duty
    req_summary = (
        f"ControlConfig{{hysteresis={req.hysteresis}, ramp_up={req.ramp_up}, "
        f"ramp_down={req.ramp_down}, failsafe_policy={req.failsafe_policy}, "
        f"safe_duty={req.safe_duty}}}"
    )
    code, payload = do_request(COAP_PUT, "/control", req)
    summary = ""
    actual = code_str(code)
    verdict = "FAIL"
    if code == 0x80:
        actual = "4.00"
        try:
            st = pb.StatusResponse()
            st.ParseFromString(payload)
        except Exception as exc:
            summary = f"decode error: {exc}"
        else:
            summary = (
                f"ok={st.ok}, error_code={st.error_code}, "
                f"error_msg={st.error_msg!r}"
            )
            verdict = "PASS" if (not st.ok and st.error_msg) else "FAIL"
    elif code == 0x44:
        try:
            st = pb.StatusResponse()
            st.ParseFromString(payload)
        except Exception as exc:
            summary = f"decode error: {exc}"
        else:
            summary = f"unexpectedly accepted: ok={st.ok}, error_msg={st.error_msg!r}"
    elif code is None:
        actual = "TIMEOUT"
        summary = "TIMEOUT"
    else:
        summary = payload.hex() if payload else "(empty payload)"
    state.results.append(
        CaseResult(
            label="PUT /control (out-of-range)",
            surface="PUT /control (out-of-range)",
            method=COAP_PUT,
            path="/control",
            request_summary=req_summary,
            expected_status="4.00",
            actual_status=actual,
            response_summary=summary,
            verdict=verdict,
        )
    )


# ============================================================
# Phase 7 — hostname update + cleanup deletes (R43-R48)
# ============================================================


def _status_case(
    label: str,
    surface: str,
    path: str,
    expected: str,
    method: int,
    code: int | None,
    payload: bytes | None,
    request_summary: str,
) -> CaseResult:
    """Build a CaseResult from a StatusResponse-carrying response (2.02/2.04)."""
    summary = ""
    actual = code_str(code)
    verdict = "FAIL"
    if code in (0x42, 0x44):  # 2.02 Deleted / 2.04 Changed
        try:
            st = pb.StatusResponse()
            st.ParseFromString(payload)
        except Exception as exc:
            summary = f"decode error: {exc}"
        else:
            summary = _status_summary(payload)
            verdict = "PASS" if st.ok else "FAIL"
    elif code in (0x80, 0x84):  # 4.00 / 4.04 — decode StatusResponse if present
        summary = _status_summary(payload)
    elif code is None:
        actual = "TIMEOUT"
        summary = "TIMEOUT"
    else:
        summary = payload.hex() if payload else "(empty payload)"
    return CaseResult(
        label=label,
        surface=surface,
        method=method,
        path=path,
        request_summary=request_summary,
        expected_status=expected,
        actual_status=actual,
        response_summary=summary,
        verdict=verdict,
    )


def run_hostname_test(state: RunState) -> None:
    """Set a test hostname via PUT /system/hostname (R43)."""
    req = pb.HostnameRequest(hostname="espfm-test")
    code, payload = do_request(COAP_PUT, "/system/hostname", req)
    state.results.append(
        _status_case(
            "PUT /system/hostname",
            "PUT /system/hostname",
            "/system/hostname",
            "2.04",
            COAP_PUT,
            code,
            payload,
            f"HostnameRequest{{hostname={req.hostname!r}}}",
        )
    )


def run_hostname_restore(state: RunState) -> None:
    """Restore the original hostname (R44)."""
    if state.original_hostname is None:
        state.results.append(
            CaseResult(
                label="PUT /system/hostname (restore)",
                surface="PUT /system/hostname (restore)",
                method=COAP_PUT,
                path="/system/hostname",
                request_summary="none",
                expected_status="2.04",
                actual_status="NOT TESTED",
                response_summary="",
                verdict="NOT TESTED",
                note="no original hostname captured",
            )
        )
        return
    req = pb.HostnameRequest(hostname=state.original_hostname)
    code, payload = do_request(COAP_PUT, "/system/hostname", req)
    state.results.append(
        _status_case(
            "PUT /system/hostname (restore)",
            "PUT /system/hostname (restore)",
            "/system/hostname",
            "2.04",
            COAP_PUT,
            code,
            payload,
            f"HostnameRequest{{hostname={req.hostname!r}}}",
        )
    )


def run_delete_fan(state: RunState) -> None:
    """Delete the test-created fan, never the pre-existing fan (R45)."""
    if state.created_fan_id is None:
        state.results.append(
            CaseResult(
                label="DELETE /fans/{created_id}",
                surface="DELETE /fans/{created_id}",
                method=COAP_DELETE,
                path="DELETE /fans/{created_id}",
                request_summary="none",
                expected_status="2.02",
                actual_status="NOT TESTED",
                response_summary="",
                verdict="NOT TESTED",
                note="no test fan created",
            )
        )
        return
    if state.created_fan_id == state.live_fan_id:
        # The test fan landed on the pre-existing fan's slot (e.g. the device
        # reused id 0), so the create overwrote it rather than adding a new one.
        # Never delete it here; the final POST /config import restores the
        # pre-run snapshot. Record the collision as NOT TESTED.
        state.results.append(
            CaseResult(
                label="DELETE /fans/{created_id}",
                surface="DELETE /fans/{created_id}",
                method=COAP_DELETE,
                path="DELETE /fans/{created_id}",
                request_summary="none",
                expected_status="2.02",
                actual_status="NOT TESTED",
                response_summary="",
                verdict="NOT TESTED",
                note="test fan reused the pre-existing fan slot; skipped (config restore handles it)",
            )
        )
        return
    path = f"/fans/{state.created_fan_id}"
    code, payload = do_request(COAP_DELETE, path)
    state.results.append(
        _status_case(
            f"DELETE /fans/{state.created_fan_id}",
            f"DELETE /fans/{state.created_fan_id}",
            path,
            "2.02",
            COAP_DELETE,
            code,
            payload,
            "none",
        )
    )


def run_delete_source(state: RunState) -> None:
    """Delete the test-created source, never the pre-existing source (R46)."""
    if state.created_source_id is None:
        state.results.append(
            CaseResult(
                label="DELETE /sources/{created_id}",
                surface="DELETE /sources/{created_id}",
                method=COAP_DELETE,
                path="DELETE /sources/{created_id}",
                request_summary="none",
                expected_status="2.02",
                actual_status="NOT TESTED",
                response_summary="",
                verdict="NOT TESTED",
                note="no test source created",
            )
        )
        return
    if state.created_source_id == state.manual_source_id:
        # The test source landed on the pre-existing source's slot; never delete
        # it here. The final POST /config import restores the pre-run snapshot.
        state.results.append(
            CaseResult(
                label="DELETE /sources/{created_id}",
                surface="DELETE /sources/{created_id}",
                method=COAP_DELETE,
                path="DELETE /sources/{created_id}",
                request_summary="none",
                expected_status="2.02",
                actual_status="NOT TESTED",
                response_summary="",
                verdict="NOT TESTED",
                note="test source reused the pre-existing source slot; skipped (config restore handles it)",
            )
        )
        return
    path = f"/sources/{state.created_source_id}"
    code, payload = do_request(COAP_DELETE, path)
    state.results.append(
        _status_case(
            f"DELETE /sources/{state.created_source_id}",
            f"DELETE /sources/{state.created_source_id}",
            path,
            "2.02",
            COAP_DELETE,
            code,
            payload,
            "none",
        )
    )


def run_delete_curve(state: RunState) -> None:
    """Delete the test-created curve, never the pre-existing curve (R47)."""
    if state.created_curve_id is None:
        state.results.append(
            CaseResult(
                label="DELETE /curves/{created_id}",
                surface="DELETE /curves/{created_id}",
                method=COAP_DELETE,
                path="DELETE /curves/{created_id}",
                request_summary="none",
                expected_status="2.02",
                actual_status="NOT TESTED",
                response_summary="",
                verdict="NOT TESTED",
                note="no test curve created",
            )
        )
        return
    if state.created_curve_id == state.first_curve_id:
        # POST /curves uses f_curve_upsert which treats id 0 as "update
        # existing", so the test curve can overwrite the pre-existing curve
        # (live device assigned id 0). Never delete it here; the final POST
        # /config import restores the pre-run snapshot.
        state.results.append(
            CaseResult(
                label="DELETE /curves/{created_id}",
                surface="DELETE /curves/{created_id}",
                method=COAP_DELETE,
                path="DELETE /curves/{created_id}",
                request_summary="none",
                expected_status="2.02",
                actual_status="NOT TESTED",
                response_summary="",
                verdict="NOT TESTED",
                note="test curve reused the pre-existing curve slot; skipped (config restore handles it)",
            )
        )
        return
    path = f"/curves/{state.created_curve_id}"
    code, payload = do_request(COAP_DELETE, path)
    state.results.append(
        _status_case(
            f"DELETE /curves/{state.created_curve_id}",
            f"DELETE /curves/{state.created_curve_id}",
            path,
            "2.02",
            COAP_DELETE,
            code,
            payload,
            "none",
        )
    )


def run_delete_schedule(state: RunState) -> None:
    """Delete the test-created schedule (R48)."""
    if state.created_schedule_id is None:
        state.results.append(
            CaseResult(
                label="DELETE /schedules/{created_id}",
                surface="DELETE /schedules/{created_id}",
                method=COAP_DELETE,
                path="DELETE /schedules/{created_id}",
                request_summary="none",
                expected_status="2.02",
                actual_status="NOT TESTED",
                response_summary="",
                verdict="NOT TESTED",
                note="no test schedule created",
            )
        )
        return
    path = f"/schedules/{state.created_schedule_id}"
    code, payload = do_request(COAP_DELETE, path)
    state.results.append(
        _status_case(
            f"DELETE /schedules/{state.created_schedule_id}",
            f"DELETE /schedules/{state.created_schedule_id}",
            path,
            "2.02",
            COAP_DELETE,
            code,
            payload,
            "none",
        )
    )


# ============================================================
# Phase 8 — destructive phase and state restoration (R49-R55)
# ============================================================


def wait_for_device(state: RunState, timeout_s: int = 60) -> bool:
    """Probe GET /system/info until the device responds or timeout_s elapses.

    Tolerates the connection drop that follows a reboot-capable endpoint's
    2.04: the drop is transient, and re-probing until the device returns is
    the recovery (R50). Each probe uses a 2 s socket timeout; between failed
    probes the loop sleeps 2 s.
    """
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        # A reboot window can surface ICMP port-unreachable as WSAECONNRESET
        # (ConnectionResetError) on Windows, which CoAPTransport.request does not
        # swallow; treat any probe exception as a failed probe and keep waiting.
        try:
            code, _ = do_request(COAP_GET, "/system/info", timeout=2)
        except Exception:
            code = None
        if code is not None:
            return True
        time.sleep(2)
    return False


def run_config_import(state: RunState) -> None:
    """POST /config: re-import the pre-run snapshot, tolerate the reboot (R51/R50).

    Sends the exact pre-run ConfigFile snapshot back to the device, verifies
    the 2.04 StatusResponse has ok==True, then waits for the device to return
    after the ~2 s post-import reboot.
    """
    if state.pre_run_config is None:
        state.results.append(
            CaseResult(
                label="POST /config",
                surface="POST /config",
                method=COAP_POST,
                path="/config",
                request_summary="none",
                expected_status="2.04",
                actual_status="NOT TESTED",
                response_summary="(skipped)",
                verdict="NOT TESTED",
                note="no pre-run config snapshot",
            )
        )
        return
    req = pb.ConfigFile()
    req.CopyFrom(state.pre_run_config)
    req_summary = (
        f"ConfigFile{{version={req.version!r}, fans={len(req.fans.fans)}, "
        f"sources={len(req.sources.sources)}, curves={len(req.curves.curves)}, "
        f"schedules={len(req.schedules.schedules)}}}"
    )
    code, payload = do_request(COAP_POST, "/config", req)
    summary = ""
    actual = code_str(code)
    verdict = "FAIL"
    note: str | None = None
    if code == 0x44:
        try:
            st = pb.StatusResponse()
            st.ParseFromString(payload)
        except Exception as exc:
            summary = f"decode error: {exc}"
        else:
            summary = _status_summary(payload)
            verdict = "PASS" if st.ok else "FAIL"
            if not wait_for_device(state):
                verdict = "FAIL"
                note = "device did not return within timeout"
                summary = f"{summary}; device did not return within timeout"
            else:
                summary = f"{summary}; device returned after reboot"
    elif code in (0x80, 0x50):  # 4.00 / 5.00
        summary = _status_summary(payload)
    elif code is None:
        actual = "TIMEOUT"
        summary = "TIMEOUT"
    else:
        summary = payload.hex() if payload else "(empty payload)"
    state.results.append(
        CaseResult(
            label="POST /config",
            surface="POST /config",
            method=COAP_POST,
            path="/config",
            request_summary=req_summary,
            expected_status="2.04",
            actual_status=actual,
            response_summary=summary,
            verdict=verdict,
            note=note,
        )
    )


def run_system_reboot(state: RunState) -> None:
    """POST /system/reboot: reboot the device, tolerate the drop (R52/R53/R50).

    A 2.04 with StatusResponse.ok==True and a 5.03 carrying
    error_msg == "reboot pending" are both valid outcomes. Either path waits
    for the device to return after the reboot (R50); a device that never
    returns flips the verdict to FAIL.
    """
    code, payload = do_request(COAP_POST, "/system/reboot")
    summary = ""
    actual = code_str(code)
    verdict = "FAIL"
    note: str | None = None
    if code == 0x44:
        try:
            st = pb.StatusResponse()
            st.ParseFromString(payload)
        except Exception as exc:
            summary = f"decode error: {exc}"
        else:
            summary = _status_summary(payload)
            verdict = "PASS" if st.ok else "FAIL"
            if not wait_for_device(state):
                verdict = "FAIL"
                note = "device did not return within timeout"
                summary = f"{summary}; device did not return within timeout"
            else:
                summary = f"{summary}; device returned"
    elif code == 0xA3:  # 5.03
        actual = "5.03"
        try:
            st = pb.StatusResponse()
            st.ParseFromString(payload)
        except Exception as exc:
            summary = f"decode error: {exc}"
        else:
            if st.error_msg == "reboot pending":
                summary = _status_summary(payload)
                verdict = "PASS"
                if not wait_for_device(state):
                    verdict = "FAIL"
                    note = "device did not return within timeout"
                    summary = f"{summary}; device did not return within timeout"
                else:
                    summary = f"{summary}; device returned after pending reboot"
            else:
                summary = f"error_msg={st.error_msg!r} (not 'reboot pending')"
    elif code is None:
        actual = "TIMEOUT"
        summary = "TIMEOUT"
    else:
        summary = payload.hex() if payload else "(empty payload)"
    state.results.append(
        CaseResult(
            label="POST /system/reboot",
            surface="POST /system/reboot",
            method=COAP_POST,
            path="/system/reboot",
            request_summary="none",
            expected_status="2.04 or 5.03",
            actual_status=actual,
            response_summary=summary,
            verdict=verdict,
            note=note,
        )
    )


def _config_content_matches(live: pb.ConfigFile, expected: pb.ConfigFile) -> bool:
    """True when live fan/source/curve/schedule content equals expected.

    Compares entity counts and per-entity persisted-config field values. The
    version field is not compared. FanInfo mode/duty/rpm/alarm and SourceInfo
    status/temp_c are live-measured fields that GET /config exports via
    espfm_conv.c and differ from the snapshot after a reboot (e.g. rpm>0 when
    the fan runs, alarm state, AUTO-mode duty, source status), so only the
    persisted config fields are compared (R55).
    """
    if len(live.fans.fans) != len(expected.fans.fans):
        return False
    for live_fan, exp_fan in zip(live.fans.fans, expected.fans.fans):
        for field in (
            "id", "name", "enabled", "inverted", "pwm_gpio", "tach_gpio",
            "source_id", "curve_id", "schedule_id", "group_id",
        ):
            if getattr(live_fan, field) != getattr(exp_fan, field):
                return False
    if len(live.sources.sources) != len(expected.sources.sources):
        return False
    for live_src, exp_src in zip(live.sources.sources, expected.sources.sources):
        for field in ("id", "name", "type", "gpio", "ds18b20_rom_code"):
            if getattr(live_src, field) != getattr(exp_src, field):
                return False
    if len(live.curves.curves) != len(expected.curves.curves):
        return False
    for live_curve, exp_curve in zip(live.curves.curves, expected.curves.curves):
        if live_curve != exp_curve:
            return False
    if len(live.schedules.schedules) != len(expected.schedules.schedules):
        return False
    for live_sched, exp_sched in zip(
        live.schedules.schedules, expected.schedules.schedules
    ):
        if live_sched != exp_sched:
            return False
    return True


def run_config_verify(state: RunState) -> None:
    """GET /config (verify): confirm config matches the pre-run snapshot (R55).

    Must run immediately before POST /wifi/connect, which may drop
    connectivity.
    """
    if state.pre_run_config is None:
        state.results.append(
            CaseResult(
                label="GET /config (verify)",
                surface="GET /config (verify)",
                method=COAP_GET,
                path="/config",
                request_summary="none",
                expected_status="2.05",
                actual_status="NOT TESTED",
                response_summary="(skipped)",
                verdict="NOT TESTED",
                note="no pre-run config snapshot",
            )
        )
        return
    code, payload = do_request(COAP_GET, "/config")
    summary = ""
    actual = code_str(code)
    verdict = "FAIL"
    if code == 0x45:
        try:
            live = pb.ConfigFile()
            live.ParseFromString(payload)
        except Exception as exc:
            summary = f"decode error: {exc}"
        else:
            summary = (
                f"version={live.version!r}, fans={len(live.fans.fans)}, "
                f"sources={len(live.sources.sources)}, "
                f"curves={len(live.curves.curves)}, "
                f"schedules={len(live.schedules.schedules)}"
            )
            if _config_content_matches(live, state.pre_run_config):
                verdict = "PASS"
            else:
                summary = f"{summary}; content mismatch vs pre-run snapshot"
    elif code is None:
        actual = "TIMEOUT"
        summary = "TIMEOUT"
    else:
        summary = payload.hex() if payload else "(empty payload)"
    state.results.append(
        CaseResult(
            label="GET /config (verify)",
            surface="GET /config (verify)",
            method=COAP_GET,
            path="/config",
            request_summary="none",
            expected_status="2.05",
            actual_status=actual,
            response_summary=summary,
            verdict=verdict,
        )
    )


def run_wifi_connect(state: RunState) -> None:
    """POST /wifi/connect with WIFI_SSID/WIFI_PASSWORD env credentials (R54/R49).

    The final device endpoint of the run. Records 2.04, 5.03, or TIMEOUT as
    valid outcomes. Credential values are carried only in request_summary so
    the report evidences them without printing them to stdout.
    """
    ssid = os.environ.get("WIFI_SSID", "")
    password = os.environ.get("WIFI_PASSWORD", "")
    req = pb.WifiConnectRequest(ssid=ssid, password=password)
    if ssid == "" and password == "":
        req_summary = "WifiConnectRequest{ssid=\"\", password=\"\"} (empty credentials)"
    else:
        req_summary = f"WifiConnectRequest{{ssid={ssid!r}, password={password!r}}}"
    code, payload = do_request(COAP_POST, "/wifi/connect", req)
    summary = ""
    actual = code_str(code)
    verdict = "FAIL"
    if code == 0x44:
        try:
            st = pb.StatusResponse()
            st.ParseFromString(payload)
        except Exception as exc:
            summary = f"decode error: {exc}"
        else:
            summary = _status_summary(payload)
            verdict = "PASS" if st.ok else "FAIL"
    elif code == 0xA3:  # 5.03 set-config fail
        actual = "5.03"
        summary = _status_summary(payload)
        verdict = "PASS"
    elif code is None:  # timeout — device may have dropped CoAP/STA
        actual = "TIMEOUT"
        summary = "TIMEOUT"
        verdict = "PASS"
    else:
        summary = payload.hex() if payload else "(empty payload)"
    state.results.append(
        CaseResult(
            label="POST /wifi/connect",
            surface="POST /wifi/connect",
            method=COAP_POST,
            path="/wifi/connect",
            request_summary=req_summary,
            expected_status="2.04 or 5.03 or TIMEOUT",
            actual_status=actual,
            response_summary=summary,
            verdict=verdict,
        )
    )


def main() -> None:
    """Entry point: parse args, probe the device, run tests, write the report."""
    load_env_file()
    args = parse_args(sys.argv[1:])
    state = RunState(args.host, args.port, args.output)
    print(f"Target {state.host}:{state.port}, report -> {state.output_path}")

    if not probe_device(state.host, state.port):
        print(f"No ESPFM device reachable at {state.host}:{state.port} - skipping device tests.")
        sys.exit(0)

    global transport
    transport = CoAPTransport(state.host, state.port, timeout=3)
    transport.connect()

    try:
        run_system_info(state)
        # phase 2 — discovery, list reads, config snapshot
        run_fans_list(state)
        discover_live_fan(state)
        run_sources_list(state)
        run_curves_list(state)
        run_schedules_list(state)
        run_config_snapshot(state)
        # phase 3 — read-only endpoints (wifi, ds18b20, control)
        run_wifi_status(state)
        run_wifi_scan(state)
        run_ds18b20_scan(state)
        run_control_originals(state)
        # phase 4 — create phase (fan, source, curve, schedule, manual-temp, ds18b20 config)
        discover_free_pins(state)
        run_create_fan(state)
        run_create_source(state)
        run_create_curve(state)
        run_create_schedule(state)
        run_manual_temp(state)
        run_ds18b20_config(state)
        # phase 5 — item-level reads + error-path GET
        run_fan_get(state)
        run_source_get(state)
        run_curve_get(state)
        run_schedule_get(state)
        run_error_path_get(state)
        # phase 6 — update phase (fan, source, curve, schedule, control)
        run_fan_update(state)
        run_source_update(state)
        run_curve_update(state)
        run_schedule_update(state)
        run_control_restore(state)
        run_control_error(state)
        # phase 7 — hostname update + cleanup deletes
        run_hostname_test(state)
        run_hostname_restore(state)
        run_delete_fan(state)
        run_delete_source(state)
        run_delete_curve(state)
        run_delete_schedule(state)
        # phase 8 — destructive (config-verify runs before wifi-connect)
        run_config_import(state)
        run_system_reboot(state)
        run_config_verify(state)
        run_wifi_connect(state)
        write_report(state)
    finally:
        transport.close()


if __name__ == "__main__":
    main()
