#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Exercise real coreMQTT against a loopback Mosquitto broker, including TLS."""

import os
from pathlib import Path
import runpy
import shutil
import socket
import subprocess
import tempfile
import time
import unittest


HERE = Path(__file__).resolve().parent
PLATFORM = HERE.parents[2]
cached_provider = runpy.run_path(
    str(PLATFORM / "esy_test/utils/esy_provider_paths.py"))["cached_provider"]
MOSQUITTO = os.environ.get("MOSQUITTO_BIN") or shutil.which("mosquitto")


def unused_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


@unittest.skipUnless(MOSQUITTO, "Mosquitto is required; set MOSQUITTO_BIN or install it")
class BrokerSmokeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="esy-mqtt-broker-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name)
        provider_input = cached_provider("coremqtt")
        if not provider_input:
            raise RuntimeError("Run ./setup.sh or set ESY_COREMQTT_PATH for coreMQTT")
        provider = Path(provider_input).resolve()
        cls.client = cls.directory / "client"
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-DMQTT_DO_NOT_USE_CUSTOM_CONFIG=1",
            f"-I{PLATFORM / 'esy_middleware/mqtt/include'}",
            f"-I{PLATFORM / 'esy_middleware/network/include'}",
            f"-I{PLATFORM / 'esy_common/defs/include'}",
            f"-I{provider / 'source/include'}", f"-I{provider / 'source/interface'}",
            str(HERE / "broker_smoke.c"),
            str(PLATFORM / "esy_middleware/mqtt/src/esy_mqtt.c"),
            *[str(provider / "source" / name) for name in
              ("core_mqtt.c", "core_mqtt_state.c", "core_mqtt_serializer.c")],
            "-lssl", "-lcrypto", "-o", str(cls.client),
        ], check=True, capture_output=True, text=True)
        cls.certificate = cls.directory / "server.crt"
        key = cls.directory / "server.key"
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", str(key), "-out", str(cls.certificate), "-days", "1",
            "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
        ], check=True, capture_output=True)
        cls.plain_port, cls.tls_port = unused_port(), unused_port()
        configuration = cls.directory / "mosquitto.conf"
        configuration.write_text(
            "persistence false\nallow_anonymous true\n"
            f"listener {cls.plain_port} 127.0.0.1\n"
            f"listener {cls.tls_port} 127.0.0.1\n"
            f"certfile {cls.certificate}\nkeyfile {key}\n"
            + ("user root\n" if os.geteuid() == 0 else ""), encoding="utf-8")
        cls.log = (cls.directory / "broker.log").open("w+", encoding="utf-8")
        cls.addClassCleanup(cls.log.close)
        cls.broker = subprocess.Popen([MOSQUITTO, "-c", str(configuration)],
                                      stdout=cls.log, stderr=subprocess.STDOUT)
        cls.addClassCleanup(cls.stop_broker)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if cls.broker.poll() is not None:
                cls.log.seek(0)
                raise AssertionError(cls.log.read())
            try:
                with socket.create_connection(("127.0.0.1", cls.plain_port), timeout=0.1):
                    return
            except OSError:
                time.sleep(0.02)
        raise AssertionError("Loopback broker did not start within five seconds")

    @classmethod
    def stop_broker(cls):
        cls.broker.terminate()
        try:
            cls.broker.wait(timeout=5)
        except subprocess.TimeoutExpired:
            cls.broker.kill()
            cls.broker.wait(timeout=5)

    def test_plain_publish_subscribe_and_reconnect(self):
        result = subprocess.run([str(self.client), str(self.plain_port)],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_verified_tls_publish_subscribe_and_reconnect(self):
        result = subprocess.run([str(self.client), str(self.tls_port),
                                 str(self.certificate)], capture_output=True,
                                text=True, timeout=15)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_tls_rejects_wrong_broker_hostname(self):
        result = subprocess.run([str(self.client), str(self.tls_port),
                                 str(self.certificate), "wrong.example"],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(10, result.returncode, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
