#!/usr/bin/env python3
"""
Host-based C unit tests for the f_control control-tunables feature
(ctrl phases 2-5): f_control_get_tunables and the _ctrl_callback alarm-write
integration in components/f_control/f_control.c.

Compiles the real f_control.c (`static` demoted) against stubbed ESP-IDF /
FreeRTOS layers (see tests/host_f_control/) and runs the 13-path harness
(7 getter paths + 6 alarm-write callback paths). Requires WSL (Ubuntu-24.04)
with gcc + GNU ld; the repo has no native Windows host C toolchain.

Usage:
    python tests/test_host_f_control.py

The harness is also picked up by tests/test_all.py.
"""

from __future__ import annotations

import os
import subprocess
import sys
import unittest

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST_DIR = os.path.join(REPO_ROOT, "tests", "host_f_control")
WSL_DISTRO = "Ubuntu-24.04"

# All 13 EPA execution paths, by test function name:
#   f_control_get_tunables (P1-P7)
#   _ctrl_callback alarm-write (CT1-CT6)
ALL_PATHS = [
    # f_control_get_tunables (P1-P7)
    "test_get_tunables_null_handle_returns_invalid_arg",  # P1
    "test_get_tunables_null_hysteresis_out_returns_invalid_arg",  # P2
    "test_get_tunables_null_ramp_up_out_returns_invalid_arg",  # P3
    "test_get_tunables_null_ramp_down_out_returns_invalid_arg",  # P4
    "test_get_tunables_null_policy_out_returns_invalid_arg",  # P5
    "test_get_tunables_null_safe_duty_out_returns_invalid_arg",  # P6
    "test_get_tunables_success_copies_all_five_fields",  # P7
    # _ctrl_callback alarm-write (CT1-CT6)
    "test_ctrl_callback_manual_stall_writes_stall_alarm",  # CT1
    "test_ctrl_callback_manual_clear_writes_none",  # CT2
    "test_ctrl_callback_auto_source_invalid_writes_at_check_alarm",  # CT3
    "test_ctrl_callback_auto_curve_fail_writes_at_check_alarm",  # CT4
    "test_ctrl_callback_auto_overtemp_writes_overtemp",  # CT5
    "test_ctrl_callback_auto_normal_writes_none",  # CT6
]


def _wsl_available() -> bool:
    """Return True when the WSL distro is installed and bootable."""
    try:
        proc = subprocess.run(
            ["wsl", "-d", WSL_DISTRO, "-e", "true"],
            capture_output=True,
            text=True,
            timeout=60,
        )
        return proc.returncode == 0
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return False


def _wsl_path(windows_path: str) -> str:
    """Convert a Windows path to its /mnt/c/... form inside the WSL distro."""
    proc = subprocess.run(
        ["wsl", "-d", WSL_DISTRO, "-e", "wslpath", "-a", windows_path],
        capture_output=True,
        text=True,
        timeout=60,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"wslpath failed: {proc.stderr}")
    return proc.stdout.strip()


def _build_and_run() -> subprocess.CompletedProcess:
    """Compile and run the C harness inside WSL; return the completed process."""
    host_dir_wsl = _wsl_path(HOST_DIR)
    cmd = f"cd {host_dir_wsl} && bash ./build_and_run.sh"
    return subprocess.run(
        ["wsl", "-d", WSL_DISTRO, "-e", "bash", "-lc", cmd],
        capture_output=True,
        text=True,
        timeout=300,
    )


_WSL_AVAILABLE = _wsl_available()
if not _WSL_AVAILABLE:
    print(
        f"WARNING: WSL distro {WSL_DISTRO} not available — "
        "skipping host_f_control C harness.",
        file=sys.stderr,
    )


@unittest.skipUnless(_WSL_AVAILABLE, f"WSL distro {WSL_DISTRO} not available")
class TestFControlHostC(unittest.TestCase):
    """Runs the compiled f_control C harness and asserts all 13 paths pass."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.proc = _build_and_run()

    def _summary(self) -> str:
        return (
            f"returncode={self.proc.returncode}\n"
            f"STDOUT:\n{self.proc.stdout}\n"
            f"STDERR:\n{self.proc.stderr}"
        )

    def test_harness_exit_zero(self) -> None:
        self.assertEqual(self.proc.returncode, 0, msg=self._summary())

    def test_all_paths_reported(self) -> None:
        missing = [name for name in ALL_PATHS if name not in self.proc.stdout]
        self.assertEqual(missing, [], msg="Missing test outputs:\n" + self._summary())

    def test_zero_failed_in_summary(self) -> None:
        self.assertRegex(self.proc.stdout, r"RESULT: 13 passed, 0 failed",
                         msg=self._summary())


if __name__ == "__main__":
    unittest.main()
