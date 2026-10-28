#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Exercise the TLS adapter against a real loopback OpenSSL TLS server."""

from contextlib import contextmanager
from pathlib import Path
import os
import runpy
import selectors
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
MBEDTLS = cached_provider("mbedtls")
OPENSSL = shutil.which("openssl")
PROJECT = r"""
cmake_minimum_required(VERSION 3.20)
project(tls_smoke LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(GEN_FILES OFF CACHE BOOL "" FORCE)
include(esy_feature_import.cmake)
add_library(esy_opt_default INTERFACE)
target_compile_options(esy_opt_default INTERFACE -Wall -Wextra -Werror)
include("${PLATFORM_ROOT}/cmake/esy_mbedtls_provider.cmake")
esy_configure_mbedtls_provider()
add_subdirectory("${PLATFORM_ROOT}/esy_common/defs" defs)
add_subdirectory("${PLATFORM_ROOT}/esy_common/utils" utils)
add_subdirectory("${PLATFORM_ROOT}/esy_middleware/crypto" crypto)
add_subdirectory("${PLATFORM_ROOT}/esy_middleware/network" network)
add_subdirectory("${PLATFORM_ROOT}/esy_middleware/credentials" credentials)
add_subdirectory("${PLATFORM_ROOT}/esy_middleware/tls" tls)
add_executable(tls_smoke "${TEST_SOURCE}/tls_smoke.c")
target_compile_options(tls_smoke PRIVATE -Wall -Wextra -Werror)
target_link_libraries(tls_smoke PRIVATE esy_tls)
add_library(header_cpp OBJECT "${CMAKE_CURRENT_SOURCE_DIR}/consumer.cpp")
target_link_libraries(header_cpp PRIVATE esy_tls)
"""


def run(*arguments):
    return subprocess.run(arguments, check=True, capture_output=True, text=True)


@unittest.skipUnless(MBEDTLS and OPENSSL, "Run ./setup.sh and install OpenSSL")
class TLSFixture(unittest.TestCase):
    @classmethod
    def provider_configuration(cls):
        return os.environ.get("MBEDTLS_CONFIG_FILE")

    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="esy-tls-smoke-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name)
        (cls.directory / "CMakeLists.txt").write_text(PROJECT, encoding="utf-8")
        (cls.directory / "esy_feature_import.cmake").write_text(
            "set(ENABLE_ESY_CRYPTO ON)\n"
            "set(ENABLE_ESY_NETWORK ON)\nset(ENABLE_ESY_NETWORK_LWIP OFF)\n"
            "set(ENABLE_ESY_CREDENTIALS ON)\nset(ENABLE_ESY_TLS ON)\n",
            encoding="utf-8")
        (cls.directory / "consumer.cpp").write_text(
            '#include "esy_middleware/tls/esy_tls_mbedtls.h"\n'
            'int tls_header_cpp() { esy_tls_t tls = {}; return sizeof(tls) == 0; }\n',
            encoding="utf-8")
        try:
            configuration = ["cmake", "-S", str(cls.directory), "-B", str(cls.directory / "out"),
                             f"-DPLATFORM_ROOT={PLATFORM}", f"-DTEST_SOURCE={HERE}",
                             f"-DESY_MBEDTLS_PATH={Path(MBEDTLS).resolve()}"]
            profile = cls.provider_configuration()
            if profile:
                configuration.append(
                    f"-DMBEDTLS_CONFIG_FILE={Path(profile).resolve()}")
            run(*configuration)
            run("cmake", "--build", str(cls.directory / "out"), "--parallel", "4")
        except subprocess.CalledProcessError as error:
            raise AssertionError(error.stdout + error.stderr) from error
        cls.client = cls.directory / "out/tls_smoke"
        cls.make_certificates()

    @classmethod
    def make_certificates(cls):
        cls.ca = cls.directory / "ca.pem"
        ca_key = cls.directory / "ca.key"
        run(OPENSSL, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2",
            "-subj", "/CN=Esy TLS smoke CA", "-keyout", str(ca_key), "-out", str(cls.ca),
            "-addext", "basicConstraints=critical,CA:TRUE",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign")
        cls.other_ca = cls.directory / "other-ca.pem"
        run(OPENSSL, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2",
            "-subj", "/CN=Untrusted smoke CA", "-out", str(cls.other_ca),
            "-keyout", str(cls.directory / "other-ca.key"),
            "-addext", "basicConstraints=critical,CA:TRUE")
        cls.server_certificate, cls.server_key = cls.make_leaf("server", "serverAuth")
        cls.client_certificate, cls.client_key = cls.make_leaf("client", "clientAuth")
        cls.expired_certificate = cls.directory / "expired.pem"
        ca_config = cls.directory / "ca.conf"
        (cls.directory / "index.txt").write_text("", encoding="utf-8")
        (cls.directory / "serial").write_text("1000\n", encoding="utf-8")
        ca_config.write_text(
            "[ca]\ndefault_ca=authority\n[authority]\n"
            f"database={cls.directory / 'index.txt'}\n"
            f"serial={cls.directory / 'serial'}\nnew_certs_dir={cls.directory}\n"
            f"certificate={cls.ca}\nprivate_key={ca_key}\n"
            "default_md=sha256\npolicy=subject\nx509_extensions=server\n"
            "[subject]\ncommonName=supplied\n[server]\n"
            "basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\n"
            "extendedKeyUsage=serverAuth\nsubjectAltName=DNS:localhost\n", encoding="utf-8")
        run(OPENSSL, "ca", "-config", str(ca_config), "-batch", "-notext",
            "-startdate", "20200101000000Z", "-enddate", "20200102000000Z",
            "-in", str(cls.directory / "server.csr"), "-out", str(cls.expired_certificate))
        cls.make_rotating_trust_anchors()

    @classmethod
    def make_rotating_trust_anchors(cls):
        """Use one CA identity with explicit dates around injected UTC 2035."""
        directory = cls.directory / "rotation"
        directory.mkdir()
        key = directory / "ca.key"
        request = directory / "ca.csr"
        bootstrap = directory / "bootstrap.pem"
        run(OPENSSL, "req", "-new", "-newkey", "rsa:2048", "-nodes",
            "-subj", "/CN=Rotating TLS smoke CA", "-keyout", str(key),
            "-out", str(request))
        run(OPENSSL, "x509", "-req", "-in", str(request), "-signkey", str(key),
            "-out", str(bootstrap), "-days", "1")
        configuration = directory / "ca.conf"
        configuration.write_text(
            "[ca]\ndefault_ca=authority\n[authority]\n"
            f"database={directory / 'index.txt'}\n"
            f"serial={directory / 'serial'}\nnew_certs_dir={directory}\n"
            f"certificate={bootstrap}\nprivate_key={key}\n"
            "default_md=sha256\npolicy=subject\n"
            "[subject]\ncommonName=supplied\n[anchor]\n"
            "basicConstraints=critical,CA:TRUE\n"
            "keyUsage=critical,keyCertSign,cRLSign\n[server]\n"
            "basicConstraints=critical,CA:FALSE\n"
            "keyUsage=critical,digitalSignature,keyEncipherment\n"
            "extendedKeyUsage=serverAuth\nsubjectAltName=DNS:localhost\n",
            encoding="utf-8")

        def issue(name, start, end, *, anchor=True):
            # Independent databases allow reissued certificates with one CA DN/key.
            (directory / "index.txt").write_text("", encoding="utf-8")
            (directory / "serial").write_text(f"{len(cls.rotation_certificates) + 4096:X}\n",
                                               encoding="utf-8")
            certificate = directory / f"{name}.pem"
            command = [OPENSSL, "ca", "-config", str(configuration), "-batch", "-notext",
                       "-extensions", "anchor" if anchor else "server", "-startdate", start,
                       "-enddate", end, "-in", str(request if anchor else cls.directory / "server.csr"),
                       "-out", str(certificate)]
            if anchor:
                command.append("-selfsign")
            run(*command)
            cls.rotation_certificates[name] = certificate

        cls.rotation_certificates = {}
        issue("old", "20200101000000Z", "20300101000000Z")
        issue("active", "20300101000000Z", "20400101000000Z")
        issue("future", "20400101000000Z", "20500101000000Z")
        issue("server", "20200101000000Z", "20500101000000Z", anchor=False)
        cls.rotation_bundles = {}
        for names in (("old", "active"), ("active", "old"),
                      ("future", "active"), ("active", "future"),
                      ("old", "future"), ("future", "old")):
            bundle = directory / f"{'-'.join(names)}.pem"
            bundle.write_bytes(b"".join(cls.rotation_certificates[name].read_bytes()
                                        for name in names))
            cls.rotation_bundles[names] = bundle

    @classmethod
    def make_leaf(cls, name, purpose):
        key = cls.directory / f"{name}.key"
        request = cls.directory / f"{name}.csr"
        certificate = cls.directory / f"{name}.pem"
        extension = cls.directory / f"{name}.ext"
        run(OPENSSL, "req", "-new", "-newkey", "rsa:2048", "-nodes",
            "-subj", f"/CN={'localhost' if name == 'server' else 'smoke client'}",
            "-keyout", str(key), "-out", str(request))
        extension.write_text("basicConstraints=critical,CA:FALSE\n"
                             "keyUsage=critical,digitalSignature,keyEncipherment\n"
                             f"extendedKeyUsage={purpose}\nsubjectAltName=DNS:localhost\n",
                             encoding="utf-8")
        run(OPENSSL, "x509", "-req", "-in", str(request), "-CA", str(cls.ca),
            "-CAkey", str(cls.directory / "ca.key"), "-CAcreateserial", "-days", "1",
            "-sha256", "-extfile", str(extension), "-out", str(certificate))
        return certificate, key

    @contextmanager
    def server(self, *, expired=False, mutual=False, tls13=False, interactive=False,
               certificate=None):
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        log_path = self.directory / f"server-{port}.log"
        with log_path.open("w+", encoding="utf-8") as log:
            command = [OPENSSL, "s_server", "-accept", f"127.0.0.1:{port}", "-cert",
                       str(certificate or (self.expired_certificate if expired
                                           else self.server_certificate)),
                       "-key", str(self.server_key), "-tls1_3" if tls13 else "-tls1_2"]
            if not interactive:
                command.extend(["-quiet", "-www"])
            if mutual:
                command.extend(["-Verify", "1", "-verify_return_error", "-CAfile", str(self.ca)])
            process = subprocess.Popen(command, stdin=subprocess.PIPE if interactive else None,
                                       stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        log.seek(0)
                        self.fail(log.read())
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                            break
                    except OSError:
                        time.sleep(0.01)
                else:
                    self.fail("OpenSSL loopback listener did not start")
                yield (port, process) if interactive else port
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
                if process.stdin:
                    process.stdin.close()

    def invoke(self, mode, port=0, *, hostname="localhost", ca=None, mutual=False):
        result = subprocess.run(
            [str(self.client), mode, str(port), str(ca or self.ca), hostname,
             str(self.client_certificate) if mutual else "-",
             str(self.client_key) if mutual else "-"],
            capture_output=True, text=True, timeout=15)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)


class TLSSmokeTests(TLSFixture):
    def test_authenticated_tls12_and_copied_send_under_backpressure(self):
        with self.server() as port:
            self.invoke("success", port)

    def test_tls13_only_server_is_rejected(self):
        with self.server(tls13=True) as port:
            self.invoke("reject", port)

    def test_hostname_mismatch_is_sticky_failure(self):
        with self.server() as port:
            self.invoke("reject", port, hostname="incorrect.example")

    def test_untrusted_ca_is_rejected(self):
        with self.server() as port:
            self.invoke("reject", port, ca=self.other_ca)

    def test_expired_certificate_is_rejected(self):
        with self.server(expired=True) as port:
            self.invoke("reject", port)

    def test_client_identity_supports_mutual_tls(self):
        with self.server(mutual=True) as port:
            self.invoke("success", port, mutual=True)

    def test_missing_client_identity_is_rejected(self):
        with self.server(mutual=True) as port:
            self.invoke("reject", port)

    def test_injected_utc_rejects_not_yet_valid_certificates(self):
        with self.server() as port:
            self.invoke("past", port)

    def test_injected_utc_rejects_expired_certificates(self):
        with self.server() as port:
            self.invoke("future", port)

    def test_injected_utc_accepts_valid_rotation_anchor_alone(self):
        with self.server(certificate=self.rotation_certificates["server"]) as port:
            self.invoke("fixed-success", port, ca=self.rotation_certificates["active"])

    def test_injected_utc_selects_valid_rotation_anchor_in_either_bundle_order(self):
        for names in (("old", "active"), ("active", "old"),
                      ("future", "active"), ("active", "future")):
            with self.subTest(bundle=names):
                with self.server(certificate=self.rotation_certificates["server"]) as port:
                    self.invoke("fixed-success", port, ca=self.rotation_bundles[names])

    def test_injected_utc_rejects_rotation_bundles_without_valid_anchor(self):
        for names in (("old", "future"), ("future", "old")):
            with self.subTest(bundle=names):
                with self.server(certificate=self.rotation_certificates["server"]) as port:
                    self.invoke("fixed-reject", port, ca=self.rotation_bundles[names])

    def test_injected_utc_rejects_expired_and_not_yet_valid_rotation_anchors(self):
        for name in ("old", "future"):
            with self.subTest(anchor=name):
                with self.server(certificate=self.rotation_certificates["server"]) as port:
                    mode = "fixed-future-reject" if name == "future" else "fixed-reject"
                    self.invoke(mode, port, ca=self.rotation_certificates[name])

    def test_rng_failure_fails_closed(self):
        self.invoke("random-failure")

    def test_utc_failure_fails_closed(self):
        with self.server() as port:
            self.invoke("utc-failure", port)

    def test_lifecycle_timeout_wrap_cancellation_and_new_attempt(self):
        self.invoke("lifecycle")

    def test_post_handshake_control_write_backpressure_fails_closed(self):
        with self.server(interactive=True) as (port, server):
            client = subprocess.Popen(
                [str(self.client), "renegotiate", str(port), str(self.ca), "localhost", "-", "-"],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                with selectors.DefaultSelector() as selector:
                    selector.register(client.stdout, selectors.EVENT_READ)
                    self.assertTrue(selector.select(timeout=10), "TLS client did not become ready")
                self.assertEqual("READY\n", client.stdout.readline())
                server.stdin.write(b"r\n")
                server.stdin.flush()
                output, error = client.communicate(timeout=10)
                self.assertEqual(0, client.returncode, output + error)
            finally:
                if client.poll() is None:
                    client.kill()
                    client.wait(timeout=5)
                client.stdout.close()
                client.stderr.close()


class PSATLSSmokeTests(TLSFixture):
    @classmethod
    def provider_configuration(cls):
        profile = cls.directory / "psa-profile.h"
        profile.write_text('#include "mbedtls/mbedtls_config.h"\n'
                           '#define MBEDTLS_USE_PSA_CRYPTO\n', encoding="utf-8")
        return profile

    def test_authenticated_tls12_with_consumer_initialized_psa(self):
        with self.server() as port:
            self.invoke("success", port)

    def test_mutual_tls_with_consumer_initialized_psa(self):
        with self.server(mutual=True) as port:
            self.invoke("success", port, mutual=True)


if __name__ == "__main__":
    unittest.main()
