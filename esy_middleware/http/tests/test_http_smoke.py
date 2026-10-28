#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 EsyProject contributors
# SPDX-License-Identifier: Apache-2.0

"""Exercise streaming HTTP and authenticated HTTPS against loopback servers."""

from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import runpy
import shutil
import ssl
import subprocess
import tempfile
import threading
import unittest

HERE = Path(__file__).resolve().parent
PLATFORM = HERE.parents[2]
cached_provider = runpy.run_path(
    str(PLATFORM / "esy_test/utils/esy_provider_paths.py"))["cached_provider"]
LLHTTP = cached_provider("llhttp")
MBEDTLS = cached_provider("mbedtls")
OPENSSL = shutil.which("openssl")
DOWNLOAD_BYTES = 196613
POST_BODY = b'{"enabled":true}'

PROJECT = r"""
cmake_minimum_required(VERSION 3.21)
project(http_smoke LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
include(esy_feature_import.cmake)
add_library(esy_opt_default INTERFACE)
target_compile_options(esy_opt_default INTERFACE -Wall -Wextra -Werror)
include("${PLATFORM_ROOT}/cmake/esy_llhttp_provider.cmake")
esy_configure_llhttp_provider()
add_subdirectory("${PLATFORM_ROOT}/esy_common/defs" defs)
add_subdirectory("${PLATFORM_ROOT}/esy_middleware/network" network)
add_subdirectory("${PLATFORM_ROOT}/esy_middleware/http" http)
add_executable(http_smoke "${TEST_SOURCE}/http_smoke.c")
target_compile_options(http_smoke PRIVATE -Wall -Wextra -Werror)
target_link_libraries(http_smoke PRIVATE Esy::http_client)
if(HTTP_SMOKE_TLS)
  include("${PLATFORM_ROOT}/cmake/esy_mbedtls_provider.cmake")
  esy_configure_mbedtls_provider()
  add_subdirectory("${PLATFORM_ROOT}/esy_common/utils" utils)
  add_subdirectory("${PLATFORM_ROOT}/esy_middleware/credentials" credentials)
  add_subdirectory("${PLATFORM_ROOT}/esy_middleware/crypto" crypto)
  add_subdirectory("${PLATFORM_ROOT}/esy_middleware/tls" tls)
  target_link_libraries(http_smoke PRIVATE esy_tls)
  target_compile_definitions(http_smoke PRIVATE HTTP_SMOKE_TLS=1)
endif()
"""


def run(*arguments):
    return subprocess.run(arguments, check=True, capture_output=True, text=True)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format, *args):
        pass

    def do_GET(self):
        if self.path not in ("/file", "/chunked"):
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("X-Smoke-Test", "loopback")
        self.send_header("Connection", "close")
        chunked = self.path == "/chunked"
        if chunked:
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Trailer", "X-Smoke-Trailer")
        else:
            self.send_header("Content-Length", str(DOWNLOAD_BYTES))
        self.end_headers()
        for offset in range(0, DOWNLOAD_BYTES, 997):
            size = min(997, DOWNLOAD_BYTES - offset)
            block = bytes((offset + index) % 251 for index in range(size))
            if chunked:
                self.wfile.write(f"{size:x};smoke=yes\r\n".encode("ascii"))
            self.wfile.write(block)
            if chunked:
                self.wfile.write(b"\r\n")
        if chunked:
            self.wfile.write(b"0\r\nX-Smoke-Trailer: complete\r\n\r\n")

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        if self.path != "/post" or body != POST_BODY or \
                self.headers.get("Content-Type") != "application/json":
            self.send_error(400)
            return
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Content-Type", "application/json")
        self.send_header("X-Smoke-Test", "loopback")
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)


@unittest.skipUnless(LLHTTP, "Run ./setup.sh to prepare llhttp v9.4.3")
class HttpSmokeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="esy-http-smoke-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name)
        cls.secure_available = bool(MBEDTLS and OPENSSL)
        (cls.directory / "CMakeLists.txt").write_text(PROJECT, encoding="utf-8")
        (cls.directory / "esy_feature_import.cmake").write_text(
            "set(ESY_RTOS None)\nset(ENABLE_ESY_NETWORK ON)\n"
            "set(ENABLE_ESY_HTTP_CLIENT ON)\nset(ENABLE_ESY_COMMON_UTILS_CRC32_TABLE OFF)\n",
            encoding="utf-8")
        arguments = ["cmake", "-S", str(cls.directory), "-B", str(cls.directory / "out"),
                     f"-DPLATFORM_ROOT={PLATFORM}", f"-DTEST_SOURCE={HERE}",
                     f"-DESY_LLHTTP_PATH={Path(LLHTTP).resolve()}",
                     f"-DHTTP_SMOKE_TLS={'ON' if cls.secure_available else 'OFF'}"]
        if cls.secure_available:
            arguments.append(f"-DESY_MBEDTLS_PATH={Path(MBEDTLS).resolve()}")
            profile = os.environ.get("MBEDTLS_CONFIG_FILE")
            if profile:
                arguments.append(f"-DMBEDTLS_CONFIG_FILE={Path(profile).resolve()}")
        try:
            run(*arguments)
            run("cmake", "--build", str(cls.directory / "out"), "--parallel", "4")
        except subprocess.CalledProcessError as error:
            raise AssertionError(error.stdout + error.stderr) from error
        cls.client = cls.directory / "out/http_smoke"
        cls.ca = cls.directory / "ca.pem"
        if cls.secure_available:
            cls.make_certificate()

    @classmethod
    def make_certificate(cls):
        ca_key = cls.directory / "ca.key"
        cls.certificate = cls.directory / "server.pem"
        cls.key = cls.directory / "server.key"
        request = cls.directory / "server.csr"
        extensions = cls.directory / "server.ext"
        extensions.write_text(
            "basicConstraints=critical,CA:FALSE\n"
            "keyUsage=critical,digitalSignature,keyEncipherment\n"
            "extendedKeyUsage=serverAuth\nsubjectAltName=DNS:localhost\n",
            encoding="utf-8")
        run(OPENSSL, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2",
            "-subj", "/CN=Esy HTTP smoke CA", "-keyout", str(ca_key), "-out", str(cls.ca),
            "-addext", "basicConstraints=critical,CA:TRUE",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign")
        run(OPENSSL, "req", "-new", "-newkey", "rsa:2048", "-nodes",
            "-subj", "/CN=localhost", "-keyout", str(cls.key), "-out", str(request))
        run(OPENSSL, "x509", "-req", "-in", str(request), "-CA", str(cls.ca),
            "-CAkey", str(ca_key), "-CAcreateserial", "-days", "2",
            "-extfile", str(extensions), "-out", str(cls.certificate))

    @contextmanager
    def server(self, *, secure=False):
        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.daemon_threads = True
        if secure:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.minimum_version = ssl.TLSVersion.TLSv1_2
            context.maximum_version = ssl.TLSVersion.TLSv1_2
            context.load_cert_chain(self.certificate, self.key)
            server.socket = context.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            yield server.server_port
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)
            self.assertFalse(thread.is_alive(), "HTTP loopback server did not stop")

    def invoke(self, mode, port, path, hostname="localhost"):
        result = subprocess.run(
            [str(self.client), mode, str(port), path, str(self.ca), hostname],
            capture_output=True, text=True, timeout=30)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_plain_large_fixed_length_and_chunked_download(self):
        for path in ("/file", "/chunked"):
            with self.subTest(path=path), self.server() as port:
                self.invoke("plain", port, path)

    def test_plain_rest_post(self):
        with self.server() as port:
            self.invoke("plain", port, "/post")

    def test_authenticated_https_streaming_download(self):
        if not self.secure_available:
            self.skipTest("Set ESY_MBEDTLS_PATH and install OpenSSL for HTTPS evidence")
        for path in ("/file", "/chunked"):
            with self.subTest(path=path), self.server(secure=True) as port:
                self.invoke("tls", port, path)

    def test_authenticated_https_rest_post(self):
        if not self.secure_available:
            self.skipTest("Set ESY_MBEDTLS_PATH and install OpenSSL for HTTPS evidence")
        with self.server(secure=True) as port:
            self.invoke("tls", port, "/post")

    def test_incorrect_tls_hostname_is_rejected_before_http(self):
        if not self.secure_available:
            self.skipTest("Set ESY_MBEDTLS_PATH and install OpenSSL for HTTPS evidence")
        with self.server(secure=True) as port:
            self.invoke("reject", port, "/file", hostname="wrong.example")


if __name__ == "__main__":
    unittest.main()
