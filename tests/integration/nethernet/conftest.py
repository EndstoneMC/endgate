from __future__ import annotations

import os
from pathlib import Path

import pytest
from harness import DEFAULT_ECHO, Echo


@pytest.fixture
def echo() -> Echo:
    binary = Path(os.environ.get("ENDGATE_NETHERNET_ECHO", DEFAULT_ECHO))
    if not binary.exists():
        pytest.exit(f"endgate_nethernet_echo not found at {binary}")
    server = Echo(binary)
    yield server
    server.stop()
