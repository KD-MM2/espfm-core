#!/usr/bin/env python3
"""
Host-based C unit tests for f_config_export_all / f_config_save_all /
f_config_save_all_forced / f_config_import_all.

Compiles components/f_config/f_config.c against stubbed ESP-IDF/nanopb layers
(see tests/host_f_config/) and runs the 72-path harness. Requires WSL
(Ubuntu-24.04) with gcc + GNU ld; the repo has no native Windows host C
toolchain.

Usage:
    python tests/test_host_f_config.py

The harness is also picked up by tests/test_all.py.
"""

from __future__ import annotations

import os
import subprocess
import sys
import unittest

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST_DIR = os.path.join(REPO_ROOT, "tests", "host_f_config")
WSL_DISTRO = "Ubuntu-24.04"

# All 72 EPA execution paths (spec-01 phase-1 + spec-02 phase-1 + phase-2), by test function name.
ALL_PATHS = [
    "test_f_config_export_all_rejects_null_buf_out",  # P1
    "test_f_config_export_all_rejects_null_len_out",  # P2
    "test_f_config_export_all_cfg_calloc_failure",  # P3
    "test_f_config_export_all_success_decodes_version_and_counts",  # P4
    "test_f_config_export_all_null_fan_leaves_has_fans_true_empty",  # P5
    "test_f_config_export_all_null_source_leaves_has_sources_true_empty",  # P6
    "test_f_config_export_all_null_curve_leaves_has_curves_true_empty",  # P7
    "test_f_config_export_all_null_schedule_sets_has_schedules_false",  # P8
    "test_f_config_export_all_all_null_registries_empty_lists",  # P9
    "test_f_config_export_all_enc_buf_calloc_failure",  # P10
    "test_f_config_export_all_encode_failure_frees_buffers",  # P11
    "test_f_config_export_all_partial_registries_counts_match",  # P12
    "test_f_config_save_all_rejects_null_handle",  # S1
    "test_f_config_save_all_rejects_unmounted",  # S2
    "test_f_config_save_all_debounced_returns_ok_without_write",  # S3
    "test_f_config_save_all_propagates_export_error",  # S4
    "test_f_config_save_all_fopen_failure_frees_buffer_returns_fail",  # S5
    "test_f_config_save_all_short_write_returns_fail",  # S6
    "test_f_config_save_all_success_writes_file_returns_ok",  # S7
    "test_f_config_save_all_cfg_calloc_failure_propagates",  # S8
    "test_f_config_save_all_encode_failure_propagates",  # S9
    "test_f_config_save_all_forced_rejects_null_handle",  # F1
    "test_f_config_save_all_forced_rejects_unmounted",  # F2
    "test_f_config_save_all_forced_bypasses_debounce_writes",  # F3
    "test_f_config_save_all_forced_propagates_export_cfg_calloc_failure",  # F4a
    "test_f_config_save_all_forced_propagates_export_enc_buf_calloc_failure",  # F4b
    "test_f_config_save_all_forced_propagates_export_encode_failure",  # F4c
    "test_f_config_save_all_forced_fopen_failure_frees_buffer",  # F5
    "test_f_config_save_all_forced_short_write_returns_fail",  # F6
    "test_f_config_save_all_forced_success_writes_decodable_file",  # F7
    # spec-02 phase-2: f_config_import_all (validate / clear / apply / persist)
    "test_f_config_import_all_rejects_null_handle",  # I-P1
    "test_f_config_import_all_rejects_unmounted",  # I-P2
    "test_f_config_import_all_rejects_null_cfg",  # I-P3
    "test_f_config_import_all_rejects_fans_overflow",  # V-P1
    "test_f_config_import_all_rejects_sources_overflow",  # V-P2
    "test_f_config_import_all_rejects_curves_overflow",  # V-P3
    "test_f_config_import_all_rejects_schedules_overflow",  # V-P4
    "test_f_config_import_all_accepts_max_capacity",  # V-P5
    "test_f_config_import_all_rejects_fan_pwm_gpio",  # V-P6
    "test_f_config_import_all_rejects_fan_tach_gpio",  # V-P7
    "test_f_config_import_all_rejects_fan_mode",  # V-P8
    "test_f_config_import_all_rejects_fan_duty",  # V-P9
    "test_f_config_import_all_rejects_ntc_source_gpio",  # V-P10
    "test_f_config_import_all_rejects_curve_too_few_points",  # V-P11a
    "test_f_config_import_all_rejects_curve_too_many_points",  # V-P11b
    "test_f_config_import_all_rejects_curve_unsorted",  # V-P12
    "test_f_config_import_all_rejects_curve_point_duty",  # V-P13
    "test_f_config_import_all_rejects_schedule_time",  # V-P14
    "test_f_config_import_all_rejects_schedule_duty",  # V-P15
    "test_f_config_import_all_rejects_schedule_missing_fan",  # V-P16
    "test_f_config_import_all_clear_tolerates_empty_and_null_handles",  # C-P1
    "test_f_config_import_all_happy_path_all_registries",  # H-P1
    "test_f_config_import_all_empty_config_clears_all_registries",  # H-P2
    "test_f_config_import_all_happy_path_null_err_msg",  # H-P3
    "test_f_config_import_all_apply_skips_fan_bindings_when_ff",  # A-P2
    "test_f_config_import_all_apply_skips_manual_update_when_temp_zero",  # A-P3
    "test_f_config_import_all_apply_unknown_source_type_as_manual",  # A-P4
    "test_f_config_import_all_apply_fan_add_fails",  # A-ERR-FAN-ADD
    "test_f_config_import_all_apply_fan_set_mode_fails",  # A-ERR-FAN-SET-MODE
    "test_f_config_import_all_apply_fan_set_duty_fails",  # A-ERR-FAN-SET-DUTY
    "test_f_config_import_all_apply_fan_set_group_fails",  # A-ERR-FAN-SET-GROUP
    "test_f_config_import_all_apply_fan_set_inverted_fails",  # A-ERR-FAN-SET-INVERTED
    "test_f_config_import_all_apply_fan_set_source_fails",  # A-ERR-FAN-SET-SOURCE
    "test_f_config_import_all_apply_fan_set_curve_fails",  # A-ERR-FAN-SET-CURVE
    "test_f_config_import_all_apply_fan_set_schedule_fails",  # A-ERR-FAN-SET-SCHEDULE
    "test_f_config_import_all_apply_source_add_ds18b20_fails",  # A-ERR-SRC-ADD-DS18B20
    "test_f_config_import_all_apply_source_add_fails",  # A-ERR-SRC-ADD
    "test_f_config_import_all_apply_source_update_manual_fails",  # A-ERR-SRC-UPDATE-MANUAL
    "test_f_config_import_all_apply_curve_upsert_fails",  # A-ERR-CURVE-UPSERT
    "test_f_config_import_all_apply_schedule_add_fails",  # A-ERR-SCHED-ADD
    "test_f_config_import_all_persist_fopen_fails",  # P-ERR-1
    "test_f_config_import_all_persist_export_calloc_fails",  # P-ERR-2
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
        "skipping host_f_config C harness.",
        file=sys.stderr,
    )


@unittest.skipUnless(_WSL_AVAILABLE, f"WSL distro {WSL_DISTRO} not available")
class TestFConfigHostC(unittest.TestCase):
    """Runs the compiled f_config C harness and asserts all 72 paths pass."""

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
        self.assertRegex(self.proc.stdout, r"RESULT: 72 passed, 0 failed",
                         msg=self._summary())


if __name__ == "__main__":
    unittest.main()
