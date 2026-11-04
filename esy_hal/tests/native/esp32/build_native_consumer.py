#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Build and inspect the pinned classic ESP32 native consumers."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


IDF_COMMIT = "1b459d9c4950395dec12ce19256c73f9a41e306f"
TOOLCHAIN_VERSION = "esp-13.2.0_20240530"
ROOT = Path(__file__).resolve().parent
PLATFORM = ROOT.parents[3]
PROFILES = ("core", "peripherals", "managed", "radio", "power")


def capture(*command):
    return subprocess.check_output(command, text=True).strip()


def run(*command):
    print("+ " + " ".join(str(value) for value in command), flush=True)
    subprocess.run([str(value) for value in command], check=True)


def source_fingerprint():
    paths = subprocess.check_output([
        "git", "-C", str(PLATFORM), "ls-files", "--cached", "--others", "--exclude-standard", "-z"])
    digest = hashlib.sha256()
    for name in sorted(set(paths.split(b"\0")) - {b""}):
        path = PLATFORM / os.fsdecode(name)
        digest.update(name + b"\0")
        digest.update(path.read_bytes() if path.is_file() else b"<removed>")
        digest.update(b"\0")
    diff = subprocess.check_output(["git", "-C", str(PLATFORM), "diff", "--binary", "HEAD"])
    return {"source_sha256": digest.hexdigest(),
            "tracked_diff_sha256": hashlib.sha256(diff).hexdigest(),
            "dirty": bool(capture("git", "-C", str(PLATFORM), "status", "--porcelain"))}


def prepare_build(build, profile):
    marker = build / ".esy-native-owner.json"
    identity = {"consumer": str(ROOT), "profile": profile, "idf_commit": IDF_COMMIT}
    if build.exists() and any(build.iterdir()):
        if not marker.is_file() or json.loads(marker.read_text()) != identity:
            raise RuntimeError(f"Build directory is not owned by this consumer; choose a fresh --build-root: {build}")
    build.mkdir(parents=True, exist_ok=True)
    marker.write_text(json.dumps(identity, indent=2) + "\n")
    # This consumer owns its generated SDK configuration. Recreate it from the
    # selected fixed defaults so prior menuconfig/cache state cannot win.
    for name in ("sdkconfig", "sdkconfig.old"):
        (build / name).unlink(missing_ok=True)


def inspect_build(build, profile, compiler_version, fingerprint):
    elf = build / "esy_esp32_native.elf"
    symbols = set(line.split()[-1] for line in
                  capture("xtensa-esp32-elf-nm", "--defined-only", str(elf)).splitlines()
                  if line.split())
    required = {"esy_native_probe_core", "esy_hal_system_init", "esy_os_init",
                "esy_os_task_set_core_affinity"}
    if profile != "core":
        required.update({"esy_native_probe_peripherals", "esy_hal_uart_init",
                         "esy_hal_gpio_enable_wakeup", "esy_hal_gpio_disable_wakeup",
                         "esy_hal_uart_rx_stream_start", "esy_hal_spi_init",
                         "esy_hal_spi_write_dma", "esy_hal_uart_send_timeout", "esy_hal_spi_abort",
                         "esy_hal_uart_send_dma", "esy_hal_uart_receive_dma",
                         "esy_hal_uart_is_busy", "esy_hal_uart_abort_dma",
                         "esy_hal_can_init", "esy_hal_can_set_filters",
                         "esy_hal_can_send", "esy_hal_can_receive", "esy_hal_can_get_status",
                         "esy_hal_i2c_init", "esy_hal_pwm_init", "esy_hal_timer_init",
                         "esy_hal_timer_capture_init", "esy_hal_timer_capture_get_caps",
                         "esy_hal_timer_capture_get_value", "esy_hal_timer_capture_get_interval",
                         "mcpwm_new_capture_timer", "mcpwm_new_capture_channel",
                         "mcpwm_capture_channel_register_event_callbacks",
                         "esy_hal_adc_sample_once", "esy_hal_flash_init",
                         "esy_os_queue_delete", "esy_os_mutex_delete",
                         "esy_os_semaphore_delete", "esy_os_timer_delete",
                         "esy_os_work_cancel_sync", "esy_os_stream_buffer_delete",
                         "esy_os_msg_buffer_delete"})
    if profile == "managed":
        required.update({"esy_spi_scheduler_create", "esy_uart_tx_scheduler_os_create",
                         "esy_uart_tx_scheduler_os_submit", "esy_uart_tx_scheduler_os_destroy"})
        required.update({"esy_native_probe_managed", "esy_bootloader_init",
                         "esy_ota_init", "esy_ota_flash_init", "esy_coredump_init",
                         "esy_hal_fault_init", "__wrap_esp_panic_handler"})
    if profile == "radio":
        required.update({"esy_native_probe_radio", "esy_hal_wifi_init_sta",
                         "esy_wifi_manager_init", "esy_network_lwip_init"})
    if profile == "power":
        required.update({"esy_native_probe_power", "esy_hal_power_sleep",
                         "esy_hal_shutdown", "esp_light_sleep_start", "esp_deep_sleep_start"})
    missing = required - symbols
    if missing:
        raise RuntimeError("Final ELF lost required public call closures: " + ", ".join(sorted(missing)))
    commands = json.loads((build / "compile_commands.json").read_text())
    backend_sources = sorted({Path(row["file"]).name for row in commands
                              if "/esy_hal_backend/src/esp32/" in row["file"]})
    if profile == "core" and any(name in backend_sources for name in
            ("esy_hb_uart.c", "esy_hb_spi.c", "esy_hb_adc.c", "esy_hb_can.c",
             "esy_hb_low_power.c", "esy_hb_wakeup_timer.c", "esy_hb_wifi.c",
             "esy_hb_boot.c", "esy_hb_fault.c")):
        raise RuntimeError("Disabled native features leaked into the core consumer")
    if profile != "managed" and "esy_hb_fault.c" in backend_sources:
        raise RuntimeError("Disabled fault adapter leaked into the native consumer")
    if profile != "power" and any(name in backend_sources for name in
                                  ("esy_hb_low_power.c", "esy_hb_wakeup_timer.c")):
        raise RuntimeError("Disabled power adapters leaked into the native consumer")
    if profile == "managed":
        table = capture("xtensa-esp32-elf-objdump", "-t", str(elf))
        if not any(".iram" in row and row.endswith("__wrap_esp_panic_handler")
                   for row in table.splitlines()):
            raise RuntimeError("Native panic wrapper is not linked into IRAM")
    metadata = {"profile": profile, "idf_commit": IDF_COMMIT,
                "toolchain": compiler_version, "elf": str(elf),
                "platform_commit": capture("git", "-C", str(PLATFORM), "rev-parse", "HEAD"),
                "backend_sources": backend_sources, "verified_symbols": sorted(required)}
    metadata.update(fingerprint)
    (build / "esy-native-evidence.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(f"Verified native {profile} ELF: {elf}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--profile", choices=(*PROFILES, "all"), default="all")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    idf = Path(os.environ.get("IDF_PATH", ""))
    if not (idf / "tools/cmake/project.cmake").is_file():
        parser.error("Source the ESP-IDF v5.3.4 export.sh before running this command")
    actual_commit = capture("git", "-C", str(idf), "rev-parse", "HEAD")
    if actual_commit != IDF_COMMIT:
        parser.error(f"ESP-IDF commit must be {IDF_COMMIT}; received {actual_commit}")
    for executable in ("cmake", "ninja", "xtensa-esp32-elf-gcc", "xtensa-esp32-elf-nm", "xtensa-esp32-elf-objdump"):
        if shutil.which(executable) is None:
            parser.error(f"Missing native build tool: {executable}")
    compiler_version = capture("xtensa-esp32-elf-gcc", "--version").splitlines()[0]
    if TOOLCHAIN_VERSION not in compiler_version:
        parser.error(f"Native compiler must use {TOOLCHAIN_VERSION}; received {compiler_version}")
    profiles = PROFILES if args.profile == "all" else (args.profile,)
    for profile in profiles:
        build = args.build_root.resolve() / profile
        if PLATFORM == build or PLATFORM in build.parents:
            parser.error("--build-root must be outside the platform source checkout")
        prepare_build(build, profile)
        fingerprint = source_fingerprint()
        defaults = str(ROOT / "sdkconfig.defaults")
        if profile == "radio":
            defaults += ";" + str(ROOT / "sdkconfig.radio")
        if profile == "power":
            defaults += ";" + str(ROOT / "sdkconfig.power")
        run("cmake", "-S", ROOT, "-B", build, "-G", "Ninja",
            "-DIDF_TARGET=esp32", f"-DESY_NATIVE_PROFILE={profile}",
            f"-DSDKCONFIG={build / 'sdkconfig'}", f"-DSDKCONFIG_DEFAULTS={defaults}")
        run("cmake", "--build", build, "--parallel", args.jobs)
        if fingerprint != source_fingerprint():
            raise RuntimeError("Platform source files changed during the native build; rerun after edits finish")
        inspect_build(build, profile, compiler_version, fingerprint)


if __name__ == "__main__":
    try:
        main()
    except (subprocess.CalledProcessError, RuntimeError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
