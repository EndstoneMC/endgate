from __future__ import annotations

import gzip
import http.server
import json
import re
import threading
import time
from dataclasses import dataclass, field

import pytest

PATH = "/api/v2/data/server-implementation"


@dataclass
class Submission:
    path: str
    headers: dict[str, str]
    payload: dict = field(default_factory=dict)


class BStats:
    """A stand-in for bStats' data endpoint, on loopback."""

    def __init__(self) -> None:
        self.submissions: list[Submission] = []
        stats = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self) -> None:
                body = self.rfile.read(int(self.headers["Content-Length"]))
                stats.submissions.append(Submission(self.path, dict(self.headers), json.loads(gzip.decompress(body))))
                self.send_response(201)
                self.send_header("Content-Length", "0")
                self.end_headers()

            def log_message(self, *args) -> None:
                pass

        self._server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=self._server.serve_forever, daemon=True).start()

    @property
    def uri(self) -> str:
        return f"http://127.0.0.1:{self._server.server_address[1]}{PATH}"

    def wait(self, timeout: float = 10.0) -> Submission | None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.submissions:
                return self.submissions[0]
            time.sleep(0.05)
        return None

    def close(self) -> None:
        self._server.shutdown()
        self._server.server_close()


@pytest.fixture
def bstats(monkeypatch: pytest.MonkeyPatch):
    stats = BStats()
    monkeypatch.setenv("ENDGATE_BSTATS_URI", stats.uri)
    yield stats
    stats.close()


def charts(submission: Submission) -> dict[str, dict]:
    return {chart["chartId"]: chart["data"] for chart in submission.payload["service"]["customCharts"]}


def test_metrics_are_submitted_to_bstats(make_proxy, backend, bstats):
    proxy = make_proxy(backend.port)

    submission = bstats.wait()

    assert submission is not None, proxy.lines[-20:]
    assert submission.path == PATH
    assert submission.headers["Content-Encoding"] == "gzip"
    assert submission.headers["Content-Type"] == "application/json"
    assert submission.headers["Accept"] == "application/json"
    assert submission.headers["User-Agent"] == "Metrics-Service/1"
    assert submission.headers["Connection"] == "close"
    assert proxy.wait_for("WARN Metrics are sent to")


def test_the_payload_has_bstats_shape(make_proxy, backend, bstats):
    proxy = make_proxy(backend.port, servers={"second": "127.0.0.1:19999"})

    submission = bstats.wait()

    assert submission is not None, proxy.lines[-20:]
    payload = submission.payload
    assert list(payload) == ["osName", "osArch", "osVersion", "coreCount", "service", "serverUUID", "metricsVersion"]
    assert re.fullmatch(r"Windows 1[01]|Linux", payload["osName"])
    assert payload["osArch"] in ("amd64", "aarch64")
    assert payload["osVersion"]
    assert payload["coreCount"] >= 1
    assert re.fullmatch(r"[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}", payload["serverUUID"])
    assert payload["metricsVersion"] == "3.0.3"
    assert list(payload["service"]) == ["id", "customCharts"]
    assert payload["service"]["id"] == 30353


def test_the_charts_are_velocitys(make_proxy, backend, bstats):
    proxy = make_proxy(backend.port, servers={"second": "127.0.0.1:19999"})

    submission = bstats.wait()

    assert submission is not None, proxy.lines[-20:]
    data = charts(submission)
    assert set(data) == {"managed_servers", "online_mode", "endgate_version", "operating_system"}
    assert data["managed_servers"] == {"value": 2}
    assert data["online_mode"] == {"value": "offline"}
    assert re.fullmatch(r"\d+\.\d+\.\d+", data["endgate_version"]["value"])
    ((family, versions),) = data["operating_system"]["values"].items()
    assert family in ("Windows", "Linux")
    assert list(versions.values()) == [1]


def test_the_first_run_writes_the_config_and_says_so(make_proxy, backend, bstats):
    proxy = make_proxy(backend.port)
    assert bstats.wait() is not None

    config = (proxy.workdir / "plugins" / "bstats" / "config.toml").read_text()

    assert "enabled = true" in config
    assert re.search(r'server-uuid = "[0-9a-f-]{36}"', config)
    assert "log-errors = false" in config
    assert proxy.wait_for(r"INFO Endgate collects metrics and sends them to bStats")
    assert proxy.wait_for(r"INFO the 'plugins/bstats' folder and setting enabled to false\.")


def test_the_server_uuid_is_kept_between_runs(make_proxy, backend, bstats):
    first = make_proxy(backend.port)
    assert bstats.wait() is not None
    uuid = bstats.submissions[0].payload["serverUUID"]
    assert first.stop() == 0

    bstats.submissions.clear()
    first.start()

    assert bstats.wait() is not None
    assert bstats.submissions[0].payload["serverUUID"] == uuid
    assert not first.events("collects metrics")


def test_the_metrics_table_turns_it_off(make_proxy, backend, bstats):
    proxy = make_proxy(backend.port, settings={"metrics": {"enabled": "false"}})

    assert bstats.wait(2) is None
    assert "enabled = false" in (proxy.workdir / "plugins" / "bstats" / "config.toml").read_text()


def test_config_txt_turns_it_off(make_proxy, backend, bstats):
    first = make_proxy(backend.port)
    assert bstats.wait() is not None
    assert first.stop() == 0
    config = first.workdir / "plugins" / "bstats" / "config.toml"
    config.write_text(config.read_text().replace("enabled = true", "enabled = false"))

    bstats.submissions.clear()
    first.start()

    assert bstats.wait(2) is None
