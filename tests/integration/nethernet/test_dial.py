from __future__ import annotations

import asyncio
import os
import subprocess
import threading
from pathlib import Path

import pytest
from nethernet import serve_http
from nethernet.identity import IdentitySigner, generate_operator_key

REPOSITORY = Path(__file__).resolve().parents[3]
DEFAULT_DIAL = REPOSITORY / "build" / "RelWithDebInfo" / "tests" / "interop" / "endgate_nethernet_dial"


def dial(port: int, network_id: str = "endgate-dialer") -> subprocess.CompletedProcess[str]:
    binary = Path(os.environ.get("ENDGATE_NETHERNET_DIAL", DEFAULT_DIAL))
    if not binary.exists():
        pytest.exit(f"endgate_nethernet_dial not found at {binary}")
    return subprocess.run(
        [str(binary), "127.0.0.1", str(port), network_id],
        stdin=subprocess.DEVNULL,
        capture_output=True,
        text=True,
        timeout=40,
    )


class PythonServer:
    """nethernet-python's serve_http, an independent aiortc NetherNet server, running in its own loop."""

    def __init__(self) -> None:
        self._loop = asyncio.new_event_loop()
        self._ready = threading.Event()
        self._server = None
        self.port = 0
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        if not self._ready.wait(20):
            raise RuntimeError("python server did not start")

    def _run(self) -> None:
        asyncio.set_event_loop(self._loop)
        self._loop.run_until_complete(self._start())
        self._loop.run_forever()

    async def _start(self) -> None:
        signer = IdentitySigner(generate_operator_key(), domain="endgate-test")

        async def handler(connection) -> None:
            try:
                while True:
                    await connection.send(await connection.recv())
            except Exception:
                pass

        self._server = serve_http(handler, host="127.0.0.1", port=0, identity_signer=signer)
        await self._server.start()
        self.port = self._server.bound_port
        self._ready.set()

    def stop(self) -> None:
        async def close() -> None:
            await self._server.aclose()

        asyncio.run_coroutine_threadsafe(close(), self._loop).result(10)
        self._loop.call_soon_threadsafe(self._loop.stop)
        self._thread.join(10)


@pytest.fixture
def python_server() -> PythonServer:
    server = PythonServer()
    yield server
    server.stop()


def test_dialer_reaches_the_endgate_echo(echo):
    result = dial(echo.port)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "echo-ok" in result.stdout, result.stdout
    assert any(line.startswith("open ") for line in echo.lines)


def test_dialer_verifies_the_endgate_echo_identity(echo):
    result = dial(echo.port)
    assert "identity verified=1" in result.stdout, result.stdout


def test_dialer_reaches_an_aiortc_server(python_server):
    result = dial(python_server.port)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "echo-ok" in result.stdout, result.stdout
