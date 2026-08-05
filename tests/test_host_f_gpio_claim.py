#!/usr/bin/env python3
"""
Host-based C unit tests for the f_gpio claim-wiring feature (gpio phases 2-5):
cross-peripheral pin-conflict rejection in f_fan / f_source and the ONEWIRE
bus-level claim in f_ds18b20_init.

Compiles the REAL f_fan.c, f_source.c, f_gpio.c, f_ds18b20.c and
f_constraints.c against stubbed ESP-IDF layers (see tests/host_f_gpio_claim/)
and runs the 73-path harness (68 gpio/constr paths + 5 f_fan_set_alarm
paths from ctrl phase-3). Requires WSL (Ubuntu-24.04) with gcc + GNU ld; the
repo has no native Windows host C toolchain.

Usage:
    python tests/test_host_f_gpio_claim.py

The harness is also picked up by tests/test_all.py.
"""

from __future__ import annotations

import os
import subprocess
import sys
import unittest

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST_DIR = os.path.join(REPO_ROOT, "tests", "host_f_gpio_claim")
WSL_DISTRO = "Ubuntu-24.04"

# All 73 EPA execution paths (gpio phases 2-5: f_fan_add / f_fan_set_gpio /
# f_fan_remove / f_source_add / f_source_remove / f_ds18b20_init +
# constr phase-1: f_source_update_manual / f_constraints_temp_c +
# ctrl phase-3: f_fan_set_alarm), by test function name.
ALL_PATHS = [
    # f_fan_add (FA-P1..P13)
    "test_fan_add_null_args_returns_invalid_arg",  # FA-P1
    "test_fan_add_pwm_out_of_constraints_returns_invalid_arg",  # FA-P2
    "test_fan_add_tach_out_of_constraints_returns_invalid_arg",  # FA-P3
    "test_fan_add_registry_full_returns_no_mem",  # FA-P4
    "test_fan_add_claim_pwm_reserved_returns_invalid_arg",  # FA-P5
    "test_fan_add_claim_pwm_out_of_range_returns_invalid_arg",  # FA-P6
    "test_fan_add_claim_pwm_foreign_claim_returns_invalid_state",  # FA-P7
    "test_fan_add_success_no_tach_claims_pwm_only",  # FA-P8
    "test_fan_add_success_with_tach_claims_both",  # FA-P9
    "test_fan_add_tach_reserved_releases_pwm_returns_invalid_arg",  # FA-P10
    "test_fan_add_tach_out_of_range_releases_pwm",  # FA-P11
    "test_fan_add_tach_foreign_claim_releases_pwm",  # FA-P12
    "test_fan_add_pwm_equals_tach_rejected_and_pwm_released",  # FA-P13
    # f_fan_set_gpio (FS-P1..P20)
    "test_fan_set_gpio_null_args_returns_invalid_arg",  # FS-P1
    "test_fan_set_gpio_new_pwm_out_of_constraints_returns_invalid_arg",  # FS-P2
    "test_fan_set_gpio_new_tach_out_of_constraints_returns_invalid_arg",  # FS-P3
    "test_fan_set_gpio_slot_not_used_returns_not_found",  # FS-P4
    "test_fan_set_gpio_pwm_equals_tach_returns_invalid_arg_fan_intact",  # FS-P5
    "test_fan_set_gpio_new_pwm_out_of_range_fan_intact",  # FS-P6
    "test_fan_set_gpio_new_pwm_reserved_fan_intact",  # FS-P7
    "test_fan_set_gpio_new_pwm_foreign_fan_intact",  # FS-P8
    "test_fan_set_gpio_new_tach_out_of_range_fan_intact",  # FS-P9
    "test_fan_set_gpio_new_tach_reserved_fan_intact",  # FS-P10
    "test_fan_set_gpio_new_tach_foreign_fan_intact",  # FS-P11
    "test_fan_set_gpio_identity_succeeds_reclaims_same_pins",  # FS-P12
    "test_fan_set_gpio_pwm_only_change_reclaims",  # FS-P13
    "test_fan_set_gpio_tach_only_change_reclaims",  # FS-P14
    "test_fan_set_gpio_full_swap_succeeds",  # FS-P15
    "test_fan_set_gpio_both_new_pins_succeeds",  # FS-P16
    "test_fan_set_gpio_tach_removed_releases_tach",  # FS-P17
    "test_fan_set_gpio_tach_added_claims_new_tach",  # FS-P18
    "test_fan_set_gpio_pwm_claim_injected_failure_returns_error",  # FS-P19
    "test_fan_set_gpio_tach_claim_injected_failure_releases_new_pwm",  # FS-P20
    # f_fan_remove (FR-P1..P4)
    "test_fan_remove_null_args_returns_invalid_arg",  # FR-P1
    "test_fan_remove_unused_slot_returns_not_found",  # FR-P2
    "test_fan_remove_no_tach_releases_pwm",  # FR-P3
    "test_fan_remove_with_tach_releases_both",  # FR-P4
    # f_fan_set_alarm (P1-P5) — ctrl phase-3 alarm persistence
    "test_fan_set_alarm_null_handle_returns_invalid_arg",  # P1
    "test_fan_set_alarm_id_out_of_range_returns_invalid_arg",  # P2
    "test_fan_set_alarm_unused_slot_returns_not_found",  # P3
    "test_fan_set_alarm_success_writes_alarm",  # P4
    "test_fan_set_alarm_reentrant_under_mutex_ok",  # P5
    # f_source_add (SA-P1..P8)
    "test_source_add_null_args_returns_invalid_arg",  # SA-P1
    "test_source_add_registry_full_returns_no_mem",  # SA-P2
    "test_source_add_ntc_claims_adc_succeeds",  # SA-P3
    "test_source_add_ntc_sentinel_gpio_skips_claim",  # SA-P4
    "test_source_add_manual_skips_claim",  # SA-P5
    "test_source_add_ntc_reserved_returns_invalid_arg",  # SA-P6
    "test_source_add_ntc_out_of_range_returns_invalid_arg",  # SA-P7
    "test_source_add_ntc_foreign_claim_returns_invalid_state",  # SA-P8
    # f_source_remove (SR-P1..P5)
    "test_source_remove_null_args_returns_invalid_arg",  # SR-P1
    "test_source_remove_unused_slot_returns_not_found",  # SR-P2
    "test_source_remove_ntc_releases_adc",  # SR-P3
    "test_source_remove_ntc_sentinel_no_release",  # SR-P4
    "test_source_remove_manual_no_release",  # SR-P5
    # f_ds18b20_init (DI-P1..P6)
    "test_ds18b20_init_null_handle_returns_invalid_arg",  # DI-P1
    "test_ds18b20_init_calloc_failure_returns_no_mem_handle_unset",  # DI-P2
    "test_ds18b20_init_success_claims_onwire",  # DI-P3
    "test_ds18b20_init_reserved_cleans_up_handle_unset",  # DI-P4
    "test_ds18b20_init_out_of_range_cleans_up_handle_unset",  # DI-P5
    "test_ds18b20_init_foreign_claim_cleans_up_handle_unset",  # DI-P6
    # f_source_update_manual (SU-P1..P9) — constr phase-1 temp validator
    "test_source_update_manual_null_handle_returns_invalid_arg",  # SU-P1
    "test_source_update_manual_id_out_of_range_returns_invalid_arg",  # SU-P2
    "test_source_update_manual_temp_below_min_rejected_before_write",  # SU-P3
    "test_source_update_manual_temp_above_max_rejected_before_write",  # SU-P4
    "test_source_update_manual_unused_slot_returns_not_found",  # SU-P5
    "test_source_update_manual_non_manual_type_rejected",  # SU-P6
    "test_source_update_manual_happy_path_writes_and_marks_valid",  # SU-P7
    "test_source_update_manual_accepts_min_boundary",  # SU-P8
    "test_source_update_manual_accepts_max_boundary",  # SU-P9
    # f_constraints_temp_c (CT-1..3) — real validator branches
    "test_constraints_temp_c_in_range_returns_ok",  # CT-1
    "test_constraints_temp_c_below_min_returns_invalid_arg",  # CT-2
    "test_constraints_temp_c_above_max_returns_invalid_arg",  # CT-3
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
        "skipping host_f_gpio_claim C harness.",
        file=sys.stderr,
    )


@unittest.skipUnless(_WSL_AVAILABLE, f"WSL distro {WSL_DISTRO} not available")
class TestFGPIOClaimHostC(unittest.TestCase):
    """Runs the compiled f_gpio_claim C harness and asserts all 73 paths pass."""

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
        self.assertRegex(self.proc.stdout, r"RESULT: 73 passed, 0 failed",
                         msg=self._summary())


if __name__ == "__main__":
    unittest.main()
