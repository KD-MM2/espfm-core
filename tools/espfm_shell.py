#!/usr/bin/env python3
"""
ESPFM Interactive Shell — CoAP+Protobuf client for ESPFanManager v3.

Usage:
    python espfm_shell.py
    python espfm_shell.py --host 192.168.4.1 --port 5683 --timeout 3

Dependencies:
    pip install protobuf rich prompt_toolkit
"""

from __future__ import annotations

import argparse
import datetime
import json
import socket
import struct
import sys
import time
from pathlib import Path
from typing import Any, Optional

try:
    import rich
except ImportError:
    print("Missing dependency: rich. Install with `pip install rich`.")
    sys.exit(1)

try:
    import prompt_toolkit
except ImportError:
    print("Missing dependency: prompt_toolkit. Install with `pip install prompt_toolkit`.")
    sys.exit(1)

try:
    import zeroconf
except ImportError:
    print("Missing dependency: zeroconf. Install with `pip install zeroconf`.")
    sys.exit(1)

from rich.console import Console
from rich.panel import Panel
from rich.table import Table

# Protobuf message classes — generated from espfm.proto
import espfm_pb2 as pb

# prompt_toolkit for REPL
from prompt_toolkit import PromptSession
from prompt_toolkit.completion import WordCompleter
from prompt_toolkit.history import InMemoryHistory

try:
    from zeroconf import Zeroconf, ServiceBrowser, ServiceListener
    HAS_ZEROCONF = True
except ImportError:
    HAS_ZEROCONF = False

console = Console()

# ============================================================
# CoAP constants
# ============================================================

COAP_GET = 1
COAP_POST = 2
COAP_PUT = 3
COAP_DELETE = 4

COAP_CODES: dict[int, str] = {
    0x41: "2.01 Created",
    0x42: "2.02 Deleted",
    0x44: "2.04 Changed",
    0x45: "2.05 Content",
    0x80: "4.00 Bad Request",
    0x84: "4.04 Not Found",
    0xA3: "5.03 Service Unavailable",
}

# ============================================================
# Enum display maps
# ============================================================

FAN_MODE_LABELS: dict[int, str] = {0: "manual", 1: "auto"}
SOURCE_TYPE_LABELS: dict[int, str] = {0: "ntc", 1: "ds18b20", 2: "manual"}
SOURCE_STATUS_LABELS: dict[int, str] = {0: "valid", 1: "stale", 2: "invalid"}
FAN_ALARM_LABELS: dict[int, str] = {0: "none", 1: "stall", 2: "overtemp"}

# Reverse maps for input parsing
FAN_MODE_VALUES: dict[str, int] = {"manual": 0, "auto": 1, "0": 0, "1": 1}
SOURCE_TYPE_VALUES: dict[str, int] = {
    "ntc": 0, "ds18b20": 1, "manual": 2, "0": 0, "1": 1, "2": 2,
}
SOURCE_STATUS_VALUES: dict[str, int] = {
    "valid": 0, "stale": 1, "invalid": 2, "0": 0, "1": 1, "2": 2,
}
FAN_ALARM_VALUES: dict[str, int] = {
    "none": 0, "stall": 1, "overtemp": 2, "0": 0, "1": 1, "2": 2,
}

# WiFi auth mode labels
WIFI_AUTH_LABELS: dict[int, str] = {
    0: "OPEN", 1: "WEP", 2: "WPA_PSK", 3: "WPA2_PSK",
    4: "WPA_WPA2_PSK", 5: "ENTERPRISE", 6: "WPA3_PSK",
    7: "WPA2_WPA3_PSK",
}

# ============================================================
# CoAPTransport — raw UDP CoAP client
# ============================================================


class CoAPTransport:
    """Low-level CoAP over raw UDP."""

    def __init__(self, host: str, port: int = 5683, timeout: float = 3.0) -> None:
        self.host = host
        self.port = port
        self.timeout = timeout
        self._sock: Optional[socket.socket] = None
        self._mid: int = int(time.time() * 1000) & 0xFFFF

    @property
    def connected(self) -> bool:
        return self._sock is not None

    def connect(self) -> None:
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.settimeout(self.timeout)

    def close(self) -> None:
        if self._sock:
            self._sock.close()
            self._sock = None

    def _next_mid(self) -> int:
        self._mid = (self._mid + 1) & 0xFFFF
        return self._mid

    def request(
        self, method: int, path: str, payload: bytes = b""
    ) -> tuple[Optional[int], Optional[bytes]]:
        """Send CoAP request. Returns (status_code, payload_bytes) or (None, None) on timeout."""
        if self._sock is None:
            raise RuntimeError("Not connected")

        segments = [s for s in path.split("/") if s]
        token = struct.pack(">I", int(time.time() * 1e6) & 0xFFFFFFFF)[:4]
        mid = self._next_mid()

        # Header: ver=1, type=CON(0), token_len=4
        header = bytes([
            0x40 | len(token),
            method,
            (mid >> 8) & 0xFF,
            mid & 0xFF,
        ]) + token

        # URI-Path options (number 11)
        for i, seg in enumerate(segments):
            delta = 11 if i == 0 else 0
            header += bytes([(delta << 4) | len(seg)]) + seg.encode()

        if payload:
            header += b"\xff" + payload

        self._sock.sendto(header, (self.host, self.port))

        try:
            data, _ = self._sock.recvfrom(4096)
        except socket.timeout:
            return None, None

        code = data[1]
        # Skip options to find payload marker
        pos = 4 + (data[0] & 0x0F)  # skip header + token
        while pos < len(data) and data[pos] != 0xFF:
            dl = data[pos]
            pos += 1
            delta_ext = (dl >> 4) & 0xF
            length_ext = dl & 0xF
            if delta_ext == 13:
                pos += 1
            elif delta_ext == 14:
                pos += 2
            if length_ext == 13:
                length_ext = data[pos] + 13
                pos += 1
            elif length_ext == 14:
                length_ext = (data[pos] << 8) + data[pos + 1] + 269
                pos += 2
            pos += length_ext

        response_payload = data[pos + 1:] if pos < len(data) and data[pos] == 0xFF else b""
        return code, response_payload


# ============================================================
# ESPFMClient — CoAP + Protobuf
# ============================================================


class CoapError(Exception):
    """Raised when a CoAP request fails with an error status."""

    def __init__(self, code: int, payload: bytes = b"") -> None:
        self.code = code
        self.payload = payload
        self.status_str = COAP_CODES.get(code, f"{code >> 5}.{code & 0x1F:02d}")
        super().__init__(self.status_str)


class ESPFMClient:
    """High-level client wrapping CoAP transport and protobuf encode/decode."""

    def __init__(self, transport: CoAPTransport) -> None:
        self.transport = transport

    # -- Low-level request -------------------------------------------------

    def _request(
        self, method: int, path: str, msg: Any = None
    ) -> tuple[Optional[int], bytes]:
        """Send a protobuf-encoded CoAP request. Returns (code, raw_payload)."""
        payload = msg.SerializeToString() if msg is not None else b""
        code, data = self.transport.request(method, path, payload)
        if code is None:
            raise CoapError(0, b"")
        if code >= 0x80:
            raise CoapError(code, data or b"")
        return code, data or b""

    def _decode(self, msg_cls, data: bytes):
        """Decode protobuf response, raising CoapError on failure."""
        try:
            msg = msg_cls()
            msg.ParseFromString(data)
            return msg
        except Exception:
            raise CoapError(0, f'Decode failed ({len(data)} bytes)'.encode())

    def _get(self, path: str) -> tuple[Optional[int], bytes]:
        return self._request(COAP_GET, path)

    def _post(self, path: str, msg: Any = None) -> tuple[Optional[int], bytes]:
        return self._request(COAP_POST, path, msg)

    def _put(self, path: str, msg: Any = None) -> tuple[Optional[int], bytes]:
        return self._request(COAP_PUT, path, msg)

    def _delete(self, path: str) -> tuple[Optional[int], bytes]:
        return self._request(COAP_DELETE, path)

    # -- Fans --------------------------------------------------------------

    def fans_list(self) -> pb.FanList:
        _, data = self._get("/fans")
        return self._decode(pb.FanList, data)

    def fans_get(self, fan_id: int) -> pb.FanInfo:
        _, data = self._get(f"/fans/{fan_id}")
        return self._decode(pb.FanInfo, data)

    def fans_create(self, pwm_gpio: int, name: str, tach_gpio: int = 255) -> pb.FanInfo:
        req = pb.FanCreateRequest(pwm_gpio=pwm_gpio, name=name, tach_gpio=tach_gpio)
        _, data = self._post("/fans", req)
        return self._decode(pb.FanInfo, data)

    def fans_update(self, fan_id: int, **kwargs: Any) -> pb.FanInfo:
        req = pb.FanUpdateRequest(id=fan_id)
        field_map = {
            "mode": "mode", "duty": "duty", "source_id": "source_id",
            "curve_id": "curve_id", "schedule_id": "schedule_id",
            "group_id": "group_id", "inverted": "inverted",
            "enabled": "enabled",
        }
        for key, field in field_map.items():
            if key in kwargs:
                setattr(req, field, kwargs[key])
        _, data = self._put(f"/fans/{fan_id}", req)
        return self._decode(pb.FanInfo, data)

    def fans_delete(self, fan_id: int) -> pb.StatusResponse:
        _, data = self._delete(f"/fans/{fan_id}")
        return self._decode(pb.StatusResponse, data)

    # -- Sources -----------------------------------------------------------

    def sources_list(self) -> pb.SourceList:
        _, data = self._get("/sources")
        return self._decode(pb.SourceList, data)

    def sources_get(self, source_id: int) -> pb.SourceInfo:
        _, data = self._get(f"/sources/{source_id}")
        return self._decode(pb.SourceInfo, data)

    def sources_create(
        self, source_type: int, name: str, gpio: int = 255
    ) -> pb.SourceInfo:
        req = pb.SourceCreateRequest(type=source_type, name=name, gpio=gpio)
        _, data = self._post("/sources", req)
        return self._decode(pb.SourceInfo, data)

    def sources_set_temp(self, source_id: int, temp_c: float) -> pb.StatusResponse:
        req = pb.ManualTempRequest(id=source_id, temp_c=temp_c)
        _, data = self._post("/sources/temp", req)
        return self._decode(pb.StatusResponse, data)

    def sources_update(self, source_id: int, name: str) -> pb.SourceInfo:
        req = pb.SourceUpdateRequest(id=source_id, name=name)
        _, data = self._put(f"/sources/{source_id}", req)
        return self._decode(pb.SourceInfo, data)

    def sources_delete(self, source_id: int) -> pb.StatusResponse:
        _, data = self._delete(f"/sources/{source_id}")
        return self._decode(pb.StatusResponse, data)

    # -- Curves ------------------------------------------------------------

    def curves_list(self) -> pb.CurveList:
        _, data = self._get("/curves")
        return self._decode(pb.CurveList, data)

    def curves_get(self, curve_id: int) -> pb.CurveInfo:
        _, data = self._get(f"/curves/{curve_id}")
        return self._decode(pb.CurveInfo, data)

    def curves_create(self, name: str, points: list[tuple[float, int]]) -> pb.CurveInfo:
        req = pb.CurveCreateRequest(name=name)
        for temp_c, duty in points:
            req.points.append(pb.CurvePoint(temp_c=temp_c, duty=duty))
        _, data = self._post("/curves", req)
        return self._decode(pb.CurveInfo, data)

    def curves_update(
        self, curve_id: int, name: str = "", points: list[tuple[float, int]] | None = None
    ) -> pb.CurveInfo:
        req = pb.CurveUpdateRequest(id=curve_id)
        if name:
            req.name = name
        if points:
            for temp_c, duty in points:
                req.points.append(pb.CurvePoint(temp_c=temp_c, duty=duty))
        _, data = self._put(f"/curves/{curve_id}", req)
        return self._decode(pb.CurveInfo, data)

    def curves_delete(self, curve_id: int) -> pb.StatusResponse:
        _, data = self._delete(f"/curves/{curve_id}")
        return self._decode(pb.StatusResponse, data)

    # -- Schedules ---------------------------------------------------------

    def schedules_list(self) -> pb.ScheduleList:
        _, data = self._get("/schedules")
        return self._decode(pb.ScheduleList, data)

    def schedules_create(
        self, fan_id: int, duty: int, start_min: int, end_min: int, enabled: bool = True
    ) -> pb.ScheduleInfo:
        req = pb.ScheduleCreateRequest(
            fan_id=fan_id, duty=duty, start_min=start_min,
            end_min=end_min, enabled=enabled,
        )
        _, data = self._post("/schedules", req)
        return self._decode(pb.ScheduleInfo, data)

    def schedules_update(self, schedule_id: int, **kwargs: Any) -> pb.ScheduleInfo:
        req = pb.ScheduleUpdateRequest(id=schedule_id)
        field_map = {
            "fan_id": "fan_id", "duty": "duty",
            "start_min": "start_min", "end_min": "end_min", "enabled": "enabled",
        }
        for key, field in field_map.items():
            if key in kwargs:
                setattr(req, field, kwargs[key])
        _, data = self._put(f"/schedules/{schedule_id}", req)
        return self._decode(pb.ScheduleInfo, data)

    def schedules_delete(self, schedule_id: int) -> pb.StatusResponse:
        _, data = self._delete(f"/schedules/{schedule_id}")
        return self._decode(pb.StatusResponse, data)

    # -- WiFi --------------------------------------------------------------

    def wifi_scan(self) -> pb.WifiScanResult:
        # Scan blocks ~3.5s on device, temporarily increase timeout
        sock = self.transport._sock
        old_timeout = sock.gettimeout()
        sock.settimeout(10.0)
        try:
            _, data = self._get("/wifi/scan")
            return self._decode(pb.WifiScanResult, data)
        finally:
            sock.settimeout(old_timeout)

    def wifi_status(self) -> pb.WifiStatus:
        _, data = self._get("/wifi/status")
        return self._decode(pb.WifiStatus, data)

    def wifi_connect(self, ssid: str, password: str) -> pb.StatusResponse:
        req = pb.WifiConnectRequest(ssid=ssid, password=password)
        _, data = self._post("/wifi/connect", req)
        return self._decode(pb.StatusResponse, data)

    # -- System ------------------------------------------------------------

    def system_info(self) -> pb.SystemInfo:
        _, data = self._get("/system/info")
        return self._decode(pb.SystemInfo, data)

    def system_set_hostname(self, hostname: str) -> pb.StatusResponse:
        req = pb.HostnameRequest(hostname=hostname)
        _, data = self._put("/system/hostname", req)
        return self._decode(pb.StatusResponse, data)

    def system_reboot(self) -> pb.StatusResponse:
        _, data = self._post("/system/reboot")
        return self._decode(pb.StatusResponse, data)

    # -- DS18B20 -----------------------------------------------------------

    def ds18b20_config(self, gpio: int) -> pb.StatusResponse:
        req = pb.Ds18b20ConfigRequest(gpio=gpio)
        _, data = self._post("/ds18b20/config", req)
        return self._decode(pb.StatusResponse, data)


# ============================================================
# Utility helpers
# ============================================================


def _fmt_minutes(m: int) -> str:
    """Convert minutes-since-midnight to HH:MM."""
    return f"{m // 60:02d}:{m % 60:02d}"


def _parse_points(s: str) -> list[tuple[float, int]]:
    """Parse 'temp:duty,temp:duty,...' into list of (temp_c, duty)."""
    points: list[tuple[float, int]] = []
    for pair in s.split(","):
        parts = pair.strip().split(":")
        if len(parts) != 2:
            raise ValueError(f"Invalid point format: '{pair}' (expected temp:duty)")
        points.append((float(parts[0]), int(parts[1])))
    return points


def _parse_bool(s: str) -> bool:
    """Parse a boolean from user input."""
    return s.lower() in ("true", "1", "yes")


def _resolve_enum(value: str, labels: dict[str, int]) -> int:
    """Resolve a string to an enum value, accepting both labels and integers."""
    key = value.lower()
    if key in labels:
        return labels[key]
    try:
        iv = int(value)
        if iv in labels.values():
            return iv
    except ValueError:
        pass
    raise ValueError(f"Invalid value: '{value}'. Valid: {', '.join(labels.keys())}")


def _error_message(e: CoapError) -> str:
    """Format a CoAP error into a human-readable message."""
    if e.code == 0:
        return "Request timed out. Device unreachable."
    if e.code == 0x80:
        # Try to decode StatusResponse for error_msg
        if e.payload:
            try:
                sr = pb.StatusResponse()
                sr.ParseFromString(e.payload)
                if sr.error_msg:
                    return f"Bad request: {sr.error_msg}"
            except Exception:
                pass
        return "Bad request (4.00)."
    if e.code == 0x84:
        return "Not found: resource does not exist (4.04)."
    if e.code == 0xA3:
        return "Service unavailable (5.03)."
    return f"CoAP error: {e.status_str}"


# ============================================================
# Command handlers
# ============================================================


def _check_connected(shell: ESPFMShell) -> bool:
    """Return True if connected, print error and return False otherwise."""
    if shell.client is None:
        console.print("[red]Not connected. Use `connect <host>` first.[/red]")
        return False
    return True


def _handle_fans(shell: ESPFMShell, args: list[str]) -> None:
    if not args:
        console.print("[yellow]Usage: fans <list|get|create|update|delete> ...[/yellow]")
        return
    action = args[0]
    if not _check_connected(shell):
        return
    client = shell.client

    try:
        if action == "list":
            result = client.fans_list()
            if not result.fans:
                console.print("[dim]No fans configured.[/dim]")
                return
            table = Table(title="Fans")
            table.add_column("ID", justify="right")
            table.add_column("Name")
            table.add_column("Mode")
            table.add_column("Duty %", justify="right")
            table.add_column("RPM", justify="right")
            table.add_column("Enabled")
            table.add_column("Inverted")
            table.add_column("PWM GPIO", justify="right")
            table.add_column("Tach GPIO", justify="right")
            table.add_column("Source", justify="right")
            table.add_column("Curve", justify="right")
            table.add_column("Schedule", justify="right")
            table.add_column("Group", justify="right")
            table.add_column("Alarm")
            for f in result.fans:
                table.add_row(
                    str(f.id), f.name, FAN_MODE_LABELS.get(f.mode, str(f.mode)),
                    str(f.duty), str(f.rpm),
                    "yes" if f.enabled else "no",
                    "yes" if f.inverted else "no",
                    str(f.pwm_gpio),
                    str(f.tach_gpio) if f.tach_gpio != 255 else "-",
                    str(f.source_id) if f.source_id != 255 else "-",
                    str(f.curve_id) if f.curve_id != 255 else "-",
                    str(f.schedule_id) if f.schedule_id != 255 else "-",
                    str(f.group_id),
                    FAN_ALARM_LABELS.get(f.alarm, str(f.alarm)),
                )
            console.print(table)

        elif action == "get":
            if len(args) < 2:
                console.print("[yellow]Usage: fans get <id>[/yellow]")
                return
            fan_id = int(args[1])
            f = client.fans_get(fan_id)
            lines = [
                f"[bold]Fan {f.id}[/bold]",
                f"  Name:      {f.name}",
                f"  Mode:      {FAN_MODE_LABELS.get(f.mode, str(f.mode))}",
                f"  Duty:      {f.duty}%",
                f"  RPM:       {f.rpm}",
                f"  Enabled:   {'yes' if f.enabled else 'no'}",
                f"  Inverted:  {'yes' if f.inverted else 'no'}",
                f"  PWM GPIO:  {f.pwm_gpio}",
                f"  Tach GPIO: {f.tach_gpio if f.tach_gpio != 255 else 'none'}",
                f"  Source:    {f.source_id if f.source_id != 255 else 'none'}",
                f"  Curve:     {f.curve_id if f.curve_id != 255 else 'none'}",
                f"  Schedule:  {f.schedule_id if f.schedule_id != 255 else 'none'}",
                f"  Group:     {f.group_id}",
                f"  Alarm:     {FAN_ALARM_LABELS.get(f.alarm, str(f.alarm))}",
            ]
            console.print(Panel("\n".join(lines), title=f"Fan {f.id}"))

        elif action == "create":
            flags = _parse_flags(args[1:])
            if "pwm" not in flags or "name" not in flags:
                console.print(
                    "[yellow]Usage: fans create --pwm <gpio> --name <name> [--tach <gpio>] "
                    "[--source N] [--curve N] [--mode auto|manual] [--inverted true|false] "
                    "[--group N] [--enabled true|false][/yellow]"
                )
                return
            f = client.fans_create(
                pwm_gpio=int(flags["pwm"]),
                name=flags["name"],
                tach_gpio=int(flags.get("tach", 255)),
            )
            # Apply optional flags via update
            kwargs: dict[str, Any] = {}
            if "source" in flags:
                kwargs["source_id"] = int(flags["source"])
            if "curve" in flags:
                kwargs["curve_id"] = int(flags["curve"])
            if "schedule" in flags:
                kwargs["schedule_id"] = int(flags["schedule"])
            if "mode" in flags:
                kwargs["mode"] = _resolve_enum(flags["mode"], FAN_MODE_VALUES)
            if "inverted" in flags:
                kwargs["inverted"] = _parse_bool(flags["inverted"])
            if "group" in flags:
                kwargs["group_id"] = int(flags["group"])
            if "enabled" in flags:
                kwargs["enabled"] = _parse_bool(flags["enabled"])
            if kwargs:
                f = client.fans_update(f.id, **kwargs)
            console.print(f"[green]Created fan {f.id}:[/green] {f.name} (PWM GPIO {f.pwm_gpio})")

        elif action == "update":
            if len(args) < 2:
                console.print("[yellow]Usage: fans update <id> [--duty N] [--mode auto|manual] ...[/yellow]")
                return
            fan_id = int(args[1])
            flags = _parse_flags(args[2:])
            kwargs: dict[str, Any] = {}
            if "duty" in flags:
                kwargs["duty"] = int(flags["duty"])
            if "mode" in flags:
                kwargs["mode"] = _resolve_enum(flags["mode"], FAN_MODE_VALUES)
            if "source" in flags:
                kwargs["source_id"] = int(flags["source"])
            if "curve" in flags:
                kwargs["curve_id"] = int(flags["curve"])
            if "schedule" in flags:
                kwargs["schedule_id"] = int(flags["schedule"])
            if "group" in flags:
                kwargs["group_id"] = int(flags["group"])
            if "inverted" in flags:
                kwargs["inverted"] = _parse_bool(flags["inverted"])
            if "enabled" in flags:
                kwargs["enabled"] = _parse_bool(flags["enabled"])
            if not kwargs:
                console.print("[yellow]No fields to update.[/yellow]")
                return
            f = client.fans_update(fan_id, **kwargs)
            console.print(f"[green]Updated fan {f.id}:[/green] {f.name}")

        elif action == "enable":
            if len(args) < 2:
                console.print("[yellow]Usage: fans enable <id>[/yellow]")
                return
            fan_id = int(args[1])
            f = client.fans_update(fan_id, enabled=True)
            console.print(f"[green]Enabled fan {f.id}.[/green]")

        elif action == "disable":
            if len(args) < 2:
                console.print("[yellow]Usage: fans disable <id>[/yellow]")
                return
            fan_id = int(args[1])
            f = client.fans_update(fan_id, enabled=False)
            console.print(f"[green]Disabled fan {f.id}.[/green]")

        elif action == "delete":
            if len(args) < 2:
                console.print("[yellow]Usage: fans delete <id>[/yellow]")
                return
            fan_id = int(args[1])
            client.fans_delete(fan_id)
            console.print(f"[green]Deleted fan {fan_id}.[/green]")

        else:
            console.print(f"[yellow]Unknown fans action: {action}[/yellow]")

    except CoapError as e:
        console.print(f"[red]{_error_message(e)}[/red]")
    except (ValueError, IndexError) as e:
        console.print(f"[red]Invalid argument: {e}[/red]")


def _handle_sources(shell: ESPFMShell, args: list[str]) -> None:
    if not args:
        console.print("[yellow]Usage: sources <list|get|create|update|temp|delete> ...[/yellow]")
        return
    action = args[0]
    if not _check_connected(shell):
        return
    client = shell.client

    try:
        if action == "list":
            result = client.sources_list()
            if not result.sources:
                console.print("[dim]No sources configured.[/dim]")
                return
            table = Table(title="Sources")
            table.add_column("ID", justify="right")
            table.add_column("Name")
            table.add_column("Type")
            table.add_column("Status")
            table.add_column("Temp (C)", justify="right")
            table.add_column("GPIO", justify="right")
            for s in result.sources:
                table.add_row(
                    str(s.id), s.name,
                    SOURCE_TYPE_LABELS.get(s.type, str(s.type)),
                    SOURCE_STATUS_LABELS.get(s.status, str(s.status)),
                    f"{s.temp_c:.1f}",
                    str(s.gpio) if s.gpio != 255 else "-",
                )
            console.print(table)

        elif action == "get":
            if len(args) < 2:
                console.print("[yellow]Usage: sources get <id>[/yellow]")
                return
            sid = int(args[1])
            s = client.sources_get(sid)
            lines = [
                f"[bold]Source {s.id}[/bold]",
                f"  Name:   {s.name}",
                f"  Type:   {SOURCE_TYPE_LABELS.get(s.type, str(s.type))}",
                f"  Status: {SOURCE_STATUS_LABELS.get(s.status, str(s.status))}",
                f"  Temp:   {s.temp_c:.1f} C",
                f"  GPIO:   {s.gpio if s.gpio != 255 else 'none'}",
            ]
            console.print(Panel("\n".join(lines), title=f"Source {s.id}"))

        elif action == "create":
            flags = _parse_flags(args[1:])
            if "type" not in flags or "name" not in flags:
                console.print(
                    "[yellow]Usage: sources create --type <ntc|ds18b20|manual> --name <name> "
                    "[--gpio N] [--rom HEX_ROM_CODE][/yellow]"
                )
                return
            source_type = _resolve_enum(flags["type"], SOURCE_TYPE_VALUES)
            # Parse ROM code for DS18B20 sources (hex string with optional colons)
            rom_code = int(flags["rom"].replace(":", ""), 16) if "rom" in flags else 0
            req = pb.SourceCreateRequest(
                type=source_type, name=flags["name"],
                gpio=int(flags.get("gpio", 255)),
            )
            if source_type == 1 and rom_code:  # 1 = ds18b20
                req.ds18b20_rom_code = rom_code
            _, data = client._post("/sources", req)
            s = client._decode(pb.SourceInfo, data)
            console.print(
                f"[green]Created source {s.id}:[/green] {s.name} ({SOURCE_TYPE_LABELS.get(s.type, '?')})"
            )

        elif action == "temp":
            if len(args) < 3:
                console.print("[yellow]Usage: sources temp <id> <temp_c>[/yellow]")
                return
            sid = int(args[1])
            temp_c = float(args[2])
            client.sources_set_temp(sid, temp_c)
            console.print(f"[green]Set source {sid} temperature to {temp_c:.1f} C.[/green]")

        elif action == "update":
            flags = _parse_flags(args[1:])
            if len(args) < 2 or "name" not in flags:
                console.print("[yellow]Usage: sources update <id> --name <name>[/yellow]")
                return
            sid = int(args[0]) if args[0].isdigit() else int(args[1])
            s = client.sources_update(sid, flags["name"])
            console.print(f"[green]Updated source {s.id}:[/green] {s.name}")

        elif action == "delete":
            if len(args) < 2:
                console.print("[yellow]Usage: sources delete <id>[/yellow]")
                return
            sid = int(args[1])
            client.sources_delete(sid)
            console.print(f"[green]Deleted source {sid}.[/green]")

        else:
            console.print(f"[yellow]Unknown sources action: {action}[/yellow]")

    except CoapError as e:
        console.print(f"[red]{_error_message(e)}[/red]")
    except (ValueError, IndexError) as e:
        console.print(f"[red]Invalid argument: {e}[/red]")


def _handle_curves(shell: ESPFMShell, args: list[str]) -> None:
    if not args:
        console.print("[yellow]Usage: curves <list|get|create|update|delete> ...[/yellow]")
        return
    action = args[0]
    if not _check_connected(shell):
        return
    client = shell.client

    try:
        if action == "list":
            result = client.curves_list()
            if not result.curves:
                console.print("[dim]No curves configured.[/dim]")
                return
            table = Table(title="Curves")
            table.add_column("ID", justify="right")
            table.add_column("Name")
            table.add_column("Points", justify="right")
            table.add_column("Details")
            for c in result.curves:
                pts = ", ".join(f"{p.temp_c:.0f}C:{p.duty}%" for p in c.points)
                table.add_row(str(c.id), c.name, str(len(c.points)), pts)
            console.print(table)

        elif action == "get":
            if len(args) < 2:
                console.print("[yellow]Usage: curves get <id>[/yellow]")
                return
            cid = int(args[1])
            c = client.curves_get(cid)
            lines = [f"[bold]Curve {c.id}[/bold]", f"  Name: {c.name}", "  Points:"]
            for i, p in enumerate(c.points):
                lines.append(f"    [{i}] {p.temp_c:.1f} C -> {p.duty}%")
            console.print(Panel("\n".join(lines), title=f"Curve {c.id}"))

        elif action == "create":
            flags = _parse_flags(args[1:])
            if "name" not in flags or "points" not in flags:
                console.print(
                    '[yellow]Usage: curves create --name <name> --points "temp:duty,temp:duty,..."[/yellow]'
                )
                return
            points = _parse_points(flags["points"])
            c = client.curves_create(flags["name"], points)
            console.print(f"[green]Created curve {c.id}:[/green] {c.name} ({len(c.points)} points)")

        elif action == "update":
            if len(args) < 2:
                console.print("[yellow]Usage: curves update <id> [--name ...] [--points ...][/yellow]")
                return
            cid = int(args[1])
            flags = _parse_flags(args[2:])
            name = flags.get("name", "")
            points = _parse_points(flags["points"]) if "points" in flags else None
            c = client.curves_update(cid, name=name, points=points)
            console.print(f"[green]Updated curve {c.id}:[/green] {c.name}")

        elif action == "delete":
            if len(args) < 2:
                console.print("[yellow]Usage: curves delete <id>[/yellow]")
                return
            cid = int(args[1])
            client.curves_delete(cid)
            console.print(f"[green]Deleted curve {cid}.[/green]")

        else:
            console.print(f"[yellow]Unknown curves action: {action}[/yellow]")

    except CoapError as e:
        console.print(f"[red]{_error_message(e)}[/red]")
    except (ValueError, IndexError) as e:
        console.print(f"[red]Invalid argument: {e}[/red]")


def _handle_schedules(shell: ESPFMShell, args: list[str]) -> None:
    if not args:
        console.print("[yellow]Usage: schedules <list|create|update|delete> ...[/yellow]")
        return
    action = args[0]
    if not _check_connected(shell):
        return
    client = shell.client

    try:
        if action == "list":
            result = client.schedules_list()
            if not result.schedules:
                console.print("[dim]No schedules configured.[/dim]")
                return
            table = Table(title="Schedules")
            table.add_column("ID", justify="right")
            table.add_column("Fan", justify="right")
            table.add_column("Duty %", justify="right")
            table.add_column("Start")
            table.add_column("End")
            table.add_column("Enabled")
            for s in result.schedules:
                table.add_row(
                    str(s.id), str(s.fan_id), str(s.duty),
                    _fmt_minutes(s.start_min), _fmt_minutes(s.end_min),
                    "yes" if s.enabled else "no",
                )
            console.print(table)

        elif action == "create":
            flags = _parse_flags(args[1:])
            required = ["fan", "duty", "start", "end"]
            missing = [k for k in required if k not in flags]
            if missing:
                console.print(
                    f"[yellow]Missing required flags: {', '.join('--' + k for k in missing)}[/yellow]"
                )
                return
            s = client.schedules_create(
                fan_id=int(flags["fan"]),
                duty=int(flags["duty"]),
                start_min=int(flags["start"]),
                end_min=int(flags["end"]),
                enabled=_parse_bool(flags.get("enabled", "true")),
            )
            console.print(
                f"[green]Created schedule {s.id}:[/green] fan {s.fan_id}, "
                f"{s.duty}%, {_fmt_minutes(s.start_min)}-{_fmt_minutes(s.end_min)}"
            )

        elif action == "update":
            if len(args) < 2:
                console.print("[yellow]Usage: schedules update <id> [--fan N] [--duty N] ...[/yellow]")
                return
            sid = int(args[1])
            flags = _parse_flags(args[2:])
            kwargs: dict[str, Any] = {}
            if "fan" in flags:
                kwargs["fan_id"] = int(flags["fan"])
            if "duty" in flags:
                kwargs["duty"] = int(flags["duty"])
            if "start" in flags:
                kwargs["start_min"] = int(flags["start"])
            if "end" in flags:
                kwargs["end_min"] = int(flags["end"])
            if "enabled" in flags:
                kwargs["enabled"] = _parse_bool(flags["enabled"])
            if not kwargs:
                console.print("[yellow]No fields to update.[/yellow]")
                return
            s = client.schedules_update(sid, **kwargs)
            console.print(f"[green]Updated schedule {s.id}.[/green]")

        elif action == "delete":
            if len(args) < 2:
                console.print("[yellow]Usage: schedules delete <id>[/yellow]")
                return
            sid = int(args[1])
            client.schedules_delete(sid)
            console.print(f"[green]Deleted schedule {sid}.[/green]")

        else:
            console.print(f"[yellow]Unknown schedules action: {action}[/yellow]")

    except CoapError as e:
        console.print(f"[red]{_error_message(e)}[/red]")
    except (ValueError, IndexError) as e:
        console.print(f"[red]Invalid argument: {e}[/red]")


def _handle_wifi(shell: ESPFMShell, args: list[str]) -> None:
    if not args:
        console.print("[yellow]Usage: wifi <scan|status|connect> ...[/yellow]")
        return
    action = args[0]
    if not _check_connected(shell):
        return
    client = shell.client

    try:
        if action == "scan":
            result = client.wifi_scan()
            if not result.aps:
                console.print("[dim]No APs found.[/dim]")
                return
            table = Table(title="WiFi Scan Results")
            table.add_column("SSID")
            table.add_column("RSSI", justify="right")
            table.add_column("Channel", justify="right")
            table.add_column("Auth")
            for ap in result.aps:
                table.add_row(
                    ap.ssid, str(ap.rssi), str(ap.channel),
                    WIFI_AUTH_LABELS.get(ap.authmode, str(ap.authmode)),
                )
            console.print(table)

        elif action == "status":
            ws = client.wifi_status()
            lines = [
                f"[bold]WiFi Status[/bold]",
                f"  STA Connected: {'yes' if ws.sta_connected else 'no'}",
                f"  STA IP:        {ws.sta_ip or 'none'}",
                f"  AP IP:         {ws.ap_ip or 'none'}",
            ]
            console.print(Panel("\n".join(lines), title="WiFi"))

        elif action == "connect":
            flags = _parse_flags(args[1:])
            if "ssid" not in flags:
                console.print("[yellow]Usage: wifi connect --ssid <name> --pass <password>[/yellow]")
                return
            client.wifi_connect(flags["ssid"], flags.get("pass", ""))
            console.print(f"[green]WiFi connect request sent for '{flags['ssid']}'.[/green]")

        else:
            console.print(f"[yellow]Unknown wifi action: {action}[/yellow]")

    except CoapError as e:
        console.print(f"[red]{_error_message(e)}[/red]")
    except (ValueError, IndexError) as e:
        console.print(f"[red]Invalid argument: {e}[/red]")


def _handle_system(shell: ESPFMShell, args: list[str]) -> None:
    if not args:
        console.print("[yellow]Usage: system <info|reboot>[/yellow]")
        return
    action = args[0]

    if action == "info":
        if not _check_connected(shell):
            return
        try:
            si = shell.client.system_info()
            uptime_h = si.uptime_s // 3600
            uptime_m = (si.uptime_s % 3600) // 60
            uptime_s = si.uptime_s % 60
            lines = [
                f"[bold]System Info[/bold]",
                f"  Version:   {si.version}",
                f"  Uptime:    {uptime_h}h {uptime_m}m {uptime_s}s",
                f"  Heap Free: {si.heap_free} bytes",
                f"  Fans:      {si.fan_count}",
                f"  Sources:   {si.source_count}",
                f"  Curves:    {si.curve_count}",
                f"  Schedules: {si.schedule_count}",
            ]
            console.print(Panel("\n".join(lines), title="System"))
        except CoapError as e:
            console.print(f"[red]{_error_message(e)}[/red]")

    elif action == "reboot":
        if not _check_connected(shell):
            return
        try:
            result = shell.client.system_reboot()
            if result.ok:
                console.print("[yellow]Rebooting device in 2 seconds...[/yellow]")
            else:
                console.print(f"[red]Device reboot already pending.[/red]")
        except CoapError as e:
            console.print(f"[red]{_error_message(e)}[/red]")

    else:
        console.print(f"[yellow]Unknown system action: {action}[/yellow]")
        console.print("[yellow]Usage: system <info|reboot>[/yellow]")


def _handle_ds18b20(shell: ESPFMShell, args: list[str]) -> None:
    """DS18B20 sensor operations: ds18b20 scan | config."""
    if not args:
        console.print("[yellow]Usage: ds18b20 <scan|config> ...[/yellow]")
        return
    action = args[0]
    if not _check_connected(shell):
        return
    client = shell.client

    try:
        if action == "scan":
            resp = client._decode(pb.Ds18b20ScanResponse, client._get("/ds18b20/scan")[1])

            if resp.device_count == 0:
                console.print("[dim]No DS18B20 devices found.[/dim]")
                return

            table = Table(title="DS18B20 Devices")
            table.add_column("Index", justify="right")
            table.add_column("ROM Code", style="cyan")
            table.add_column("Temperature", justify="right")

            for dev in resp.devices:
                rom_hex = f"{dev.rom_code:016X}"
                rom_formatted = ":".join(rom_hex[i:i+2] for i in range(0, 16, 2))
                table.add_row(str(dev.index), rom_formatted, f"{dev.temp_c:.1f} C")

            console.print(table)

            # Add ROM codes to completer for tab completion
            if hasattr(shell, "_completer_words"):
                for dev in resp.devices:
                    rom_hex = f"{dev.rom_code:016X}"
                    rom_formatted = ":".join(rom_hex[i:i+2] for i in range(0, 16, 2))
                    if rom_formatted not in shell._completer_words:
                        shell._completer_words.append(rom_formatted)

        elif action == "config":
            flags = _parse_flags(args[1:])
            if "gpio" not in flags:
                console.print("[yellow]Usage: ds18b20 config --gpio <pin>[/yellow]")
                return
            gpio = int(flags["gpio"])
            result = client.ds18b20_config(gpio)
            if result.ok:
                console.print(f"[green]DS18B20 bus configured on GPIO {gpio}.[/green]")
            else:
                console.print(f"[red]Failed to configure DS18B20: {result.error_msg}[/red]")

        else:
            console.print(f"[yellow]Unknown ds18b20 action: {action}[/yellow]")

    except CoapError as e:
        console.print(f"[red]{_error_message(e)}[/red]")
    except (ValueError, IndexError) as e:
        console.print(f"[red]Invalid argument: {e}[/red]")


def _handle_devices(shell: ESPFMShell, args: list[str]) -> None:
    if not args:
        console.print("[yellow]Usage: devices <scan|connect|update> ...[/yellow]")
        return
    action = args[0]

    if action == "scan":
        if not HAS_ZEROCONF:
            console.print("[red]zeroconf not installed. Run: pip install zeroconf[/red]")
            return
        flags = _parse_flags(args[1:])
        timeout = float(flags.get("timeout", 3.0))
        _devices_scan(shell, timeout)

    elif action == "connect":
        if len(args) < 2:
            console.print("[yellow]Usage: devices connect <XXYY>[/yellow]")
            return
        suffix = args[1].lower()
        _devices_connect(shell, suffix)

    elif action == "update":
        if len(args) < 2:
            console.print("[yellow]Usage: devices update <XXYY> --hostname <name>[/yellow]")
            return
        suffix = args[1].lower()
        flags = _parse_flags(args[2:])
        hostname = flags.get("hostname", "")
        if not hostname:
            console.print("[yellow]Missing --hostname <name>[/yellow]")
            return
        _devices_update(shell, suffix, hostname)

    else:
        console.print(f"[yellow]Unknown devices action: {action}[/yellow]")


def _devices_scan(shell: ESPFMShell, timeout: float) -> None:
    """Scan LAN for ESPFM devices via mDNS."""
    results: list[dict[str, Any]] = []

    class _Listener(ServiceListener):
        def add_service(self, zc: Zeroconf, svc_type: str, name: str) -> None:
            info = zc.get_service_info(svc_type, name)
            if info is None:
                return
            hostname = info.server.rstrip(".") if info.server else name.split(".")[0]
            hostname = hostname.replace(".local", "")
            ip = ".".join(str(b) for b in info.addresses[0]) if info.addresses else "?"
            txt: dict[str, str] = {}
            if info.properties:
                for k, v in info.properties.items():
                    key = k.decode() if isinstance(k, bytes) else str(k)
                    val = v.decode() if isinstance(v, bytes) else str(v)
                    txt[key] = val
            results.append({"hostname": hostname, "ip": ip, "port": info.port, "txt": txt})

        def remove_service(self, zc: Zeroconf, svc_type: str, name: str) -> None:
            pass

        def update_service(self, zc: Zeroconf, svc_type: str, name: str) -> None:
            pass

    console.print(f"[dim]Scanning for ESPFM devices ({timeout}s)...[/dim]")
    zc = Zeroconf()
    listener = _Listener()
    browser = ServiceBrowser(zc, "_espfm._tcp.local.", listener)
    time.sleep(timeout)
    browser.cancel()
    zc.close()

    if not results:
        console.print("[dim]No ESPFM devices found.[/dim]")
        return

    table = Table(title="ESPFM Devices")
    table.add_column("Hostname")
    table.add_column("IP Address")
    table.add_column("Port", justify="right")
    table.add_column("Version")
    table.add_column("Firmware")
    for r in results:
        table.add_row(
            r["hostname"],
            r["ip"],
            str(r["port"]),
            r["txt"].get("version", "?"),
            r["txt"].get("fw", "?"),
        )
    console.print(table)

    # Add hostnames to completer for tab completion
    if hasattr(shell, "_completer_words"):
        for r in results:
            hostname = r["hostname"]
            if hostname not in shell._completer_words:
                shell._completer_words.append(hostname)


def _devices_connect(shell: ESPFMShell, suffix: str) -> None:
    """Scan for device by MAC suffix and auto-connect."""
    if not HAS_ZEROCONF:
        console.print("[red]zeroconf not installed. Run: pip install zeroconf[/red]")
        return

    console.print(f"[dim]Scanning for device ending in '{suffix}'...[/dim]")
    results: list[tuple[str, int]] = []

    class _Listener(ServiceListener):
        def add_service(self, zc: Zeroconf, svc_type: str, name: str) -> None:
            info = zc.get_service_info(svc_type, name)
            if info is None or not info.addresses:
                return
            hostname = info.server.rstrip(".") if info.server else name.split(".")[0]
            hostname = hostname.replace(".local", "")
            if hostname.endswith(suffix):
                ip = ".".join(str(b) for b in info.addresses[0])
                results.append((ip, info.port))

        def remove_service(self, zc: Zeroconf, svc_type: str, name: str) -> None:
            pass

        def update_service(self, zc: Zeroconf, svc_type: str, name: str) -> None:
            pass

    zc = Zeroconf()
    listener = _Listener()
    browser = ServiceBrowser(zc, "_espfm._tcp.local.", listener)
    time.sleep(3)
    browser.cancel()
    zc.close()

    if not results:
        console.print(f"[red]No device found with suffix '{suffix}'.[/red]")
        return

    ip, port = results[0]
    shell._disconnect()
    shell._connect(ip, port)


def _devices_update(shell: ESPFMShell, suffix: str, hostname: str) -> None:
    """Scan for device, connect, and update hostname."""
    if not HAS_ZEROCONF:
        console.print("[red]zeroconf not installed. Run: pip install zeroconf[/red]")
        return

    console.print(f"[dim]Scanning for device ending in '{suffix}'...[/dim]")
    results: list[tuple[str, int]] = []

    class _Listener(ServiceListener):
        def add_service(self, zc: Zeroconf, svc_type: str, name: str) -> None:
            info = zc.get_service_info(svc_type, name)
            if info is None or not info.addresses:
                return
            hn = info.server.rstrip(".") if info.server else name.split(".")[0]
            hn = hn.replace(".local", "")
            if hn.endswith(suffix):
                ip = ".".join(str(b) for b in info.addresses[0])
                results.append((ip, info.port))

        def remove_service(self, zc: Zeroconf, svc_type: str, name: str) -> None:
            pass

        def update_service(self, zc: Zeroconf, svc_type: str, name: str) -> None:
            pass

    zc = Zeroconf()
    listener = _Listener()
    browser = ServiceBrowser(zc, "_espfm._tcp.local.", listener)
    time.sleep(3)
    browser.cancel()
    zc.close()

    if not results:
        console.print(f"[red]No device found with suffix '{suffix}'.[/red]")
        return

    ip, port = results[0]
    shell._disconnect()
    shell._connect(ip, port)

    if not shell.client:
        console.print("[red]Failed to connect.[/red]")
        return

    try:
        result = shell.client.system_set_hostname(hostname)
        if result.ok:
            console.print(f"[green]Hostname set to '{hostname}'. Reboot to take effect.[/green]")
        else:
            console.print(f"[red]Failed: {result.error_msg}[/red]")
    except Exception as e:
        console.print(f"[red]Error: {e}[/red]")


def _handle_dashboard(shell: ESPFMShell) -> None:
    if not _check_connected(shell):
        return
    client = shell.client

    try:
        # Fans
        fans = client.fans_list()
        if fans.fans:
            t = Table(title="Fans")
            t.add_column("ID", justify="right")
            t.add_column("Name")
            t.add_column("Mode")
            t.add_column("Duty %", justify="right")
            t.add_column("RPM", justify="right")
            t.add_column("Alarm")
            for f in fans.fans:
                t.add_row(
                    str(f.id), f.name,
                    FAN_MODE_LABELS.get(f.mode, str(f.mode)),
                    str(f.duty), str(f.rpm),
                    FAN_ALARM_LABELS.get(f.alarm, str(f.alarm)),
                )
            console.print(t)

        # Sources
        sources = client.sources_list()
        if sources.sources:
            t = Table(title="Sources")
            t.add_column("ID", justify="right")
            t.add_column("Name")
            t.add_column("Type")
            t.add_column("Temp (C)", justify="right")
            t.add_column("Status")
            for s in sources.sources:
                t.add_row(
                    str(s.id), s.name,
                    SOURCE_TYPE_LABELS.get(s.type, str(s.type)),
                    f"{s.temp_c:.1f}",
                    SOURCE_STATUS_LABELS.get(s.status, str(s.status)),
                )
            console.print(t)

        # Curves
        curves = client.curves_list()
        if curves.curves:
            t = Table(title="Curves")
            t.add_column("ID", justify="right")
            t.add_column("Name")
            t.add_column("Points")
            for c in curves.curves:
                pts = ", ".join(f"{p.temp_c:.0f}C:{p.duty}%" for p in c.points)
                t.add_row(str(c.id), c.name, pts)
            console.print(t)

        # Schedules
        schedules = client.schedules_list()
        if schedules.schedules:
            t = Table(title="Schedules")
            t.add_column("ID", justify="right")
            t.add_column("Fan", justify="right")
            t.add_column("Duty %", justify="right")
            t.add_column("Start")
            t.add_column("End")
            t.add_column("Enabled")
            for s in schedules.schedules:
                t.add_row(
                    str(s.id), str(s.fan_id), str(s.duty),
                    _fmt_minutes(s.start_min), _fmt_minutes(s.end_min),
                    "yes" if s.enabled else "no",
                )
            console.print(t)

        # WiFi status
        try:
            ws = client.wifi_status()
            t = Table(title="WiFi")
            t.add_column("Property")
            t.add_column("Value")
            t.add_row("STA Connected", "yes" if ws.sta_connected else "no")
            t.add_row("STA IP", ws.sta_ip or "none")
            t.add_row("AP IP", ws.ap_ip or "none")
            console.print(t)
        except CoapError:
            pass

        # System info
        try:
            si = client.system_info()
            t = Table(title="System")
            t.add_column("Property")
            t.add_column("Value")
            t.add_row("Version", si.version)
            t.add_row("Uptime", f"{si.uptime_s // 3600}h {(si.uptime_s % 3600) // 60}m")
            t.add_row("Heap Free", f"{si.heap_free} bytes")
            console.print(t)
        except CoapError:
            pass

    except CoapError as e:
        console.print(f"[red]{_error_message(e)}[/red]")


def _handle_export(shell: ESPFMShell, args: list[str]) -> None:
    if not args:
        console.print("[yellow]Usage: export <file.json>[/yellow]")
        return
    if not _check_connected(shell):
        return
    client = shell.client
    filepath = args[0]

    try:
        data: dict[str, Any] = {
            "version": "3.0",
            "exported_at": datetime.datetime.now().isoformat(timespec="seconds"),
            "device": shell.transport.host,
        }

        # Fans
        fans = client.fans_list()
        data["fans"] = []
        for f in fans.fans:
            data["fans"].append({
                "id": f.id, "name": f.name,
                "mode": FAN_MODE_LABELS.get(f.mode, str(f.mode)),
                "duty": f.duty, "rpm": f.rpm,
                "enabled": f.enabled, "inverted": f.inverted,
                "pwm_gpio": f.pwm_gpio,
                "tach_gpio": f.tach_gpio,
                "source_id": f.source_id, "curve_id": f.curve_id,
                "schedule_id": f.schedule_id, "group_id": f.group_id,
                "alarm": FAN_ALARM_LABELS.get(f.alarm, str(f.alarm)),
            })

        # Sources
        sources = client.sources_list()
        data["sources"] = []
        for s in sources.sources:
            data["sources"].append({
                "id": s.id, "name": s.name,
                "type": SOURCE_TYPE_LABELS.get(s.type, str(s.type)),
                "status": SOURCE_STATUS_LABELS.get(s.status, str(s.status)),
                "temp_c": round(s.temp_c, 1),
                "gpio": s.gpio,
            })

        # Curves
        curves = client.curves_list()
        data["curves"] = []
        for c in curves.curves:
            data["curves"].append({
                "id": c.id, "name": c.name,
                "points": [{"temp_c": round(p.temp_c, 1), "duty": p.duty} for p in c.points],
            })

        # Schedules
        schedules = client.schedules_list()
        data["schedules"] = []
        for s in schedules.schedules:
            data["schedules"].append({
                "id": s.id, "fan_id": s.fan_id, "duty": s.duty,
                "start_min": s.start_min, "end_min": s.end_min,
                "enabled": s.enabled,
            })

        # WiFi status
        try:
            ws = client.wifi_status()
            data["wifi"] = {
                "sta_connected": ws.sta_connected,
                "sta_ip": ws.sta_ip,
                "ap_ip": ws.ap_ip,
            }
        except CoapError:
            data["wifi"] = {}

        with open(filepath, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)

        n_fans = len(data["fans"])
        n_sources = len(data["sources"])
        n_curves = len(data["curves"])
        n_schedules = len(data["schedules"])
        console.print(
            f"[green]Exported to {filepath}[/green]: "
            f"{n_fans} fans, {n_sources} sources, {n_curves} curves, {n_schedules} schedules"
        )

    except CoapError as e:
        console.print(f"[red]{_error_message(e)}[/red]")
    except OSError as e:
        console.print(f"[red]File error: {e}[/red]")


def _handle_import(shell: ESPFMShell, args: list[str]) -> None:
    if not args:
        console.print("[yellow]Usage: import <file.json> [--no-delete][/yellow]")
        return
    if not _check_connected(shell):
        return
    client = shell.client
    filepath = args[0]
    flags = _parse_flags(args[1:])
    no_delete = "no-delete" in flags

    try:
        with open(filepath, "r", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        console.print(f"[red]Invalid JSON: {e}[/red]")
        return

    created = updated = deleted = 0

    try:
        # --- Fans ---
        device_fans = {f.id: f for f in client.fans_list().fans}
        json_fans = {f["id"]: f for f in data.get("fans", [])}

        # Create missing
        for fid, jf in json_fans.items():
            if fid not in device_fans:
                client.fans_create(
                    pwm_gpio=jf["pwm_gpio"], name=jf["name"],
                    tach_gpio=jf.get("tach_gpio", 255),
                )
                created += 1

        # Update existing
        for fid, jf in json_fans.items():
            if fid in device_fans:
                df = device_fans[fid]
                kwargs: dict[str, Any] = {}
                jmode = FAN_MODE_VALUES.get(jf.get("mode", ""), -1)
                if jmode >= 0 and jmode != df.mode:
                    kwargs["mode"] = jmode
                if jf.get("duty") is not None and jf["duty"] != df.duty:
                    kwargs["duty"] = jf["duty"]
                if jf.get("source_id") is not None and jf["source_id"] != df.source_id:
                    kwargs["source_id"] = jf["source_id"]
                if jf.get("curve_id") is not None and jf["curve_id"] != df.curve_id:
                    kwargs["curve_id"] = jf["curve_id"]
                if jf.get("schedule_id") is not None and jf["schedule_id"] != df.schedule_id:
                    kwargs["schedule_id"] = jf["schedule_id"]
                if jf.get("group_id") is not None and jf["group_id"] != df.group_id:
                    kwargs["group_id"] = jf["group_id"]
                if jf.get("inverted") is not None and jf["inverted"] != df.inverted:
                    kwargs["inverted"] = jf["inverted"]
                if kwargs:
                    client.fans_update(fid, **kwargs)
                    updated += 1

        # Delete extra
        if not no_delete:
            for fid in device_fans:
                if fid not in json_fans:
                    client.fans_delete(fid)
                    deleted += 1

        fan_stats = (created, updated, deleted)
        created = updated = deleted = 0

        # --- Sources ---
        device_sources = {s.id: s for s in client.sources_list().sources}
        json_sources = {s["id"]: s for s in data.get("sources", [])}

        for sid, js in json_sources.items():
            if sid not in device_sources:
                client.sources_create(
                    source_type=SOURCE_TYPE_VALUES.get(js.get("type", "manual"), 2),
                    name=js["name"],
                    gpio=js.get("gpio", 255),
                )
                created += 1

        if not no_delete:
            for sid in device_sources:
                if sid not in json_sources:
                    client.sources_delete(sid)
                    deleted += 1

        source_stats = (created, updated, deleted)
        created = updated = deleted = 0

        # --- Curves ---
        device_curves = {c.id: c for c in client.curves_list().curves}
        json_curves = {c["id"]: c for c in data.get("curves", [])}

        for cid, jc in json_curves.items():
            points = [(p["temp_c"], p["duty"]) for p in jc.get("points", [])]
            if cid not in device_curves:
                client.curves_create(jc["name"], points)
                created += 1
            else:
                dc = device_curves[cid]
                name_changed = jc.get("name", dc.name) != dc.name
                pts_changed = len(points) != len(dc.points) or any(
                    abs(p[0] - dp.temp_c) > 0.01 or p[1] != dp.duty
                    for p, dp in zip(points, dc.points)
                )
                if name_changed or pts_changed:
                    client.curves_update(
                        cid, name=jc.get("name", ""), points=points if pts_changed else None,
                    )
                    updated += 1

        if not no_delete:
            for cid in device_curves:
                if cid not in json_curves:
                    client.curves_delete(cid)
                    deleted += 1

        curve_stats = (created, updated, deleted)
        created = updated = deleted = 0

        # --- Schedules ---
        device_scheds = {s.id: s for s in client.schedules_list().schedules}
        json_scheds = {s["id"]: s for s in data.get("schedules", [])}

        for sid, js in json_scheds.items():
            if sid not in device_scheds:
                client.schedules_create(
                    fan_id=js["fan_id"], duty=js["duty"],
                    start_min=js["start_min"], end_min=js["end_min"],
                    enabled=js.get("enabled", True),
                )
                created += 1
            else:
                ds = device_scheds[sid]
                kwargs = {}
                for key in ("fan_id", "duty", "start_min", "end_min", "enabled"):
                    if js.get(key) is not None and js[key] != getattr(ds, key):
                        kwargs[key] = js[key]
                if kwargs:
                    client.schedules_update(sid, **kwargs)
                    updated += 1

        if not no_delete:
            for sid in device_scheds:
                if sid not in json_scheds:
                    client.schedules_delete(sid)
                    deleted += 1

        sched_stats = (created, updated, deleted)

        # Summary
        labels = ["fans", "sources", "curves", "schedules"]
        for name, stats in zip(labels, [fan_stats, source_stats, curve_stats, sched_stats]):
            c, u, d = stats
            if c or u or d:
                parts = []
                if c:
                    parts.append(f"created {c}")
                if u:
                    parts.append(f"updated {u}")
                if d:
                    parts.append(f"deleted {d}")
                console.print(f"[green]{name.title()}:[/green] {', '.join(parts)}")

        total_created = fan_stats[0] + source_stats[0] + curve_stats[0] + sched_stats[0]
        total_updated = fan_stats[1] + source_stats[1] + curve_stats[1] + sched_stats[1]
        total_deleted = fan_stats[2] + source_stats[2] + curve_stats[2] + sched_stats[2]
        if not (total_created or total_updated or total_deleted):
            console.print("[dim]No changes needed — device state matches import.[/dim]")

    except CoapError as e:
        console.print(f"[red]{_error_message(e)}[/red]")


def _handle_help(shell: ESPFMShell, args: list[str]) -> None:
    """Print help text."""
    if args:
        topic = args[0]
        help_texts: dict[str, str] = {
            "connect": "connect <host> [--port N] [--timeout N]  — Connect to device",
            "disconnect": "disconnect  — Close connection",
            "fans": (
                "fans list                              — List all fans\n"
                "  fans get <id>                        — Show fan detail\n"
                "  fans create --pwm <gpio> --name <n> [--tach <gpio>] [--source N]\n"
                "              [--curve N] [--mode auto|manual] [--inverted]\n"
                "              [--group N] [--enabled] — Create fan\n"
                "  fans update <id> [--duty N] [--mode auto|manual] [--source N]\n"
                "                   [--curve N] [--schedule N] [--group N]\n"
                "                   [--inverted] [--enabled] — Update fan\n"
                "  fans enable <id>                     — Enable a fan\n"
                "  fans disable <id>                    — Disable a fan\n"
                "  fans delete <id>                     — Delete fan"
            ),
            "sources": (
                "sources list                           — List all sources\n"
                "  sources get <id>                     — Show source detail\n"
                "  sources create --type <ntc|ds18b20|manual> --name <n>\n"
                "                [--gpio N] [--rom HEX] — Create source\n"
                "  sources update <id> --name <name>    — Rename source\n"
                "  sources temp <id> <temp_c>           — Set manual temperature\n"
                "  sources delete <id>                  — Delete source"
            ),
            "curves": (
                "curves list                            — List all curves\n"
                "  curves get <id>                      — Show curve with points\n"
                "  curves create --name <n> --points \"t:d,t:d,...\"  — Create curve\n"
                "  curves update <id> [--name ...] [--points ...]  — Update curve\n"
                "  curves delete <id>                   — Delete curve"
            ),
            "schedules": (
                "schedules list                         — List all schedules\n"
                "  schedules create --fan N --duty N --start N --end N  — Create\n"
                "  schedules update <id> [--fan N] [--duty N] ...  — Update\n"
                "  schedules delete <id>                — Delete schedule"
            ),
            "wifi": (
                "wifi scan                              — Scan nearby APs\n"
                "  wifi status                          — Show STA status\n"
                "  wifi connect --ssid <n> --pass <p>   — Connect to AP"
            ),
            "system": (
                "system info    — Show version, uptime, heap, entity counts\n"
                "  system reboot  — Reboot the device (2s delay)"
            ),
            "ds18b20": (
                "ds18b20 scan            — Scan for DS18B20 devices on the 1-Wire bus\n"
                "  ds18b20 config --gpio <pin>  — Configure DS18B20 bus GPIO at runtime"
            ),
            "devices": (
                "devices scan [--timeout N]            — Scan LAN for ESPFM devices (mDNS)\n"
                "  devices connect XXYY                — Connect to device by MAC suffix\n"
                "  devices update XXYY --hostname NAME — Change device hostname"
            ),
            "dashboard": "dashboard  — Poll all resources, show multi-table summary",
            "export": "export <file.json>  — Dump full device state to JSON",
            "import": "import <file.json> [--no-delete]  — Apply config from JSON",
        }
        if topic in help_texts:
            console.print(help_texts[topic])
        else:
            console.print(f"[yellow]No help for '{topic}'.[/yellow]")
        return

    text = """[bold]ESPFM Interactive Shell[/bold]

[bold cyan]Connection[/bold cyan]
  connect <host> [--port N] [--timeout N]   Connect to device
  disconnect                                 Close connection
  help [command]                             Show help
  exit / quit                                Exit shell

[bold cyan]Resources[/bold cyan]
  fans       list | get | create | update | delete | enable | disable
  sources    list | get | create | update | temp | delete
  curves     list | get | create | update | delete
  schedules  list | create | update | delete
  wifi       scan | status | connect
  system     info | reboot
  ds18b20    scan | config
  devices    scan | connect | update

[bold cyan]Data Operations[/bold cyan]
  dashboard                 Multi-table summary
  export <file.json>        Dump config to JSON
  import <file.json> [--no-delete]  Apply config from JSON"""
    console.print(Panel(text, title="Help"))


# ============================================================
# Flag parser
# ============================================================


def _parse_flags(tokens: list[str]) -> dict[str, str]:
    """Parse --key value pairs from a token list."""
    flags: dict[str, str] = {}
    i = 0
    while i < len(tokens):
        tok = tokens[i]
        if tok.startswith("--"):
            key = tok[2:]
            if i + 1 < len(tokens) and not tokens[i + 1].startswith("--"):
                flags[key] = tokens[i + 1]
                i += 2
            else:
                flags[key] = "true"
                i += 1
        else:
            i += 1
    return flags


# ============================================================
# Shell / REPL
# ============================================================


class ESPFMShell:
    """Interactive shell for ESPFanManager v3."""

    def __init__(self, host: str = "", port: int = 5683, timeout: float = 3.0) -> None:
        self.transport = CoAPTransport(host, port, timeout)
        self.client: Optional[ESPFMClient] = None
        if host:
            self._connect(host, port, timeout)

    def _connect(self, host: str, port: int = 5683, timeout: float = 3.0) -> None:
        self.transport = CoAPTransport(host, port, timeout)
        self.transport.connect()
        self.client = ESPFMClient(self.transport)
        console.print(f"[green]Connected to {host}:{port}[/green]")

    def _disconnect(self) -> None:
        if self.client:
            self.transport.close()
            self.client = None
            console.print("[yellow]Disconnected.[/yellow]")
        else:
            console.print("[dim]Not connected.[/dim]")

    def run(self) -> None:
        """Start the interactive REPL."""
        self._completer_words: list[str] = [
            "connect", "disconnect", "help", "exit", "quit",
            "fans", "sources", "curves", "schedules", "wifi", "system", "devices", "ds18b20",
            "dashboard", "export", "import",
            "list", "get", "create", "update", "delete", "enable", "disable", "temp",
            "scan", "status", "info", "reboot", "config",
            "--pwm", "--tach", "--name", "--type", "--gpio", "--temp",
            "--duty", "--mode", "--source", "--curve", "--schedule",
            "--group", "--inverted", "--enabled",
            "--points", "--fan", "--start", "--end",
            "--rom", "--ssid", "--pass", "--port", "--timeout", "--no-delete",
            "--hostname",
            "auto", "manual", "ntc", "true", "false",
        ]
        completer = WordCompleter(self._completer_words, ignore_case=True)
        session: PromptSession[str] = PromptSession(
            history=InMemoryHistory(),
            completer=completer,
        )

        console.print("[bold]ESPFM Interactive Shell v3.0[/bold]")
        console.print("Type 'help' for commands, 'exit' to quit.\n")

        while True:
            try:
                line: str = session.prompt("espfm> ")
            except KeyboardInterrupt:
                continue
            except EOFError:
                console.print("\n[dim]Bye.[/dim]")
                break

            line = line.strip()
            if not line:
                continue

            tokens = line.split()
            cmd = tokens[0].lower()
            args = tokens[1:]

            try:
                if cmd in ("exit", "quit"):
                    break
                elif cmd == "help":
                    _handle_help(self, args)
                elif cmd == "connect":
                    flags = _parse_flags(args)
                    if not args:
                        console.print("[yellow]Usage: connect <host> [--port N] [--timeout N][/yellow]")
                        continue
                    host = args[0]
                    port = int(flags.get("port", 5683))
                    timeout = float(flags.get("timeout", 3.0))
                    self._connect(host, port, timeout)
                elif cmd == "disconnect":
                    self._disconnect()
                elif cmd == "fans":
                    _handle_fans(self, args)
                elif cmd == "sources":
                    _handle_sources(self, args)
                elif cmd == "curves":
                    _handle_curves(self, args)
                elif cmd == "schedules":
                    _handle_schedules(self, args)
                elif cmd == "wifi":
                    _handle_wifi(self, args)
                elif cmd == "system":
                    _handle_system(self, args)
                elif cmd == "ds18b20":
                    _handle_ds18b20(self, args)
                elif cmd == "devices":
                    _handle_devices(self, args)
                elif cmd == "dashboard":
                    _handle_dashboard(self)
                elif cmd == "export":
                    _handle_export(self, args)
                elif cmd == "import":
                    _handle_import(self, args)
                else:
                    console.print(f"[yellow]Unknown command: {cmd}. Type 'help' for commands.[/yellow]")
            except KeyboardInterrupt:
                console.print("\n[dim]Command cancelled.[/dim]")
            except Exception as e:
                console.print(f"[red]Error: {e}[/red]")

        self._disconnect()


# ============================================================
# CLI entry point
# ============================================================


def main() -> None:
    parser = argparse.ArgumentParser(description="ESPFM Interactive Shell v3")
    parser.add_argument("--host", default="", help="Device IP (auto-connects)")
    parser.add_argument("--port", type=int, default=5683, help="CoAP port")
    parser.add_argument("--timeout", type=float, default=3.0, help="CoAP timeout (seconds)")
    args = parser.parse_args()

    shell = ESPFMShell(host=args.host, port=args.port, timeout=args.timeout)
    shell.run()


if __name__ == "__main__":
    main()
