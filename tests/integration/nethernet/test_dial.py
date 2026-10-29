from __future__ import annotations

import os
import subprocess
from pathlib import Path

import pytest
from harness import PythonServer

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
