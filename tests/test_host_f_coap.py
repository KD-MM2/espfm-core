#!/usr/bin/env python3
"""
Host-based C unit tests for handle_config_get / coap_free_config_data
(spec-01 phase-3) and handle_config_post (spec-02 phase-4) in
components/f_coap/f_coap_routes.c.

Compiles the real f_coap_routes.c (`static` demoted) against stubbed ESP-IDF
layers + a fake coap3/coap.h (see tests/host_f_coap/) and runs the 57-path
harness (18 config-handler paths + 21 gpio claim-wiring handler paths + 18
control-tunables handler paths from ctrl phase-5). Requires WSL (Ubuntu-24.04)
with gcc + GNU ld; the repo has no native Windows host C toolchain.

Usage:
    python tests/test_host_f_coap.py

The harness is also picked up by tests/test_all.py.
"""

from __future__ import annotations

import os
import subprocess
import sys
import unittest

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST_DIR = os.path.join(REPO_ROOT, "tests", "host_f_coap")
WSL_DISTRO = "Ubuntu-24.04"

# All EPA execution paths, by test function name:
#   spec-01 phase-3 (GET + release callback): P1-P8
#   spec-02 phase-4 (POST /config import handler): P1-P10
#   gpio phases 2-5 (handle_fan_post/handle_fan_put/handle_source_post): HFP/HFU/HSP
#   ctrl phase-5 (handle_control_get/handle_control_put): HCG/HCP
ALL_PATHS = [
    # handle_config_get / coap_free_config_data (spec-01 phase-3)
    "test_config_get_export_failure_returns_500",  # P1
    "test_config_get_success_2_05_passes_buffer_to_libcoap",  # P2
    "test_config_get_success_empty_registries_empty_lists",  # P3
    "test_config_get_success_partial_registries_counts_match",  # P4
    "test_config_get_success_fully_loaded_passes_full_buffer_for_block2",  # P5
    "test_config_get_large_response_failure_no_double_free",  # P6
    "test_coap_free_config_data_frees_buffer",  # P7
    "test_coap_free_config_data_null_is_noop",  # P8
    # handle_config_post (spec-02 phase-4)
    "test_config_post_decode_failure_empty_body_returns_400",  # P1
    "test_config_post_decode_failure_malformed_body_returns_400",  # P2
    "test_config_post_validation_failure_returns_400_with_error_msg",  # P3
    "test_config_post_validation_failure_no_err_msg_returns_400_empty_error",  # P4
    "test_config_post_persist_failure_returns_500",  # P5
    "test_config_post_apply_failure_returns_400_and_reboots",  # P5b (import-fix M1)
    "test_config_post_success_schedules_reboot_timer_created",  # P6
    "test_config_post_success_reuses_existing_timer",  # P7
    "test_config_post_success_timer_start_failure_no_pending",  # P8
    "test_config_post_success_reboot_already_pending_skips_timer",  # P9
    "test_config_post_success_empty_config_clears_and_reboots",  # P10
    # handle_fan_post (gpio phases 2-5)
    "test_fan_post_decode_failure_returns_400",  # HFP-P1
    "test_fan_post_add_failure_returns_400_with_error_msg",  # HFP-P2
    "test_fan_post_add_failure_no_err_msg_returns_400_empty_error",  # HFP-P3
    "test_fan_post_success_returns_201_fan_info",  # HFP-P4
    # handle_fan_put (gpio phases 2-5)
    "test_fan_put_short_path_returns_404",  # HFU-P1
    "test_fan_put_decode_failure_returns_400",  # HFU-P2
    "test_fan_put_fan_not_found_returns_404",  # HFU-P3
    "test_fan_put_no_gpio_fields_skips_set_gpio",  # HFU-P4
    "test_fan_put_gpio_update_success_returns_204",  # HFU-P5
    "test_fan_put_gpio_update_failure_returns_400_with_error_msg_no_save",  # HFU-P6
    "test_fan_put_gpio_update_failure_no_err_msg_returns_400_empty_error",  # HFU-P7
    # handle_source_post (gpio phases 2-5)
    "test_source_post_temp_decode_failure_returns_400",  # HSP-P1
    "test_source_post_temp_source_not_found_returns_404",  # HSP-P2
    "test_source_post_temp_update_failure_returns_400",  # HSP-P3
    "test_source_post_temp_success_returns_204_ok",  # HSP-P4
    "test_source_post_create_decode_failure_returns_400",  # HSP-P5
    "test_source_post_ds18b20_add_failure_returns_400_no_status_body",  # HSP-P6
    "test_source_post_ds18b20_add_success_returns_201",  # HSP-P7
    "test_source_post_ntc_add_failure_returns_400_with_error_msg",  # HSP-P8
    "test_source_post_ntc_add_failure_no_err_msg_returns_400_empty_error",  # HSP-P9
    "test_source_post_manual_add_success_returns_201",  # HSP-P10
    # handle_control_get (ctrl phase-5)
    "test_control_get_null_control_returns_503",  # HCG-P1
    "test_control_get_tunables_failure_returns_500",  # HCG-P2
    "test_control_get_success_returns_205_all_fields",  # HCG-P3
    # handle_control_put (ctrl phase-5)
    "test_control_put_null_control_returns_503",  # HCP-P1
    "test_control_put_decode_failure_returns_400_decode_failed",  # HCP-P2
    "test_control_put_merge_base_failure_returns_500",  # HCP-P3
    "test_control_put_hysteresis_oob_returns_400",  # HCP-P4
    "test_control_put_ramp_up_oob_returns_400",  # HCP-P5
    "test_control_put_ramp_down_oob_returns_400",  # HCP-P6
    "test_control_put_failsafe_policy_oob_returns_400",  # HCP-P7
    "test_control_put_safe_duty_oob_returns_400",  # HCP-P8
    "test_control_put_all_fields_success_returns_204",  # HCP-P9
    "test_control_put_partial_hysteresis_only",  # HCP-P10
    "test_control_put_partial_ramp_up_only",  # HCP-P11
    "test_control_put_partial_ramp_down_only",  # HCP-P12
    "test_control_put_partial_failsafe_policy_only",  # HCP-P13
    "test_control_put_partial_safe_duty_only",  # HCP-P14
    "test_control_put_no_fields_noop_returns_204",  # HCP-P15
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
        "skipping host_f_coap C harness.",
        file=sys.stderr,
    )


@unittest.skipUnless(_WSL_AVAILABLE, f"WSL distro {WSL_DISTRO} not available")
class TestFCoapConfigHostC(unittest.TestCase):
    """Runs the compiled f_coap C harness and asserts all 57 paths pass."""

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
        self.assertRegex(self.proc.stdout, r"RESULT: 58 passed, 0 failed",
                         msg=self._summary())


if __name__ == "__main__":
    unittest.main()
