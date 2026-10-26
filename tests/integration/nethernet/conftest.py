from __future__ import annotations

import os
import subprocess
import threading
from pathlib import Path

import pytest

REPOSITORY = Path(__file__).resolve().parents[3]
DEFAULT_ECHO = REPOSITORY / "build" / "RelWithDebInfo" / "tests" / "interop" / "endgate_nethernet_echo"


class Echo:
    """endgate's NetherNet stack behind one port, echoing every packet it receives."""

    def __init__(self, binary: Path) -> None:
        self.process = subprocess.Popen(
            [str(binary), "127.0.0.1"], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, text=True
        )
        first = self.process.stdout.readline().split()
        if first[:1] != ["listening"]:
            self.process.kill()
            raise RuntimeError(f"echo did not start: {first}")
        self.port = int(first[1])
        self.lines: list[str] = []
        threading.Thread(target=self._pump, daemon=True).start()

    @property
    def url(self) -> str:
        return f"http://127.0.0.1:{self.port}"

    def _pump(self) -> None:
        for line in self.process.stdout:
            self.lines.append(line.rstrip())

    def stop(self) -> None:
        self.process.terminate()
        self.process.wait(5)


@pytest.fixture
def echo() -> Echo:
    binary = Path(os.environ.get("ENDGATE_NETHERNET_ECHO", DEFAULT_ECHO))
    if not binary.exists():
        pytest.exit(f"endgate_nethernet_echo not found at {binary}")
    server = Echo(binary)
    yield server
    server.stop()
